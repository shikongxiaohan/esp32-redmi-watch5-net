/*
 * BtLink 实现，见 btlink.h 的接口与实现依据说明。
 */
#include "btlink.h"

#include <esp_spp_api.h>
#if defined(ESP32) || defined(ESP_PLATFORM)
#include <esp_gap_bt_api.h>
// 用于按 ESP-IDF 主版本适配 GAP bonded 列表接口的原型差异
#include <esp_idf_version.h>
#endif
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <cstdio>
#include <cstring>

namespace xw {

// ==================== SppStream ====================

int BtLink::SppStream::available() {
    size_t head = _rxHead, tail = _rxTail;
    return int(head - tail);
}

int BtLink::SppStream::read() {
    if (_hasPeek) {
        _hasPeek = false;
        return _peekByte;
    }
    portENTER_CRITICAL(&_rxMux);
    size_t head = _rxHead, tail = _rxTail;
    int r = -1;
    if (head != tail) {
        r = _rx[tail & RX_MASK];
        _rxTail = tail + 1;
    }
    portEXIT_CRITICAL(&_rxMux);
    return r;
}

int BtLink::SppStream::peek() {
    if (_hasPeek) return _peekByte;
    int c = read();
    if (c < 0) return -1;
    _peekByte = uint8_t(c);
    _hasPeek = true;
    return _peekByte;
}

size_t BtLink::SppStream::write(const uint8_t* buf, size_t n) {
    if (!_serial) return 0;
    size_t w = _serial->write(buf, n);
    txBytes += uint32_t(w); // 性能监控用累计发送字节
    return w;
}

size_t BtLink::SppStream::write(uint8_t c) {
    uint8_t b = c;
    return write(&b, 1);
}

size_t BtLink::SppStream::readBulk(uint8_t* out, size_t cap) {
    if (_hasPeek && cap > 0) {
        out[0] = _peekByte;
        _hasPeek = false;
        return 1 + readBulk(out + 1, cap - 1);
    }
    portENTER_CRITICAL(&_rxMux);
    size_t head = _rxHead, tail = _rxTail;
    size_t avail = head - tail;
    size_t n = avail < cap ? avail : cap;
    for (size_t i = 0; i < n; i++) {
        out[i] = _rx[(tail + i) & RX_MASK];
    }
    _rxTail = tail + n;
    portEXIT_CRITICAL(&_rxMux);
    return n;
}

void BtLink::SppStream::pushRx(const uint8_t* data, size_t len) {
    portENTER_CRITICAL(&_rxMux);
    size_t head = _rxHead, tail = _rxTail;
    size_t free_ = RX_CAP - (head - tail);
    size_t n = len < free_ ? len : free_;
    for (size_t i = 0; i < n; i++) {
        _rx[(head + i) & RX_MASK] = data[i];
    }
    _rxHead = head + n;
    portEXIT_CRITICAL(&_rxMux);
    rxBytes += uint32_t(n); // 性能监控用累计接收字节
    if (n < len) dropped += uint32_t(len - n);
}

// ==================== 静态工具 ====================

bool BtLink::parseMac(const char* s, uint8_t out[6]) {
    if (!s) return false;
    int nib[12];
    int n = 0;
    const char* p = s;
    for (; *p && n < 12; p++) {
        char c = *p;
        if (c == ':' || c == '-' || c == ' ' || c == '\t') continue;
        int d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else return false;
        nib[n++] = d;
    }
    if (n != 12) return false;
    // 12 个 hex 之后的剩余字符必须全是分隔符（拒绝 "AABBCCDDEEFFG"）
    for (; *p; p++) {
        if (*p != ':' && *p != '-' && *p != ' ' && *p != '\t') return false;
    }
    for (int i = 0; i < 6; i++) {
        out[i] = uint8_t((nib[i * 2] << 4) | nib[i * 2 + 1]);
    }
    return true;
}

void BtLink::macToString(const uint8_t mac[6], char out[18]) {
    std::snprintf(out, 18, "%02X:%02X:%02X:%02X:%02X:%02X",
                  mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

// ==================== 生命周期 ====================

void BtLink::begin(const char* watchMac) {
    _log("bt: init classic BT (master)");
    _serial.begin("RedmiWatch5Net", true);
    _serial.enableSSP(); // Just Works（手表端确认配对）
    _stream.attach(_serial);

    // RX：DATA_IND（BT 栈上下文）-> 自建环形缓冲
    _serial.onData([this](const uint8_t* data, size_t len) {
        _stream.pushRx(data, len);
    });

    // 配对请求自动接受
    _serial.onConfirmRequest([this](uint32_t num) {
        (void)num;
        _log("bt: ssp confirm request -> accept");
        _serial.confirmReply(true);
    });

    if (watchMac && watchMac[0]) {
        setWatchMac(watchMac);
    }

    _started = true;
    _stop = false;
    xTaskCreatePinnedToCore(_taskEntry, "btlink", 8192, this, 3, &_btTask, 0);
}

void BtLink::end() {
    _stop = true;
    vTaskDelay(pdMS_TO_TICKS(300));
    _serial.end();
    _started = false;
}

void BtLink::_taskEntry(void* arg) {
    static_cast<BtLink*>(arg)->_taskBody();
}

void BtLink::_taskBody() {
    for (;;) {
        if (_stop) break;

        // 待执行动作（切换 MAC / 断开 / 重连）
        PendingAct act = _pendingAct;
        if (act != PendingAct::None) {
            _pendingAct = PendingAct::None;
            if (_serial.connected()) {
                _serial.disconnect();
                vTaskDelay(pdMS_TO_TICKS(300));
            }
            if (act == PendingAct::Disconnect) continue;
        }

        if (_serial.connected()) {
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        uint8_t addr[6];
        if (!_resolveTarget(addr)) {
            // 没有目标：等用户在 Web 页配置或配对
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }

        _connecting = true;
        bool ok = _tryConnect();
        _connecting = false;
        if (!ok) {
            vTaskDelay(pdMS_TO_TICKS(5000)); // 退避
        }
    }
    _btTask = nullptr;
    vTaskDelete(nullptr);
}

bool BtLink::_resolveTarget(uint8_t out[6]) {
    if (_watchMacSet && parseMac(_watchMacStr, out)) return true;
    // 从已配对设备中挑选：优先名字含 Watch/Redmi/Xiaomi 的
    bool noBonded = false;
    portENTER_CRITICAL(&_bondedMux);
    noBonded = (_bondedCount == 0);
    portEXIT_CRITICAL(&_bondedMux);
    if (noBonded) refreshBonded();
    // 取快照后再判断，避免与 refreshBonded 的写入交错
    Device snap[MAX_BONDED];
    size_t n = copyBonded(snap, MAX_BONDED);
    int fallback = -1;
    for (size_t i = 0; i < n; i++) {
        const char* nm = snap[i].name;
        if (std::strstr(nm, "Watch") || std::strstr(nm, "watch") ||
            std::strstr(nm, "Redmi") || std::strstr(nm, "Xiaomi") ||
            std::strstr(nm, "手环") || std::strstr(nm, "手表")) {
            fallback = int(i);
            break;
        }
        if (fallback < 0) fallback = int(i);
    }
    if (fallback < 0) return false;
    return parseMac(snap[fallback].mac, out);
}

bool BtLink::_tryConnect() {
    uint8_t addr[6];
    if (!_resolveTarget(addr)) return false;
    char m[18];
    macToString(addr, m);
    char msg[48];
    std::snprintf(msg, sizeof(msg), "bt: connecting %s ...", m);
    _log(msg);
    // channel=0：先 SDP 发现 SCN，最多阻塞 READY_TIMEOUT=10s
    return _serial.connect(addr, 0);
}

// ==================== 主循环 ====================

void BtLink::loop() {
    if (!_started) return;

    // 连接状态沿检测
    bool now = _serial.connected();
    if (now != _connected) {
        _connected = now;
        if (now) {
            _log("bt: connected");
            if (onConnect) onConnect();
        } else {
            _log("bt: disconnected");
            if (onDisconnect) onDisconnect();
        }
    }

    // 扫描超时
    if (_scanning && int32_t(millis() - _scanDeadlineMs) >= 0) {
        _serial.discoverAsyncStop();
        _scanning = false;
        _log("bt: scan done");
    }

    // TX 冲刷（队列空时立即返回）
    _serial.flush();
}

// ==================== 扫描 ====================

void BtLink::startScan(uint32_t durationMs) {
    if (_scanning || !_started) return;
    portENTER_CRITICAL(&_scanMux);
    _scanCount = 0;
    portEXIT_CRITICAL(&_scanMux);
    _serial.discoverClear();
    _scanDeadlineMs = millis() + durationMs;
    bool ok = _serial.discoverAsync(
        [this](BTAdvertisedDevice* dev) { _onScanDevice(dev); }, durationMs);
    _scanning = ok;
    _log(ok ? "bt: scan started" : "bt: scan start failed");
}

void BtLink::stopScan() {
    if (!_scanning) return;
    _serial.discoverAsyncStop();
    _scanning = false;
}

void BtLink::_onScanDevice(BTAdvertisedDevice* dev) {
    if (!dev) return;
    char name[40] = {0};
    if (dev->haveName()) {
        std::string n = dev->getName();
        std::strncpy(name, n.c_str(), sizeof(name) - 1);
        name[sizeof(name) - 1] = 0;
    }
    char mac[18] = {0};
    BTAddress addr = dev->getAddress();
    const uint8_t* b = (*addr.getNative());
    std::snprintf(mac, sizeof(mac), "%02X:%02X:%02X:%02X:%02X:%02X",
                  b[0], b[1], b[2], b[3], b[4], b[5]);
    int8_t rssi = dev->haveRSSI() ? dev->getRSSI() : 0;
    _addScan(name, mac, rssi);
}

void BtLink::_addScan(const char* name, const char* mac, int8_t rssi) {
    portENTER_CRITICAL(&_scanMux);
    for (size_t i = 0; i < _scanCount; i++) {
        if (std::strcmp(_scanResults[i].mac, mac) == 0) {
            // 刷新 RSSI（同名设备多次上报）
            _scanResults[i].rssi = rssi;
            portEXIT_CRITICAL(&_scanMux);
            return;
        }
    }
    if (_scanCount < MAX_SCAN) {
        Device& d = _scanResults[_scanCount];
        std::strncpy(d.name, name, sizeof(d.name) - 1);
        d.name[sizeof(d.name) - 1] = 0;
        std::strncpy(d.mac, mac, sizeof(d.mac) - 1);
        d.mac[sizeof(d.mac) - 1] = 0;
        d.rssi = rssi;
        d.bonded = _isBonded(mac);
        _scanCount++;
    }
    portEXIT_CRITICAL(&_scanMux);
}

// ==================== bonded ====================

void BtLink::refreshBonded() {
    esp_bd_addr_t list[MAX_BONDED];
    int got = 0;
#if defined(ESP32) || defined(ESP_PLATFORM)
    // arduino-esp32 3.20017 的 BluetoothSerial 没有 bonded 枚举 API，
    // 直接用 ESP-IDF GAP classic 接口（3.1.x+ 才有库封装）
    int num = esp_bt_gap_get_bond_device_num();
    if (num <= 0) {
        portENTER_CRITICAL(&_bondedMux);
        _bondedCount = 0;
        portEXIT_CRITICAL(&_bondedMux);
        return;
    }
    if (num > int(MAX_BONDED)) num = int(MAX_BONDED);
    // ESP-IDF 4.x 的原型是 esp_bt_gap_get_bond_device_list(int*, esp_bd_addr_t*)，
    // IDF 5.x 改成了 uint16_t*。按 IDF 主版本号适配，避免升级工具链后编译失败。
#if defined(ESP_IDF_VERSION_MAJOR) && ESP_IDF_VERSION_MAJOR >= 5
    uint16_t cap = (uint16_t)num;
    if (esp_bt_gap_get_bond_device_list(&cap, list) != ESP_OK || cap == 0) {
        portENTER_CRITICAL(&_bondedMux);
        _bondedCount = 0;
        portEXIT_CRITICAL(&_bondedMux);
        return;
    }
    got = int(cap);
#else
    int cap = num;
    if (esp_bt_gap_get_bond_device_list(&cap, list) != ESP_OK || cap <= 0) {
        portENTER_CRITICAL(&_bondedMux);
        _bondedCount = 0;
        portEXIT_CRITICAL(&_bondedMux);
        return;
    }
    got = cap;
#endif
#else
    int num = _serial.getNumberOfBondedDevices();
    if (num <= 0) {
        portENTER_CRITICAL(&_bondedMux);
        _bondedCount = 0;
        portEXIT_CRITICAL(&_bondedMux);
        return;
    }
    if (num > int(MAX_BONDED)) num = int(MAX_BONDED);
    got = _serial.getBondedDevices(uint(num), list);
    if (got < 0) got = 0;
#endif
    if (got > int(MAX_BONDED)) got = int(MAX_BONDED);

    // 先在局部缓冲里填好，再一次性加锁写入：避免读者看到
    // "_bondedCount 已更新但 _bonded[i].mac 还没写完" 的中间态。
    Device tmp[MAX_BONDED];
    for (int i = 0; i < got; i++) {
        macToString(list[i], tmp[i].mac);
        tmp[i].name[0] = 0; // bond 列表只有地址；名字经扫描获取
        tmp[i].rssi = 0;
        tmp[i].bonded = true;
    }
    portENTER_CRITICAL(&_bondedMux);
    _bondedCount = size_t(got);
    for (size_t i = 0; i < _bondedCount; i++) _bonded[i] = tmp[i];
    portEXIT_CRITICAL(&_bondedMux);
}

size_t BtLink::copyBonded(Device* out, size_t max) const {
    if (!out || max == 0) return 0;
    size_t n = 0;
    portENTER_CRITICAL(&_bondedMux);
    n = _bondedCount;
    if (n > max) n = max;
    for (size_t i = 0; i < n; i++) out[i] = _bonded[i];
    portEXIT_CRITICAL(&_bondedMux);
    return n;
}

bool BtLink::_isBonded(const char* mac) {
    if (!mac) return false;
    bool found = false;
    portENTER_CRITICAL(&_bondedMux);
    for (size_t i = 0; i < _bondedCount; i++) {
        if (std::strcmp(_bonded[i].mac, mac) == 0) {
            found = true;
            break;
        }
    }
    portEXIT_CRITICAL(&_bondedMux);
    return found;
}

// ==================== 连接管理 ====================

void BtLink::setWatchMac(const char* mac) {
    if (!mac || !mac[0]) {
        _watchMacSet = false;
        _watchMacStr[0] = 0;
        _pendingAct = PendingAct::Reconnect;
        return;
    }
    std::strncpy(_watchMacStr, mac, sizeof(_watchMacStr) - 1);
    _watchMacStr[sizeof(_watchMacStr) - 1] = 0;
    if (parseMac(_watchMacStr, _watchMacBytes)) {
        _watchMacSet = true;
        macToString(_watchMacBytes, _watchMacStr); // 规范化为 "XX:XX:XX:XX:XX:XX"
        char msg[64];
        std::snprintf(msg, sizeof(msg), "bt: target mac %s", _watchMacStr);
        _log(msg);
    } else {
        _watchMacSet = false;
        _log("bt: invalid MAC format");
    }
    _pendingAct = PendingAct::Reconnect;
}

void BtLink::disconnect() {
    _pendingAct = PendingAct::Disconnect;
}

void BtLink::reconnectNow() {
    _pendingAct = PendingAct::Reconnect;
}

// ==================== 日志 ====================

void BtLink::_log(const char* msg) {
    if (onLog) {
        onLog(msg);
        return;
    }
    Serial.print("[xw] ");
    Serial.println(msg);
}

} // namespace xw
