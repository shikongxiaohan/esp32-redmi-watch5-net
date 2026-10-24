/*
 * OTA 固件升级实现
 */
#include "ota.h"
#include "ota_image.h"

#include <Update.h>
#include <esp_app_format.h>
#include <esp_image_format.h>

#include <cstring>

namespace xw {

// Arduino 核心默认在启动时立即确认新镜像；返回 true 后由本服务在健康检查
// 通过时再确认，未通过则允许 bootloader 回滚。
extern "C" bool verifyRollbackLater() {
    return true;
}

static const char* otaImgStateName(esp_ota_img_states_t s) {
    switch (s) {
        case ESP_OTA_IMG_NEW: return "新镜像";
        case ESP_OTA_IMG_PENDING_VERIFY: return "待确认";
        case ESP_OTA_IMG_VALID: return "有效";
        case ESP_OTA_IMG_INVALID: return "无效";
        case ESP_OTA_IMG_ABORTED: return "已中止";
        case ESP_OTA_IMG_UNDEFINED: return "未定义";
    }
    return "未知";
}

void OtaService::begin() {
    _bootMs = millis();
    _running = esp_ota_get_running_partition();
    if (_running) {
        std::strncpy(_runningLabel, _running->label, sizeof(_runningLabel) - 1);
        esp_app_desc_t desc{};
        if (esp_ota_get_partition_description(_running, &desc) == ESP_OK) {
            std::strncpy(_runningVersion, desc.version, sizeof(_runningVersion) - 1);
        }
    }
    if (_runningVersion[0] == 0) {
        std::strncpy(_runningVersion, XW_FW_VERSION, sizeof(_runningVersion) - 1);
    }
    esp_ota_img_states_t st;
    if (_running && esp_ota_get_state_partition(_running, &st) == ESP_OK) {
        _pendingVerify = (st == ESP_OTA_IMG_PENDING_VERIFY);
    }
    Serial.printf("[xw][ota] running=%s version=%s state=%s pending=%d slots=%u\n",
                  _runningLabel, _runningVersion, runningStateName(), _pendingVerify ? 1 : 0,
                  (unsigned)esp_ota_get_app_partition_count());
}

void OtaService::loop(bool networkReady) {
    if (!_pendingVerify) return;
    const uint32_t now = millis();
    if (now - _lastCheckMs < 1000) return;
    _lastCheckMs = now;
    const uint32_t elapsed = now - _bootMs;

    if (_keepRequested || (elapsed >= 60000 && networkReady)) {
        if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK) {
            _pendingVerify = false;
            Serial.println("[xw][ota] new firmware confirmed");
        }
        return;
    }

    if (elapsed >= 180000) {
        if (rollbackAvailable()) {
            Serial.println("[xw][ota] health check failed, rolling back");
            esp_err_t err = esp_ota_mark_app_invalid_rollback_and_reboot();
            if (err != ESP_OK) {
                Serial.printf("[xw][ota] rollback failed (%d), keep current\n", (int)err);
                if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK) _pendingVerify = false;
            }
        } else {
            // 没有可回滚镜像时必须确认当前镜像，避免反复启动失败。
            if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK) {
                _pendingVerify = false;
                Serial.println("[xw][ota] no rollback target, keep current firmware");
            }
        }
    }
}

bool OtaService::start() {
    if (_state == OtaState::RECEIVING) return false;
    if (Update.isRunning()) Update.abort();

    _error.clear();
    _headerLen = 0;
    _headerValid = false;
    _written = 0;
    _target = esp_ota_get_next_update_partition(nullptr);
    if (!_target || (_running && _target->address == _running->address)) {
        _error = "没有可用的 OTA 分区";
        _state = OtaState::FAILED;
        return false;
    }
    if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH)) {
        _error = Update.errorString();
        if (_error.empty()) _error = "无法开始 OTA 写入";
        _state = OtaState::FAILED;
        return false;
    }
    _state = OtaState::RECEIVING;
    Serial.printf("[xw][ota] target=%s size=%u\n", _target->label, (unsigned)_target->size);
    return true;
}

bool OtaService::_rawWrite(const uint8_t* data, size_t len) {
    if (!data || !len) return true;
    if (_target && _written + len > _target->size) {
        return _fail("固件超过 OTA 分区容量");
    }
    size_t n = Update.write(const_cast<uint8_t*>(data), len);
    if (n != len) {
        std::string msg = Update.errorString();
        if (msg.empty()) msg = "固件写入失败";
        return _fail(msg.c_str());
    }
    _written += uint32_t(len);
    return true;
}

bool OtaService::write(const uint8_t* data, size_t len) {
    if (_state != OtaState::RECEIVING) return false;
    if (!data || !len) return true;

    size_t off = 0;
    if (_headerLen < sizeof(_header)) {
        size_t take = sizeof(_header) - _headerLen;
        if (take > len) take = len;
        std::memcpy(_header + _headerLen, data, take);
        _headerLen += take;
        off += take;
        if (_headerLen < sizeof(_header)) return true;
        if (!isValidAppImageHeader(_header, sizeof(_header))) {
            return _fail("不是有效的 ESP32 应用固件（请上传 firmware.bin，不要上传合并镜像）");
        }
        _headerValid = true;
        if (!_rawWrite(_header, sizeof(_header))) return false;
    }
    if (off < len && !_rawWrite(data + off, len - off)) return false;
    return true;
}

bool OtaService::finish() {
    if (_state != OtaState::RECEIVING) return false;
    if (!_headerValid || _headerLen < sizeof(_header)) {
        return _fail("固件头部不完整");
    }
    if (_written < 64u * 1024u) {
        return _fail("固件过小，拒绝写入");
    }

    const uint32_t written = _written;
    if (!Update.end(true)) {
        _error = Update.errorString();
        if (_error.empty()) _error = "固件校验失败";
        _state = OtaState::FAILED;
        return false;
    }

    esp_partition_pos_t pos{};
    pos.offset = _target ? _target->address : 0;
    pos.size = _target ? _target->size : 0;
    esp_image_metadata_t meta{};
    esp_err_t err = esp_image_verify(ESP_IMAGE_VERIFY_SILENT, &pos, &meta);
    if (err != ESP_OK || meta.image_len == 0 || meta.image_len > written) {
        // Update 已切换启动分区；校验失败时恢复原运行分区并擦除目标头部。
        if (_running) esp_ota_set_boot_partition(_running);
        if (_target) esp_partition_erase_range(_target, 0, 0x1000);
        _state = OtaState::FAILED;
        _error = "固件镜像校验失败，已保留原固件";
        return false;
    }

    _state = OtaState::VERIFIED;
    Serial.printf("[xw][ota] verified %u bytes\n", (unsigned)written);
    return true;
}

void OtaService::abort() {
    if (Update.isRunning()) Update.abort();
    _state = OtaState::IDLE;
    _headerLen = 0;
    _headerValid = false;
    _written = 0;
}

bool OtaService::_fail(const char* why) {
    _error = why ? why : "OTA 失败";
    if (Update.isRunning()) Update.abort();
    _state = OtaState::FAILED;
    return false;
}

const char* OtaService::stateName() const {
    switch (_state) {
        case OtaState::IDLE: return "空闲";
        case OtaState::RECEIVING: return "正在接收";
        case OtaState::VERIFIED: return "校验通过";
        case OtaState::FAILED: return "失败";
    }
    return "?";
}

const char* OtaService::runningStateName() const {
    if (!_running) return "未知";
    esp_ota_img_states_t st;
    if (esp_ota_get_state_partition(_running, &st) != ESP_OK) return "未知";
    return otaImgStateName(st);
}

const esp_partition_t* OtaService::inactivePartition() const {
    if (_state == OtaState::RECEIVING && _target) return _target;
    return esp_ota_get_next_update_partition(nullptr);
}

bool OtaService::_otherPartition(const esp_partition_t*& out) {
    out = nullptr;
    if (_running) {
        const esp_partition_t* a = esp_partition_find_first(
            ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, nullptr);
        const esp_partition_t* b = esp_partition_find_first(
            ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_1, nullptr);
        if (a && b) {
            if (a->address == _running->address) out = b;
            else if (b->address == _running->address) out = a;
        }
    }
    if (!out) out = esp_ota_get_next_update_partition(nullptr);
    return out && (!_running || out->address != _running->address);
}

bool OtaService::rollbackAvailable() {
    if (_state == OtaState::RECEIVING) return false;
    const esp_partition_t* other = nullptr;
    if (!_otherPartition(other)) return false;

    esp_ota_img_states_t st;
    if (esp_ota_get_state_partition(other, &st) != ESP_OK) return false;
    if (st != ESP_OTA_IMG_VALID && st != ESP_OTA_IMG_NEW) return false;

    uint8_t hdr[0x24] = {0};
    if (esp_partition_read(other, 0, hdr, sizeof(hdr)) != ESP_OK) return false;
    return isValidAppImageHeader(hdr, sizeof(hdr));
}

bool OtaService::rollback() {
    if (_state == OtaState::RECEIVING) return false;
    const esp_partition_t* other = nullptr;
    if (!_otherPartition(other)) {
        _error = "没有可回滚的分区";
        return false;
    }

    esp_partition_pos_t pos{};
    pos.offset = other->address;
    pos.size = other->size;
    esp_image_metadata_t meta{};
    if (esp_image_verify(ESP_IMAGE_VERIFY_SILENT, &pos, &meta) != ESP_OK ||
        meta.image_len == 0) {
        _error = "另一分区没有完整固件（可能已被表盘临时文件覆盖）";
        return false;
    }
    if (esp_ota_set_boot_partition(other) != ESP_OK) {
        _error = "设置回滚分区失败";
        return false;
    }
    _error.clear();
    return true;
}

bool OtaService::keepCurrent() {
    if (!_pendingVerify) return true;
    if (esp_ota_mark_app_valid_cancel_rollback() != ESP_OK) {
        _error = "确认当前固件失败";
        return false;
    }
    _pendingVerify = false;
    _keepRequested = true;
    return true;
}

} // namespace xw
