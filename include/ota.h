/*
 * OTA 固件升级服务
 *
 * - 通过 Arduino Update 写入非活动 OTA 分区
 * - 写入前校验应用描述符，结束后执行 esp_image_verify
 * - 首次启动确认与自动回滚
 * - 手动回滚前校验另一分区的固件完整性
 */
#pragma once

#include <Arduino.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>

#include <string>

namespace xw {

#ifndef XW_FW_VERSION
#define XW_FW_VERSION "dev"
#endif

enum class OtaState { IDLE, RECEIVING, VERIFIED, FAILED };

class OtaService {
public:
    void begin();
    void loop(bool networkReady);

    bool start();
    bool write(const uint8_t* data, size_t len);
    bool finish();
    void abort();

    bool active() const { return _state == OtaState::RECEIVING; }
    bool verified() const { return _state == OtaState::VERIFIED; }
    OtaState state() const { return _state; }
    const char* stateName() const;
    const std::string& error() const { return _error; }
    uint32_t written() const { return _written; }

    const char* version() const { return XW_FW_VERSION; }
    const char* runningLabel() const { return _runningLabel; }
    const char* runningVersion() const { return _runningVersion; }
    const char* runningStateName() const;
    bool pendingVerify() const { return _pendingVerify; }
    bool rollbackAvailable();
    bool rollback();
    bool keepCurrent();

    // 非活动 OTA 槽（表盘临时暂存 / OTA 目标）
    const esp_partition_t* inactivePartition() const;

private:
    const esp_partition_t* _running = nullptr;
    const esp_partition_t* _target = nullptr;
    OtaState _state = OtaState::IDLE;
    std::string _error;

    uint8_t _header[0x24] = {0};
    size_t _headerLen = 0;
    bool _headerValid = false;
    uint32_t _written = 0;

    uint32_t _bootMs = 0;
    uint32_t _lastCheckMs = 0;
    bool _pendingVerify = false;
    bool _keepRequested = false;

    char _runningLabel[17] = {0};
    char _runningVersion[33] = {0};

    bool _rawWrite(const uint8_t* data, size_t len);
    bool _fail(const char* why);
    bool _otherPartition(const esp_partition_t*& out);
};

} // namespace xw
