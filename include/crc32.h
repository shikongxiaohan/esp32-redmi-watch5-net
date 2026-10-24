/*
 * CRC-32/ISO-HDLC（即 "CRC-32 IEEE" / zlib crc32）
 * 参数: poly=0x04C11DB7 (反射实现 0xEDB88320), init=0xFFFFFFFF,
 *       refin=true, refout=true, xorout=0xFFFFFFFF
 *
 * 用途: MiWear 表盘上传 payload 的尾部校验（与 Gadgetbridge
 *       CheckSums.getCRC32 -> java.util.zip.CRC32 完全一致）
 * 标准检查值: CRC32("123456789") = 0xCBF43926
 *
 * 流式接口用于大文件（1MB+ 表盘不整读入内存）：
 *   Crc32 c; c.update(prefix, 23); c.update(fileChunk, n); c.final();
 */
#pragma once

#include <cstdint>
#include <cstddef>

namespace xw {

class Crc32 {
public:
    Crc32() { reset(); }
    void reset();
    void update(const uint8_t* data, size_t len);
    // 返回最终 CRC（内部状态不变，可继续 update）
    uint32_t final() const { return _crc ^ 0xFFFFFFFFu; }

    // 一次性便捷接口
    static uint32_t hash(const uint8_t* data, size_t len);

private:
    uint32_t _crc = 0xFFFFFFFFu;
};

} // namespace xw
