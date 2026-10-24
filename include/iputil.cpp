/*
 * IP / TCP / UDP 校验和实现（RFC 1071）
 */
#include "iputil.h"

namespace xw {

uint16_t ipChecksumContinue(uint16_t sum, const uint8_t* data, size_t len) {
    uint32_t acc = sum;
    size_t i = 0;
    while (i + 1 < len) {
        acc += (uint32_t(data[i]) << 8) | uint32_t(data[i + 1]);
        i += 2;
    }
    if (i < len) {
        acc += uint32_t(data[i]) << 8; // 奇数字节补高位
    }
    while (acc >> 16) {
        acc = (acc & 0xFFFF) + (acc >> 16);
    }
    return uint16_t(acc);
}

uint16_t ipChecksum(const uint8_t* data, size_t len) {
    return uint16_t(~ipChecksumContinue(0, data, len) & 0xFFFF);
}

static uint16_t pseudoHeaderSum(uint32_t srcIpBe, uint32_t dstIpBe,
                                uint8_t proto, uint16_t len) {
    uint8_t ph[12];
    // 伪首部按网络序字节拼接
    ph[0] = uint8_t(srcIpBe);
    ph[1] = uint8_t(srcIpBe >> 8);
    ph[2] = uint8_t(srcIpBe >> 16);
    ph[3] = uint8_t(srcIpBe >> 24);
    ph[4] = uint8_t(dstIpBe);
    ph[5] = uint8_t(dstIpBe >> 8);
    ph[6] = uint8_t(dstIpBe >> 16);
    ph[7] = uint8_t(dstIpBe >> 24);
    ph[8] = 0;
    ph[9] = proto;
    ph[10] = uint8_t(len >> 8);
    ph[11] = uint8_t(len);
    return ipChecksumContinue(0, ph, sizeof(ph));
}

uint16_t udpChecksum(uint32_t srcIpBe, uint32_t dstIpBe,
                     const uint8_t* udp, size_t udpLen) {
    if (udpLen < 8) return 0;
    uint16_t len = uint16_t(udpLen);
    uint16_t sum = pseudoHeaderSum(srcIpBe, dstIpBe, IP_PROTO_UDP, len);
    sum = ipChecksumContinue(sum, udp, udpLen);
    // 校验和为 0x0000 时按协议规定存为 0xFFFF（避免与"无校验和"混淆）
    uint16_t out = uint16_t(~sum & 0xFFFF);
    return out == 0 ? 0xFFFF : out;
}

uint16_t tcpChecksum(uint32_t srcIpBe, uint32_t dstIpBe,
                     const uint8_t* tcp, size_t tcpLen) {
    if (tcpLen < 20) return 0;
    uint16_t len = uint16_t(tcpLen);
    uint16_t sum = pseudoHeaderSum(srcIpBe, dstIpBe, IP_PROTO_TCP, len);
    sum = ipChecksumContinue(sum, tcp, tcpLen);
    return uint16_t(~sum & 0xFFFF);
}

bool ipIsFragment(const uint8_t* pkt, size_t len) {
    if (len < 6) return true;
    uint16_t frag = ntohs16(*(const uint16_t*)(pkt + 6));
    return (frag & 0x3FFF) != 0; // MF 位或非零 offset
}

bool ipPayload(const uint8_t* pkt, size_t len,
               const uint8_t*& payload, size_t& payloadLen) {
    if (len < 20) return false;
    if ((pkt[0] >> 4) != 4) return false;
    size_t ihl = ipHeaderLen(pkt);
    if (ihl < 20 || ihl > len) return false;
    uint16_t total = ntohs16(*(const uint16_t*)(pkt + 2));
    if (total > len) total = uint16_t(len); // 容忍截断包
    if (total < ihl) return false;
    payload = pkt + ihl;
    payloadLen = size_t(total) - ihl;
    return true;
}

} // namespace xw
