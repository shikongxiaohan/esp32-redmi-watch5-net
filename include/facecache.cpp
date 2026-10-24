/*
 * 表盘缓存元数据 —— 纯逻辑实现
 */
#include "facecache.h"

#include <cstdio>

namespace xw {

std::string faceCacheRemainingText(uint32_t remainSec) {
    if (remainSec == 0) return "已过期";
    uint32_t min = remainSec / 60u;
    if (min < 60) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%u 分钟", min);
        return buf;
    }
    uint32_t hour = min / 60u;
    uint32_t remMin = min % 60u;
    char buf[48];
    if (remMin == 0) {
        std::snprintf(buf, sizeof(buf), "%u 小时", hour);
    } else {
        std::snprintf(buf, sizeof(buf), "%u 小时 %u 分钟", hour, remMin);
    }
    return buf;
}

} // namespace xw
