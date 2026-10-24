#include "dhcp.h"
#include "iputil.h"

namespace xw {

// DHCP 固定字段偏移（RFC 2131）
static const size_t DH_OP = 0;
static const size_t DH_HTYPE = 1;
static const size_t DH_HLEN = 2;
static const size_t DH_HOPS = 3;
static const size_t DH_XID = 4;   // 4
static const size_t DH_SECS = 8;  // 2
static const size_t DH_FLAGS = 10; // 2
static const size_t DH_CIADDR = 12; // 4
static const size_t DH_YIADDR = 16; // 4
static const size_t DH_SIADDR = 20; // 4
static const size_t DH_GIADDR = 24; // 4
static const size_t DH_CHADDR = 28; // 16
static const size_t DH_SNAME = 44;  // 64
static const size_t DH_FILE = 108;  // 128
static const size_t DH_MAGIC = 236; // 4 (63 82 53 63)
static const size_t DH_OPTIONS = 240;

static const uint8_t DHCP_MAGIC[4] = {0x63, 0x82, 0x53, 0x63};

// DHCP 消息类型
static const uint8_t DHCP_DISCOVER = 1;
static const uint8_t DHCP_OFFER = 2;
static const uint8_t DHCP_REQUEST = 3;
static const uint8_t DHCP_DECLINE = 4;
static const uint8_t DHCP_ACK = 5;
static const uint8_t DHCP_NAK = 6;
static const uint8_t DHCP_RELEASE = 7;

// DHCP 选项
static const uint8_t OPT_SUBNET_MASK = 1;
static const uint8_t OPT_ROUTER = 3;
static const uint8_t OPT_DNS = 6;
static const uint8_t OPT_LEASE_TIME = 51;
static const uint8_t OPT_MSG_TYPE = 53;
static const uint8_t OPT_SERVER_ID = 54;

// 网关侧地址（网络字节的小端原生读法，约定同 iputil.h）
static const uint32_t GW_IP = 0x010A010A;     // 10.1.10.1
static const uint32_t WATCH_IP = 0x020A010A;  // 10.1.10.2
static const uint32_t NETMASK = 0x00FFFFFF;   // 255.255.255.0
static const uint32_t LEASE_TIME = 269352960;

static void putBe32(std::vector<uint8_t>& b, uint32_t v) {
    b.push_back(uint8_t(v >> 24));
    b.push_back(uint8_t(v >> 16));
    b.push_back(uint8_t(v >> 8));
    b.push_back(uint8_t(v));
}

// 按网络字节序写入 IP（v 为网络字节的小端原生读法）
static void putIp(std::vector<uint8_t>& b, uint32_t v) {
    b.push_back(uint8_t(v));
    b.push_back(uint8_t(v >> 8));
    b.push_back(uint8_t(v >> 16));
    b.push_back(uint8_t(v >> 24));
}

static void putBe16(std::vector<uint8_t>& b, uint16_t v) {
    b.push_back(uint8_t(v >> 8));
    b.push_back(uint8_t(v));
}

static void putOpt(std::vector<uint8_t>& b, uint8_t code, uint8_t len, const uint8_t* val) {
    b.push_back(code);
    b.push_back(len);
    for (uint8_t i = 0; i < len; i++) b.push_back(val[i]);
}

// 在 DHCP 选项中查找 message type；找不到返回 0
static uint8_t dhcpMessageType(const uint8_t* opts, size_t len) {
    size_t i = 0;
    while (i + 2 <= len) {
        uint8_t code = opts[i];
        if (code == 0) { i++; continue; }        // padding
        if (code == 255) break;                   // end
        if (i + 2 > len) break;
        uint8_t olen = opts[i + 1];
        if (i + 2 + olen > len) break;
        if (code == OPT_MSG_TYPE && olen >= 1) return opts[i + 2];
        i += 2 + olen;
    }
    return 0;
}

bool dhcpHandlePacket(const uint8_t* pkt, size_t len,
                      std::vector<uint8_t>& outPacket) {
    outPacket.clear();
    if (len < 20) return false;
    if ((pkt[0] >> 4) != 4) return false;
    size_t ihl = ipHeaderLen(pkt);
    if (ihl < 20 || ihl + 8 > len) return false;
    if (pkt[9] != IP_PROTO_UDP) return false;

    const IpHeader* ip = (const IpHeader*)pkt;
    uint16_t total = ntohs16(ip->totalLen);
    if (total > len) total = uint16_t(len);
    if (total < ihl + 8) return false;

    const UdpHeader* udp = (const UdpHeader*)(pkt + ihl);
    uint16_t sport = ntohs16(udp->srcPort);
    uint16_t dport = ntohs16(udp->dstPort);
    // DHCP: 客户端 68 -> 服务器 67（广播/单播均可）
    if (!(sport == 68 && dport == 67)) return false;

    const uint8_t* dhcp = pkt + ihl + 8;
    size_t dhcpLen = size_t(total) - ihl - 8;
    if (dhcpLen < DH_OPTIONS + 4) return false;
    if (dhcp[DH_OP] != 1) return false; // BOOTREQUEST
    if (std::memcmp(dhcp + DH_MAGIC, DHCP_MAGIC, 4) != 0) return false;

    uint8_t msgType = dhcpMessageType(dhcp + DH_OPTIONS, dhcpLen - DH_OPTIONS);
    uint8_t replyType = 0;
    if (msgType == DHCP_DISCOVER) replyType = DHCP_OFFER;
    else if (msgType == DHCP_REQUEST) replyType = DHCP_ACK;
    else return false; // RELEASE/DECLINE/INFORM 不处理

    // ---- 构造 DHCP 应答 ----
    std::vector<uint8_t> dh;
    dh.reserve(300);
    dh.push_back(2);                    // op = BOOTREPLY
    dh.push_back(dhcp[DH_HTYPE]);       // htype
    dh.push_back(dhcp[DH_HLEN]);        // hlen
    dh.push_back(0);                    // hops
    // xid（回显）
    for (int i = 0; i < 4; i++) dh.push_back(dhcp[DH_XID + i]);
    putBe16(dh, 0);                     // secs
    putBe16(dh, ntohs16(*(const uint16_t*)(dhcp + DH_FLAGS))); // flags 回显
    putBe32(dh, 0);                     // ciaddr
    putIp(dh, WATCH_IP);                // yiaddr = 10.1.10.2
    putIp(dh, GW_IP);                   // siaddr = 10.1.10.1
    putBe32(dh, 0);                     // giaddr
    // chaddr（16 字节，回显）
    for (int i = 0; i < 16; i++) dh.push_back(dhcp[DH_CHADDR + i]);
    // sname + file（192 字节置零）
    for (int i = 0; i < 192; i++) dh.push_back(0);
    // magic cookie
    for (int i = 0; i < 4; i++) dh.push_back(DHCP_MAGIC[i]);
    // options
    {
        uint8_t mt = replyType;
        putOpt(dh, OPT_MSG_TYPE, 1, &mt);
        uint8_t sid[4] = {10, 1, 10, 1};
        putOpt(dh, OPT_SERVER_ID, 4, sid);
        uint8_t mask[4] = {255, 255, 255, 0};
        putOpt(dh, OPT_SUBNET_MASK, 4, mask);
        uint8_t router[4] = {10, 1, 10, 1};
        putOpt(dh, OPT_ROUTER, 4, router);
        uint8_t lease[4] = {
            uint8_t(LEASE_TIME >> 24), uint8_t(LEASE_TIME >> 16),
            uint8_t(LEASE_TIME >> 8), uint8_t(LEASE_TIME)};
        putOpt(dh, OPT_LEASE_TIME, 4, lease);
        uint8_t dns[4] = {223, 5, 5, 5};
        putOpt(dh, OPT_DNS, 4, dns);
        dh.push_back(255); // end
    }

    // ---- UDP 头（67 -> 68）----
    std::vector<uint8_t> udpOut;
    udpOut.reserve(8 + dh.size());
    putBe16(udpOut, 67);                  // src port
    putBe16(udpOut, 68);                  // dst port
    putBe16(udpOut, uint16_t(8 + dh.size())); // length
    putBe16(udpOut, 0);                   // checksum 占位
    udpOut.insert(udpOut.end(), dh.begin(), dh.end());

    // ---- IP 头（10.1.10.1 -> 255.255.255.255）----
    uint16_t ipTotal = uint16_t(20 + udpOut.size());
    std::vector<uint8_t> out;
    out.reserve(ipTotal);
    out.push_back(0x45);               // version 4, IHL 5
    out.push_back(0x00);               // TOS
    putBe16(out, ipTotal);             // total length
    putBe16(out, 0);                   // id
    putBe16(out, 0);                   // flags/frag offset（不分片位由对端处理）
    out.push_back(64);                 // TTL
    out.push_back(IP_PROTO_UDP);       // protocol
    putBe16(out, 0);                   // checksum 占位
    putIp(out, GW_IP);                 // src = 10.1.10.1
    putIp(out, 0xFFFFFFFF);            // dst = 255.255.255.255
    out.insert(out.end(), udpOut.begin(), udpOut.end());

    // 计算 UDP 校验和（伪首部 src=GW_IP dst=255.255.255.255）
    {
        uint16_t csum = udpChecksum(GW_IP, 0xFFFFFFFF, out.data() + 20, udpOut.size());
        out[20 + 6] = uint8_t(csum >> 8);
        out[20 + 7] = uint8_t(csum);
    }
    // 计算 IP 头校验和
    {
        uint16_t csum = ipChecksum(out.data(), 20);
        out[10] = uint8_t(csum >> 8);
        out[11] = uint8_t(csum);
    }

    outPacket = std::move(out);
    return true;
}

} // namespace xw
