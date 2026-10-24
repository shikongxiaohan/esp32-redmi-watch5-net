/*
 * 表盘临时暂存实现
 */
#include "staging.h"

#include <esp_ota_ops.h>

namespace xw {

bool WatchfaceStaging::begin(uint32_t userMax, std::string& err) {
    err.clear();
    _error.clear();
    _part = esp_ota_get_next_update_partition(nullptr);
    if (!_part) {
        err = "没有可用的临时分区";
        return false;
    }
    const esp_partition_t* running = esp_ota_get_running_partition();
    if (running && running->address == _part->address) {
        err = "临时分区与运行分区冲突";
        return false;
    }

    _limit = _part->size;
    if (userMax && userMax < _limit) _limit = userMax;
    if (_limit < 64u * 1024u) {
        err = "临时分区容量不足";
        return false;
    }

    // 先使旧应用镜像头部失效；即使随后掉电，也不会从该槽启动。
    if (esp_partition_erase_range(_part, 0, 0x1000) != ESP_OK) {
        err = "无法擦除临时分区";
        return false;
    }
    _erasedEnd = 0x1000;
    _pos = 0;
    _busy = true;
    return true;
}

bool WatchfaceStaging::_eraseThrough(uint32_t end) {
    if (end > _part->size) end = _part->size;
    uint32_t aligned = (end + 0xFFF) & ~0xFFFu;
    if (aligned > _part->size) aligned = _part->size;
    if (aligned <= _erasedEnd) return true;
    if (esp_partition_erase_range(_part, _erasedEnd, aligned - _erasedEnd) != ESP_OK) {
        _error = "临时分区擦除失败";
        return false;
    }
    _erasedEnd = aligned;
    return true;
}

size_t WatchfaceStaging::write(const uint8_t* data, size_t len) {
    if (!_busy || !data || !len) return 0;
    if (_pos + len > _limit) {
        _error = "表盘文件超过临时安装区容量";
        return 0;
    }
    if (!_eraseThrough(_pos + uint32_t(len))) return 0;
    if (esp_partition_write(_part, _pos, data, len) != ESP_OK) {
        _error = "表盘写入临时分区失败";
        return 0;
    }
    _pos += uint32_t(len);
    return len;
}

bool WatchfaceStaging::finish(uint32_t& outBytes) {
    outBytes = _pos;
    if (!_busy || _pos == 0) {
        _error = "没有收到表盘数据";
        _busy = false;
        return false;
    }
    _busy = false;
    return true;
}

void WatchfaceStaging::abort() {
    if (_part) {
        // 擦除已写入范围，避免表盘残片长期占用（OTA 分区）flash。
        uint32_t end = _pos ? ((_pos + 0xFFFu) & ~0xFFFu) : 0x1000u;
        if (end > _part->size) end = _part->size;
        esp_partition_erase_range(_part, 0, end);
    }
    _pos = 0;
    _busy = false;
}

} // namespace xw
