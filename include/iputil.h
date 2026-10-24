/*
 * IP / TCP / UDP 头解析与校验和工具（纯 C++，无平台依赖，可主机单测）
 *
 * 用于手表网络网关：解析手表经 NetWork 通道发来的原始 IPv4 包，
 * 构造 DHCP 应答、TCP/UDP 中继的回程包。
 * 所有多字节字段按网络序（大端）处理，与 lwIP/BSD socket 的字节序一致。
 */
#pragma once

#include <cstdint>
#include <cstddef>
#include <cstring>

namespace xw {

#pragma pack(push, 1)

struct IpHeader {
    uint8_t  verIhl;      // 版本(4) + IHL(4)
    uint8_t  tos;
    uint16_t totalLen;    // 大端
    uint16_t id;
    uint16_t fragOff;     // 大端
    uint8_t  ttl;
    uint8_t  protocol;    // 1=ICMP 6=TCP 17=UDP
    uint16_t checksum;    // 大端
    uint32_t srcIp;       // 大端
    uint32_t dstIp;       // 大端
};

struct UdpHeader {
    uint16_t srcPort;     // 大端
    uint16_t dstPort;     // 大端
    uint16_t len;         // 大端（含头部）
    uint16_t checksum;    // 大端（IPv4 可为 0）
};

struct TcpHeader {
    uint16_t srcPort;     // 大端
    uint16_t dstPort;     // 大端
    uint32_t seq;         // 大端
    uint32_t ack;         // 大端
    uint8_t  dataOffset;  // 高4位为 offset（单位4字节）
    uint8_t  flags;       // FIN=0x01 SYN=0x02 RST=0x04 PSH=0x08 ACK=0x10
    uint16_t window;      // 大端
    uint16_t checksum;    // 大端
    uint16_t urgent;
};

#pragma pack(pop)

// TCP 标志位
static const uint8_t TCP_FIN = 0x01;
static const uint8_t TCP_SYN = 0x02;
static const uint8_t TCP_RST = 0x04;
static const uint8_t TCP_PSH = 0x08;
static const uint8_t TCP_ACK = 0x10;

// IP 协议号
static const uint8_t IP_PROTO_ICMP = 1;
static const uint8_t IP_PROTO_TCP = 6;
static const uint8_t IP_PROTO_UDP = 17;

// 手环侧地址（与 AstroBox 网络栈一致）。
// 字节序约定：IP/端口等多字节字段一律以"网络字节的原生读法"存放——
// 即把包中的网络序字节按小端机型直接读成 uint32/uint16（反之 memcpy 写回即网络序）。
// 例：10.1.10.1 的网络字节为 0A 01 0A 01，小端读法得 0x010A010A。
// （ESP32 Xtensa 与测试主机 x86 均为小端，语义一致。）
static const uint32_t WATCH_NET_BE = 0x010A010A;  // 10.1.10.1（网关/本机）
static const uint32_t WATCH_MASK_BE = 0x00FFFFFF;  // 255.255.255.0
static const uint32_t BROADCAST_BE = 0xFFFFFFFF;

// 大端序辅助（host 侧按小端机器验证用；ESP32 亦为小端，语义一致）
static inline uint16_t ntohs16(uint16_t v) {
    return uint16_t((v >> 8) | (v << 8));
}
static inline uint32_t ntohl32(uint32_t v) {
    return ((v & 0xFF) << 24) | ((v & 0xFF00) << 8) |
           ((v >> 8) & 0xFF00) | (v >> 24);
}
static inline uint16_t htons16(uint16_t v) { return ntohs16(v); }
static inline uint32_t htonl32(uint32_t v) { return ntohl32(v); }

// 标准 Internet 校验和（RFC 1071）。data 长度任意，可为奇数字节。
uint16_t ipChecksum(const uint8_t* data, size_t len);
// 在已有校验和基础上累加（用于伪首部+头部组合）
uint16_t ipChecksumContinue(uint16_t sum, const uint8_t* data, size_t len);

// UDP 校验和（IPv4；伪首部 + UDP 头 + 数据），返回大端序值（0 表示无校验和，
// 但本实现总是计算）
uint16_t udpChecksum(uint32_t srcIpBe, uint32_t dstIpBe,
                     const uint8_t* udp, size_t udpLen);

// TCP 校验和（IPv4）
uint16_t tcpChecksum(uint32_t srcIpBe, uint32_t dstIpBe,
                     const uint8_t* tcp, size_t tcpLen);

// 判断 IPv4 包是否分片（MF 位或 frag offset 非零）
bool ipIsFragment(const uint8_t* pkt, size_t len);

// IP 头长度（字节）
inline size_t ipHeaderLen(const uint8_t* pkt) {
    return size_t(pkt[0] & 0x0F) * 4;
}

// 从 IP 包中取出传输层负载指针与长度；失败返回 false
bool ipPayload(const uint8_t* pkt, size_t len,
               const uint8_t*& payload, size_t& payloadLen);

} // namespace xw
