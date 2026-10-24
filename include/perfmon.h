/*
 * 性能监控（ESP32 采样层）
 *
 * 采样内容与来源：
 *   CPU 占用   : FreeRTOS 空闲钩子调用频率估算（无硬件负载计数器）
 *   RAM 占用   : heap_caps_get_free_size / MALLOC_CAP_8BIT
 *   Flash 占用 : 应用镜像大小 + 表盘缓存 + 系统区，对总 Flash
 *   网络上下行 : NetGw 的 TCP/UDP 累计字节数，按采样间隔算速率
 *   蓝牙吞吐   : MiWear/BtLink 的 SPP 累计收发字节数，按间隔算速率
 *   网络延迟   : 定期对公共 DNS 做 TCP 握手计时（非阻塞，超时 1s）
 *   CPU 温度   : Arduino temperatureRead()（ESP32 内部传感器，±10℃ 精度）
 *
 * 采样周期 2 秒，保留最近 3 分钟（90 点）历史供网页画折线图。
 */
#pragma once

#include "perfdata.h"

#include <cstdint>
#include <functional>

namespace xw {

class PerfMon {
public:
    // 数据源回调（避免 perfmon 直接依赖 netgw/btlink，便于解耦与单测）
    struct Sources {
        // 累计字节数（单调递增，允许 32 位回绕）
        std::function<uint32_t()> netUpBytes;
        std::function<uint32_t()> netDownBytes;
        std::function<uint32_t()> btUpBytes;
        std::function<uint32_t()> btDownBytes;
        // 已用 Flash 字节数（应用镜像 + 表盘缓存 + 系统区）
        std::function<uint32_t()> flashUsedBytes;
        // 总 Flash 字节数
        std::function<uint32_t()> flashTotalBytes;
        // 链路状态
        std::function<bool()> wifiOk;
        std::function<bool()> authOk;
    };

    void begin(const Sources& src);
    void loop();

    const PerfSample& latest() const { return _latest; }
    const PerfHistory& history() const { return _hist; }
    // 让网页触发的"立即刷新"能同步一次读数
    void forceSample();

    static const uint32_t SAMPLE_INTERVAL_MS = 2000;
    static const uint32_t PING_INTERVAL_MS = 15000;
    // 温度读取间隔：ESP32 内部温度传感器占 ADC2，与 WiFi 共用，
    // 高频读取会干扰射频。30 秒一次足够反映趋势。
    static const uint32_t TEMP_INTERVAL_MS = 30000;

private:
    Sources _src;
    PerfHistory _hist;
    PerfSample _latest{};

    bool _started = false;
    uint32_t _lastSampleMs = 0;
    uint32_t _lastPingMs = 0;

    // 上一次的累计字节数（算速率用）
    uint32_t _prevNetUp = 0, _prevNetDown = 0;
    uint32_t _prevBtUp = 0, _prevBtDown = 0;
    bool _prevValid = false;

    // CPU 空闲钩子基准（次/毫秒）
    uint32_t _idleBaseRate = 1;
    uint32_t _idleLastCount = 0;
    uint32_t _idleLastMs = 0;
    bool _cpuAvailable = true; // idle hook 注册失败时为 false，CPU 读数不可用

    // 延迟探测状态（非阻塞）
    int _pingSock = -1;
    uint32_t _pingStartMs = 0;
    int16_t _lastPing = -1;
    uint32_t _lastPingOkMs = 0;

    // 温度缓存（30 秒刷新一次，避免频繁占用 ADC2）
    int8_t _cachedTemp = -128;
    uint32_t _lastTempMs = 0;
    bool _tempValid = false;

    void _sample(uint32_t now);
    void _startPing(uint32_t now);
    void _pollPing(uint32_t now);
    uint8_t _cpuPercent();
    int8_t _readTemp();
    uint32_t _flashUsed();
    uint32_t _flashTotal();
};

} // namespace xw
