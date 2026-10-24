/*
 * MD5（RFC 1321）— 纯 C++ 实现，无平台依赖
 *
 * 用途: MiWear 表盘上传请求中的 md5sum 字段（与 Gadgetbridge
 *       CheckSums.md5 -> java MessageDigest MD5 一致）。
 *
 * 说明: 这里没有使用 ESP-IDF 的 mbedtls_md5_ret，而是主机/设备共用
 * 同一份实现——保证主机单测验证的就是设备上运行的代码路径。
 * 1MB 表盘在本实现下约 0.3s（ESP32@240MHz），可接受。
 *
 * 标准向量（RFC 1319 / 附录 A.5）:
 *   MD5("")               = d41d8cd98f00b204e9800998ecf8427e
 *   MD5("abc")            = 900150983cd24fb0d6963f7d28e17f72
 *   MD5("12345678901234567890123456789012345678901234567890123456789012345678901234567890")
 *                         = 57edf4a22be3c955ac49da2e2107b67a
 */
#pragma once

#include <cstdint>
#include <cstddef>

namespace xw {

class Md5 {
public:
    Md5() { reset(); }
    void reset();
    void update(const uint8_t* data, size_t len);
    void final(uint8_t out[16]);

    // 一次性便捷接口
    static void hash(const uint8_t* data, size_t len, uint8_t out[16]);

private:
    uint32_t _state[4];
    uint64_t _bitlen;
    uint8_t  _buf[64];
    size_t   _buflen;
    void _transform(const uint8_t* chunk);
};

} // namespace xw
