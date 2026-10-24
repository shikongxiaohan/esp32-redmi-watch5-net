/*
 * 性能监控纯逻辑 主机单元测试
 *
 * 编译运行: cd tests && make
 */
#include "perfdata.h"

#include <cstdio>
#include <cstring>
#include <string>

using namespace xw;

static int g_checks = 0;
static int g_failed = 0;

#define CHECK(cond) do { \
    g_checks++; \
    if (!(cond)) { \
        g_failed++; \
        std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
    } \
} while (0)

#define CHECK_EQ(a, b) do { \
    g_checks++; \
    if (!((a) == (b))) { \
        g_failed++; \
        std::printf("FAIL %s:%d  %s(%lld) != %s(%lld)\n", __FILE__, __LINE__, \
                    #a, (long long)(a), #b, (long long)(b)); \
    } \
} while (0)

// ============ 百分比 ============

static void testPercent() {
    CHECK_EQ(perfPercent(0, 100), 0);
    CHECK_EQ(perfPercent(50, 100), 50);
    CHECK_EQ(perfPercent(100, 100), 100);
    // 超过总量必须 clamp 到 100
    CHECK_EQ(perfPercent(150, 100), 100);
    // 总量为 0 不能除零
    CHECK_EQ(perfPercent(10, 0), 0);
    // 四舍五入：1/3 = 33.3% -> 33；2/3 = 66.7% -> 67
    CHECK_EQ(perfPercent(1, 3), 33);
    CHECK_EQ(perfPercent(2, 3), 67);
    // 极小值不为 0 但要正确取整
    CHECK_EQ(perfPercent(1, 1000), 0);
    CHECK_EQ(perfPercent(5, 1000), 1); // 0.5% 四舍五入到 1
    // 大数不溢出（64 位中间量）
    CHECK_EQ(perfPercent(3435973836ull, 3435973836ull), 100);
    CHECK_EQ(perfPercent(0, 3435973836ull), 0);
    // 典型内存场景：87KB / 320KB
    CHECK_EQ(perfPercent(89088, 327680), 27);
}

// ============ 速率 ============

static void testRate() {
    // 除零保护
    CHECK_EQ(perfRatePerSec(0, 1000, 0), 0u);
    // 1 秒内 1000 字节 = 1000 B/s
    CHECK_EQ(perfRatePerSec(0, 1000, 1000), 1000u);
    // 2 秒内 1000 字节 = 500 B/s
    CHECK_EQ(perfRatePerSec(0, 1000, 2000), 500u);
    // 2 秒内 3000 字节 = 1500 B/s
    CHECK_EQ(perfRatePerSec(1000, 4000, 2000), 1500u);
    // 无变化
    CHECK_EQ(perfRatePerSec(5000, 5000, 2000), 0u);
    // 32 位回绕：prev 接近上限，cur 是回绕后的小值
    // delta = cur - prev（无符号）= 1000
    uint32_t prev = 0xFFFFFE00u; // 4294966784
    uint32_t cur = 0x000001E8u;  // 488 -> delta = 1000
    CHECK_EQ(perfRatePerSec(prev, cur, 1000), 1000u);
    // 大速率不溢出：1 秒内 10MB
    CHECK_EQ(perfRatePerSec(0, 10u * 1024u * 1024u, 1000), 10u * 1024u * 1024u);
    // 2 秒内 10MB = 5MB/s
    CHECK_EQ(perfRatePerSec(0, 10u * 1024u * 1024u, 2000), 5u * 1024u * 1024u);
}

// ============ 格式化 ============

static void testFormat() {
    char buf[32];
    perfFormatRate(0, buf, sizeof(buf));
    CHECK(std::strcmp(buf, "0 B/s") == 0);

    perfFormatRate(512, buf, sizeof(buf));
    CHECK(std::strcmp(buf, "512 B/s") == 0);

    perfFormatRate(1023, buf, sizeof(buf));
    CHECK(std::strcmp(buf, "1023 B/s") == 0);

    perfFormatRate(1024, buf, sizeof(buf));
    CHECK(std::strcmp(buf, "1.0 KB/s") == 0);

    perfFormatRate(1536, buf, sizeof(buf));
    CHECK(std::strcmp(buf, "1.5 KB/s") == 0);

    // >= 10KB 取整
    perfFormatRate(10 * 1024, buf, sizeof(buf));
    CHECK(std::strcmp(buf, "10 KB/s") == 0);

    perfFormatRate(1024u * 1024u, buf, sizeof(buf));
    CHECK(std::strcmp(buf, "1.00 MB/s") == 0);

    perfFormatRate(1536u * 1024u, buf, sizeof(buf));
    CHECK(std::strcmp(buf, "1.50 MB/s") == 0);

    // 空指针 / 零长度不能崩
    perfFormatRate(100, nullptr, 10);
    perfFormatRate(100, buf, 0);
}

// ============ 健康度 ============

static void testHealth() {
    // 全部正常
    CHECK_EQ(perfHealth(true, true, 10, 30, 20, true), PERF_HEALTH_GOOD);
    // 断网 -> 异常
    CHECK_EQ(perfHealth(false, true, 10, 30, 20, true), PERF_HEALTH_BAD);
    // 手表未认证 -> 异常
    CHECK_EQ(perfHealth(true, false, 10, 30, 20, true), PERF_HEALTH_BAD);
    // 内存 >= 90 -> 异常
    CHECK_EQ(perfHealth(true, true, 10, 90, 20, true), PERF_HEALTH_BAD);
    // CPU >= 90 -> 异常
    CHECK_EQ(perfHealth(true, true, 90, 30, 20, true), PERF_HEALTH_BAD);
    // 延迟 > 500 -> 异常
    CHECK_EQ(perfHealth(true, true, 10, 30, 600, true), PERF_HEALTH_BAD);
    // 内存 >= 75 -> 异常
    CHECK_EQ(perfHealth(true, true, 10, 75, 20, true), PERF_HEALTH_BAD);
    // 延迟 > 200 -> 一般
    CHECK_EQ(perfHealth(true, true, 10, 30, 250, true), PERF_HEALTH_WARN);
    // CPU >= 70 -> 一般
    CHECK_EQ(perfHealth(true, true, 70, 30, 20, true), PERF_HEALTH_WARN);
    // 内存 >= 60 -> 一般
    CHECK_EQ(perfHealth(true, true, 10, 60, 20, true), PERF_HEALTH_WARN);
    // 延迟未知（-1）不参与判定 -> 良好
    CHECK_EQ(perfHealth(true, true, 10, 30, -1, true), PERF_HEALTH_GOOD);
    // 延迟值过期（pingFresh=false）不参与判定 -> 良好
    CHECK_EQ(perfHealth(true, true, 10, 30, 900, false), PERF_HEALTH_GOOD);
    // 边界：恰好低于阈值应仍是"良好"
    CHECK_EQ(perfHealth(true, true, 69, 59, 199, true), PERF_HEALTH_GOOD);
    // 边界：恰好达到阈值应升级为"一般"
    CHECK_EQ(perfHealth(true, true, 70, 30, 20, true), PERF_HEALTH_WARN);
    CHECK_EQ(perfHealth(true, true, 10, 60, 20, true), PERF_HEALTH_WARN);
    CHECK_EQ(perfHealth(true, true, 10, 30, 200, true), PERF_HEALTH_WARN);
    // 边界：恰好达到"异常"阈值
    CHECK_EQ(perfHealth(true, true, 90, 30, 20, true), PERF_HEALTH_BAD);
    CHECK_EQ(perfHealth(true, true, 10, 90, 20, true), PERF_HEALTH_BAD);
    CHECK_EQ(perfHealth(true, true, 10, 30, 500, true), PERF_HEALTH_BAD);
    CHECK_EQ(perfHealth(true, true, 10, 30, 499, true), PERF_HEALTH_WARN);
}

// ============ 历史环形缓冲 ============

static void testHistory() {
    PerfHistory h;
    CHECK(h.empty());
    CHECK_EQ(h.count(), 0);

    // 推 3 条
    for (int i = 0; i < 3; i++) {
        PerfSample s;
        s.t = uint32_t(i);
        s.cpu = uint8_t(i * 10);
        h.push(s);
    }
    CHECK_EQ(h.count(), 3);
    CHECK_EQ(h.at(0).cpu, 0);
    CHECK_EQ(h.at(1).cpu, 10);
    CHECK_EQ(h.at(2).cpu, 20);
    CHECK_EQ(h.latest().cpu, 20);

    // 填满
    h.clear();
    for (int i = 0; i < PerfHistory::CAP; i++) {
        PerfSample s;
        s.cpu = uint8_t(i % 100);
        h.push(s);
    }
    CHECK_EQ(h.count(), PerfHistory::CAP);
    CHECK_EQ(h.at(0).cpu, 0);
    CHECK_EQ(h.at(PerfHistory::CAP - 1).cpu, uint8_t((PerfHistory::CAP - 1) % 100));

    // 再推 5 条：应覆盖最旧的 5 条，顺序仍为从旧到新
    for (int i = 0; i < 5; i++) {
        PerfSample s;
        s.cpu = 200;
        h.push(s);
    }
    CHECK_EQ(h.count(), PerfHistory::CAP);
    // 最新 5 条都是 200
    for (int i = 1; i <= 5; i++) {
        CHECK_EQ(h.at(PerfHistory::CAP - i).cpu, 200);
    }
    // 第 0 条应是原先的第 5 条
    CHECK_EQ(h.at(0).cpu, 5);
    CHECK_EQ(h.latest().cpu, 200);
}

// ============ 回归：默认值语义 ============

static void testDefaults() {
    PerfSample s;
    // 未采样的默认值必须能被前端识别为"未知"
    CHECK_EQ(s.ping, -1);
    CHECK_EQ(s.temp, int8_t(-128));
    CHECK_EQ(s.cpu, 0);
    CHECK_EQ(s.health, PERF_HEALTH_GOOD);
    // ping = -1 表示未知，不能与"0ms"混淆
    CHECK(s.ping < 0);
    // temp = -128 表示不可用
    CHECK(s.temp < -100);
}

int main() {
    testPercent();
    testRate();
    testFormat();
    testHealth();
    testHistory();
    testDefaults();

    std::printf("test_perf: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
