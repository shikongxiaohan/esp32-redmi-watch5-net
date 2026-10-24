/*
 * MiWear KDF 与 AES 包装
 * KDF 为纯函数（仅依赖 SHA-256），可主机单测；
 * AES-128-CTR / AES-128-CCM 在 ESP32 上由 mbedTLS 实现。
 */
#include "miwear.h"
#include "sha256.h"
#include "crc16.h"
#include "proto.h"

#include <cstring>
#include <cstdio>
#include <vector>

#if defined(ESP32) || defined(ESP_PLATFORM)
#include <mbedtls/aes.h>
#include <mbedtls/ccm.h>
#include <esp_random.h>
#define XW_HAS_MBEDTLS 1
#else
#define XW_HAS_MBEDTLS 0
#endif

namespace xw {

void kdfMiwear(const uint8_t secretKey[16], const uint8_t phoneNonce[16],
               const uint8_t watchNonce[16], SessionKeys& out) {
    // 1) hmac_key = HMAC-SHA256(key = phoneNonce || watchNonce, data = secretKey)
    uint8_t initKey[32];
    std::memcpy(initKey, phoneNonce, 16);
    std::memcpy(initKey + 16, watchNonce, 16);
    uint8_t hmacKey[32];
    hmacSha256(initKey, 32, secretKey, 16, hmacKey);

    // 2) okm = T_1 || T_2,  T_i = HMAC(hmac_key, T_{i-1} || "miwear-auth" || i)
    static const uint8_t LABEL[] = "miwear-auth";
    uint8_t okm[64];
    uint8_t prev[32];
    size_t prevLen = 0;
    for (uint8_t counter = 1; counter <= 2; counter++) { // 2*32 = 64 字节
        // msg = prev || label || counter
        uint8_t msg[32 + sizeof(LABEL) - 1 + 1];
        size_t off = 0;
        if (prevLen) { std::memcpy(msg, prev, prevLen); off = prevLen; }
        std::memcpy(msg + off, LABEL, sizeof(LABEL) - 1);
        off += sizeof(LABEL) - 1;
        msg[off++] = counter;
        uint8_t t[32];
        hmacSha256(hmacKey, 32, msg, off, t);
        std::memcpy(okm + (counter - 1) * 32, t, 32);
        std::memcpy(prev, t, 32);
        prevLen = 32;
    }

    std::memcpy(out.decKey, okm, 16);
    std::memcpy(out.encKey, okm + 16, 16);
    std::memcpy(out.decNonce, okm + 32, 4);
    std::memcpy(out.encNonce, okm + 36, 4);
}

bool aes128Ctr(const uint8_t key[16], const uint8_t* in, uint8_t* out, size_t len) {
#if XW_HAS_MBEDTLS
    mbedtls_aes_context ctx;
    mbedtls_aes_init(&ctx);
    if (mbedtls_aes_setkey_enc(&ctx, key, 128) != 0) {
        mbedtls_aes_free(&ctx);
        return false;
    }
    uint8_t nonceCounter[16];
    uint8_t streamBlock[16];
    size_t ncOff = 0;
    // 协议规定 IV = key 本身，且每条消息计数器从 0 开始
    std::memcpy(nonceCounter, key, 16);
    std::memset(streamBlock, 0, 16);
    mbedtls_aes_crypt_ctr(&ctx, len, &ncOff, nonceCounter, streamBlock, in, out);
    mbedtls_aes_free(&ctx);
    return true;
#else
    (void)key; (void)in; (void)out; (void)len;
    return false;
#endif
}

bool aes128CcmEncrypt(const uint8_t key[16], const uint8_t nonce[12],
                      const uint8_t* plain, size_t len,
                      uint8_t* out, size_t outCap, size_t& outLen) {
#if XW_HAS_MBEDTLS
    if (outCap < len + 4) return false;
    mbedtls_ccm_context ctx;
    mbedtls_ccm_init(&ctx);
    // keybits = 128, iv_len = 12, tag_len = 4
    // ESP-IDF 5.x（mbedtls 3.x）只有 setkey + encrypt_and_tag 组合 API
    if (mbedtls_ccm_setkey(&ctx, MBEDTLS_CIPHER_ID_AES, key, 128) != 0 ||
        mbedtls_ccm_encrypt_and_tag(&ctx, len, nonce, 12, nullptr, 0,
                                    plain, out, out + len, 4) != 0) {
        mbedtls_ccm_free(&ctx);
        return false;
    }
    mbedtls_ccm_free(&ctx);
    outLen = len + 4;
    return true;
#else
    (void)key; (void)nonce; (void)plain; (void)len; (void)out; (void)outCap; (void)outLen;
    return false;
#endif
}

} // namespace xw

#if defined(ESP32) || defined(ESP_PLATFORM) || defined(XW_HOST_TEST)
/*
 * ===================== MiWear 协议状态机 =====================
 *
 * 时序（与 Gadgetbridge XiaomiSppSupport 一致）：
 *   SPP onConnected -> 发 V1 版本查询(badcfe00c00300000100ef)
 *   -> 收 V1 版本响应(payload[0] >= 2)
 *   -> 发 SessionConfig(StartSessionRequest)
 *   -> 收手表 SessionConfig
 *   -> startAuth(): 发 PhoneNonce(Pb 通道, 明文)
 *   -> 收 WatchNonce(校验 HMAC) -> 派生密钥 -> 发 AuthStep3(明文)
 *   -> 收手表确认(subtype=27) -> AUTHENTICATED
 *   -> 此后 Pb 通道 AES-128-CTR 加密; NetWork/Mass 通道始终明文
 *
 * XW_HOST_TEST: 主机端单测编译同一份状态机（测试 sendIpPacket /
 * sendDataChunk 的帧格式与 sendCommand 的明文路径；AES 相关路径在
 * 无 mbedtls 的主机上返回 false，由 ESP32 真机覆盖）。
 */

namespace xw {

// ---- 内部帧构造辅助 ----

// V1 版本查询帧: BA DC FE 00 C0 03 00 00 01 00 EF
// (channel=0, flags=0xC0, len=3(payload 0 + 3), opcode=0, serial=1, dataType=0)
static const uint8_t V1_VERSION_QUERY[] = {
    0xBA, 0xDC, 0xFE, 0x00, 0xC0, 0x03, 0x00, 0x00, 0x01, 0x00, 0xEF
};

void MiWear::begin(Stream& io) {
    _io = &io;
    _state = MwState::DISCONNECTED;
    _rx.clear();
    _seq = 0;
    _hasKeys = false;
    _lastActivityMs = millis();
    _lastSentMs = 0;
    std::memset(_phoneNonce, 0, sizeof(_phoneNonce));
    std::memset(_authKey, 0, sizeof(_authKey));
}

void MiWear::end() {
    _io = nullptr;
    _state = MwState::DISCONNECTED;
    _rx.clear();
    _hasKeys = false;
}

// SPP 连接建立后调用：发送 V1 版本查询
void MiWear::onConnected() {
    if (!_io) return;
    _rx.clear();
    _seq = 0;
    _hasKeys = false;
    _state = MwState::CONNECTED;
    _lastActivityMs = millis();
    _io->write(V1_VERSION_QUERY, sizeof(V1_VERSION_QUERY));
    _io->flush();
    _log("miwear: version query sent");
}

void MiWear::onBytes(const uint8_t* data, size_t len) {
    if (!len) return;
    _rx.insert(_rx.end(), data, data + len);
    // 防御：缓冲无限增长（异常流）。
    // V2 帧的 payload 长度是 uint16，单帧最大 8 + 65535 = 65543 字节，
    // 所以缓冲容量必须容得下一整帧，否则「攒帧等完整」的逻辑会被
    // 截断逻辑打断（帧被腰斩 → CRC 校验失败 → 丢帧 + 通道失步）。
    // 超过上限说明流已损坏（不是合法大帧），此时整块丢弃并重新同步，
    // 比「丢头留尾」安全：丢头留尾会把帧头切在半帧中间，
    // 之后必须等下一次出现 A5A5 才能重新同步。
    if (_rx.size() > RX_BUF_MAX) {
        _rx.clear();
        rxOverflows++;
        _log("miwear: rx buffer overflow, resync");
    }
    _lastActivityMs = millis();
    _processBuffer();
}

bool MiWear::_sendFrame(uint8_t type, uint8_t seq, const uint8_t* payload, size_t len) {
    if (!_io || len > 0xFFFF) return false;
    uint8_t hdr[8];
    hdr[0] = MW_MAGIC[0];
    hdr[1] = MW_MAGIC[1];
    hdr[2] = type;
    hdr[3] = seq;
    hdr[4] = uint8_t(len);
    hdr[5] = uint8_t(len >> 8);
    uint16_t crc = crc16Arc(payload, len);
    hdr[6] = uint8_t(crc);
    hdr[7] = uint8_t(crc >> 8);
    size_t w1 = _io->write(hdr, 8);
    size_t w2 = len ? _io->write(payload, len) : 0;
    _io->flush();
    if (w1 != 8 || w2 != len) return false;
    txFrames++;
    _lastSentMs = millis();
    return true;
}

bool MiWear::_sendAck(uint8_t seq) {
    // a5a5 01 {seq} 00 00 00 00（空 payload, crc=0）
    uint8_t frame[8] = {0xA5, 0xA5, MW_PKT_ACK, seq, 0, 0, 0, 0};
    if (!_io) return false;
    size_t w = _io->write(frame, 8);
    _io->flush();
    if (w != 8) return false;
    txFrames++;
    return true;
}

void MiWear::_processBuffer() {
    for (;;) {
        size_t n = _rx.size();
        if (n < 2) return;

        // V1 帧（仅版本查询阶段）
        // 布局（Gadgetbridge XiaomiSppPacketV1.encode）:
        //   [BA DC FE][ch:1][flags:1][size:2 LE][op:1][serial:1][dtype:1][payload][EF]
        //   size = payload.len + 3, 总长 = size + 8 = 11 + payload.len
        if (_state == MwState::CONNECTED && n >= 10 &&
            _rx[0] == MW_V1_MAGIC[0] && _rx[1] == MW_V1_MAGIC[1] && _rx[2] == MW_V1_MAGIC[2]) {
            if (n < 10) return; // 等头部
            uint16_t size = uint16_t(_rx[5]) | (uint16_t(_rx[6]) << 8);
            size_t total = size_t(8) + size;
            if (n < total) return; // 等完整帧
            if (_rx[total - 1] != 0xEF) {
                // 非法帧，丢一个字节重新同步
                _rx.erase(_rx.begin());
                continue;
            }
            _handleV1Frame(_rx.data(), total);
            _rx.erase(_rx.begin(), _rx.begin() + total);
            continue;
        }

        // V2 帧
        if (_rx[0] == MW_MAGIC[0] && _rx[1] == MW_MAGIC[1]) {
            if (n < 8) return;
            uint16_t len = uint16_t(_rx[4]) | (uint16_t(_rx[5]) << 8);
            size_t total = size_t(8) + len;
            if (n < total) return;
            _handleFrameV2(_rx.data(), total);
            _rx.erase(_rx.begin(), _rx.begin() + total);
            continue;
        }

        // 未识别：丢一个字节重新同步（SPP 字节流可能丢帧）
        _rx.erase(_rx.begin());
    }
}

void MiWear::_handleV1Frame(const uint8_t* frame, size_t total) {
    rxFrames++;
    // 版本响应: 头部 10 字节（magic3+ch1+flags1+size2+op1+serial1+dtype1），
    // payload = frame[10 .. total-1)（去掉结尾 EF），payload[0] = 协议版本
    if (total < 11) return;
    const uint8_t* payload = frame + 10;
    size_t plen = total - 11; // 去掉结尾 EF
    if (plen < 1) {
        // 空 payload：超时路径（Gadgetbridge 传空数组也会继续）
        _log("miwear: empty version response, assuming v2");
    } else {
        char buf[64];
        snprintf(buf, sizeof(buf), "miwear: version response v%d", payload[0]);
        _log(buf);
        if (payload[0] < 2) {
            _log("miwear: protocol version < 2, unsupported");
            return;
        }
    }
    // 版本就绪：发送 SessionConfig
    _state = MwState::VERSION_OK;
    _sendSessionConfig();
}

void MiWear::_handleFrameV2(const uint8_t* frame, size_t total) {
    rxFrames++;
    uint8_t type = frame[2];
    uint8_t seq = frame[3];
    uint16_t len = uint16_t(frame[4]) | (uint16_t(frame[5]) << 8);
    uint16_t crcGiven = uint16_t(frame[6]) | (uint16_t(frame[7]) << 8);
    const uint8_t* payload = frame + 8;
    if (total != size_t(8) + len) return;
    uint16_t crcCalc = crc16Arc(payload, len);
    if (crcCalc != crcGiven) {
        _log("miwear: crc mismatch, frame dropped");
        return;
    }

    switch (type) {
        case MW_PKT_ACK:
            // 手表的发送确认；本实现 fire-and-forget，无需处理
            break;
        case MW_PKT_SESSION_CONFIG: {
            // 手表的 SessionConfig 响应：opcode + key/value 列表
            // 我们只需要知道会话已建立
            if (_state == MwState::SESSION_SENT || _state == MwState::VERSION_OK) {
                _state = MwState::SESSION_OK;
                _log("miwear: session established");
                // 由上层（main）在 SESSION_OK 时调用 startAuth()
            }
            break;
        }
        case MW_PKT_DATA: {
            if (len < 2) break;
            uint8_t channel = payload[0] & 0x0F;
            uint8_t opcode = payload[1];
            _handleDataFrame(channel, opcode, payload + 2, len - 2);
            // 每个 Data 帧都要回 ACK（AstroBox btrecv / Gadgetbridge 一致）
            _sendAck(seq);
            break;
        }
        default:
            break;
    }
}

void MiWear::_handleDataFrame(uint8_t channel, uint8_t opcode, const uint8_t* data, size_t len) {
    switch (channel) {
        case MW_CH_NETWORK:
            // 原始 IP 包，明文。交给网络栈
            if (opcode == MW_OP_PLAIN && len > 0 && onIpPacket) {
                rxIpPackets++;
                onIpPacket(data, len);
            }
            break;
        case MW_CH_PB: {
            // protobuf 命令。认证相关(type=1)为明文；其余加密
            if (opcode == MW_OP_PLAIN) {
                _handleCommandBytes(data, len);
            } else if (opcode == MW_OP_ENCRYPTED && _hasKeys) {
                std::vector<uint8_t> plain(len);
                if (_decrypt(data, plain.data(), len)) {
                    _handleCommandBytes(plain.data(), len);
                } else {
                    _log("miwear: decrypt failed");
                }
            }
            break;
        }
        default:
            // Mass/Activity 等其他通道本项目不用
            break;
    }
}

void MiWear::_handleCommandBytes(const uint8_t* data, size_t len) {
    MiCommand cmd = parseCommand(data, len);
    if (!cmd.ok) return;

    if (cmd.type == cmd::TYPE_AUTH) {
        if (cmd.subtype == cmd::AUTH_NONCE) {
            // 手表 WatchNonce
            _handleWatchNonce(data, len);
        } else if (cmd.subtype == cmd::AUTH_CONFIRM || cmd.subtype == cmd::AUTH_SEND_USERID) {
            _finishAuthIfConfirmed(data, len);
        }
        return;
    }

    // 其他命令上抛给上层（天气请求、设备信息响应等）
    if (onCommand) onCommand(data, len);
}

void MiWear::_sendSessionConfig() {
    // 与官方 App 抓包一致（Gadgetbridge SessionConfigPacket）：
    // opcode=1(StartSessionRequest)
    // VERSION=1.0.0, MAX_PACKET_SIZE=0xfc00, TX_WIN=32, SEND_TIMEOUT=10000ms
    static const uint8_t payload[] = {
        0x01,                         // opcode: START_SESSION_REQUEST
        0x01, 0x03, 0x00, 0x01, 0x00, 0x00,   // KEY_VERSION, size=3, 1.0.0
        0x02, 0x02, 0x00, 0x00, 0xFC,         // KEY_MAX_PACKET_SIZE, size=2, 0xfc00
        0x03, 0x02, 0x00, 0x20, 0x00,         // KEY_TX_WIN, size=2, 32
        0x04, 0x02, 0x00, 0x10, 0x27,         // KEY_SEND_TIMEOUT, size=2, 10000ms
    };
    _sendFrame(MW_PKT_SESSION_CONFIG, 0, payload, sizeof(payload));
    _state = MwState::SESSION_SENT;
    _log("miwear: session config sent");
}

void MiWear::startAuth(const uint8_t authKey[16]) {
    if (_state != MwState::SESSION_OK) {
        _log("miwear: startAuth ignored (no session)");
        return;
    }
    std::memcpy(_authKey, authKey, 16);
    _sendPhoneNonce();
}

void MiWear::_sendPhoneNonce() {
    // phoneNonce: 16 字节随机数（ESP32 硬件 RNG）
    for (int i = 0; i < 16; i += 4) {
        uint32_t r = esp_random();
        std::memcpy(_phoneNonce + i, &r, 4);
    }
    std::vector<uint8_t> cmd = buildAuthPhoneNonceCmd(_phoneNonce);
    // 认证命令（type=1）在 Pb 通道明文发送
    bool ok = sendCommand(cmd.data(), cmd.size());
    if (ok) {
        _state = MwState::AUTH_NONCE_SENT;
        _log("miwear: phone nonce sent");
    }
}

void MiWear::_handleWatchNonce(const uint8_t* data, size_t len) {
    if (_state != MwState::AUTH_NONCE_SENT && _state != MwState::SESSION_OK) return;

    MiCommand cmd = parseCommand(data, len);
    PbReader authR(cmd.auth.data, cmd.auth.len);
    PbField wn;
    if (!authR.find(31, wn) || wn.wire != 2) { // WatchNonce = field 31
        _log("miwear: watchNonce missing");
        return;
    }
    PbReader wnR(wn.data, wn.len);
    std::vector<uint8_t> watchNonce, watchHmac;
    if (!wnR.bytesOf(1, watchNonce) || !wnR.bytesOf(2, watchHmac) ||
        watchNonce.size() != 16 || watchHmac.size() != 32) {
        _log("miwear: watchNonce malformed");
        return;
    }

    // 1) 派生会话密钥
    SessionKeys keys;
    kdfMiwear(_authKey, _phoneNonce, watchNonce.data(), keys);

    // 2) 校验手表 HMAC: HMAC-SHA256(decKey, watchNonce || phoneNonce)
    uint8_t confirm[32];
    {
        uint8_t msg[32];
        std::memcpy(msg, watchNonce.data(), 16);
        std::memcpy(msg + 16, _phoneNonce, 16);
        hmacSha256(keys.decKey, 16, msg, 32, confirm);
    }
    if (std::memcmp(confirm, watchHmac.data(), 32) != 0) {
        _log("miwear: watch hmac mismatch (wrong auth key?)");
        return;
    }

    // 3) encryptedNonces = HMAC-SHA256(encKey, phoneNonce || watchNonce)
    uint8_t encSigns[32];
    {
        uint8_t msg[32];
        std::memcpy(msg, _phoneNonce, 16);
        std::memcpy(msg + 16, watchNonce.data(), 16);
        hmacSha256(keys.encKey, 16, msg, 32, encSigns);
    }

    // 4) encryptedDeviceInfo = AES-128-CCM(encKey, encNonce||0||0)
    //    AuthDeviceInfo: unknown1=0, phoneApiLevel, phoneName, unknown3=224, region
    std::vector<uint8_t> devInfo = buildAuthDeviceInfoBytes(
        33, "ESP32", "CN");
    std::vector<uint8_t> encDevInfo(devInfo.size() + 4);
    size_t encLen = 0;
    {
        uint8_t nonce[12];
        std::memcpy(nonce, keys.encNonce, 4);
        std::memset(nonce + 4, 0, 8);
        if (!aes128CcmEncrypt(keys.encKey, nonce, devInfo.data(), devInfo.size(),
                              encDevInfo.data(), encDevInfo.size(), encLen)) {
            _log("miwear: device info encrypt failed");
            return;
        }
    }

    // 5) 发送 AuthStep3（明文）
    std::vector<uint8_t> step3 = buildAuthStep3Cmd(encSigns, encDevInfo.data(), encLen);
    if (sendCommand(step3.data(), step3.size())) {
        _keys = keys;
        _hasKeys = true;
        _log("miwear: auth step3 sent, awaiting confirmation");
    }
}

void MiWear::_finishAuthIfConfirmed(const uint8_t* data, size_t len) {
    MiCommand cmd = parseCommand(data, len);
    // 判定逻辑在 proto.cpp 的 authConfirmSucceeded()（纯函数，可主机端单测）：
    // Auth.status 存在时以它为准（1=成功，0=手表明确拒绝）；
    // 字段缺失时才回退到「命令子类型即确认」的宽松判定。
    // 修复前用 varintOf(8, 1) 且对 subtype 短路，会把手表明确返回的
    // status=0 当成认证成功，导致加密通道用错密钥、连接随机断开。
    if (authConfirmSucceeded(cmd)) {
        _state = MwState::AUTHENTICATED;
        _log("miwear: AUTHENTICATED");
    } else {
        _state = MwState::AUTH_FAILED;
        PbReader authR(cmd.auth.data, cmd.auth.len);
        char buf[64];
        std::snprintf(buf, sizeof(buf), "miwear: auth failed by watch (status=%llu)",
                      (unsigned long long)authR.varintOf(8, 0));
        _log(buf);
    }
}

bool MiWear::_decrypt(const uint8_t* in, uint8_t* out, size_t len) {
    if (!_hasKeys || len == 0) return false;
    return aes128Ctr(_keys.decKey, in, out, len);
}

bool MiWear::_encrypt(const uint8_t* in, uint8_t* out, size_t len) {
    if (!_hasKeys || len == 0) return false;
    return aes128Ctr(_keys.encKey, in, out, len);
}

bool MiWear::sendCommand(const uint8_t* cmd, size_t len) {
    if (!_io || !cmd || len == 0) return false;

    // 判断命令类型：type=1(AUTH) 明文，其余加密（认证后）
    MiCommand parsed = parseCommand(cmd, len);
    bool isAuth = (parsed.type == cmd::TYPE_AUTH);
    uint8_t opcode = MW_OP_PLAIN;
    std::vector<uint8_t> body;

    if (!isAuth && _state == MwState::AUTHENTICATED) {
        body.resize(len);
        if (!_encrypt(cmd, body.data(), len)) return false;
        opcode = MW_OP_ENCRYPTED;
    } else {
        body.assign(cmd, cmd + len);
    }

    // Data 帧 payload: [channel:1][opcode:1][data...]
    std::vector<uint8_t> payload;
    payload.reserve(2 + body.size());
    payload.push_back(MW_CH_PB);
    payload.push_back(opcode);
    payload.insert(payload.end(), body.begin(), body.end());

    return _sendFrame(MW_PKT_DATA, _seq++, payload.data(), payload.size());
}

bool MiWear::sendIpPacket(const uint8_t* pkt, size_t len) {
    if (!_io || !pkt || len == 0) return false;
    if (len > MW_NETWORK_MTU) {
        // 超过表端 buffer 上限，丢弃（netgw 侧已按 MTU 分片）
        return false;
    }
    // Data 帧 payload: [channel:1][opcode:1][data...]
    // NetWork 通道明文（AstroBox packet.rs: 所有 Data 帧都带完整 V2 帧头）
    uint8_t hdr[2] = {MW_CH_NETWORK, MW_OP_PLAIN};
    std::vector<uint8_t> payload;
    payload.reserve(2 + len);
    payload.insert(payload.end(), hdr, hdr + 2);
    payload.insert(payload.end(), pkt, pkt + len);
    bool ok = _sendFrame(MW_PKT_DATA, _seq++, payload.data(), payload.size());
    if (ok) txIpPackets++;
    return ok;
}

bool MiWear::sendDataChunk(const uint8_t* data, size_t len) {
    if (!_io || !data || len == 0) return false;
    // Mass 通道明文（Gadgetbridge XiaomiDataUploadService.sendDataChunk
    // -> encodePacket(Channel.Data, chunk)，opcode = SEND_PLAINTEXT）
    uint8_t hdr[2] = {MW_CH_MASS, MW_OP_PLAIN};
    std::vector<uint8_t> payload;
    payload.reserve(2 + len);
    payload.insert(payload.end(), hdr, hdr + 2);
    payload.insert(payload.end(), data, data + len);
    bool ok = _sendFrame(MW_PKT_DATA, _seq++, payload.data(), payload.size());
    if (ok) txDataChunks++;
    return ok;
}

void MiWear::tick() {
    uint32_t now = millis();
    switch (_state) {
        case MwState::CONNECTED:
            // 版本查询超时（10s）：重发
            if (now - _lastActivityMs > 10000) {
                _log("miwear: version timeout, retry");
                if (_io) {
                    _io->write(V1_VERSION_QUERY, sizeof(V1_VERSION_QUERY));
                    _io->flush();
                }
                _lastActivityMs = now;
            }
            break;
        case MwState::SESSION_SENT:
            // SessionConfig 无响应（10s）：重发
            if (now - _lastActivityMs > 10000) {
                _log("miwear: session config timeout, retry");
                _sendSessionConfig();
            }
            break;
        case MwState::AUTH_NONCE_SENT:
            // 手表不回 WatchNonce（15s）：重发 PhoneNonce
            if (now - _lastActivityMs > 15000) {
                _log("miwear: auth timeout, resend nonce");
                _sendPhoneNonce();
            }
            break;
        default:
            break;
    }
}

void MiWear::_log(const char* msg) {
    if (onLog) onLog(msg);
}

} // namespace xw
#endif // ESP32
