/*
 * 小米 MiWear SPP V2 协议层
 *
 * 实现依据（见 DOCUMENTATION.md 第 9 节）：
 *  - Gadgetbridge XiaomiSppPacketV2 / XiaomiSppProtocolV2 / XiaomiAuthService（AGPL-3.0）
 *  - AstroBox-Public miwear/packet.rs / device/auth.rs / device/hello.rs（AGPL-3.0）
 *
 * 帧格式（小端）:
 *   [0xA5 0xA5] [type:1] [seq:1] [len:2 LE] [crc16:2 LE] [payload:len]
 *   type: 1=ACK 2=SessionConfig 3=Data
 *   Data payload: [channel:1(低4位)] [opcode:1] [data...]
 *     channel: 1=Pb(protobuf, 认证后加密) 2=Mass(数据, 明文) 7=NetWork(IP包, 明文)
 *     opcode:  1=Plain 2=Encrypted
 *
 * 连接流程（与 Gadgetbridge 一致）:
 *   SPP 连接 -> 发 V1 版本查询(badcfe..ef) -> 收手表版本(>=2 用 V2)
 *   -> 发 SessionConfig(StartSession) -> 收手表 SessionConfig
 *   -> 认证: PhoneNonce -> WatchNonce(校验) -> AuthStep3 -> 手表确认
 *   -> 此后 Pb 通道 AES-128-CTR 加密
 */
#pragma once

#include <Arduino.h>
#include <functional>
#include <cstdint>
#include <cstddef>
#include <vector>

namespace xw {

// SPP V2 帧常量
static const uint8_t MW_MAGIC[2] = {0xA5, 0xA5};
static const uint8_t MW_PKT_ACK = 1;
static const uint8_t MW_PKT_SESSION_CONFIG = 2;
static const uint8_t MW_PKT_DATA = 3;

// 通道
static const uint8_t MW_CH_PB = 1;      // protobuf 命令（认证后加密）
static const uint8_t MW_CH_MASS = 2;    // 数据传输（明文）
static const uint8_t MW_CH_NETWORK = 7; // 网络（IP 包，明文）

// opcode
static const uint8_t MW_OP_PLAIN = 1;
static const uint8_t MW_OP_ENCRYPTED = 2;

// V1（版本查询）帧常量
static const uint8_t MW_V1_MAGIC[3] = {0xBA, 0xDC, 0xFE};

// 网络栈 MTU（表端 buffer 限制，参考 AstroBox：默认 800，过大导致表端溢出）
static const uint16_t MW_NETWORK_MTU = 800;

// 会话密钥（KDF 输出）
struct SessionKeys {
    uint8_t decKey[16];
    uint8_t encKey[16];
    uint8_t decNonce[4];
    uint8_t encNonce[4];
};

// MiWear KDF（纯函数，可主机单测）
// secretKey: 16 字节 auth key; phoneNonce/watchNonce: 各 16 字节
// 与 Gadgetbridge computeAuthStep3Hmac / AstroBox kdf_miwear 一致
void kdfMiwear(const uint8_t secretKey[16], const uint8_t phoneNonce[16],
               const uint8_t watchNonce[16], SessionKeys& out);

// AES-128-CTR（key == IV，每消息独立计数器，协议规定）
// 在 ESP32 上由 mbedTLS 实现；返回 false 表示失败
bool aes128Ctr(const uint8_t key[16], const uint8_t* in, uint8_t* out, size_t len);

// AES-128-CCM 加密（4 字节 tag，输出 = 密文 || tag）
bool aes128CcmEncrypt(const uint8_t key[16], const uint8_t nonce[12],
                      const uint8_t* plain, size_t len,
                      uint8_t* out, size_t outCap, size_t& outLen);

// ---- 协议状态机 ----
enum class MwState {
    DISCONNECTED,
    CONNECTED,       // SPP 已连接，已发版本查询
    VERSION_OK,      // 收到版本，协议版本 >= 2
    SESSION_SENT,    // 已发 SessionConfig
    SESSION_OK,      // 会话建立
    AUTH_NONCE_SENT, // 已发 PhoneNonce
    AUTHENTICATED,   // 认证完成，加密通道就绪
    AUTH_FAILED,     // 手表明确拒绝认证（Auth.status=0）
};

class MiWear {
public:
    using CommandHandler = std::function<void(const uint8_t* data, size_t len)>;
    using IpPacketHandler = std::function<void(const uint8_t* pkt, size_t len)>;

    void begin(Stream& io);
    void end();

    // SPP 连接建立后调用：发送 V1 版本查询，启动握手
    void onConnected();

    // 喂入 SPP 收到的字节（内部缓冲、拆帧、分发）
    void onBytes(const uint8_t* data, size_t len);

    // 发送 protobuf 命令（自动选择明文/加密）
    bool sendCommand(const uint8_t* cmd, size_t len);
    // 发送网络通道 IP 包（明文）
    bool sendIpPacket(const uint8_t* pkt, size_t len);
    // 发送 Mass/Data 通道明文数据块（表盘上传分块，Gadgetbridge sendDataChunk）
    bool sendDataChunk(const uint8_t* data, size_t len);

    // 启动认证（传入 16 字节 auth key）
    void startAuth(const uint8_t authKey[16]);

    // 主循环调用：处理超时/重试
    void tick();

    MwState state() const { return _state; }
    bool authenticated() const { return _state == MwState::AUTHENTICATED; }
    const SessionKeys& keys() const { return _keys; }
    bool hasKeys() const { return _hasKeys; }

    // 回调
    CommandHandler onCommand;   // 解密后的 protobuf Command 字节
    IpPacketHandler onIpPacket; // NetWork 通道的原始 IP 包

    // 统计
    uint32_t rxFrames = 0;
    uint32_t txFrames = 0;
    uint32_t rxIpPackets = 0;
    uint32_t txIpPackets = 0;
    uint32_t txDataChunks = 0; // Mass 通道数据块（表盘上传）
    uint32_t rxOverflows = 0;  // 接收缓冲溢出（流损坏）次数

    // 日志（弱依赖，可空）
    std::function<void(const char*)> onLog;

private:
    // V2 帧最大长度：8 字节头 + uint16 payload = 65543。
    // 接收缓冲必须容得下一整帧 + 余量，否则攒帧逻辑会被截断打断。
    static constexpr size_t RX_BUF_MAX = 66048;

    Stream* _io = nullptr;
    MwState _state = MwState::DISCONNECTED;
    std::vector<uint8_t> _rx;
    uint8_t _seq = 0;
    uint8_t _phoneNonce[16] = {0};
    uint8_t _authKey[16] = {0};
    SessionKeys _keys{};
    bool _hasKeys = false;
    uint32_t _lastActivityMs = 0;
    uint32_t _lastSentMs = 0;

    void _log(const char* msg);
    bool _sendFrame(uint8_t type, uint8_t seq, const uint8_t* payload, size_t len);
    bool _sendAck(uint8_t seq);
    void _processBuffer();
    void _handleFrameV2(const uint8_t* frame, size_t total);
    void _handleV1Frame(const uint8_t* frame, size_t total);
    void _handleDataFrame(uint8_t channel, uint8_t opcode, const uint8_t* data, size_t len);
    void _handleCommandBytes(const uint8_t* data, size_t len); // 解密后的 Command
    void _sendSessionConfig();
    void _sendPhoneNonce();
    void _handleWatchNonce(const uint8_t* data, size_t len);
    void _finishAuthIfConfirmed(const uint8_t* data, size_t len);
    bool _decrypt(const uint8_t* in, uint8_t* out, size_t len);
    bool _encrypt(const uint8_t* in, uint8_t* out, size_t len);
};

} // namespace xw
