/*
 * LED 状态指示逻辑 + 本轮缺陷修复 的主机单元测试
 *
 * 编译运行: cd tests && make
 */
#include "ledlogic.h"
#include "serialpush_proto.h"
#include "facecache.h"
#include "proto.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

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

// ============ LED 逻辑 ============

static void testLedPatterns() {
    // 常灭 / 常亮
    CHECK(ledPatternOf(LedState::Off).periodMs == 0);
    CHECK(ledPatternOf(LedState::Online).periodMs == -1);
    // 故障提示必须能与"正常联网（常亮）"区分开，不能也是常亮
    CHECK(ledPatternOf(LedState::Error).periodMs != -1);
    CHECK(ledPatternOf(LedState::Error).periodMs != 0);
    // 闪烁模式：周期内亮起时长必须小于周期
    const LedState flashing[] = {LedState::Booting, LedState::ApMode,
                                 LedState::Connecting, LedState::OnlineNoWatch,
                                 LedState::Pairing, LedState::WatchfaceBusy,
                                 LedState::FirmwareBusy, LedState::Error};
    for (LedState s : flashing) {
        LedPattern p = ledPatternOf(s);
        CHECK(p.periodMs > 0);
        CHECK(p.onMs > 0);
        CHECK(p.onMs < (uint16_t)p.periodMs);
    }
}

static void testLedShouldBeOn() {
    // 常灭
    CHECK(!ledShouldBeOn(LedState::Off, 0));
    CHECK(!ledShouldBeOn(LedState::Off, 123456));
    // 常亮
    CHECK(ledShouldBeOn(LedState::Online, 0));
    CHECK(ledShouldBeOn(LedState::Online, 999999));

    // 启动单眨：周期 1600ms，亮 80ms
    CHECK(ledShouldBeOn(LedState::Booting, 0));
    CHECK(ledShouldBeOn(LedState::Booting, 79));
    CHECK(!ledShouldBeOn(LedState::Booting, 80));
    CHECK(!ledShouldBeOn(LedState::Booting, 1599));
    CHECK(ledShouldBeOn(LedState::Booting, 1600));
    CHECK(ledShouldBeOn(LedState::Booting, 1679));
    CHECK(!ledShouldBeOn(LedState::Booting, 1680));

    // 故障「三连眨 + 停顿」：周期 2400ms
    //   0-99 亮 / 100-199 灭 / 200-299 亮 / 300-399 灭 / 400-499 亮 / 500-2399 灭
    CHECK(ledShouldBeOn(LedState::Error, 0));
    CHECK(!ledShouldBeOn(LedState::Error, 100));
    CHECK(ledShouldBeOn(LedState::Error, 200));
    CHECK(!ledShouldBeOn(LedState::Error, 300));
    CHECK(ledShouldBeOn(LedState::Error, 400));
    CHECK(!ledShouldBeOn(LedState::Error, 500));
    // 停顿要够长，肉眼才能数清"三下"；不能像重启那样平均 200ms 眨一下
    CHECK(!ledShouldBeOn(LedState::Error, 2399));
    CHECK(ledShouldBeOn(LedState::Error, 2400));

    // 关键回归：Error 一个周期内的亮段必须是 3 段，且总亮时长远小于周期。
    // 早期版本 Error = {1200,150}，周期 1.2 秒，看起来正是"闪两下停一下"，
    // 和 bootloader 崩溃重启的节奏撞车，排查时被误判成刷机失败。
    {
        int edges = 0;
        bool prev = ledShouldBeOn(LedState::Error, 0);
        for (uint32_t t = 1; t < 2400; t++) {
            bool cur = ledShouldBeOn(LedState::Error, t);
            if (cur && !prev) edges++;
            prev = cur;
        }
        CHECK_EQ(edges, 2); // 除 t=0 的首段外，周期内还有两次由灭转亮 = 共 3 段
    }
    // 每个"非一次性"状态的周期都不应短于 1 秒，避免节奏被数成崩溃重启
    const LedState steady[] = {LedState::Connecting, LedState::OnlineNoWatch,
                               LedState::Error};
    for (LedState s : steady) {
        CHECK(ledPatternOf(s).periodMs >= 1000);
    }
}

static void testLedWraparound() {
    // millis() 回绕：now 从 0xFFFFFFF0 之后绕到 0x00000010
    // 状态机用 now % period 计算相位，回绕后仍应正确
    LedState s = LedState::Booting; // 周期 1600ms，亮 80ms
    LedPattern p = ledPatternOf(s);
    uint32_t base = 0xFFFFFFF0u;
    // 环绕点附近
    for (uint32_t d = 0; d < 64; d++) {
        uint32_t now = base + d; // 会自然回绕
        bool expect = (now % (uint32_t)p.periodMs) < (uint32_t)p.onMs;
        CHECK_EQ(ledShouldBeOn(s, now) ? 1 : 0, expect ? 1 : 0);
    }
}

static void testLedPriority() {
    // 高优先级状态覆盖低优先级
    // （表盘安装 > 固件升级 > 配网 AP > 蓝牙配对 > 连接中 > 已联网未连手表 > 已联网）
    CHECK(ledPriority(LedState::WatchfaceBusy) > ledPriority(LedState::FirmwareBusy));
    CHECK(ledPriority(LedState::FirmwareBusy) > ledPriority(LedState::ApMode));
    CHECK(ledPriority(LedState::ApMode) > ledPriority(LedState::Pairing));
    CHECK(ledPriority(LedState::Pairing) > ledPriority(LedState::Connecting));
    CHECK(ledPriority(LedState::Connecting) > ledPriority(LedState::OnlineNoWatch));
    CHECK(ledPriority(LedState::OnlineNoWatch) > ledPriority(LedState::Online));
    CHECK(ledPriority(LedState::Online) > ledPriority(LedState::Off));

    CHECK(ledMerge(LedState::Online, LedState::WatchfaceBusy) == LedState::WatchfaceBusy);
    CHECK(ledMerge(LedState::ApMode, LedState::Online) == LedState::ApMode);
    CHECK(ledMerge(LedState::Online, LedState::Online) == LedState::Online);
    CHECK(ledMerge(LedState::Off, LedState::Online) == LedState::Online);
}


// ============ 修复：状态内相位（进状态立刻可见） ============

static void testLedPhase() {
    // 常亮/常灭不看相位
    CHECK(ledPhaseFor(0, ledPatternOf(LedState::Online)) == 0);
    CHECK(ledPhaseFor(9999, ledPatternOf(LedState::Off)) == 0);

    // 首个周期内：相位 = 已持续时长，进状态即为 0 -> 立刻点亮
    LedPattern p = ledPatternOf(LedState::OnlineNoWatch); // {2000, 100}
    CHECK(ledPhaseFor(0, p) == 0);
    CHECK(ledPhaseFor(50, p) == 50);
    CHECK(ledShouldBeOn(LedState::OnlineNoWatch, ledPhaseFor(0, p)));

    // 超过一个周期后按周期回绕
    CHECK(ledPhaseFor(2000, p) == 0);
    CHECK(ledPhaseFor(2050, p) == 50);
    CHECK(ledPhaseFor(4100, p) == 100);

    // 关键回归：以前直接用 millis() 绝对值，上电时刻落在"灭"区间时
    // 进状态后一段时间看不到任何反应。现在无论"上电多久后"进状态，
    // 第一帧一定是亮的。
    const LedState all[] = {LedState::Booting, LedState::ApMode,
                            LedState::Connecting, LedState::OnlineNoWatch,
                            LedState::Pairing, LedState::WatchfaceBusy,
                            LedState::FirmwareBusy, LedState::Error};
    for (LedState s : all) {
        LedPattern q = ledPatternOf(s);
        CHECK(ledShouldBeOn(s, ledPhaseFor(0, q)));
    }
}

// ============ 修复 1：protobuf 长度字段整数溢出 ============

static void testPbLengthOverflow() {
    // 构造一个 length-delimited 字段，varint 长度 = 0xFFFFFFFFFFFFFFFF
    // 原实现用 p + len > _n 判断，加法回绕导致边界检查被绕过
    std::vector<uint8_t> buf;
    // tag: field 1, wire type 2 -> 0x0A
    buf.push_back(0x0A);
    // 长度 varint：10 字节，每字节 0xFF（最后一位也要有续位）
    for (int i = 0; i < 9; i++) buf.push_back(0xFF);
    buf.push_back(0x01); // 10 字节 varint：最高位组 = 1 -> len = 0x01FFFFFFFFFFFFFF
    buf.push_back(0xAA); // 一点载荷

    PbReader r(buf.data(), buf.size());
    PbField f;
    CHECK(!r.next(f)); // 越界长度必须被拒绝

    // 长度恰好等于剩余字节：合法
    std::vector<uint8_t> ok;
    ok.push_back(0x0A);
    ok.push_back(0x02);
    ok.push_back(0xBB);
    ok.push_back(0xCC);
    PbReader r2(ok.data(), ok.size());
    CHECK(r2.next(f));
    CHECK_EQ(f.len, 2);
    CHECK(f.data[0] == 0xBB && f.data[1] == 0xCC);

    // 长度比剩余多 1：拒绝
    std::vector<uint8_t> bad;
    bad.push_back(0x0A);
    bad.push_back(0x03);
    bad.push_back(0xBB);
    PbReader r3(bad.data(), bad.size());
    CHECK(!r3.next(f));
}

// ============ 修复 5：串口 BAUD 整数溢出 ============

static void testBaudOverflow() {
    // 正常值
    PushLine p = PushParser::parse("XWFP BAUD 921600");
    CHECK(p.cmd == PushCmd::BAUD);
    CHECK_EQ(p.num, 921600u);

    // 上限边界
    p = PushParser::parse("XWFP BAUD 4000000");
    CHECK(p.cmd == PushCmd::BAUD);
    CHECK_EQ(p.num, 4000000u);

    // 超过上限：必须拒绝
    p = PushParser::parse("XWFP BAUD 4000001");
    CHECK(p.cmd == PushCmd::UNKNOWN);

    // 关键回归：10 位以上十进制串。
    // 原实现先乘后判，uint32 回绕后 v 可能落回 <= 4000000 的区间而通过校验。
    // 例如 "4294967297" = 2^32+1，回绕后 v = 1 <= 4000000 -> 原实现会通过。
    p = PushParser::parse("XWFP BAUD 4294967297");
    CHECK(p.cmd == PushCmd::UNKNOWN);
    p = PushParser::parse("XWFP BAUD 4294967296");
    CHECK(p.cmd == PushCmd::UNKNOWN);
    p = PushParser::parse("XWFP BAUD 99999999999");
    CHECK(p.cmd == PushCmd::UNKNOWN);
    // 非数字
    p = PushParser::parse("XWFP BAUD 12ab");
    CHECK(p.cmd == PushCmd::UNKNOWN);
    p = PushParser::parse("XWFP BAUD");
    CHECK(p.cmd == PushCmd::UNKNOWN);
}

// ============ 修复 6：超长行残余被当作新命令 ============

static void testLongLineResidual() {
    // PushParser::MAX_LINE 之外的输入必须不被解析成命令
    std::string huge(PushParser::MAX_LINE + 100, 'X');
    CHECK(!PushParser::isCommand(huge.c_str()));

    // 超长行中混入 "END" 也不能被解析
    std::string mixed = "AAAA";
    mixed.append(PushParser::MAX_LINE + 10, 'B');
    mixed += "END";
    CHECK(PushParser::parse(mixed.c_str()).cmd == PushCmd::NONE);
}

// ============ 修复 2：认证状态机误判成功 ============

// 构造 Command{type=1, subtype=27, auth{status=N}}
static std::vector<uint8_t> buildAuthConfirm(uint32_t subtype, int status /* -1 = 不带 status 字段 */) {
    std::vector<uint8_t> auth;
    if (status >= 0) {
        pbPutVarintField(auth, 8, uint64_t(status));
    }
    std::vector<uint8_t> out;
    pbPutVarintField(out, 1, cmd::TYPE_AUTH);
    pbPutVarintField(out, 2, subtype);
    pbPutMessageField(out, 3, auth);
    return out;
}

static void testAuthConfirm() {
    // status=1 -> 成功
    {
        std::vector<uint8_t> b = buildAuthConfirm(cmd::AUTH_CONFIRM, 1);
        MiCommand c = parseCommand(b.data(), b.size());
        CHECK(c.ok);
        CHECK(authConfirmSucceeded(c));
    }
    // 关键回归：status=0（手表明确拒绝）必须判为失败。
    // 修复前用 varintOf(8, 1) 且对 subtype 短路，会误判为成功。
    {
        std::vector<uint8_t> b = buildAuthConfirm(cmd::AUTH_CONFIRM, 0);
        MiCommand c = parseCommand(b.data(), b.size());
        CHECK(c.ok);
        CHECK(!authConfirmSucceeded(c));
    }
    // 其他非 1 的 status 也是失败
    {
        std::vector<uint8_t> b = buildAuthConfirm(cmd::AUTH_CONFIRM, 2);
        MiCommand c = parseCommand(b.data(), b.size());
        CHECK(!authConfirmSucceeded(c));
    }
    // status 缺失 + subtype=AUTH_CONFIRM -> 回退为成功
    {
        std::vector<uint8_t> b = buildAuthConfirm(cmd::AUTH_CONFIRM, -1);
        MiCommand c = parseCommand(b.data(), b.size());
        CHECK(authConfirmSucceeded(c));
    }
    // status 缺失 + 其他 subtype -> 失败
    {
        std::vector<uint8_t> b = buildAuthConfirm(cmd::AUTH_SEND_USERID, -1);
        MiCommand c = parseCommand(b.data(), b.size());
        CHECK(!authConfirmSucceeded(c));
    }
    // status=0 + subtype=AUTH_SEND_USERID -> 失败（status 优先）
    {
        std::vector<uint8_t> b = buildAuthConfirm(cmd::AUTH_SEND_USERID, 0);
        MiCommand c = parseCommand(b.data(), b.size());
        CHECK(!authConfirmSucceeded(c));
    }
    // 解析失败的命令 -> 失败
    {
        std::vector<uint8_t> b;
        MiCommand c = parseCommand(b.data(), b.size());
        CHECK(!c.ok);
        CHECK(!authConfirmSucceeded(c));
    }
}

// ============ 故障提示有效期 ============

static void testLedFault() {
    // 无故障提示
    CHECK(!ledFaultActive(0, 0));
    CHECK(!ledFaultActive(0, 12345));

    // 提示期：until = 1000 + 3000 = 4000
    uint32_t until = 1000 + 3000;
    CHECK(ledFaultActive(until, 1000));   // 刚开始
    CHECK(ledFaultActive(until, 2000));
    CHECK(ledFaultActive(until, 3999));   // 差 1ms
    CHECK(!ledFaultActive(until, 4000));  // 正好到期
    CHECK(!ledFaultActive(until, 5000));
    CHECK(!ledFaultActive(until, 100000));

    // 通用截止时刻判定（LED 测试超时用同一套语义）
    CHECK(ledDeadlineActive(0, 0) == false);
    CHECK(ledDeadlineActive(until, 1000));
    CHECK(ledDeadlineActive(until, 3999));
    CHECK(!ledDeadlineActive(until, 4000));
    CHECK(!ledDeadlineActive(until, 5000));
}

// ============ 回归：既有功能不被破坏 ============

static void testSerialPushRegression() {
    PushLine p = PushParser::parse("XWFP BEGIN 1048576 face.bin");
    CHECK(p.cmd == PushCmd::BEGIN);
    CHECK_EQ(p.num, 1048576u);
    CHECK(p.text == "face.bin");

    p = PushParser::parse("XWFP DATA SGVsbG8=");
    CHECK(p.cmd == PushCmd::DATA);

    p = PushParser::parse("XWFP END");
    CHECK(p.cmd == PushCmd::END);

    p = PushParser::parse("XWFP ABORT");
    CHECK(p.cmd == PushCmd::ABORT);

    p = PushParser::parse("XWFP STATUS");
    CHECK(p.cmd == PushCmd::STATUS);

    CHECK(!PushParser::isCommand("[xw] log line"));
    CHECK(PushParser::parse("[xw] log line").cmd == PushCmd::NONE);
}

static void testFaceCacheRegression() {
    FaceCacheMeta m;
    m.present = true;
    m.name = "a.bin";
    m.size = 100;
    m.uploadedAt = 1000;
    CHECK(!faceCacheExpired(m, 1000, 48u * 3600u));
    CHECK(!faceCacheExpired(m, 1000 + 48u * 3600u - 1, 48u * 3600u));
    CHECK(faceCacheExpired(m, 1000 + 48u * 3600u, 48u * 3600u));
    // 时钟回拨：不清理
    CHECK(!faceCacheExpired(m, 500, 48u * 3600u));
    // 无缓存包：不过期
    FaceCacheMeta none;
    CHECK(!faceCacheExpired(none, 999999, 48u * 3600u));
}

int main() {
    testLedPatterns();
    testLedShouldBeOn();
    testLedWraparound();
    testLedPriority();
    testLedPhase();
    testLedFault();
    testPbLengthOverflow();
    testAuthConfirm();
    testBaudOverflow();
    testLongLineResidual();
    testSerialPushRegression();
    testFaceCacheRegression();

    std::printf("test_led: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
