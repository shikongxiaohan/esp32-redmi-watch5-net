/*
 * 性能监控纯逻辑实现
 */
#include "perfdata.h"

#include <cstdio>

namespace xw {

uint8_t perfPercent(uint64_t used, uint64_t total) {
    if (total == 0) return 0;
    if (used >= total) return 100;
    // 四舍五入：先乘 100 再补半分母，避免整数截断导致长期显示偏低
    uint64_t v = (used * 100u + total / 2u) / total;
    if (v > 100) v = 100;
    return uint8_t(v);
}

uint32_t perfRatePerSec(uint32_t prev, uint32_t cur, uint32_t dtMs) {
    if (dtMs == 0) return 0;
    // 无符号差值天然处理 32 位回绕
    uint32_t delta = cur - prev;
    // delta * 1000 可能超过 32 位（delta > 4.29e6 时），用 64 位中间量
    uint64_t bps = (uint64_t)delta * 1000u / dtMs;
    if (bps > 0xFFFFFFFFull) bps = 0xFFFFFFFFull;
    return uint32_t(bps);
}

uint8_t perfHealth(bool wifiOk, bool authOk, uint8_t cpu, uint8_t mem,
                   int16_t pingMs, bool pingFresh) {
    // 断网 / 手表未认证：直接异常
    if (!wifiOk || !authOk) return PERF_HEALTH_BAD;
    // 内存逼近耗尽（<10% 剩余）视为异常
    if (mem >= 90) return PERF_HEALTH_BAD;
    // CPU 读数不可用（PERF_CPU_UNAVAILABLE）时不参与判定，
    // 否则 255 >= 90 会被误判成"异常"。
    const bool cpuKnown = (cpu != PERF_CPU_UNAVAILABLE);
    // 未知延迟不参与判定（探测可能暂时失败，不代表故障）
    bool pingBad = pingFresh && pingMs >= 0 && pingMs >= 500;
    bool pingWarn = pingFresh && pingMs >= 0 && pingMs >= 200;
    if (pingBad || (cpuKnown && cpu >= 90) || mem >= 75) return PERF_HEALTH_BAD;
    if (pingWarn || (cpuKnown && cpu >= 70) || mem >= 60) return PERF_HEALTH_WARN;
    return PERF_HEALTH_GOOD;
}

void perfFormatRate(uint32_t bytesPerSec, char* out, unsigned outLen) {
    if (!out || outLen == 0) return;
    if (bytesPerSec < 1024u) {
        std::snprintf(out, outLen, "%u B/s", (unsigned)bytesPerSec);
    } else if (bytesPerSec < 1024u * 1024u) {
        // KB/s：<10 时保留 1 位小数，否则取整
        double kb = double(bytesPerSec) / 1024.0;
        if (kb < 10.0) std::snprintf(out, outLen, "%.1f KB/s", kb);
        else std::snprintf(out, outLen, "%.0f KB/s", kb);
    } else {
        double mb = double(bytesPerSec) / (1024.0 * 1024.0);
        std::snprintf(out, outLen, "%.2f MB/s", mb);
    }
}

int perfAxisMax(uint8_t maxPct) {
    // 纵轴上限取 100；数据有峰值时也固定 100，便于横向对比
    (void)maxPct;
    return 100;
}

void PerfHistory::push(const PerfSample& s) {
    if (_count < CAP) {
        _buf[(_head + _count) % CAP] = s;
        _count++;
    } else {
        // 满了：覆盖最旧
        _buf[_head] = s;
        _head = (_head + 1) % CAP;
    }
}

void PerfHistory::clear() {
    _head = 0;
    _count = 0;
}

} // namespace xw
