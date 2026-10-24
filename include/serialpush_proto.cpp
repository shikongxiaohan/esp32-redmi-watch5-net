/*
 * 表盘串口推送协议 —— 纯逻辑实现（无 Arduino 依赖，可在主机端单测）
 */
#include "serialpush_proto.h"

#include <cctype>
#include <cstring>

namespace xw {

static const char* B64_ALPHA = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static int b64Value(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

bool PushParser::isCommand(const std::string& line) {
    const size_t n = std::strlen(prefix());
    if (line.size() < n) return false;
    return line.compare(0, n, prefix()) == 0;
}

PushLine PushParser::parse(const std::string& line) {
    PushLine out;
    out.raw = line;
    if (!isCommand(line)) return out;

    std::string rest = line.substr(std::strlen(prefix()));

    // 去掉行尾空白
    while (!rest.empty() && (rest.back() == '\r' || rest.back() == ' ' || rest.back() == '\t')) {
        rest.pop_back();
    }

    // 第一个空格分隔命令字
    size_t sp = rest.find(' ');
    std::string verb = (sp == std::string::npos) ? rest : rest.substr(0, sp);
    std::string args = (sp == std::string::npos) ? std::string() : rest.substr(sp + 1);

    // 去掉参数前导空白
    size_t a = 0;
    while (a < args.size() && (args[a] == ' ' || args[a] == '\t')) a++;
    args = args.substr(a);

    if (verb == "BEGIN") {
        out.cmd = PushCmd::BEGIN;
        // BEGIN <size> <name>
        size_t s2 = args.find(' ');
        std::string numStr = (s2 == std::string::npos) ? args : args.substr(0, s2);
        if (s2 != std::string::npos) {
            size_t b = s2 + 1;
            while (b < args.size() && args[b] == ' ') b++;
            out.text = args.substr(b);
        }
        // 手写十进制解析（避免依赖 strtol 的异常输入）
        uint32_t v = 0;
        bool ok = !numStr.empty();
        for (char c : numStr) {
            if (c < '0' || c > '9') { ok = false; break; }
            if (v > 0x0FFFFFFFu) { ok = false; break; }  // 乘前判溢出（保证 v*10 不溢出）
            v = v * 10 + uint32_t(c - '0');
        }
        if (!ok) {
            out.cmd = PushCmd::UNKNOWN;
        } else {
            out.num = v;
        }
    } else if (verb == "DATA") {
        out.cmd = PushCmd::DATA;
        out.text = args;
    } else if (verb == "END") {
        out.cmd = PushCmd::END;
    } else if (verb == "ABORT") {
        out.cmd = PushCmd::ABORT;
    } else if (verb == "STATUS") {
        out.cmd = PushCmd::STATUS;
    } else if (verb == "BAUD") {
        out.cmd = PushCmd::BAUD;
        uint32_t v = 0;
        bool ok = !args.empty();
        for (char c : args) {
            if (c < '0' || c > '9') { ok = false; break; }
            v = v * 10 + uint32_t(c - '0');
            if (v > 4000000u) { ok = false; break; }
        }
        if (!ok) out.cmd = PushCmd::UNKNOWN;
        else out.num = v;
    } else {
        out.cmd = PushCmd::UNKNOWN;
    }
    return out;
}

bool base64Decode(const std::string& in, uint8_t* out, size_t outCap, size_t& outLen) {
    outLen = 0;
    uint32_t acc = 0;
    int bits = 0;
    for (char c : in) {
        if (c == '=' ) break;               // 填充结束
        if (c == ' ' || c == '\r' || c == '\n' || c == '\t') continue;
        int v = b64Value(c);
        if (v < 0) return false;
        acc = (acc << 6) | uint32_t(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (outLen >= outCap) return false;
            out[outLen++] = uint8_t((acc >> bits) & 0xFF);
        }
    }
    return true;
}

std::string base64Encode(const uint8_t* in, size_t len) {
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    size_t i = 0;
    while (i + 2 < len) {
        uint32_t v = (uint32_t(in[i]) << 16) | (uint32_t(in[i + 1]) << 8) | in[i + 2];
        out += B64_ALPHA[(v >> 18) & 0x3F];
        out += B64_ALPHA[(v >> 12) & 0x3F];
        out += B64_ALPHA[(v >> 6) & 0x3F];
        out += B64_ALPHA[v & 0x3F];
        i += 3;
    }
    size_t rem = len - i;
    if (rem == 1) {
        uint32_t v = uint32_t(in[i]) << 16;
        out += B64_ALPHA[(v >> 18) & 0x3F];
        out += B64_ALPHA[(v >> 12) & 0x3F];
        out += "==";
    } else if (rem == 2) {
        uint32_t v = (uint32_t(in[i]) << 16) | (uint32_t(in[i + 1]) << 8);
        out += B64_ALPHA[(v >> 18) & 0x3F];
        out += B64_ALPHA[(v >> 12) & 0x3F];
        out += B64_ALPHA[(v >> 6) & 0x3F];
        out += "=";
    }
    return out;
}

} // namespace xw
