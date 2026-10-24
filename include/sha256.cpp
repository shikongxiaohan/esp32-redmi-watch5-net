/*
 * SHA-256 实现（FIPS 180-4，公共领域风格实现，经过主机端单测验证）
 */
#include "sha256.h"

namespace xw {

static const uint32_t K256[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
};

static inline uint32_t rotr(uint32_t x, uint32_t n) { return (x >> n) | (x << (32 - n)); }

Sha256::Sha256() { reset(); }

void Sha256::reset() {
    _state[0] = 0x6a09e667u; _state[1] = 0xbb67ae85u;
    _state[2] = 0x3c6ef372u; _state[3] = 0xa54ff53au;
    _state[4] = 0x510e527fu; _state[5] = 0x9b05688cu;
    _state[6] = 0x1f83d9abu; _state[7] = 0x5be0cd19u;
    _bitlen = 0;
    _buflen = 0;
}

void Sha256::_transform(const uint8_t* chunk) {
    uint32_t w[64];
    for (int i = 0; i < 16; i++) {
        w[i] = (uint32_t(chunk[i * 4]) << 24) | (uint32_t(chunk[i * 4 + 1]) << 16) |
               (uint32_t(chunk[i * 4 + 2]) << 8) | uint32_t(chunk[i * 4 + 3]);
    }
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = _state[0], b = _state[1], c = _state[2], d = _state[3];
    uint32_t e = _state[4], f = _state[5], g = _state[6], h = _state[7];
    for (int i = 0; i < 64; i++) {
        uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = h + S1 + ch + K256[i] + w[i];
        uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = S0 + maj;
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }
    _state[0] += a; _state[1] += b; _state[2] += c; _state[3] += d;
    _state[4] += e; _state[5] += f; _state[6] += g; _state[7] += h;
}

void Sha256::update(const uint8_t* data, size_t len) {
    for (size_t i = 0; i < len; i++) {
        _buf[_buflen++] = data[i];
        if (_buflen == 64) {
            _transform(_buf);
            _buflen = 0;
        }
    }
    _bitlen += uint64_t(len) * 8;
}

void Sha256::final(uint8_t out[32]) {
    size_t i = _buflen;
    _buf[i++] = 0x80;
    if (i > 56) {
        while (i < 64) _buf[i++] = 0;
        _transform(_buf);
        i = 0;
    }
    while (i < 56) _buf[i++] = 0;
    for (int s = 7; s >= 0; s--) _buf[i++] = uint8_t(_bitlen >> (s * 8));
    _transform(_buf);
    for (int j = 0; j < 8; j++) {
        out[j * 4]     = uint8_t(_state[j] >> 24);
        out[j * 4 + 1] = uint8_t(_state[j] >> 16);
        out[j * 4 + 2] = uint8_t(_state[j] >> 8);
        out[j * 4 + 3] = uint8_t(_state[j]);
    }
}

void Sha256::hash(const uint8_t* data, size_t len, uint8_t out[32]) {
    Sha256 s;
    s.update(data, len);
    s.final(out);
}

void hmacSha256(const uint8_t* key, size_t keyLen,
                const uint8_t* msg, size_t msgLen,
                uint8_t out[32]) {
    uint8_t k[64];
    uint8_t kipad[64], kopad[64];
    if (keyLen > 64) {
        Sha256::hash(key, keyLen, k);
        for (size_t i = 32; i < 64; i++) k[i] = 0;
    } else {
        for (size_t i = 0; i < keyLen; i++) k[i] = key[i];
        for (size_t i = keyLen; i < 64; i++) k[i] = 0;
    }
    for (int i = 0; i < 64; i++) {
        kipad[i] = k[i] ^ 0x36;
        kopad[i] = k[i] ^ 0x5c;
    }
    uint8_t inner[32];
    Sha256 s;
    s.update(kipad, 64);
    s.update(msg, msgLen);
    s.final(inner);
    Sha256 s2;
    s2.update(kopad, 64);
    s2.update(inner, 32);
    s2.final(out);
}

} // namespace xw
