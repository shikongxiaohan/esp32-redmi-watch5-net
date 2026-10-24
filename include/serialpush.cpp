/*
 * 串口表盘推送接收器实现
 */
#include "serialpush.h"
#include "serialpush_proto.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <vector>

namespace xw {

void SerialPush::begin(WebUi& web) {
    _web = &web;
    _line.reserve(1024);
}

void SerialPush::_reply(const char* fmt, ...) {
    char buf[192];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    Serial.print("XWFS ");
    Serial.println(buf);
}

void SerialPush::_reset() {
    _receiving = false;
    _expected = 0;
    _received = 0;
    _discardLine = false;
    _line.clear();
}

void SerialPush::loop() {
    // 逐字节读取（Arduino Stream 缓冲），行首前缀判定
    while (Serial.available() > 0) {
        int c = Serial.read();
        if (c < 0) break;
        char ch = (char)c;
        if (ch == '\n') {
            if (_discardLine) {
                _discardLine = false;
                _line.clear();
                continue;
            }
            if (!_line.empty() && _line.back() == '\r') _line.pop_back();
            _handleLine(_line);
            _line.clear();
            if (_line.capacity() > 4096) { _line.clear(); _line.shrink_to_fit(); }
            continue;
        }
        if (ch == '\r') continue;
        if (_discardLine) {
            // 上一行超长被丢弃：把本行剩余字符全部丢掉，直到换行。
            // 否则残余字节会被当作一条新命令重新累积。
            continue;
        }
        if (_line.size() >= PushParser::MAX_LINE) {
            // 超长行：丢弃整行（直到换行），避免缓冲无限增长，
            // 也避免残余字符被拼成伪命令（如 DATA/END）误触发写盘流程。
            _line.clear();
            _discardLine = true;
            _reply("ERR line too long");
            continue;
        }
        _line.push_back(ch);
    }

    // 推送超时（30 秒无数据）→ 中止
    if (_receiving && millis() - _lastDataMs > 30000) {
        _reply("ERR timeout");
        if (_web) _web->wfPushAbort();
        _reset();
    }
}

void SerialPush::_handleLine(const std::string& line) {
    if (!PushParser::isCommand(line)) return;  // 日志行，忽略
    PushLine p = PushParser::parse(line);

    switch (p.cmd) {
        case PushCmd::BEGIN: {
            if (!_web) { _reply("ERR no web"); return; }
            if (_receiving) {
                _web->wfPushAbort();
                _reset();
            }
            std::string err;
            if (!_web->wfPushBegin(p.num, p.text, err)) {
                _reply("ERR %s", err.c_str());
                return;
            }
            _receiving = true;
            _expected = p.num;
            _received = 0;
            _lastDataMs = millis();
            _reply("READY %u", (unsigned)p.num);
            return;
        }
        case PushCmd::DATA: {
            if (!_receiving) { _reply("ERR not receiving"); return; }
            if (p.text.empty()) return;
            // base64 解码
            size_t cap = (p.text.size() / 4 + 1) * 3;
            std::vector<uint8_t> buf(cap);
            size_t len = 0;
            if (!base64Decode(p.text, buf.data(), buf.size(), len) || len == 0) {
                _reply("ERR bad base64");
                _web->wfPushAbort();
                _reset();
                return;
            }
            if (_received + len > _expected) {
                _reply("ERR overflow");
                _web->wfPushAbort();
                _reset();
                return;
            }
            size_t n = _web->wfPushData(buf.data(), len);
            if (n != len) {
                _reply("ERR write failed");
                _web->wfPushAbort();
                _reset();
                return;
            }
            _received += uint32_t(len);
            _lastDataMs = millis();
            _reply("PROGRESS %u", (unsigned)_received);
            return;
        }
        case PushCmd::END: {
            if (!_receiving) { _reply("ERR not receiving"); return; }
            if (_received != _expected) {
                _reply("ERR size mismatch %u/%u", (unsigned)_received, (unsigned)_expected);
                _web->wfPushAbort();
                _reset();
                return;
            }
            std::string err;
            bool ok = _web->wfPushEnd(err);
            if (!ok) {
                _reply("ERR %s", err.c_str());
                _reset();
                return;
            }
            _reply("OK %u", (unsigned)_received);
            _reset();
            return;
        }
        case PushCmd::ABORT: {
            if (_web) _web->wfPushAbort();
            _reset();
            _reply("OK 0");
            return;
        }
        case PushCmd::STATUS: {
            _reply("STATE %u %u", (unsigned)_received, (unsigned)_expected);
            return;
        }
        case PushCmd::BAUD: {
            // 建议 PC 端切换波特率；板子侧 UART0 波特率在下次 Serial.begin 生效
            _reply("BAUD %u", (unsigned)p.num);
            return;
        }
        default:
            _reply("ERR unknown command");
            return;
    }
}

} // namespace xw
