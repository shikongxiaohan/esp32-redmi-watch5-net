/*
 * 性能监控（纯逻辑层，可在主机端单测）
 *
 * 职责划分：
 *   - 本文件：采样数据结构、速率/百分比/健康度判定、历史环形缓冲，
 *     全部是确定性纯函数，不依赖 ESP32
 *   - include/perfmon.h/.cpp：ESP32 侧采样（CPU idle hook、堆、Flash、
 *     WiFi/BT 字节计数、延迟探测、芯片温度）
 *
 * 重要说明（能力边界，避免读数被误读）：
 *   - ESP32 没有硬件 CPU 负载计数器。这里用「空闲钩子调用频率」估算：
 *     满空闲时钩子调用最快，忙碌时变慢。数值是近似值，不是精确占用率。
 *   - 「储存」指 Flash 占用（应用镜像 + 表盘缓存 + 系统区），不是 RAM。
 *   - 「蓝牙占用」指 SPP 链路吞吐（字节/秒），不是射频占空比。
 *   - 「网络延迟」是到公共 DNS 的 TCP 握手往返时间，不是 ICMP ping。
 */
#pragma once

#include <cstdint>

namespace xw {

// 一次性能采样
struct PerfSample {
    uint32_t t = 0;      // 采样时刻（millis）
    uint8_t cpu = 0;     // CPU 占用 0..100（估算）
    uint8_t mem = 0;     // RAM 占用 0..100
    uint8_t sto = 0;     // Flash 占用 0..100
    uint32_t netUp = 0;  // 上行 字节/秒
    uint32_t netDown = 0;// 下行 字节/秒
    uint32_t btUp = 0;   // 蓝牙上行 字节/秒
    uint32_t btDown = 0; // 蓝牙下行 字节/秒
    int16_t ping = -1;   // 延迟 ms，-1 = 未知/探测失败
    int8_t temp = -128;  // 芯片温度 °C，-128 = 不可用
    uint8_t health = 0;  // 0=良好 1=一般 2=异常
    // 绝对值明细（与上面的百分比同一次读取，保证两者一致）
    uint32_t memFree = 0;
    uint32_t memTotal = 0;
    uint32_t stoUsed = 0;
    uint32_t stoTotal = 0;
};

// 健康度
enum : uint8_t {
    PERF_HEALTH_GOOD = 0,
    PERF_HEALTH_WARN = 1,
    PERF_HEALTH_BAD = 2,
};

// CPU 读数不可用（空闲钩子注册失败时的哨兵值，网页显示为 –）
static const uint8_t PERF_CPU_UNAVAILABLE = 255;

// 百分比：used/total，四舍五入，clamp 0..100；total=0 返回 0
uint8_t perfPercent(uint64_t used, uint64_t total);

/*
 * 速率：由两次累计字节数算字节/秒。
 * 处理两种边界：
 *   - dtMs == 0：返回 0（不能除零）
 *   - 计数器回绕（cur < prev）：按 32 位无符号差值处理
 */
uint32_t perfRatePerSec(uint32_t prev, uint32_t cur, uint32_t dtMs);

/*
 * 健康度判定（纯函数）
 *   wifiOk     : WiFi 已连接
 *   authOk     : 手表已认证（链路可用）
 *   cpu/mem    : 百分比 0..100
 *   pingMs     : 延迟，-1 表示未知
 *   pingFresh  : 该延迟值是否是本次采样得到的（过期值不参与判定）
 */
uint8_t perfHealth(bool wifiOk, bool authOk, uint8_t cpu, uint8_t mem,
                   int16_t pingMs, bool pingFresh);

// 速率的可读文本，复合单位："0 B/s" / "512 B/s" / "12.3 KB/s" / "1.20 MB/s"
// out 建议至少 24 字节
void perfFormatRate(uint32_t bytesPerSec, char* out, unsigned outLen);

// 百分比的显示宽度辅助：把 0..100 映射到图表纵轴刻度
int perfAxisMax(uint8_t maxPct);

/*
 * 历史环形缓冲：固定容量，O(1) 写入，不动态分配。
 * 存储的是原始采样点，供网页画折线图。
 */
class PerfHistory {
public:
    static const int CAP = 90; // 90 点 × 2 秒 = 3 分钟

    void push(const PerfSample& s);
    void clear();
    int count() const { return _count; }
    // i = 0 为最旧，count()-1 为最新
    const PerfSample& at(int i) const { return _buf[(_head + i) % CAP]; }
    const PerfSample& latest() const { return at(_count - 1); }
    bool empty() const { return _count == 0; }

private:
    PerfSample _buf[CAP];
    int _head = 0;  // 最旧元素下标
    int _count = 0;
};

} // namespace xw
