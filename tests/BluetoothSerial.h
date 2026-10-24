/*
 * 主机端 BluetoothSerial.h 桩。
 *
 * 签名与 arduino-esp32 master 分支的 BluetoothSerial.h 保持一致
 * （这样主机编译即可发现签名/类型不匹配），行为用测试钩子模拟：
 *   - fakeDeliver()     模拟 SPP DATA_IND 回调
 *   - fakeScanDevice()  模拟 GAP 扫描结果上报
 *   - fakeConnected     控制 connected() 返回值
 *   - fakeConnectResult 控制 connect() 返回值
 *   - txBuffer          记录 write() 的数据
 *
 * esp_spp_sec_t / esp_spp_role_t 定义在 esp_spp_api.h 桩中。
 */
#pragma once

#include <Arduino.h>
#include <esp_spp_api.h>

#include <functional>
#include <string>
#include <vector>
#include <cstring>

// ---- 与真实头文件一致的类型 ----
typedef std::function<void(const uint8_t* buffer, size_t size)> BluetoothSerialDataCb;
typedef std::function<void(uint32_t num_val)> ConfirmRequestCb;
typedef std::function<void(bool success)> AuthCompleteCb;
typedef std::function<void(class BTAdvertisedDevice* pAdvertisedDevice)> BTAdvertisedDeviceCb;

class BTAddress {
public:
    BTAddress() { std::memset(m_address, 0, 6); }
    BTAddress(const esp_bd_addr_t addr) { std::memcpy(m_address, addr, 6); }
    bool equals(const BTAddress& o) const { return std::memcmp(m_address, o.m_address, 6) == 0; }
    esp_bd_addr_t* getNative() const { return const_cast<esp_bd_addr_t*>(&m_address); }
    String toString(bool capital = false) const {
        char s[18];
        if (capital) {
            std::snprintf(s, sizeof(s), "%02X:%02X:%02X:%02X:%02X:%02X",
                          m_address[0], m_address[1], m_address[2], m_address[3], m_address[4], m_address[5]);
        } else {
            std::snprintf(s, sizeof(s), "%02x:%02x:%02x:%02x:%02x:%02x",
                          m_address[0], m_address[1], m_address[2], m_address[3], m_address[4], m_address[5]);
        }
        return String(s);
    }

private:
    esp_bd_addr_t m_address;
};

class BTAdvertisedDevice {
public:
    virtual ~BTAdvertisedDevice() = default;
    virtual BTAddress getAddress() = 0;
    virtual uint32_t getCOD() const = 0;
    virtual std::string getName() const = 0;
    virtual int8_t getRSSI() const = 0;
    virtual bool haveCOD() const = 0;
    virtual bool haveName() const = 0;
    virtual bool haveRSSI() const = 0;
    virtual std::string toString() = 0;
};

// 测试用具体设备
class FakeDevice : public BTAdvertisedDevice {
public:
    FakeDevice(const char* macStr, const char* name, int8_t rssi, bool hasName)
        : _addr(parse(macStr)), _name(name ? name : ""), _rssi(rssi), _hasName(hasName) {}
    BTAddress getAddress() override { return _addr; }
    uint32_t getCOD() const override { return 0; }
    std::string getName() const override { return _name; }
    int8_t getRSSI() const override { return _rssi; }
    bool haveCOD() const override { return false; }
    bool haveName() const override { return _hasName; }
    bool haveRSSI() const override { return true; }
    std::string toString() override { return _addr.toString().c_str(); }

private:
    static BTAddress parse(const char* s) {
        uint8_t b[6] = {0};
        if (s) {
            unsigned v[6] = {0};
            if (std::sscanf(s, "%x:%x:%x:%x:%x:%x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) == 6) {
                for (int i = 0; i < 6; i++) b[i] = uint8_t(v[i]);
            }
        }
        return BTAddress(b);
    }
    BTAddress _addr;
    std::string _name;
    int8_t _rssi;
    bool _hasName;
};

// ---- BluetoothSerial 桩 ----
class BluetoothSerial : public Stream {
public:
    bool begin(String localName = String(), bool isMaster = false, bool disableBLE = false) {
        (void)localName; (void)disableBLE;
        _begun = true;
        _isMaster = isMaster;
        return true;
    }
    void end() { _begun = false; }
    void memrelease() {}
    void setTimeout(int) {}

    void onData(BluetoothSerialDataCb cb) { _onData = cb; }
    void onConfirmRequest(ConfirmRequestCb cb) { _onConfirm = cb; }
    void confirmReply(bool confirm) {
        _confirmReply = confirm;
        _confirmCount++;
    }
    void onAuthComplete(AuthCompleteCb cb) { _onAuth = cb; }
    void respondPasskey(uint32_t) {}
    void enableSSP() { _ssp = true; }
    void enableSSP(bool, bool) { _ssp = true; }
    void disableSSP() { _ssp = false; }
    bool setPin(const char*, uint8_t) { return true; }

    bool connect(uint8_t remoteAddress[], int channel = 0,
                 esp_spp_sec_t sec_mask = (ESP_SPP_SEC_ENCRYPT | ESP_SPP_SEC_AUTHENTICATE),
                 esp_spp_role_t role = ESP_SPP_ROLE_MASTER) {
        (void)channel; (void)sec_mask; (void)role;
        _connectCount++;
        if (remoteAddress) std::memcpy(_lastConnectAddr, remoteAddress, 6);
        if (!fakeConnectResult) return false;
        fakeConnected = true;
        return true;
    }
    bool connected(int timeout = 0) {
        (void)timeout;
        return fakeConnected;
    }
    bool isClosed() { return !fakeConnected; }
    bool isReady(bool = false, int = 0) { return _begun; }
    bool disconnect() {
        if (!fakeConnected) return false;
        fakeConnected = false;
        _disconnectCount++;
        return true;
    }
    bool unpairDevice(uint8_t*) { return true; }

    // Stream IO
    int available() override { return 0; }
    int peek() override { return -1; }
    int read() override { return -1; }
    size_t write(uint8_t c) override {
        uint8_t b = c;
        return write(&b, 1);
    }
    size_t write(const uint8_t* buffer, size_t size) override {
        if (!fakeConnected) return 0;
        txBuffer.insert(txBuffer.end(), buffer, buffer + size);
        return size;
    }
    void flush() { _flushCount++; }

    // 扫描
    bool discoverAsync(BTAdvertisedDeviceCb cb, int timeoutMs = 0x30 * 1280) {
        (void)timeoutMs;
        _scanCb = cb;
        _discoverAsyncCount++;
        return fakeDiscoverOk;
    }
    void discoverAsyncStop() { _scanCb = nullptr; }
    void discoverClear() { _discoverClearCount++; }
    void discoverCancel() {}

    int getNumberOfBondedDevices() { return _fakeBondedN; }
    int getBondedDevices(uint dev_num, esp_bd_addr_t* dev_list) {
        if (!dev_list || dev_num == 0) return 0;
        int n = _fakeBondedN < int(dev_num) ? _fakeBondedN : int(dev_num);
        for (int i = 0; i < n; i++) std::memcpy(dev_list[i], _fakeBonded[i], 6);
        return n;
    }
    bool deleteBondedDevice(uint8_t*) { return true; }
    void deleteAllBondedDevices() { _fakeBondedN = 0; }

    String getBtAddressString() { return String("00:00:00:00:00:00"); }

    // ---------------- 测试钩子 ----------------
    BluetoothSerialDataCb _onData;
    ConfirmRequestCb _onConfirm;
    AuthCompleteCb _onAuth;
    BTAdvertisedDeviceCb _scanCb;

    bool fakeConnected = false;
    bool fakeConnectResult = true;
    bool fakeDiscoverOk = true;
    bool _begun = false;
    bool _isMaster = false;
    bool _ssp = false;
    int _confirmCount = 0;
    bool _confirmReply = false;
    int _connectCount = 0;
    int _disconnectCount = 0;
    int _flushCount = 0;
    int _discoverAsyncCount = 0;
    int _discoverClearCount = 0;
    uint8_t _lastConnectAddr[6] = {0};
    std::vector<uint8_t> txBuffer;

    int _fakeBondedN = 0;
    uint8_t _fakeBonded[10][6] = {{0}};

    // 模拟 SPP 收到数据（触发 onData 回调）
    void fakeDeliver(const uint8_t* data, size_t len) {
        if (_onData) _onData(data, len);
    }
    // 模拟 GAP 上报一个扫描到的设备（触发 discoverAsync 回调）
    void fakeScanDevice(BTAdvertisedDevice* dev) {
        if (_scanCb) _scanCb(dev);
    }
};
