/*
 * 性能监控 ESP32 采样层实现
 */
#include "perfmon.h"

#include <Arduino.h>

#include <esp_heap_caps.h>
#include <esp_freertos_hooks.h>
#include <esp_partition.h>
#include <esp_system.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

#include <cstring>

namespace xw {

// ==================== CPU 占用：空闲钩子计数 ====================
/*
 * ESP32 没有硬件 CPU 负载计数器，FreeRTOS 运行时统计（configGENERATE_RUN_TIME_STATS）
 * 在 Arduino 预编译框架里是关闭的。可行方案：统计空闲钩子的调用频率。
 *
 * 原理：空闲任务死循环调用钩子。CPU 越忙，空闲任务被抢占越久，钩子调用越慢。
 *   占用率 ≈ 100% - (当前钩子速率 / 满空闲钩子速率)
 *
 * 满空闲速率在 begin() 时标定（此时 WiFi/BT 尚未启动，接近真正空闲）。
 * 之后取「标定值」与「运行中观测到的最大值」的较大者作为基准，
 * 避免一次异常高峰把基准压得过低导致读数长期为 0。
 */
static volatile uint32_t s_idleCount = 0;
static bool s_idleHookOk = false;

static bool idleHook() {
    s_idleCount++; // 单条自增，钩子里不能做重活
    return false;  // false = 不让出，保持空闲任务原行为（尽可能快地被反复调用）
}

void PerfMon::begin(const Sources& src) {
    _src = src;
    _hist.clear();
    _latest = PerfSample{};
    _prevValid = false;

    // 注册 CPU1 的空闲钩子（Arduino loopTask 跑在 CPU1）
    s_idleCount = 0;
    s_idleHookOk = (esp_register_freertos_idle_hook_for_cpu(idleHook, 1) == ESP_OK);

    if (s_idleHookOk) {
        // 标定满空闲速率：此时 WiFi/BT 还没起来，loop 任务阻塞在 delay 里
        uint32_t t0 = millis();
        delay(120);
        uint32_t dt = millis() - t0;
        uint32_t c = s_idleCount;
        _idleBaseRate = (dt > 0) ? (c / dt) : 1; // 次/毫秒
        if (_idleBaseRate == 0) _idleBaseRate = 1;
        _idleLastCount = c;
        _idleLastMs = millis();
    } else {
        // 注册失败（同一核的 idle hook 槽位已满）：CPU 读数不可信，
        // 标记为不可用而不是硬算出一个假的 100%。
        _cpuAvailable = false;
    }

    _started = true;
    _lastSampleMs = millis() - SAMPLE_INTERVAL_MS; // 让首轮立即采样
    _lastPingMs = millis();
}

uint8_t PerfMon::_cpuPercent() {
    if (!_cpuAvailable) return PERF_CPU_UNAVAILABLE;
    uint32_t now = millis();
    uint32_t dt = now - _idleLastMs;
    if (dt < 20) return _latest.cpu; // 间隔太短，噪声大，沿用上次
    uint32_t cnt = s_idleCount;
    uint32_t delta = cnt - _idleLastCount; // 无符号差值，抗回绕
    _idleLastCount = cnt;
    _idleLastMs = now;

    uint32_t rate = delta / dt; // 次/毫秒
    // 基准只升不降：观测到更高的空闲速率说明当时更空闲，作为更准的 100% 参考
    if (rate > _idleBaseRate) _idleBaseRate = rate;
    if (_idleBaseRate == 0) return 0;

    if (rate >= _idleBaseRate) return 0;
    uint32_t busy = (_idleBaseRate - rate) * 100u / _idleBaseRate;
    if (busy > 100) busy = 100;
    return uint8_t(busy);
}

// ==================== 温度 ====================
int8_t PerfMon::_readTemp() {
    /*
     * ESP32（经典款）的内部温度传感器与 WiFi 共用 ADC2：
     * WiFi 处于工作状态时，传感器读数会失效——底层 temprature_sens_read()
     * 固定返回 128（华氏），换算后约 53.3℃。
     *
     * 这是芯片的已知限制，不是本项目能绕过的。若不处理，用户会看到
     * 一个恒定 53℃ 的假读数，误以为芯片在高温运行。因此把该特征值
     * 判为"不可用"，网页显示为 –。
     *
     * 另外，传感器精度约 ±10℃，只表示芯片温度趋势，不是环境温度。
     */
    float t = temperatureRead();
    if (isnan(t) || t < -40.0f || t > 125.0f) return -128;
    // 128°F 特征值：WiFi 工作时传感器失效的标志
    if (t > 52.5f && t < 54.0f) return -128;
    return int8_t(lroundf(t));
}

// ==================== Flash ====================
uint32_t PerfMon::_flashUsed() {
    if (_src.flashUsedBytes) return _src.flashUsedBytes();
    return 0;
}

uint32_t PerfMon::_flashTotal() {
    if (_src.flashTotalBytes) return _src.flashTotalBytes();
    return ESP.getFlashChipSize();
}

// ==================== 延迟探测（非阻塞 TCP 握手） ====================
/*
 * 对公共 DNS 的 53 端口做一次 TCP 三次握手，测往返时间。
 * 不用 ICMP ping：lwIP 的 ping 会话 API 需要额外组件，且要处理权限；
 * TCP 握手耗时与 RTT 高度相关，对「网络通不通、快不快」足够。
 */
// 223.5.5.5 在内存中的字节序：0xDF,0x05,0x05,0x05 -> 小端读作 0x050505DF
// 与 netgw.cpp 的 DNS_SERVER_BE 保持一致（同一台 AliDNS，同一字节序约定）
static const uint32_t PING_TARGET_IP = 0x050505DFu;

void PerfMon::_startPing(uint32_t now) {
    if (_pingSock >= 0) return;
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return;
    int flags = ::fcntl(fd, F_GETFL, 0);
    ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    sockaddr_in sa;
    std::memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(53);
    sa.sin_addr.s_addr = PING_TARGET_IP;

    int r = ::connect(fd, (sockaddr*)&sa, sizeof(sa));
    if (r == 0) {
        // 立即成功（极快或本地缓存）
        _lastPing = 0;
        _lastPingOkMs = now;
        ::close(fd);
        return;
    }
    if (errno != EINPROGRESS) {
        ::close(fd);
        _lastPing = -1;
        return;
    }
    _pingSock = fd;
    _pingStartMs = now;
}

void PerfMon::_pollPing(uint32_t now) {
    if (_pingSock < 0) return;
    uint32_t elapsed = now - _pingStartMs;

    fd_set wfds;
    FD_ZERO(&wfds);
    FD_SET(_pingSock, &wfds);
    timeval tv{0, 0};
    int sel = ::select(_pingSock + 1, nullptr, &wfds, nullptr, &tv);

    if (sel > 0 && FD_ISSET(_pingSock, &wfds)) {
        int err = 0;
        socklen_t el = sizeof(err);
        if (::getsockopt(_pingSock, SOL_SOCKET, SO_ERROR, &err, &el) == 0 && err == 0) {
            _lastPing = int16_t(elapsed > 30000 ? 30000 : elapsed);
            _lastPingOkMs = now;
        } else {
            _lastPing = -1;
        }
        ::close(_pingSock);
        _pingSock = -1;
        return;
    }
    // 超时 1 秒视为失败
    if (elapsed > 1000) {
        ::close(_pingSock);
        _pingSock = -1;
        _lastPing = -1;
    }
}

// ==================== 采样 ====================
void PerfMon::_sample(uint32_t now) {
    PerfSample s;
    s.t = now;
    s.cpu = _cpuPercent();

    // RAM：8bit 可分配堆（不含 PSRAM），这是最贴近「可用内存」的口径
    size_t freeHeap = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    size_t totalHeap = heap_caps_get_total_size(MALLOC_CAP_8BIT);
    if (totalHeap == 0) totalHeap = ESP.getHeapSize();
    // 理论上 free <= total；若底层给出异常值导致相减下溢，clamp 到 100%
    size_t usedHeap = (freeHeap < totalHeap) ? (totalHeap - freeHeap) : totalHeap;
    s.mem = perfPercent(usedHeap, totalHeap);
    s.memFree = uint32_t(freeHeap);
    s.memTotal = uint32_t(totalHeap);

    // Flash
    uint32_t fUsed = _flashUsed();
    uint32_t fTotal = _flashTotal();
    s.sto = perfPercent(fUsed, fTotal);
    s.stoUsed = fUsed;
    s.stoTotal = fTotal;

    // 速率
    uint32_t nu = _src.netUpBytes ? _src.netUpBytes() : 0;
    uint32_t nd = _src.netDownBytes ? _src.netDownBytes() : 0;
    uint32_t bu = _src.btUpBytes ? _src.btUpBytes() : 0;
    uint32_t bd = _src.btDownBytes ? _src.btDownBytes() : 0;
    if (_prevValid) {
        uint32_t dt = now - _lastSampleMs;
        s.netUp = perfRatePerSec(_prevNetUp, nu, dt);
        s.netDown = perfRatePerSec(_prevNetDown, nd, dt);
        s.btUp = perfRatePerSec(_prevBtUp, bu, dt);
        s.btDown = perfRatePerSec(_prevBtDown, bd, dt);
    }
    _prevNetUp = nu;
    _prevNetDown = nd;
    _prevBtUp = bu;
    _prevBtDown = bd;
    _prevValid = true;

    // 延迟：15 秒内的读数算"新鲜"
    s.ping = _lastPing;
    bool pingFresh = (now - _lastPingOkMs) < 2 * PING_INTERVAL_MS;
    if (!pingFresh) s.ping = -1;

    // 温度：30 秒刷新一次；读取本身会短暂占用 ADC2（与 WiFi 共用），
    // 所以不做每 2 秒一次的采样。
    if (!_tempValid || (now - _lastTempMs) >= TEMP_INTERVAL_MS) {
        _cachedTemp = _readTemp();
        _lastTempMs = now;
        _tempValid = true;
    }
    s.temp = _cachedTemp;

    bool wifiOk = _src.wifiOk ? _src.wifiOk() : false;
    bool authOk = _src.authOk ? _src.authOk() : false;
    s.health = perfHealth(wifiOk, authOk, s.cpu, s.mem, s.ping, pingFresh);

    _latest = s;
    _hist.push(s);
}

void PerfMon::forceSample() {
    if (!_started) return;
    uint32_t now = millis();
    // 至少间隔 200ms，避免网页狂点刷新把采样打爆
    if (now - _lastSampleMs < 200) return;
    _sample(now);
    _lastSampleMs = now;
}

void PerfMon::loop() {
    if (!_started) return;
    uint32_t now = millis();

    _pollPing(now);
    if (now - _lastPingMs >= PING_INTERVAL_MS && _pingSock < 0) {
        _lastPingMs = now;
        _startPing(now);
    }

    if (now - _lastSampleMs >= SAMPLE_INTERVAL_MS) {
        _lastSampleMs = now;
        _sample(now);
    }
}

} // namespace xw
