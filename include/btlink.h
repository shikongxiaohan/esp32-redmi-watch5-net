/*
 * 蓝牙 SPP 链路管理（BtLink）
 *
 * 封装 Arduino BluetoothSerial，负责：
 *   - SPP 连接 / 断开 / 自动重连（退避 5s）
 *   - 配对：SSP confirm request 自动接受（手表端确认）
 *   - 设备扫描（Web 配对页用，非阻塞）
 *   - 已配对（bonded）设备枚举
 *
 * 实现要点（依据 arduino-esp32 BluetoothSerial 源码，master 分支核实）：
 *   - RX：BluetoothSerial 内部 _spp_rx_queue 仅 512 字节，且注册 onData 后
 *     该队列不再被填充。手表网络流量单帧可达 ~800 字节且突发，因此本模块
 *     注册 onData，把数据写入自建 16KB 固定环形缓冲（临界区保护，无动态
 *     分配），由主循环 readBulk() 消费后交给 MiWear。
 *   - TX：BluetoothSerial::write() 内部 xQueueSend 入队（队列 32 包，满时
 *     阻塞最多 1s），真正的 esp_spp_write 在其 TX 任务中完成。本模块在
 *     loop() 中周期调用 flush()（队列空时立即返回，零开销）。
 *   - connect() 会做 SDP 发现（channel=0）并阻塞最多 10s，且内部会先
 *     disconnect()。因此必须在独立 FreeRTOS 任务中调用，不能放在主循环
 *     或蓝牙回调上下文。
 *   - connected(timeout=0) 读取 SPP_CONNECTED 事件位，非阻塞。
 *
 * 线程模型：
 *   - BT 任务（core 0, prio 3）：阻塞 connect()、断开重连、pending 动作
 *   - BT 栈回调（BTC task）：DATA_IND -> 环形缓冲；GAP DISC_RES -> 扫描结果
 *   - 主循环：loop() 检测连接状态沿、扫描超时、flush TX
 */
#pragma once

#include <Arduino.h>
#include <BluetoothSerial.h>
#include <functional>
#include <string>
#include <cstdint>
#include <cstddef>

namespace xw {

class BtLink {
public:
    // 扫描 / bonded 列表条目（固定长度字段，回调上下文安全）
    struct Device {
        char name[40] = {0};
        char mac[18] = {0}; // "AA:BB:CC:DD:EE:FF"
        int8_t rssi = 0;
        bool bonded = false;
    };

    /*
     * MiWear 的 IO Stream。
     * write -> BluetoothSerial 入队；read/available -> 自建环形缓冲。
     */
    class SppStream : public Stream {
    public:
        void attach(BluetoothSerial& serial) { _serial = &serial; }

        int available() override;
        int read() override;
        int peek() override;
        size_t write(uint8_t c) override;
        size_t write(const uint8_t* buf, size_t n) override;
        void flush() override {} // 实际刷新在 BtLink::loop()

        // 批量读出（主循环用），返回实际字节数
        size_t readBulk(uint8_t* out, size_t cap);

        // 由 BtLink 的 SPP DATA_IND 回调调用（BT 栈上下文，须快速返回）
        void pushRx(const uint8_t* data, size_t len);

        uint32_t dropped = 0; // 环形缓冲满丢弃计数

    private:
        BluetoothSerial* _serial = nullptr;
        static constexpr size_t RX_CAP = 16384; // 2 的幂
        static constexpr size_t RX_MASK = RX_CAP - 1;
        uint8_t _rx[RX_CAP];
        volatile size_t _rxHead = 0; // 单调递增，不回绕
        volatile size_t _rxTail = 0;
        portMUX_TYPE _rxMux = portMUX_INITIALIZER_UNLOCKED;
        bool _hasPeek = false;
        uint8_t _peekByte = 0;
    };

    BtLink() = default;

    // watchMac 可空（nullptr/空串 = 自动从已配对设备中挑选）
    void begin(const char* watchMac);
    void end();

    // 主循环调用
    void loop();

    SppStream& stream() { return _stream; }

    // 底层 BluetoothSerial（调试/诊断用；勿在回调上下文调用阻塞方法）
    BluetoothSerial& serial() { return _serial; }
    const BluetoothSerial& serial() const { return _serial; }

    bool connected() const { return _connected; }
    bool hasWatchMac() const { return _watchMacSet; }
    const char* watchMacStr() const { return _watchMacStr; }

    std::function<void()> onConnect;    // SPP 连接建立（需重新握手）
    std::function<void()> onDisconnect; // SPP 断开（清理状态）
    std::function<void(const char*)> onLog;

    // ---- 扫描（Web 配对页） ----
    void startScan(uint32_t durationMs = 8000);
    void stopScan();
    bool scanning() const { return _scanning; }
    size_t scanCount() const { return _scanCount; }
    const Device& scanResult(size_t i) const { return _scanResults[i]; }

    // ---- 已配对设备 ----
    void refreshBonded();
    size_t bondedCount() const { return _bondedCount; }
    const Device& bondedDevice(size_t i) const { return _bonded[i]; }

    // ---- 连接管理（均在 BT 任务中异步执行，不阻塞调用方） ----
    void setWatchMac(const char* mac); // 切换目标 MAC 并重连
    void disconnect();                 // 断开当前连接
    void reconnectNow();               // 立即重连（用于配对后/测试）

    // ---- 静态工具 ----
    static bool parseMac(const char* s, uint8_t out[6]);
    static void macToString(const uint8_t mac[6], char out[18]);

private:
    static constexpr size_t MAX_SCAN = 32;
    static constexpr size_t MAX_BONDED = 10;

    enum class PendingAct { None, Reconnect, Disconnect };

    BluetoothSerial _serial;
    SppStream _stream;

    char _watchMacStr[18] = {0};
    uint8_t _watchMacBytes[6] = {0};
    bool _watchMacSet = false;

    Device _scanResults[MAX_SCAN];
    volatile size_t _scanCount = 0;
    portMUX_TYPE _scanMux = portMUX_INITIALIZER_UNLOCKED;
    bool _scanning = false;
    uint32_t _scanDeadlineMs = 0;

    Device _bonded[MAX_BONDED];
    size_t _bondedCount = 0;

    bool _connected = false;
    bool _started = false;
    bool _stop = false;

    TaskHandle_t _btTask = nullptr;
    volatile bool _connecting = false;
    volatile PendingAct _pendingAct = PendingAct::None;

    void _log(const char* msg);
    void _taskBody();
    static void _taskEntry(void* arg);
    bool _resolveTarget(uint8_t out[6]);
    bool _tryConnect();
    void _onScanDevice(BTAdvertisedDevice* dev);
    void _addScan(const char* name, const char* mac, int8_t rssi);
    bool _isBonded(const char* mac);
};

} // namespace xw
