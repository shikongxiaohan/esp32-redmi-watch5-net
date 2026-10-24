/*
 * 表盘安装包缓存元数据（纯逻辑层，可在主机端单测）
 *
 * 语义：
 *   - 上传的表盘包落到暂存分区（"watchface" 独立分区，或兼容的非活动 OTA 槽）
 *   - 安装成功后立即删除缓存包（用户选择"检测到安装成功就删掉"）
 *   - 安装失败时保留缓存包，可在网页上重装或手动删除
 *   - 缓存包超过 TTL（默认 48 小时）未被使用则自动删除
 *
 * 时间基准用 epoch 秒（time(nullptr)），跨重启有效；
 * 纯逻辑判定放在本文件，NVS 存取在 webui 侧完成。
 */
#pragma once

#include <cstdint>
#include <string>

namespace xw {

// 缓存包默认保留时长：48 小时
static const uint32_t FACE_CACHE_TTL_SEC = 48u * 3600u;

// 缓存元数据
struct FaceCacheMeta {
    bool present = false;      // 是否有缓存包
    std::string name;          // 原始文件名
    uint32_t size = 0;         // 字节数
    uint32_t uploadedAt = 0;   // 上传完成时刻（epoch 秒）

    void clear() {
        present = false;
        name.clear();
        size = 0;
        uploadedAt = 0;
    }
};

// 缓存包是否已过期（超过 TTL 未使用）
inline bool faceCacheExpired(const FaceCacheMeta& meta, uint32_t nowSec, uint32_t ttlSec) {
    if (!meta.present) return false;
    if (nowSec < meta.uploadedAt) return false;  // 时钟被回拨：暂不清理
    return (uint32_t)(nowSec - meta.uploadedAt) > ttlSec;
}

// 缓存包剩余寿命（秒）；已过期返回 0
inline uint32_t faceCacheRemaining(const FaceCacheMeta& meta, uint32_t nowSec, uint32_t ttlSec) {
    if (!meta.present) return 0;
    if (nowSec < meta.uploadedAt) return ttlSec;
    uint32_t age = (uint32_t)(nowSec - meta.uploadedAt);
    if (age >= ttlSec) return 0;
    return ttlSec - age;
}

// 剩余寿命的可读文本（"47 小时" / "12 分钟" / "已过期"）
std::string faceCacheRemainingText(uint32_t remainSec);

} // namespace xw
