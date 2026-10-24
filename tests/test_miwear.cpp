/*
 * MiWear 协议层主机单元测试（XW_HOST_TEST 编译同一份状态机）
 *   - sendIpPacket / sendDataChunk 的 V2 帧格式（magic/type/seq/len/crc16 + 通道头）
 *   - sendCommand 认证前明文路径
 *   - V1 版本查询 -> SessionConfig -> 手表确认 的状态推进
 *   - Data 帧 ACK 应答
 *
 * 注意：AES 路径（认证后加密、AuthStep3）依赖 mbedtls，仅在 ESP32 真机覆盖；
 * 主机端 aes128CcmEncrypt/aes128Ctr 返回 false，相关流程不在此测。
 */
#include "miwear.h"
#include "crc16.h"
#include "proto.h"

#include <Arduino.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace xw;

// ---- 测试可控制的假时钟（millis() 宏映射到这里） ----
static uint32_t g_fakeNow = 1000;
uint32_t xw_test_millis() { return g_fakeNow; }

// ---- 记录写出的字节流 ----
class RecStream : public Stream {
public:
    std::vector<uint8_t> out;
    size_t write(const uint8_t* buf, size_t n) override {
        out.insert(out.end(), buf, buf + n);
        return n;
    }
    size_t write(uint8_t c) override { return write(&c, 1); }
    void flush() override {}
};

static int g_failures = 0;
static int g_checks = 0;

static void check(bool cond, const char* what) {
    g_checks++;
    if (!cond) {
        g_failures++;
        std::printf("  FAIL: %s\n", what);
    }
}

// 从帧缓冲中取出第 idx 个完整 V2 帧（返回 false 表示不够/畸形）
// 非 A5A5 前缀（如 V1 版本查询）自动跳过，与 _processBuffer 的重新同步逻辑一致
static bool nthFrame(const std::vector<uint8_t>& buf, size_t& pos, size_t idx,
                     std::vector<uint8_t>& frame) {
    size_t p = 0;
    for (size_t i = 0;; i++) {
        while (p + 1 < buf.size() && !(buf[p] == 0xA5 && buf[p + 1] == 0xA5)) p++;
        if (p + 8 > buf.size()) return false;
        uint16_t len = uint16_t(buf[p + 4]) | (uint16_t(buf[p + 5]) << 8);
        size_t total = size_t(8) + len;
        if (p + total > buf.size()) return false;
        if (i == idx) {
            frame.assign(buf.begin() + p, buf.begin() + p + total);
            pos = p;
            return true;
        }
        p += total;
    }
}

// 构造手表 -> 手机的 Data 帧（供 onBytes 喂入）
static std::vector<uint8_t> makeDataFrame(uint8_t seq, uint8_t channel, uint8_t opcode,
                                          const uint8_t* data, size_t len) {
    std::vector<uint8_t> body;
    body.push_back(channel);
    body.push_back(opcode);
    body.insert(body.end(), data, data + len);
    std::vector<uint8_t> f;
    f.push_back(0xA5); f.push_back(0xA5);
    f.push_back(0x03); f.push_back(seq);
    f.push_back(uint8_t(body.size())); f.push_back(uint8_t(body.size() >> 8));
    uint16_t crc = crc16Arc(body.data(), body.size());
    f.push_back(uint8_t(crc)); f.push_back(uint8_t(crc >> 8));
    f.insert(f.end(), body.begin(), body.end());
    return f;
}

// ===================== 测试 =====================

static void testSendFraming() {
    std::printf("[miwear: send framing]\n");
    RecStream io;
    MiWear wear;
    wear.begin(io);

    // ---- sendIpPacket: [A5A5][03][seq][len][crc16][07][01][ip...]
    uint8_t ip[20] = {0x45, 0, 0, 20, 0, 0, 0, 0, 64, 17, 0, 0,
                      10, 1, 10, 2, 223, 5, 5, 5};
    check(wear.sendIpPacket(ip, sizeof(ip)), "sendIpPacket ok");
    check(wear.txIpPackets == 1 && wear.txFrames == 1, "ip packet stats");
    std::vector<uint8_t> f;
    size_t pos = 0;
    check(nthFrame(io.out, pos, 0, f), "ip frame parseable");
    if (!f.empty()) {
        check(f[2] == 0x03, "ip frame type = DATA");
        check(f[3] == 0, "ip frame seq = 0");
        uint16_t len = uint16_t(f[4]) | (uint16_t(f[5]) << 8);
        check(len == 2 + sizeof(ip), "ip frame len = 2 + ip");
        uint16_t crc = uint16_t(f[6]) | (uint16_t(f[7]) << 8);
        check(crc == crc16Arc(f.data() + 8, len), "ip frame crc16 over payload");
        check(f[8] == 0x07 && f[9] == 0x01, "ip frame channel 7 plaintext");
        check(f.size() == 8 + 2 + sizeof(ip) &&
                  std::memcmp(f.data() + 10, ip, sizeof(ip)) == 0, "ip frame payload bytes");
    }

    // ---- MTU 上限：801 字节拒绝
    std::vector<uint8_t> big(MW_NETWORK_MTU + 1, 0xAB);
    size_t before = io.out.size();
    check(!wear.sendIpPacket(big.data(), big.size()), "sendIpPacket over MTU rejected");
    check(io.out.size() == before, "no bytes written for oversized ip");

    // ---- sendDataChunk: [A5A5][03][seq][len][crc16][02][01][data...]
    uint8_t chunk[16];
    for (int i = 0; i < 16; i++) chunk[i] = uint8_t(i * 17);
    check(wear.sendDataChunk(chunk, sizeof(chunk)), "sendDataChunk ok");
    check(wear.txDataChunks == 1, "data chunk stat");
    check(nthFrame(io.out, pos, 1, f), "chunk frame parseable");
    if (!f.empty()) {
        check(f[2] == 0x03, "chunk frame type = DATA");
        check(f[3] == 1, "chunk frame seq = 1");
        uint16_t len = uint16_t(f[4]) | (uint16_t(f[5]) << 8);
        check(len == 2 + sizeof(chunk), "chunk frame len = 2 + data");
        uint16_t crc = uint16_t(f[6]) | (uint16_t(f[7]) << 8);
        check(crc == crc16Arc(f.data() + 8, len), "chunk frame crc16");
        check(f[8] == 0x02 && f[9] == 0x01, "chunk frame channel 2 plaintext");
        check(f.size() == 8 + 2 + sizeof(chunk) &&
                  std::memcmp(f.data() + 10, chunk, sizeof(chunk)) == 0, "chunk payload bytes");
    }
    // 第二块 seq = 2
    check(wear.sendDataChunk(chunk, 4), "sendDataChunk #2 ok");
    check(nthFrame(io.out, pos, 2, f) && f[3] == 2, "chunk seq increments");

    // ---- sendCommand 认证前: Pb 通道明文
    std::vector<uint8_t> clockCmd = buildClockCmd({2026, 1, 2, 3, 4, 5, 480, 0, "CST", true});
    check(wear.sendCommand(clockCmd.data(), clockCmd.size()), "sendCommand pre-auth ok");
    check(nthFrame(io.out, pos, 3, f), "cmd frame parseable");
    if (!f.empty()) {
        check(f[8] == 0x01 && f[9] == 0x01, "cmd frame channel 1 plaintext pre-auth");
        uint16_t len = uint16_t(f[4]) | (uint16_t(f[5]) << 8);
        check(len == 2 + clockCmd.size() &&
                  std::memcmp(f.data() + 10, clockCmd.data(), clockCmd.size()) == 0,
              "cmd payload verbatim pre-auth");
    }
    wear.end();
}

static void testHandshakeProgress() {
    std::printf("[miwear: handshake]\n");
    RecStream io;
    MiWear wear;
    wear.begin(io);
    wear.onConnected();
    check(wear.state() == MwState::CONNECTED, "state CONNECTED after onConnected");
    // V1 版本查询已发出（11 字节原始帧，非 V2）
    check(io.out.size() == 11 && io.out[0] == 0xBA && io.out[1] == 0xDC && io.out[2] == 0xFE,
          "v1 version query bytes");

    // 手表版本响应: BA DC FE 00 C0 04 00 00 01 00 02 EF
    // (size 字段 = payload.len+3 = 4, payload = [02] 即协议版本 2)
    uint8_t verResp[] = {0xBA, 0xDC, 0xFE, 0x00, 0xC0, 0x04, 0x00, 0x00, 0x01, 0x00, 0x02, 0xEF};
    wear.onBytes(verResp, sizeof(verResp));
    check(wear.state() == MwState::SESSION_SENT, "state SESSION_SENT after version");
    // 应发出 SessionConfig V2 帧
    std::vector<uint8_t> f;
    size_t pos = 0;
    check(nthFrame(io.out, pos, 0, f), "session config frame emitted");
    if (!f.empty()) {
        check(f[2] == 0x02, "session config frame type");
        check(f[8] == 0x01, "session config opcode = START_SESSION_REQUEST");
    }

    // 手表 SessionConfig 响应（type=2 帧，opcode + key/value 列表内容本实现不关心）
    uint8_t scPayload[] = {0x02, 0x01, 0x03, 0x00, 0x01, 0x00, 0x00};
    std::vector<uint8_t> sc;
    sc.push_back(0xA5); sc.push_back(0xA5); sc.push_back(0x02); sc.push_back(0x00);
    sc.push_back(uint8_t(sizeof(scPayload))); sc.push_back(0x00);
    uint16_t crc = crc16Arc(scPayload, sizeof(scPayload));
    sc.push_back(uint8_t(crc)); sc.push_back(uint8_t(crc >> 8));
    sc.insert(sc.end(), scPayload, scPayload + sizeof(scPayload));
    wear.onBytes(sc.data(), sc.size());
    check(wear.state() == MwState::SESSION_OK, "state SESSION_OK after watch session config");

    // 认证后 AES 不可用（主机无 mbedtls）：sendCommand 加密失败返回 false，
    // 但 startAuth 前的状态检查应先忽略——这里只验证 AUTHENTICATED 前不加密
    check(!wear.authenticated(), "not authenticated on host");
    wear.end();
}

static void testAckReply() {
    std::printf("[miwear: ack]\n");
    RecStream io;
    MiWear wear;
    wear.begin(io);
    wear.onConnected();

    // 喂入一个手表 Data 帧（Pb 通道明文 command, seq=7）
    std::vector<uint8_t> listCmd = buildWatchfaceListCmd();
    std::vector<uint8_t> frame = makeDataFrame(7, 0x01, 0x01, listCmd.data(), listCmd.size());
    wear.onBytes(frame.data(), frame.size());
    // 应回 ACK: [A5A5][01][07][0000][0000]
    std::vector<uint8_t> f;
    size_t pos = 0;
    check(nthFrame(io.out, pos, 0, f), "ack frame emitted");
    if (!f.empty()) {
        check(f.size() == 8 && f[2] == 0x01 && f[3] == 7 &&
                  f[4] == 0 && f[5] == 0 && f[6] == 0 && f[7] == 0,
              "ack frame bytes [A5A5 01 seq 0000 0000]");
    }
    // 命令内容应通过 onCommand 上抛（type=4 非认证命令）
    // （main 才会解析，这里只验证帧被消费：rxFrames 增加）
    check(wear.rxFrames == 1, "rxFrames counted");
    wear.end();
}

int main() {
    testSendFraming();
    testHandshakeProgress();
    testAckReply();
    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
