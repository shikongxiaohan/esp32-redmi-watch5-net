/*
 * MD5 实现（RFC 1321）
 *
 * 结构上与 RFC 1321 附录 A 的参考实现逐行对应（F/G/H/I + FF/GG/HH/II 双层宏），
 * 便于逐行审计。注意：内层函数宏的参数名必须为 x/y/z，不能与外层的 a/b/c/d
 * 重名——否则轮转调用处（如 FF(d,a,b,c,...)）的布尔表达式会绑定到错误的变量
 * （本项目初版即踩此坑，48/64 步静默算错）。
 *
 * 标准向量（RFC 1319 / 附录 A.5）:
 *   MD5("")               = d41d8cd98f00b204e9800998ecf8427e
 *   MD5("abc")            = 900150983cd24fb0d6963f7d28e17f72
 *   MD5("12345678901234567890123456789012345678901234567890123456789012345678901234567890")
 *                         = 57edf4a22be3c955ac49da2e2107b67a
 */
#include "md5.h"

#include <cstring>

namespace xw {

static inline uint32_t rotl32(uint32_t x, int n) {
    return (x << n) | (x >> (32 - n));
}

// RFC 1321 基本函数（参数名 x/y/z 避免与步骤宏的 a/b/c/d 冲突）
#define F(x, y, z) (((x) & (y)) | ((~x) & (z)))
#define G(x, y, z) (((x) & (z)) | ((y) & (~z)))
#define H(x, y, z) ((x) ^ (y) ^ (z))
#define I(x, y, z) ((y) ^ ((x) | (~z)))

#define ROTATE_LEFT(x, n) (((x) << (n)) | ((x) >> (32 - (n))))

// RFC 1321 变换步骤（ac = T 表常量，见 RFC 附录 A.4 / 由 floor(2^32*|sin(i)|) 生成）
#define FF(a, b, c, d, x, s, ac) { (a) += F((b), (c), (d)) + (x) + (ac); (a) = ROTATE_LEFT((a), (s)); (a) += (b); }
#define GG(a, b, c, d, x, s, ac) { (a) += G((b), (c), (d)) + (x) + (ac); (a) = ROTATE_LEFT((a), (s)); (a) += (b); }
#define HH(a, b, c, d, x, s, ac) { (a) += H((b), (c), (d)) + (x) + (ac); (a) = ROTATE_LEFT((a), (s)); (a) += (b); }
#define II(a, b, c, d, x, s, ac) { (a) += I((b), (c), (d)) + (x) + (ac); (a) = ROTATE_LEFT((a), (s)); (a) += (b); }

void Md5::_transform(const uint8_t* chunk) {
    uint32_t m[16];
    for (int i = 0; i < 16; i++) {
        m[i] = uint32_t(chunk[i * 4]) | (uint32_t(chunk[i * 4 + 1]) << 8) |
               (uint32_t(chunk[i * 4 + 2]) << 16) | (uint32_t(chunk[i * 4 + 3]) << 24);
    }

    uint32_t a = _state[0], b = _state[1], c = _state[2], d = _state[3];

    // Round 1
    FF(a, b, c, d, m[0], 7, 0xd76aa478u)
    FF(d, a, b, c, m[1], 12, 0xe8c7b756u)
    FF(c, d, a, b, m[2], 17, 0x242070dbu)
    FF(b, c, d, a, m[3], 22, 0xc1bdceeeu)
    FF(a, b, c, d, m[4], 7, 0xf57c0fafu)
    FF(d, a, b, c, m[5], 12, 0x4787c62au)
    FF(c, d, a, b, m[6], 17, 0xa8304613u)
    FF(b, c, d, a, m[7], 22, 0xfd469501u)
    FF(a, b, c, d, m[8], 7, 0x698098d8u)
    FF(d, a, b, c, m[9], 12, 0x8b44f7afu)
    FF(c, d, a, b, m[10], 17, 0xffff5bb1u)
    FF(b, c, d, a, m[11], 22, 0x895cd7beu)
    FF(a, b, c, d, m[12], 7, 0x6b901122u)
    FF(d, a, b, c, m[13], 12, 0xfd987193u)
    FF(c, d, a, b, m[14], 17, 0xa679438eu)
    FF(b, c, d, a, m[15], 22, 0x49b40821u)

    // Round 2
    GG(a, b, c, d, m[1], 5, 0xf61e2562u)
    GG(d, a, b, c, m[6], 9, 0xc040b340u)
    GG(c, d, a, b, m[11], 14, 0x265e5a51u)
    GG(b, c, d, a, m[0], 20, 0xe9b6c7aau)
    GG(a, b, c, d, m[5], 5, 0xd62f105du)
    GG(d, a, b, c, m[10], 9, 0x02441453u)
    GG(c, d, a, b, m[15], 14, 0xd8a1e681u)
    GG(b, c, d, a, m[4], 20, 0xe7d3fbc8u)
    GG(a, b, c, d, m[9], 5, 0x21e1cde6u)
    GG(d, a, b, c, m[14], 9, 0xc33707d6u)
    GG(c, d, a, b, m[3], 14, 0xf4d50d87u)
    GG(b, c, d, a, m[8], 20, 0x455a14edu)
    GG(a, b, c, d, m[13], 5, 0xa9e3e905u)
    GG(d, a, b, c, m[2], 9, 0xfcefa3f8u)
    GG(c, d, a, b, m[7], 14, 0x676f02d9u)
    GG(b, c, d, a, m[12], 20, 0x8d2a4c8au)

    // Round 3
    HH(a, b, c, d, m[5], 4, 0xfffa3942u)
    HH(d, a, b, c, m[8], 11, 0x8771f681u)
    HH(c, d, a, b, m[11], 16, 0x6d9d6122u)
    HH(b, c, d, a, m[14], 23, 0xfde5380cu)
    HH(a, b, c, d, m[1], 4, 0xa4beea44u)
    HH(d, a, b, c, m[4], 11, 0x4bdecfa9u)
    HH(c, d, a, b, m[7], 16, 0xf6bb4b60u)
    HH(b, c, d, a, m[10], 23, 0xbebfbc70u)
    HH(a, b, c, d, m[13], 4, 0x289b7ec6u)
    HH(d, a, b, c, m[0], 11, 0xeaa127fau)
    HH(c, d, a, b, m[3], 16, 0xd4ef3085u)
    HH(b, c, d, a, m[6], 23, 0x04881d05u)
    HH(a, b, c, d, m[9], 4, 0xd9d4d039u)
    HH(d, a, b, c, m[12], 11, 0xe6db99e5u)
    HH(c, d, a, b, m[15], 16, 0x1fa27cf8u)
    HH(b, c, d, a, m[2], 23, 0xc4ac5665u)

    // Round 4
    II(a, b, c, d, m[0], 6, 0xf4292244u)
    II(d, a, b, c, m[7], 10, 0x432aff97u)
    II(c, d, a, b, m[14], 15, 0xab9423a7u)
    II(b, c, d, a, m[5], 21, 0xfc93a039u)
    II(a, b, c, d, m[12], 6, 0x655b59c3u)
    II(d, a, b, c, m[3], 10, 0x8f0ccc92u)
    II(c, d, a, b, m[10], 15, 0xffeff47du)
    II(b, c, d, a, m[1], 21, 0x85845dd1u)
    II(a, b, c, d, m[8], 6, 0x6fa87e4fu)
    II(d, a, b, c, m[15], 10, 0xfe2ce6e0u)
    II(c, d, a, b, m[6], 15, 0xa3014314u)
    II(b, c, d, a, m[13], 21, 0x4e0811a1u)
    II(a, b, c, d, m[4], 6, 0xf7537e82u)
    II(d, a, b, c, m[11], 10, 0xbd3af235u)
    II(c, d, a, b, m[2], 15, 0x2ad7d2bbu)
    II(b, c, d, a, m[9], 21, 0xeb86d391u)

    _state[0] += a;
    _state[1] += b;
    _state[2] += c;
    _state[3] += d;
}

#undef FF
#undef GG
#undef HH
#undef II
#undef F
#undef G
#undef H
#undef I
#undef ROTATE_LEFT

void Md5::reset() {
    _state[0] = 0x67452301u;
    _state[1] = 0xefcdab89u;
    _state[2] = 0x98badcfeu;
    _state[3] = 0x10325476u;
    _bitlen = 0;
    _buflen = 0;
}

void Md5::update(const uint8_t* data, size_t len) {
    for (size_t i = 0; i < len; i++) {
        _buf[_buflen++] = data[i];
        if (_buflen == 64) {
            _transform(_buf);
            _buflen = 0;
        }
    }
    _bitlen += uint64_t(len) * 8;
}

void Md5::final(uint8_t out[16]) {
    size_t i = _buflen;
    _buf[i++] = 0x80;
    if (i > 56) {
        while (i < 64) _buf[i++] = 0;
        _transform(_buf);
        i = 0;
    }
    while (i < 56) _buf[i++] = 0;
    // RFC 1321: 消息长度（位）以 64 位小端存放（注意与 SHA-256 的大端不同）
    for (int s = 0; s < 8; s++) _buf[i++] = uint8_t(_bitlen >> (s * 8));
    _transform(_buf);
    for (int j = 0; j < 4; j++) {
        out[j * 4]     = uint8_t(_state[j]);
        out[j * 4 + 1] = uint8_t(_state[j] >> 8);
        out[j * 4 + 2] = uint8_t(_state[j] >> 16);
        out[j * 4 + 3] = uint8_t(_state[j] >> 24);
    }
}

void Md5::hash(const uint8_t* data, size_t len, uint8_t out[16]) {
    Md5 m;
    m.update(data, len);
    m.final(out);
}

} // namespace xw
