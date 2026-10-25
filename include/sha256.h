/*
 * ESP32 红米 Watch 5 网络网关
 * SHA-256 / HMAC-SHA256 实现（纯 C++，无平台依赖，可主机单测）
 *
 * 依据小米 MiWear 协议（见 DOCUMENTATION.md 第 9 节）：
 *   认证 KDF 使用 HMAC-SHA256，标签 "miwear-auth"
 */
#pragma once

#include <cstdint>
#include <cstddef>

namespace xw {

class Sha256 {
public:
    Sha256();
    void reset();
    void update(const uint8_t* data, size_t len);
    void final(uint8_t out[32]);

    // 一次性便捷接口
    static void hash(const uint8_t* data, size_t len, uint8_t out[32]);

private:
    uint32_t _state[8];
    uint64_t _bitlen;
    uint8_t  _buf[64];
    size_t   _buflen;
    void _transform(const uint8_t* chunk);
};

// HMAC-SHA256：key 长度任意，输出 32 字节
void hmacSha256(const uint8_t* key, size_t keyLen,
                const uint8_t* msg, size_t msgLen,
                uint8_t out[32]);

} // namespace xw
