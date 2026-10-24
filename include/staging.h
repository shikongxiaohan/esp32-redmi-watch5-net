/*
 * 表盘临时暂存区
 *
 * 4MB 双 OTA 布局下没有 LittleFS；利用非活动 OTA 分区作为一次性暂存：
 *   - 先擦除首 4KB，避免残留内容被 bootloader 当作应用镜像
 *   - 按 4KB 边界惰性擦除并写入
 *   - 安装完成后不保留文件，下次 OTA 或表盘上传会覆盖
 */
#pragma once

#include "watchface.h"

#include <esp_partition.h>

#include <cstdint>
#include <string>

namespace xw {

class PartitionSource : public WatchfaceSource {
public:
    bool open(const esp_partition_t* part, uint32_t base, uint32_t len) {
        _part = part;
        _base = base;
        _len = len;
        return part && len > 0;
    }
    uint32_t size() const override { return _len; }
    size_t readAt(uint32_t offset, uint8_t* buf, size_t len) override {
        if (!_part || offset >= _len) return 0;
        if (offset + len > _len) len = _len - offset;
        return esp_partition_read(_part, _base + offset, buf, len) == ESP_OK ? len : 0;
    }

private:
    const esp_partition_t* _part = nullptr;
    uint32_t _base = 0;
    uint32_t _len = 0;
};

class WatchfaceStaging {
public:
    bool begin(uint32_t userMax, std::string& err);
    size_t write(const uint8_t* data, size_t len);
    bool finish(uint32_t& outBytes);
    void abort();

    bool busy() const { return _busy; }
    uint32_t capacity() const { return _limit; }
    uint32_t bytes() const { return _pos; }
    const esp_partition_t* partition() const { return _part; }
    const std::string& error() const { return _error; }

private:
    const esp_partition_t* _part = nullptr;
    uint32_t _limit = 0;
    uint32_t _pos = 0;
    uint32_t _erasedEnd = 0;
    bool _busy = false;
    std::string _error;

    bool _eraseThrough(uint32_t end);
};

} // namespace xw
