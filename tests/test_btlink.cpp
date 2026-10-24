/*
 * BtLink 主机单元测试（桩：BluetoothSerial/FreeRTOS/esp_spp_api）。
 * 覆盖：MAC 解析、SppStream 环形缓冲（含回绕/丢弃）、SSP 自动确认、
 *       DATA_IND 数据通路、连接状态沿、write 转发、扫描去重与上限、
 *       bonded 枚举、setWatchMac 规范化。
 */
#include "btlink.h"

#include <freertos/task.h> // 主机桩：FakeTaskInfo / xTaskCreatePinnedToCore

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

// ---- 桩全局 ----
SerialStub Serial;
FakeTaskInfo g_fakeTask;

// ---- 测试框架 ----
static int g_checks = 0;
static int g_failures = 0;

#define CHECK(cond)                                                     \
    do {                                                                \
        g_checks++;                                                     \
        if (!(cond)) {                                                  \
            g_failures++;                                               \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                               \
    } while (0)

#define CHECK_EQ_STR(a, b)                                              \
    do {                                                                \
        g_checks++;                                                     \
        if (std::strcmp((a), (b)) != 0) {                               \
            g_failures++;                                               \
            std::printf("FAIL %s:%d: \"%s\" != \"%s\"\n", __FILE__,     \
                        __LINE__, (a), (b));                            \
        }                                                               \
    } while (0)

static uint32_t g_fakeNow = 1000000;
uint32_t xw_test_millis() { return g_fakeNow; }

// 全局日志收集（避免 onLog lambda 捕获栈上对象导致 use-after-return）
static std::vector<std::string> g_logs;
static std::vector<std::string> g_events;

using namespace xw;

// ==================== MAC 解析 ====================
static void testMac() {
    std::printf("[btlink: mac parse]\n");
    uint8_t b[6];
    char s[18];

    CHECK(BtLink::parseMac("AA:BB:CC:DD:EE:FF", b));
    CHECK(b[0] == 0xAA && b[1] == 0xBB && b[2] == 0xCC && b[3] == 0xDD && b[4] == 0xEE && b[5] == 0xFF);
    BtLink::macToString(b, s);
    CHECK_EQ_STR(s, "AA:BB:CC:DD:EE:FF");

    // 小写 + 短横线分隔
    CHECK(BtLink::parseMac("aa-bb-cc-dd-ee-ff", b));
    BtLink::macToString(b, s);
    CHECK_EQ_STR(s, "AA:BB:CC:DD:EE:FF");

    // 无分隔符
    CHECK(BtLink::parseMac("0123456789AB", b));
    CHECK(b[0] == 0x01 && b[5] == 0xAB);
    BtLink::macToString(b, s);
    CHECK_EQ_STR(s, "01:23:45:67:89:AB");

    // 带空格
    CHECK(BtLink::parseMac("AA: BB: CC: DD: EE: FF", b));
    CHECK(b[0] == 0xAA && b[5] == 0xFF);

    // 非法输入
    CHECK(!BtLink::parseMac("", b));
    CHECK(!BtLink::parseMac("AABBCCDDEEF", b));   // 11 位
    CHECK(!BtLink::parseMac("AABBCCDDEEFFG", b)); // 非法字符
    CHECK(!BtLink::parseMac("ZZ:BB:CC:DD:EE:FF", b));
    CHECK(!BtLink::parseMac("AA:BB:CC:DD:EE", b));
    CHECK(!BtLink::parseMac(nullptr, b));
}

// ==================== SppStream ====================
static void testStreamBasic() {
    std::printf("[btlink: spp stream basic]\n");
    BtLink::SppStream st;
    CHECK(st.available() == 0);
    CHECK(st.read() == -1);

    const uint8_t data[] = {1, 2, 3, 4, 5};
    st.pushRx(data, 5);
    CHECK(st.available() == 5);
    CHECK(st.read() == 1);
    CHECK(st.available() == 4);

    uint8_t out[16];
    CHECK(st.readBulk(out, sizeof(out)) == 4);
    CHECK(out[0] == 2 && out[1] == 3 && out[2] == 4 && out[3] == 5);
    CHECK(st.available() == 0);

    // peek 不消费（数据移入 peek 缓存，available 归零，read 时取回）
    const uint8_t d2[] = {0x7E};
    st.pushRx(d2, 1);
    CHECK(st.peek() == 0x7E);
    CHECK(st.available() == 0);
    CHECK(st.peek() == 0x7E); // 重复 peek 仍返回缓存
    CHECK(st.read() == 0x7E);
    CHECK(st.available() == 0);

    // readBulk 正确处理 peek 缓存
    st.pushRx(d2, 1);
    const uint8_t d3[] = {0x11, 0x22};
    st.pushRx(d3, 2);
    CHECK(st.peek() == 0x7E);
    uint8_t out2[8];
    CHECK(st.readBulk(out2, sizeof(out2)) == 3);
    CHECK(out2[0] == 0x7E && out2[1] == 0x11 && out2[2] == 0x22);
}

static void testStreamWrap() {
    std::printf("[btlink: spp stream wrap]\n");
    BtLink::SppStream st;
    // 写超过一轮（16KB）验证环形回绕；每批写完后立即读空（读速率 ≥ 写速率）
    std::vector<uint8_t> big(16384 * 2 + 100, 0);
    for (size_t i = 0; i < big.size(); i++) big[i] = uint8_t(i * 7 + 3);

    size_t written = 0, readPos = 0;
    while (written < big.size()) {
        size_t chunk = 997;
        if (chunk > big.size() - written) chunk = big.size() - written;
        st.pushRx(big.data() + written, chunk);
        written += chunk;
        uint8_t buf[2048];
        for (;;) {
            size_t n = st.readBulk(buf, sizeof(buf));
            if (n == 0) break;
            for (size_t i = 0; i < n; i++) {
                CHECK(buf[i] == big[readPos]);
                readPos++;
            }
        }
    }
    CHECK(readPos == big.size());
    CHECK(st.dropped == 0);
}

static void testStreamOverflow() {
    std::printf("[btlink: spp stream overflow]\n");
    BtLink::SppStream st;
    std::vector<uint8_t> big(20000, 0xAB);
    st.pushRx(big.data(), big.size());
    CHECK(st.available() == 16384);          // 容量上限
    CHECK(st.dropped == 20000 - 16384);      // 丢弃计数
    uint8_t out[16384];
    CHECK(st.readBulk(out, sizeof(out)) == 16384);
    CHECK(out[0] == 0xAB && out[16383] == 0xAB);
    CHECK(st.available() == 0);
}

// ==================== begin / 回调接线 ====================
static void testBeginWiring(BtLink& bt) {
    std::printf("[btlink: begin wiring]\n");
    g_logs.clear();
    g_events.clear();
    bt.onLog = [](const char* m) { g_logs.push_back(m ? m : ""); };
    bt.onConnect = []() { g_events.push_back("connect"); };
    bt.onDisconnect = []() { g_events.push_back("disconnect"); };

    bt.begin("aa:bb:cc:dd:ee:ff");
    CHECK(g_fakeTask.created);
    CHECK_EQ_STR(g_fakeTask.name, "btlink");
    CHECK(g_fakeTask.stack == 8192);
    CHECK(g_fakeTask.prio == 3);
    CHECK(g_fakeTask.core == 0);
    CHECK(bt.hasWatchMac());
    CHECK_EQ_STR(bt.watchMacStr(), "AA:BB:CC:DD:EE:FF"); // 规范化

    // DATA_IND -> 环形缓冲（经 stream 可读）
    const uint8_t pkt[] = {0xA5, 0xA5, 0x03, 0x01, 0x00, 0x00};
    bt.serial().fakeDeliver(pkt, sizeof(pkt));
    CHECK(bt.stream().available() == int(sizeof(pkt)));
    uint8_t out[16];
    CHECK(bt.stream().readBulk(out, sizeof(out)) == sizeof(pkt));
    CHECK(std::memcmp(out, pkt, sizeof(pkt)) == 0);

    // SSP confirm 自动接受
    bt.serial()._onConfirm(123456);
    CHECK(bt.serial()._confirmCount == 1);
    CHECK(bt.serial()._confirmReply == true);

    // 连接状态沿
    bt.serial().fakeConnected = true;
    bt.loop();
    CHECK(g_events.size() == 1);
    CHECK_EQ_STR(g_events.back().c_str(), "connect");

    // 连接后 write 转发到 TX
    size_t w = bt.stream().write((const uint8_t*)"hello", 5);
    CHECK(w == 5);
    CHECK(bt.serial().txBuffer.size() == 5);
    CHECK(std::memcmp(bt.serial().txBuffer.data(), "hello", 5) == 0);
    // loop() 中 flush
    int flushesBefore = bt.serial()._flushCount;
    bt.loop();
    CHECK(bt.serial()._flushCount > flushesBefore);

    // 断开沿
    bt.serial().fakeConnected = false;
    bt.loop();
    CHECK(g_events.size() == 2);
    CHECK_EQ_STR(g_events.back().c_str(), "disconnect");

    // 断开后 write 返回 0
    CHECK(bt.stream().write((const uint8_t*)"x", 1) == 0);

    // 日志有输出
    CHECK(!g_logs.empty());
}

// ==================== 扫描 ====================
static void testScan(BtLink& bt) {
    std::printf("[btlink: scan]\n");
    CHECK(!bt.scanning());
    bt.startScan(5000);
    CHECK(bt.scanning());
    CHECK(bt.serial()._discoverAsyncCount >= 1);
    CHECK(bt.scanCount() == 0);

    // 注入扫描结果
    FakeDevice d1("AA:BB:CC:DD:EE:FF", "Redmi Watch 5", -55, true);
    bt.serial().fakeScanDevice(&d1);
    CHECK(bt.scanCount() == 1);
    CHECK_EQ_STR(bt.scanResult(0).name, "Redmi Watch 5");
    CHECK_EQ_STR(bt.scanResult(0).mac, "AA:BB:CC:DD:EE:FF");
    CHECK(bt.scanResult(0).rssi == -55);

    // 重复 MAC 去重（刷新 RSSI）
    FakeDevice d1b("AA:BB:CC:DD:EE:FF", "Redmi Watch 5", -60, true);
    bt.serial().fakeScanDevice(&d1b);
    CHECK(bt.scanCount() == 1);
    CHECK(bt.scanResult(0).rssi == -60);

    // 无名称设备
    FakeDevice d2("11:22:33:44:55:66", "", -70, false);
    bt.serial().fakeScanDevice(&d2);
    CHECK(bt.scanCount() == 2);
    CHECK_EQ_STR(bt.scanResult(1).name, "");
    CHECK(bt.scanResult(1).rssi == -70);

    // 上限 32
    for (int i = 0; i < 40; i++) {
        char mac[18];
        std::snprintf(mac, sizeof(mac), "02:00:00:%02X:%02X:%02X", i / 256, i / 16, i);
        // 保证 MAC 唯一且合法 hex
        char mac2[18];
        std::snprintf(mac2, sizeof(mac2), "02:%02X:%02X:%02X:%02X:%02X",
                      (i * 3) % 256, (i * 5) % 256, (i * 7) % 256,
                      (i * 11) % 256, (i * 13) % 256);
        FakeDevice d(mac2, "x", -40, true);
        bt.serial().fakeScanDevice(&d);
        (void)mac;
    }
    CHECK(bt.scanCount() == 32);

    // 停止
    bt.stopScan();
    CHECK(!bt.scanning());

    // 超时自动停止
    bt.startScan(1000);
    CHECK(bt.scanning());
    g_fakeNow += 1500;
    bt.loop();
    CHECK(!bt.scanning());

    // 重扫清空结果
    bt.startScan(3000);
    CHECK(bt.scanCount() == 0);
    bt.stopScan();
}

// ==================== bonded ====================
static void testBonded(BtLink& bt) {
    std::printf("[btlink: bonded]\n");
    CHECK(bt.bondedCount() == 0);

    // 注入 bonded 列表（esp_bd_addr_t 与人类 MAC 同序）
    bt.serial()._fakeBondedN = 2;
    uint8_t m1[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
    uint8_t m2[6] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06};
    std::memcpy(bt.serial()._fakeBonded[0], m1, 6);
    std::memcpy(bt.serial()._fakeBonded[1], m2, 6);

    bt.refreshBonded();
    CHECK(bt.bondedCount() == 2);
    CHECK_EQ_STR(bt.bondedDevice(0).mac, "AA:BB:CC:DD:EE:FF");
    CHECK_EQ_STR(bt.bondedDevice(1).mac, "01:02:03:04:05:06");
    CHECK(bt.bondedDevice(0).bonded);

    // 扫描结果标记 bonded
    bt.startScan(3000);
    FakeDevice d("AA:BB:CC:DD:EE:FF", "Redmi Watch 5", -55, true);
    bt.serial().fakeScanDevice(&d);
    CHECK(bt.scanCount() == 1);
    CHECK(bt.scanResult(0).bonded);
    bt.stopScan();

    // 空 bonded
    bt.serial()._fakeBondedN = 0;
    bt.refreshBonded();
    CHECK(bt.bondedCount() == 0);
}

// ==================== setWatchMac ====================
static void testSetWatchMac(BtLink& bt) {
    std::printf("[btlink: setWatchMac]\n");
    bt.setWatchMac("112233445566");
    CHECK(bt.hasWatchMac());
    CHECK_EQ_STR(bt.watchMacStr(), "11:22:33:44:55:66");

    bt.setWatchMac("aa:bb:cc:dd:ee:ff");
    CHECK_EQ_STR(bt.watchMacStr(), "AA:BB:CC:DD:EE:FF");

    bt.setWatchMac("garbage");
    CHECK(!bt.hasWatchMac());

    bt.setWatchMac("AA:BB:CC:DD:EE:FF");
    CHECK(bt.hasWatchMac());
    bt.setWatchMac("");
    CHECK(!bt.hasWatchMac());
}

int main() {
    testMac();
    testStreamBasic();
    testStreamWrap();
    testStreamOverflow();

    static BtLink bt;
    testBeginWiring(bt);
    testScan(bt);
    testBonded(bt);
    testSetWatchMac(bt);

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
