/*
 * 表盘临时暂存区
 *
 * 分区选择（按优先级）：
 *   1. 名为 "watchface" 的独立数据分区（16MB 布局，subtype 0x40）
 *   2. 非活动 OTA 分区（4MB 布局的兼容路径）
 *
 * 语义：
 *   - 上传时写入暂存区；安装成功后由 WatchfaceCache 删除缓存
 *   - 安装失败时保留缓存，可重装或手动删除
 *   - 超过 TTL（48 小时）未使用自动删除
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
    // 开始一次上传：清空暂存区并准备写入
    bool begin(uint32_t userMax, std::string& err);
    size_t write(const uint8_t* data, size_t len);
    bool finish(uint32_t& outBytes);
    void abort();

    // 打开已缓存的文件（重装用）：len 为缓存字节数
    bool openCached(uint32_t len);

    // 擦除整个暂存区（删除缓存包）
    bool eraseAll(std::string& err);

    bool busy() const { return _busy; }
    uint32_t capacity() const { return _limit; }
    uint32_t bytes() const { return _pos; }
    const esp_partition_t* partition() const { return _part; }
    const std::string& error() const { return _error; }

    // 是否使用独立的 "watchface" 分区（true=删缓存不影响 OTA 回滚镜像）
    bool dedicated() const { return _dedicated; }

    // 选择目标分区（独立表盘分区优先，否则非活动 OTA 槽）
    static const esp_partition_t* pickPartition(bool& dedicated);

private:
    const esp_partition_t* _part = nullptr;
    uint32_t _limit = 0;
    uint32_t _pos = 0;
    uint32_t _erasedEnd = 0;
    bool _busy = false;
    bool _dedicated = false;
    std::string _error;

    bool _eraseThrough(uint32_t end);
};

} // namespace xw
