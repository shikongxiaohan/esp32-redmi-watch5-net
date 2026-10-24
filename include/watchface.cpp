/*
 * 自定义表盘安装（见 watchface.h 头注释中的协议依据）
 * 纯逻辑部分（解析 + 状态机）无平台依赖，可主机单测；
 * 文件源由 WebUi 传入（ESP32 下为 OTA 临时分区读取器）。
 */
#include "watchface.h"
#include "md5.h"
#include "crc32.h"
#include "proto.h"

#include <algorithm>
#include <cctype>
#include <cstring>

namespace xw {

// ===================== 文件头解析 =====================

// Gadgetbridge FACE_LOCALE_BITMAP_LIST 顺序（位图索引 -> locale）
static const char* const kFaceLocales[] = {
    "en_US", "zh_CN", "zh_TW", "ja_JP", "es_ES", "fr_FR", "de_DE", "ru_RU",
    "pt_BR", "pt_PT", "it_IT", "ko_KR", "tr_TR", "nl_NL", "th_TH", "sv_SE",
    "da_DK", "vi_VN", "nb_NO", "pl_PL", "fi_FI", "in_ID", "el_GR", "ro_RO",
    "cs_CZ", "uk_UA", "hu_HU", "sk_SK", "ar_EG", "iw_IL", "zh_HK",
};
static const size_t kFaceLocaleCount = sizeof(kFaceLocales) / sizeof(kFaceLocales[0]);

static uint32_t rdU32le(const uint8_t* p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

static void wrU32le(uint8_t* p, uint32_t v) {
    p[0] = uint8_t(v);
    p[1] = uint8_t(v >> 8);
    p[2] = uint8_t(v >> 16);
    p[3] = uint8_t(v >> 24);
}

// 读 NUL 结尾字符串（超出 maxLen 未找到 NUL 则失败）
static bool readCString(const uint8_t* buf, size_t bufLen, size_t offset,
                        size_t maxLen, std::string& out) {
    out.clear();
    if (offset >= bufLen) return false;
    for (size_t i = offset; i < bufLen && i - offset < maxLen; i++) {
        if (buf[i] == 0) return true;
        out += char(buf[i]);
    }
    return false;
}

// 选择 locale 位图索引: zh_CN 精确 > 中文（zh_TW/zh_HK）> en_US > 第一个可用
// 与 Gadgetbridge findBestI18n（用户语言 zh_CN）行为一致
static int pickFaceLocale(uint64_t available) {
    if (available & (1ull << 1)) return 1;  // zh_CN
    if (available & (1ull << 2)) return 2;  // zh_TW
    if (available & (1ull << 30)) return 30; // zh_HK
    if (available & (1ull << 0)) return 0;  // en_US
    for (size_t i = 0; i < kFaceLocaleCount; i++) {
        if (available & (1ull << i)) return int(i);
    }
    return -1;
}

// 从 i18n 表提取本地化名（Gadgetbridge extractTranslatableString）
static bool extractLocalizedName(WatchfaceSource& src, uint32_t tableOffset,
                                 uint32_t tableSize, std::string& out) {
    out.clear();
    if (tableSize < 8 || tableSize > 65536) return false;
    uint64_t fileSize = src.size();
    if (uint64_t(tableOffset) + tableSize > fileSize) return false;

    std::vector<uint8_t> table(tableSize);
    if (src.readAt(tableOffset, table.data(), tableSize) != tableSize) return false;

    uint64_t available = 0;
    for (int i = 0; i < 8; i++) available |= uint64_t(table[i]) << (i * 8);
    int count = 0;
    for (uint64_t v = available; v; v >>= 1) count += int(v & 1);
    if (tableSize < size_t(count * 4 + 8)) return false;

    int target = pickFaceLocale(available);
    if (target < 0) return false;

    // 长度表: 仅统计置位的 locale（每个 4 字节，紧跟位图之后）
    uint32_t targetOffset = 0, targetSize = 0;
    int li = 0;
    uint64_t bits = available;
    size_t sizeTablePos = 8;
    for (int i = 0; bits; i++) {
        if (bits & 1) {
            uint32_t sz = rdU32le(table.data() + sizeTablePos);
            sizeTablePos += 4;
            if (i < target) targetOffset += sz;
            if (i == target) targetSize = sz;
            li++;
        }
        bits >>= 1;
    }
    (void)li;
    // 数据区起点 = 位图 8 字节 + count*4 长度表
    size_t dataPos = 8 + size_t(count) * 4;
    if (dataPos + targetOffset + targetSize > tableSize) return false;

    out.assign(reinterpret_cast<const char*>(table.data() + dataPos + targetOffset), targetSize);
    return true;
}

WatchfaceInfo parseWatchfaceHeader(WatchfaceSource& src) {
    WatchfaceInfo info;
    uint32_t sz = src.size();
    if (sz < 0x80) return info; // 头部至少 128 字节

    uint8_t hdr[256];
    size_t n = src.readAt(0, hdr, sizeof(hdr));
    if (n < 0x80) return info;

    if (hdr[0] != 0x5A || hdr[1] != 0xA5) return info; // magic

    info.size = sz;
    info.previewOffset = rdU32le(hdr + 0x20);

    // id: 0x28 起，NUL 结尾，必须纯数字（Gadgetbridge ^\d+$）
    std::string id;
    if (!readCString(hdr, n, 0x28, 32, id) || id.empty()) return info;
    for (char c : id) {
        if (!std::isdigit((unsigned char)c)) return info;
    }
    info.id = id;

    // name: 0x68 起；FFFFFFFF = 本地化表
    if (hdr[0x68] == 0xFF && hdr[0x69] == 0xFF && hdr[0x6A] == 0xFF && hdr[0x6B] == 0xFF) {
        uint32_t tableOffset = rdU32le(hdr + 0x74);
        uint32_t tableSize = rdU32le(hdr + 0x78);
        std::string localized;
        extractLocalizedName(src, tableOffset, tableSize, localized);
        info.name = localized; // 失败时为空（Gadgetbridge 同样容忍）
    } else {
        std::string name;
        readCString(hdr, n, 0x68, 64, name);
        info.name = name;
    }

    info.valid = true;
    return info;
}

// ===================== 安装状态机 =====================

void WatchfaceInstaller::begin(SendCmdFn sendCmd, SendChunkFn sendChunk) {
    _sendCmd = std::move(sendCmd);
    _sendChunk = std::move(sendChunk);
}

bool WatchfaceInstaller::start(WatchfaceSource& src) {
    if (busy()) return false;
    _src = &src;
    _state = WfState::SCANNING;
    _scanPhase = 0;
    _scanPos = 0;
    _progress = 0;
    _error.clear();
    _md5.reset();
    _crc.reset();
    std::memset(_md5Digest, 0, sizeof(_md5Digest));
    std::memset(_prefix, 0, sizeof(_prefix));
    std::memset(_crcBytes, 0, sizeof(_crcBytes));
    _currentPart = 0;
    _totalParts = 0;
    _chunkBuf.clear();

    _info = parseWatchfaceHeader(src);
    if (!_info.valid) {
        _fail("无效的表盘文件（非 vela .bin）");
        return false;
    }
    if (_info.size == 0 || _info.size > _maxBytes) {
        _fail("表盘文件过大");
        return false;
    }
    if (onProgress) onProgress(0, "校验文件");
    return true;
}

void WatchfaceInstaller::cancel() {
    if (busy()) _fail("安装已取消");
}

void WatchfaceInstaller::loop() {
    switch (_state) {
        case WfState::SCANNING:
            _scanStep();
            break;
        case WfState::UPLOADING:
            _sendNextChunk();
            break;
        case WfState::INSTALL_START:
        case WfState::UPLOAD_REQUEST:
            if (millis() - _waitStartMs > 15000) {
                _fail(_state == WfState::INSTALL_START ? "手表未响应安装请求" : "手表未响应上传请求");
            }
            break;
        default:
            break;
    }
}

void WatchfaceInstaller::onCommand(const MiCommand& cmd) {
    if (_state == WfState::INSTALL_START &&
        cmd.type == cmd::TYPE_WATCHFACE && cmd.subtype == cmd::WATCHFACE_INSTALL) {
        WatchfaceInstallStatus st = parseWatchfaceInstallStatus(cmd.watchface);
        if (!st.valid) return;
        if (st.installStatus != 0) {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "手表拒绝安装 (status=%u)", st.installStatus);
            _fail(buf);
            return;
        }
        std::vector<uint8_t> c = buildDataUploadRequestCmd(
            cmd::UPLOAD_TYPE_WATCHFACE, _md5Digest, _info.size);
        if (!_sendCmd || !_sendCmd(c.data(), c.size())) {
            _fail("发送上传请求失败");
            return;
        }
        _state = WfState::UPLOAD_REQUEST;
        _waitStartMs = millis();
        _progress = 5;
        if (onProgress) onProgress(_progress, "等待手表接收");
        return;
    }

    if (_state == WfState::UPLOAD_REQUEST &&
        cmd.type == cmd::TYPE_DATA_UPLOAD && cmd.subtype == cmd::UPLOAD_START) {
        DataUploadAck ack = parseDataUploadAck(cmd.dataUpload);
        if (!ack.valid) return;
        if (ack.unknown2 != 0 || ack.resumePosition != 0) {
            _fail("手表上传应答异常");
            return;
        }
        _chunkSize = _clampChunkSize(ack.hasChunkSize ? ack.chunkSize : 2048);
        _partSize = _chunkSize - 4;
        _payloadLen = 22 + _info.size + 4;
        _totalParts = (_payloadLen + _partSize - 1) / _partSize;
        _currentPart = 0;
        _chunkBuf.clear();
        _chunkBuf.reserve(_chunkSize);
        _state = WfState::UPLOADING;
        _log("upload started");
        return;
    }
}

void WatchfaceInstaller::_scanStep() {
    uint32_t sz = _src ? _src->size() : 0;

    if (_scanPhase == 0) {
        // MD5 全文件扫描（每次 loop 一片）
        if (_scanPos >= sz) {
            _md5.final(_md5Digest);
            // 构造上传流前缀 [0x00][type][md5][size]，并启动 CRC
            _prefix[0] = 0x00;
            _prefix[1] = cmd::UPLOAD_TYPE_WATCHFACE;
            std::memcpy(_prefix + 2, _md5Digest, 16);
            wrU32le(_prefix + 18, sz);
            _crc.reset();
            _crc.update(_prefix, 22);
            _scanPhase = 1;
            _scanPos = 0;
            return;
        }
        uint32_t k = std::min<uint32_t>(SCAN_SLICE, sz - _scanPos);
        size_t r = _src->readAt(_scanPos, _scanBuf, k);
        if (r == 0) {
            _fail("读取文件失败");
            return;
        }
        _md5.update(_scanBuf, r);
        _scanPos += uint32_t(r);
        return;
    }

    // _scanPhase == 1: CRC32 文件数据分片
    if (_scanPos >= sz) {
        uint32_t crc = _crc.final();
        wrU32le(_crcBytes, crc);
        // 发送 installStart
        std::vector<uint8_t> c = buildWatchfaceInstallStartCmd(_info.id, sz);
        if (!_sendCmd || !_sendCmd(c.data(), c.size())) {
            _fail("发送安装开始命令失败");
            return;
        }
        _state = WfState::INSTALL_START;
        _waitStartMs = millis();
        _progress = 1;
        if (onProgress) onProgress(_progress, "等待手表确认");
        return;
    }
    uint32_t k = std::min<uint32_t>(SCAN_SLICE, sz - _scanPos);
    size_t r = _src->readAt(_scanPos, _scanBuf, k);
    if (r == 0) {
        _fail("读取文件失败");
        return;
    }
    _crc.update(_scanBuf, r);
    _scanPos += uint32_t(r);
}

void WatchfaceInstaller::_sendNextChunk() {
    uint32_t offset = _currentPart * _partSize;
    uint32_t remain = _payloadLen - offset;
    uint32_t dataLen = std::min(_partSize, remain);

    _chunkBuf.resize(4 + dataLen);
    uint16_t totalParts = uint16_t(_totalParts);
    uint16_t currentPart = uint16_t(_currentPart + 1); // 1 基
    _chunkBuf[0] = uint8_t(totalParts);
    _chunkBuf[1] = uint8_t(totalParts >> 8);
    _chunkBuf[2] = uint8_t(currentPart);
    _chunkBuf[3] = uint8_t(currentPart >> 8);

    size_t r = _readPayload(offset, _chunkBuf.data() + 4, dataLen);
    if (r != dataLen) {
        _fail("读取文件失败");
        return;
    }
    if (!_sendChunk || !_sendChunk(_chunkBuf.data(), _chunkBuf.size())) {
        _fail("发送数据块失败");
        return;
    }

    _currentPart++;
    int pct = int((uint64_t(_currentPart) * 100) / _totalParts);
    _progress = pct;
    if (onProgress) onProgress(pct, "上传表盘");

    if (_currentPart >= _totalParts) {
        // 全部发完：激活表盘（Gadgetbridge onUploadFinish -> setWatchface）
        std::vector<uint8_t> c = buildWatchfaceSetCmd(_info.id);
        if (_sendCmd) _sendCmd(c.data(), c.size());
        _finish(true);
    }
}

size_t WatchfaceInstaller::_readPayload(uint32_t offset, uint8_t* buf, size_t len) {
    // 虚拟上传流: [22 字节前缀][文件数据][4 字节 crc32 LE]
    size_t done = 0;
    while (done < len) {
        uint32_t o = offset + uint32_t(done);
        size_t need = len - done;
        if (o < 22) {
            size_t k = std::min(need, size_t(22 - o));
            std::memcpy(buf + done, _prefix + o, k);
            done += k;
        } else if (o < 22 + _info.size) {
            uint32_t fo = o - 22;
            size_t k = std::min(need, size_t(_info.size - fo));
            size_t r = _src ? _src->readAt(fo, buf + done, k) : 0;
            if (r != k) return 0; // 短读视为失败
            done += k;
        } else {
            uint32_t co = o - (22 + _info.size);
            size_t k = std::min(need, size_t(4 - co));
            std::memcpy(buf + done, _crcBytes + co, k);
            done += k;
        }
    }
    return done;
}

void WatchfaceInstaller::_fail(const std::string& why) {
    _error = why;
    _finish(false);
    _log(_error.c_str());
}

void WatchfaceInstaller::_finish(bool success) {
    _state = success ? WfState::DONE : WfState::FAILED;
    if (success) _progress = 100;
    _src = nullptr;
    if (onDone) onDone(success, _error);
}

void WatchfaceInstaller::_log(const char* msg) {
    if (onLog) onLog(msg);
}

} // namespace xw
