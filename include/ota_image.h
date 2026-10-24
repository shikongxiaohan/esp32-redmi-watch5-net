/*
 * OTA 镜像头校验（纯函数，可在主机端单测）
 *
 * ESP32 应用镜像布局：
 *   0x00 esp_image_header_t  （首字节 0xE9）
 *   0x20 esp_app_desc_t      （magic 0xABCD5432，小端）
 *
 * 合并镜像从 0x0 开始是 bootloader，不能写入 OTA 应用分区。
 */
#pragma once

#include <cstddef>
#include <cstdint>

namespace xw {

constexpr size_t APP_IMAGE_DESC_OFFSET = 0x20;
constexpr uint32_t APP_DESC_MAGIC = 0xABCD5432u;

inline bool isValidAppImageHeader(const uint8_t* data, size_t len) {
    if (!data || len < APP_IMAGE_DESC_OFFSET + 4) return false;
    if (data[0] != 0xE9) return false;
    const uint32_t magic = uint32_t(data[APP_IMAGE_DESC_OFFSET]) |
                           (uint32_t(data[APP_IMAGE_DESC_OFFSET + 1]) << 8) |
                           (uint32_t(data[APP_IMAGE_DESC_OFFSET + 2]) << 16) |
                           (uint32_t(data[APP_IMAGE_DESC_OFFSET + 3]) << 24);
    return magic == APP_DESC_MAGIC;
}

} // namespace xw
