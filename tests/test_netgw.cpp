/*
 * NetGw 主机端单元测试：FakeUpstream + 可控假时钟，
 * 覆盖 DHCP / ICMP / TCP 中继全流程 / 窗口与 MSS / 重传 / DNS 代答 /
 * UDP 中继 / 源过滤 / 上游连接失败 / 延迟连接。
 */
#include "netgw.h"
#include "iputil.h"
#include "dhcp.h"

#include <cstdio>
#include <cstring>
#include <vector>
#include <string>

// ---------- 测试框架 ----------
static int g_checks = 0;
static int g_failures = 0;
static void check(bool cond, const char* what) {
    g_checks++;
    if (!cond) {
        g_failures++;
        std::printf("  FAIL: %s\n", what);
    }
}
static std::string toHex(const uint8_t* p, size_t n) {
    static const char* h = "0123456789abcdef";
    std::string s;
    for (size_t i = 0; i < n; i++) {
        s += h[p[i] >> 4];
        s += h[p[i] & 0xF];
    }
    return s;
}

// ---------- 可控假时钟 ----------
static uint32_t g_now = 1000;
static void advance(uint32_t ms) { g_now += ms; }

// ---------- 字节序助手（与实现相同的"网络字节小端读法"约定） ----------
static uint32_t ip4(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
    return uint32_t(a) | (uint32_t(b) << 8) | (uint32_t(c) << 16) | (uint32_t(d) << 24);
}
static uint16_t np(uint16_t hostPort) {
    return uint16_t((hostPort >> 8) | (hostPort << 8));
}

// ---------- 假上游 ----------
struct FakeSock {
    bool isTcp = false;
    bool connected = true;    // TCP: canSend()
    bool failed = false;      // hasError()
    bool writeClosed = false; // shutdownWrite() 已调用
    bool eof = false;         // recvData 返回 0
    bool closed = false;      // closeFd 已调用
    std::vector<uint8_t> up2down;   // 本地 sendData 写入（对端可读）
    std::vector<uint8_t> down2up;   // 对端发送（本地 recvData 可读）
    uint32_t dstIp = 0;
    uint16_t dstPort = 0;
};

class FakeUpstream : public xw::Upstream {
public:
    std::vector<FakeSock> socks;
    int nextFd = 1;
    bool rejectConnect = false;    // tcpConnect 返回 -1
    bool connectDelayed = false;   // 新 TCP 连接初始未建立

    FakeSock& at(int fd) { return socks[size_t(fd) - 1]; }

    int tcpConnect(uint32_t dstIpBe, uint16_t dstPortBe) override {
        if (rejectConnect) return -1;
        FakeSock s;
        s.isTcp = true;
        s.dstIp = dstIpBe;
        s.dstPort = dstPortBe;
        if (connectDelayed) s.connected = false;
        socks.push_back(s);
        return nextFd++;
    }
    int udpConnect(uint32_t dstIpBe, uint16_t dstPortBe) override {
        FakeSock s;
        s.isTcp = false;
        s.dstIp = dstIpBe;
        s.dstPort = dstPortBe;
        socks.push_back(s);
        return nextFd++;
    }
    void closeFd(int fd) override {
        if (fd >= 1 && size_t(fd) <= socks.size()) at(fd).closed = true;
    }
    void shutdownWrite(int fd) override {
        if (fd >= 1 && size_t(fd) <= socks.size()) at(fd).writeClosed = true;
    }
    int sendData(int fd, const uint8_t* data, size_t len) override {
        if (fd < 1 || size_t(fd) > socks.size()) return -1;
        FakeSock& s = at(fd);
        if (s.closed) return -1;
        s.up2down.insert(s.up2down.end(), data, data + len);
        return int(len);
    }
    int recvData(int fd, uint8_t* buf, size_t cap) override {
        if (fd < 1 || size_t(fd) > socks.size()) return -1;
        FakeSock& s = at(fd);
        if (s.closed) return -1;
        if (s.down2up.empty()) return s.eof ? 0 : -1;
        size_t n = s.down2up.size() < cap ? s.down2up.size() : cap;
        std::memcpy(buf, s.down2up.data(), n);
        s.down2up.erase(s.down2up.begin(), s.down2up.begin() + n);
        return int(n);
    }
    bool canSend(int fd) override {
        return fd >= 1 && size_t(fd) <= socks.size() && at(fd).connected;
    }
    bool hasError(int fd) override {
        return fd < 1 || size_t(fd) > socks.size() || at(fd).failed;
    }
};

// ---------- 包构造助手（手表 -> 网关方向） ----------
static std::vector<uint8_t> buildIp(uint32_t srcBe, uint32_t dstBe, uint8_t proto,
                                    const uint8_t* payload, size_t len) {
    std::vector<uint8_t> p(20 + len);
    p[0] = 0x45;
    p[1] = 0;
    uint16_t total = uint16_t(20 + len);
    p[2] = uint8_t(total >> 8);
    p[3] = uint8_t(total);
    p[4] = 0;
    p[5] = 1;
    p[6] = 0x40;
    p[7] = 0;
    p[8] = 64;
    p[9] = proto;
    p[10] = 0;
    p[11] = 0;
    std::memcpy(&p[12], &srcBe, 4);
    std::memcpy(&p[16], &dstBe, 4);
    if (len) std::memcpy(&p[20], payload, len);
    uint16_t c = xw::ipChecksum(p.data(), 20);
    p[10] = uint8_t(c >> 8);
    p[11] = uint8_t(c);
    return p;
}

static std::vector<uint8_t> buildTcp(uint16_t sport, uint16_t dport, uint32_t seq,
                                     uint32_t ack, uint8_t flags, const uint8_t* data,
                                     size_t len, uint16_t win = 4096, uint16_t mssOpt = 0) {
    size_t hdr = mssOpt ? 24 : 20;
    std::vector<uint8_t> s(hdr + len);
    std::memcpy(&s[0], &sport, 2);
    std::memcpy(&s[2], &dport, 2);
    std::memcpy(&s[4], &seq, 4);
    std::memcpy(&s[8], &ack, 4);
    s[12] = uint8_t(hdr / 4) << 4;
    s[13] = flags;
    std::memcpy(&s[14], &win, 2);
    s[16] = 0; s[17] = 0; s[18] = 0; s[19] = 0;
    if (mssOpt) {
        s[20] = 2; s[21] = 4;
        s[22] = uint8_t(mssOpt >> 8);
        s[23] = uint8_t(mssOpt);
    }
    if (len) std::memcpy(&s[hdr], data, len);
    return s;
}

static std::vector<uint8_t> buildUdp(uint16_t sport, uint16_t dport, const uint8_t* data,
                                     size_t len) {
    std::vector<uint8_t> u(8 + len);
    std::memcpy(&u[0], &sport, 2);
    std::memcpy(&u[2], &dport, 2);
    uint16_t ulen = uint16_t(8 + len);
    std::memcpy(&u[4], &ulen, 2);
    u[6] = 0; u[7] = 0;
    if (len) std::memcpy(&u[8], data, len);
    return u;
}

// 构造 DHCP 报文（DISCOVER/REQUEST）
static std::vector<uint8_t> buildDhcpPayload(uint8_t msgType, const uint8_t mac[6],
                                             uint32_t xid) {
    std::vector<uint8_t> dh;
    dh.push_back(1); dh.push_back(1); dh.push_back(6); dh.push_back(0);
    for (int i = 0; i < 4; i++) dh.push_back(uint8_t(xid >> (8 * (3 - i))));
    dh.push_back(0); dh.push_back(0);
    dh.push_back(0x80); dh.push_back(0); // broadcast
    for (int i = 0; i < 16; i++) dh.push_back(0); // ciaddr/yiaddr/siaddr/giaddr
    for (int i = 0; i < 6; i++) dh.push_back(mac[i]);
    for (int i = 0; i < 10; i++) dh.push_back(0);
    for (int i = 0; i < 64; i++) dh.push_back(0);
    for (int i = 0; i < 128; i++) dh.push_back(0);
    const uint8_t magic[4] = {0x63, 0x82, 0x53, 0x63};
    for (int i = 0; i < 4; i++) dh.push_back(magic[i]);
    dh.push_back(53); dh.push_back(1); dh.push_back(msgType);
    dh.push_back(55); dh.push_back(4);
    dh.push_back(1); dh.push_back(3); dh.push_back(6); dh.push_back(51);
    dh.push_back(255);
    return dh;
}

// ---------- 接收侧解析 ----------
struct ParsedIp {
    uint32_t src, dst;
    uint8_t proto;
    const uint8_t* payload;
    size_t payloadLen;
    uint16_t fragField;
};
static bool parseIp(const std::vector<uint8_t>& pkt, ParsedIp& out) {
    if (pkt.size() < 20) return false;
    size_t ihl = pkt[0] & 0x0F;
    if (ihl < 5 || size_t(ihl) * 4 > pkt.size()) return false;
    out.src = 0; out.dst = 0;
    std::memcpy(&out.src, &pkt[12], 4);
    std::memcpy(&out.dst, &pkt[16], 4);
    out.proto = pkt[9];
    out.fragField = uint16_t((pkt[6] << 8) | pkt[7]);
    size_t total = size_t(pkt[2] << 8) | pkt[3];
    if (total > pkt.size()) total = pkt.size();
    if (total < size_t(ihl) * 4) return false;
    out.payload = pkt.data() + ihl * 4;
    out.payloadLen = total - size_t(ihl) * 4;
    return true;
}

struct ParsedTcp {
    uint16_t sport, dport;
    uint32_t seq, ack;
    uint8_t flags;
    const uint8_t* data;
    size_t len;
};
static bool parseTcp(const std::vector<uint8_t>& pkt, ParsedTcp& out) {
    ParsedIp ip;
    if (!parseIp(pkt, ip) || ip.proto != 6 || ip.payloadLen < 20) return false;
    const uint8_t* t = ip.payload;
    std::memcpy(&out.sport, t, 2);
    std::memcpy(&out.dport, t + 2, 2);
    std::memcpy(&out.seq, t + 4, 4);
    std::memcpy(&out.ack, t + 8, 4);
    out.flags = t[13];
    size_t hl = size_t(t[12] >> 4) * 4;
    if (hl < 20 || hl > ip.payloadLen) return false;
    out.data = t + hl;
    out.len = ip.payloadLen - hl;
    return true;
}

struct ParsedUdp {
    uint16_t sport, dport;
    const uint8_t* data;
    size_t len;
};
static bool parseUdp(const std::vector<uint8_t>& pkt, ParsedUdp& out) {
    ParsedIp ip;
    if (!parseIp(pkt, ip) || ip.proto != 17 || ip.payloadLen < 8) return false;
    std::memcpy(&out.sport, ip.payload, 2);
    std::memcpy(&out.dport, ip.payload + 2, 2);
    out.data = ip.payload + 8;
    out.len = ip.payloadLen - 8;
    return true;
}

// ---------- 测试夹具 ----------
struct Fixture {
    FakeUpstream up;
    xw::NetGw gw{up};
    std::vector<std::vector<uint8_t>> sent;
    std::vector<std::string> logs;

    Fixture() {
        g_now = 1000;
        gw.nowMs = []() -> uint32_t { return g_now; };
        gw.onLog = [this](const char* m) { logs.push_back(m); };
        gw.begin([this](const uint8_t* p, size_t n) {
            sent.emplace_back(p, p + n);
        });
    }
    void feed(const std::vector<uint8_t>& pkt) {
        gw.onIpPacket(pkt.data(), pkt.size());
    }
    void clearSent() { sent.clear(); }
};

// ================= DHCP =================
static void testDhcp() {
    std::printf("[netgw: dhcp]\n");
    Fixture fx;
    uint8_t mac[6] = {0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC};

    auto disc = buildIp(0, xw::BROADCAST_BE, 17,
                        buildDhcpPayload(1, mac, 0x11112222).data(), 0);
    // buildDhcpPayload 返回 vector，直接取出再拼 IP：
    std::vector<uint8_t> dh = buildDhcpPayload(1, mac, 0x11112222);
    std::vector<uint8_t> udp = buildUdp(np(68), np(67), dh.data(), dh.size());
    disc = buildIp(0, xw::BROADCAST_BE, 17, udp.data(), udp.size());

    fx.feed(disc);
    check(fx.sent.size() == 1, "dhcp discover -> one reply");
    check(fx.gw.dhcpReplies == 1, "dhcpReplies == 1");
    if (fx.sent.size() == 1) {
        ParsedIp ip;
        check(parseIp(fx.sent[0], ip), "reply ip parse");
        check(ip.src == xw::NETGW_LOCAL_IP_BE, "reply src = 10.1.10.1");
        check(ip.dst == xw::BROADCAST_BE, "reply dst = broadcast");
        ParsedUdp u;
        check(parseUdp(fx.sent[0], u), "reply udp parse");
        check(u.sport == np(67) && u.dport == np(68), "reply udp ports 67->68");
        check(u.len >= 240 && u.data[0] == 2, "reply dhcp BOOTREPLY");
        check(u.data[16] == 10 && u.data[17] == 1 && u.data[18] == 10 && u.data[19] == 2,
              "reply yiaddr = 10.1.10.2");
    }

    // REQUEST -> ACK
    fx.clearSent();
    std::vector<uint8_t> dh2 = buildDhcpPayload(3, mac, 0x33334444);
    std::vector<uint8_t> udp2 = buildUdp(np(68), np(67), dh2.data(), dh2.size());
    fx.feed(buildIp(0, xw::BROADCAST_BE, 17, udp2.data(), udp2.size()));
    check(fx.sent.size() == 1, "dhcp request -> one ack");
    if (fx.sent.size() == 1) {
        ParsedUdp u;
        check(parseUdp(fx.sent[0], u), "ack udp parse");
        // 选项 53 = 5 (ACK)
        bool isAck = false;
        const uint8_t* opt = u.data + 240;
        size_t left = u.len - 240;
        for (size_t i = 0; i + 2 <= left; ) {
            if (opt[i] == 255) break;
            if (opt[i] == 0) { i++; continue; }
            if (opt[i] == 53 && opt[i + 1] == 1) { isAck = opt[i + 2] == 5; break; }
            i += 2 + opt[i + 1];
        }
        check(isAck, "dhcp option 53 = ACK");
    }
    check(fx.gw.dhcpReplies == 2, "dhcpReplies == 2");
}

// ================= ICMP =================
static void testIcmp() {
    std::printf("[netgw: icmp]\n");
    Fixture fx;
    uint32_t server = ip4(93, 184, 216, 34);

    // echo request，16 字节载荷
    uint8_t icmp[8 + 16];
    icmp[0] = 8; icmp[1] = 0; icmp[2] = 0; icmp[3] = 0;
    icmp[4] = 0; icmp[5] = 1; icmp[6] = 0; icmp[7] = 1; // id/seq
    for (int i = 0; i < 16; i++) icmp[8 + i] = uint8_t(i);
    uint16_t c = xw::ipChecksum(icmp, sizeof(icmp));
    icmp[2] = uint8_t(c >> 8);
    icmp[3] = uint8_t(c);

    fx.feed(buildIp(xw::NETGW_WATCH_IP_BE, server, 1, icmp, sizeof(icmp)));
    check(fx.sent.size() == 1, "icmp echo -> one reply");
    check(fx.gw.icmpReplies == 1, "icmpReplies == 1");
    if (fx.sent.size() == 1) {
        ParsedIp ip;
        check(parseIp(fx.sent[0], ip), "icmp reply ip parse");
        check(ip.src == server && ip.dst == xw::NETGW_WATCH_IP_BE, "icmp reply addr");
        check(ip.proto == 1, "icmp reply proto");
        check(ip.payloadLen == sizeof(icmp), "icmp reply len");
        check(ip.payload[0] == 0, "icmp reply type = echo reply");
        check(std::memcmp(ip.payload + 8, icmp + 8, 16) == 0, "icmp reply payload echoed");
        // ICMP 校验和自洽
        check(xw::ipChecksum(ip.payload, ip.payloadLen) == 0, "icmp reply checksum valid");
    }
}

static void testIcmpFragmentation() {
    std::printf("[netgw: icmp fragmentation]\n");
    Fixture fx;
    uint32_t server = ip4(93, 184, 216, 34);

    // 1000 字节载荷的 echo request -> 应分 2 片（776 + 232）
    size_t plen = 8 + 1000;
    std::vector<uint8_t> icmp(plen);
    icmp[0] = 8; icmp[1] = 0;
    icmp[4] = 0; icmp[5] = 2; icmp[6] = 0; icmp[7] = 2;
    for (size_t i = 8; i < plen; i++) icmp[i] = uint8_t(i * 7);
    uint16_t c = xw::ipChecksum(icmp.data(), plen);
    icmp[2] = uint8_t(c >> 8);
    icmp[3] = uint8_t(c);

    fx.feed(buildIp(xw::NETGW_WATCH_IP_BE, server, 1, icmp.data(), plen));
    check(fx.sent.size() == 2, "large icmp -> 2 fragments");
    if (fx.sent.size() == 2) {
        ParsedIp f1, f2;
        check(parseIp(fx.sent[0], f1) && parseIp(fx.sent[1], f2), "frag parse");
        check((f1.fragField & 0x2000) != 0, "frag1 MF set");
        check((f1.fragField & 0x1FFF) == 0, "frag1 offset 0");
        check((f2.fragField & 0x2000) == 0, "frag2 MF clear");
        check((f2.fragField & 0x1FFF) == 97, "frag2 offset 97*8=776");
        // 重组后与原始回显一致（ICMP 头前 4 字节会变成 reply + 重算校验和，
        // 因此只比较 type/code 之后的数据部分）
        std::vector<uint8_t> re;
        re.insert(re.end(), f1.payload, f1.payload + f1.payloadLen);
        re.insert(re.end(), f2.payload, f2.payload + f2.payloadLen);
        check(re.size() == plen, "reassembled len");
        check(re[0] == 0 && re[1] == 0, "reassembled icmp type = echo reply");
        check(std::memcmp(re.data() + 8, icmp.data() + 8, plen - 8) == 0,
              "reassembled payload == original");
    }
}

// ================= TCP =================
static void testTcpSynSynAck() {
    std::printf("[netgw: tcp syn -> syn-ack]\n");
    Fixture fx;
    uint32_t server = ip4(93, 184, 216, 34);

    std::vector<uint8_t> syn = buildTcp(np(12345), np(80), 1000, 0, xw::TCP_SYN,
                                        nullptr, 0, 4096, 1400);
    fx.feed(buildIp(xw::NETGW_WATCH_IP_BE, server, 6, syn.data(), syn.size()));

    check(fx.sent.size() == 1, "syn -> one syn-ack");
    check(fx.up.socks.size() == 1, "upstream tcp connect called");
    check(fx.gw.tcpConnsOpened == 1, "tcpConnsOpened == 1");
    if (!fx.up.socks.empty()) {
        check(fx.up.socks[0].isTcp, "upstream sock is tcp");
        check(fx.up.socks[0].dstIp == server, "upstream dst ip");
        check(fx.up.socks[0].dstPort == np(80), "upstream dst port");
    }
    if (fx.sent.size() == 1) {
        ParsedTcp t;
        check(parseTcp(fx.sent[0], t), "syn-ack parse");
        check((t.flags & (xw::TCP_SYN | xw::TCP_ACK)) == (xw::TCP_SYN | xw::TCP_ACK),
              "syn-ack flags");
        check(t.ack == 1001, "syn-ack ack = watchIsn+1");
        check(t.sport == np(80) && t.dport == np(12345), "syn-ack ports swapped");
        // MSS 选项位于 TCP 头内（24 字节头），直接看 IP 负载的 20..23 字节
        ParsedIp ip;
        check(parseIp(fx.sent[0], ip) && ip.payloadLen >= 24, "syn-ack ip parse");
        if (ip.payloadLen >= 24) {
            const uint8_t* tcp = ip.payload;
            check(tcp[20] == 2 && tcp[21] == 4, "syn-ack MSS option present");
            uint16_t mss = uint16_t((tcp[22] << 8) | tcp[23]);
            check(mss == xw::NETGW_TCP_MSS, "syn-ack MSS = 700");
        }
    }
}

static void testTcpFullFlow() {
    std::printf("[netgw: tcp full flow]\n");
    Fixture fx;
    uint32_t server = ip4(93, 184, 216, 34);
    uint32_t myIsn = 0;

    // 1) SYN -> SYN-ACK
    std::vector<uint8_t> syn = buildTcp(np(12345), np(80), 1000, 0, xw::TCP_SYN,
                                        nullptr, 0, 4096, 1400);
    fx.feed(buildIp(xw::NETGW_WATCH_IP_BE, server, 6, syn.data(), syn.size()));
    ParsedTcp sa;
    check(fx.sent.size() == 1 && parseTcp(fx.sent[0], sa), "syn -> syn-ack");
    if (fx.sent.size() != 1) return;
    myIsn = sa.seq;

    // 2) 手表 ACK（纯 ACK，不应产生回包）
    fx.clearSent();
    std::vector<uint8_t> ack1 = buildTcp(np(12345), np(80), 1001, myIsn + 1,
                                         xw::TCP_ACK, nullptr, 0);
    fx.feed(buildIp(xw::NETGW_WATCH_IP_BE, server, 6, ack1.data(), ack1.size()));
    check(fx.sent.empty(), "pure ack -> no reply");

    // 3) 手表发数据 -> 上游
    const char* req = "GET / HTTP/1.0\r\n\r\n";
    size_t reqLen = std::strlen(req);
    fx.clearSent();
    std::vector<uint8_t> d1 = buildTcp(np(12345), np(80), 1001, myIsn + 1,
                                       xw::TCP_PSH | xw::TCP_ACK,
                                       (const uint8_t*)req, reqLen);
    fx.feed(buildIp(xw::NETGW_WATCH_IP_BE, server, 6, d1.data(), d1.size()));
    check(fx.sent.size() == 1, "data -> ack reply");
    if (fx.sent.size() == 1) {
        ParsedTcp t;
        check(parseTcp(fx.sent[0], t) && (t.flags & xw::TCP_ACK) && t.len == 0,
              "data ack flags");
        check(t.ack == 1001 + reqLen, "data ack num");
    }
    // 数据先进入待转发缓冲，loop() 才真正写入上游
    fx.gw.loop();
    check(fx.up.at(1).up2down.size() == reqLen &&
              std::memcmp(fx.up.at(1).up2down.data(), req, reqLen) == 0,
          "data forwarded to upstream");
    check(fx.gw.tcpBytesUp == reqLen, "tcpBytesUp");

    // 4) 上游回数据 -> 手表
    fx.clearSent();
    const char* resp = "HTTP/1.0 200 OK\r\n\r\nhello";
    size_t respLen = std::strlen(resp);
    fx.up.at(1).down2up.assign((const uint8_t*)resp, (const uint8_t*)resp + respLen);
    fx.gw.loop();
    check(fx.sent.size() == 1, "upstream data -> one segment");
    if (fx.sent.size() == 1) {
        ParsedTcp t;
        check(parseTcp(fx.sent[0], t), "upstream data parse");
        check(t.seq == myIsn + 1, "upstream data seq = myIsn+1");
        check(t.ack == 1001 + reqLen, "upstream data ack");
        check(t.len == respLen && std::memcmp(t.data, resp, respLen) == 0,
              "upstream data payload");
        check((t.flags & xw::TCP_PSH) != 0, "upstream data PSH");
    }
    check(fx.gw.tcpBytesDown == respLen, "tcpBytesDown");

    // 5) 手表 ACK 全部数据
    fx.clearSent();
    std::vector<uint8_t> ack2 = buildTcp(np(12345), np(80), 1001 + reqLen,
                                         myIsn + 1 + respLen, xw::TCP_ACK, nullptr, 0);
    fx.feed(buildIp(xw::NETGW_WATCH_IP_BE, server, 6, ack2.data(), ack2.size()));
    check(fx.sent.empty(), "ack -> no reply");

    // 6) 上游 EOF -> 给手表 FIN
    fx.clearSent();
    fx.up.at(1).eof = true;
    fx.gw.loop();
    check(fx.sent.size() == 1, "upstream eof -> fin");
    if (fx.sent.size() == 1) {
        ParsedTcp t;
        check(parseTcp(fx.sent[0], t), "fin parse");
        check((t.flags & xw::TCP_FIN) != 0, "fin flag");
        check(t.seq == myIsn + 1 + respLen, "fin seq");
    }

    // 7) 手表 FIN -> 回 ACK，并向上游 shutdownWrite
    fx.clearSent();
    std::vector<uint8_t> fin = buildTcp(np(12345), np(80), 1001 + reqLen,
                                        myIsn + 2 + respLen, xw::TCP_FIN | xw::TCP_ACK,
                                        nullptr, 0);
    fx.feed(buildIp(xw::NETGW_WATCH_IP_BE, server, 6, fin.data(), fin.size()));
    check(fx.sent.size() == 1, "watch fin -> ack");
    fx.gw.loop();
    check(fx.up.at(1).writeClosed, "upstream write closed after watch fin");

    // 8) 手表 ACK 我们的 FIN -> 连接回收
    std::vector<uint8_t> ack3 = buildTcp(np(12345), np(80), 1002 + reqLen,
                                         myIsn + 3 + respLen, xw::TCP_ACK, nullptr, 0);
    fx.feed(buildIp(xw::NETGW_WATCH_IP_BE, server, 6, ack3.data(), ack3.size()));
    fx.gw.loop();
    check(fx.up.at(1).closed, "conn torn down after both fins acked");
}

static void testTcpWindowMss() {
    std::printf("[netgw: tcp window/mss]\n");
    Fixture fx;
    uint32_t server = ip4(93, 184, 216, 34);
    uint32_t myIsn = 0;

    // SYN 通告 MSS=536、窗口=100
    std::vector<uint8_t> syn = buildTcp(np(23456), np(80), 5000, 0, xw::TCP_SYN,
                                        nullptr, 0, 100, 536);
    fx.feed(buildIp(xw::NETGW_WATCH_IP_BE, server, 6, syn.data(), syn.size()));
    ParsedTcp sa;
    check(fx.sent.size() == 1 && parseTcp(fx.sent[0], sa), "syn -> syn-ack");
    if (fx.sent.size() != 1) return;
    myIsn = sa.seq;

    // 手表 ACK（保持小窗口 100）
    std::vector<uint8_t> ack1 = buildTcp(np(23456), np(80), 5001, myIsn + 1,
                                         xw::TCP_ACK, nullptr, 0, 100);
    fx.feed(buildIp(xw::NETGW_WATCH_IP_BE, server, 6, ack1.data(), ack1.size()));

    // 上游 2000 字节数据
    fx.clearSent();
    std::vector<uint8_t> big(2000);
    for (size_t i = 0; i < big.size(); i++) big[i] = uint8_t(i);
    fx.up.at(1).down2up = big;
    fx.gw.loop();
    // 窗口仅 100 -> 首段只能发 100 字节
    check(fx.sent.size() == 1, "window-limited: one segment");
    if (fx.sent.size() == 1) {
        ParsedTcp t;
        check(parseTcp(fx.sent[0], t) && t.len == 100, "first segment = window (100)");
    }

    // 手表扩大窗口并 ACK
    fx.clearSent();
    std::vector<uint8_t> ack2 = buildTcp(np(23456), np(80), 5001, myIsn + 101,
                                         xw::TCP_ACK, nullptr, 0, 4096);
    fx.feed(buildIp(xw::NETGW_WATCH_IP_BE, server, 6, ack2.data(), ack2.size()));
    fx.gw.loop();
    // 现在按 MSS=536 发送
    check(fx.sent.size() == 1, "mss-limited: one segment");
    if (fx.sent.size() == 1) {
        ParsedTcp t;
        check(parseTcp(fx.sent[0], t) && t.len == 536, "segment = mss (536)");
        check(t.seq == myIsn + 101, "seq continues");
    }
}

static void testTcpRetransmit() {
    std::printf("[netgw: tcp retransmit]\n");
    Fixture fx;
    uint32_t server = ip4(93, 184, 216, 34);

    std::vector<uint8_t> syn = buildTcp(np(34567), np(443), 777, 0, xw::TCP_SYN,
                                        nullptr, 0, 4096, 1400);
    fx.feed(buildIp(xw::NETGW_WATCH_IP_BE, server, 6, syn.data(), syn.size()));
    check(fx.sent.size() == 1, "syn -> syn-ack");
    std::vector<uint8_t> first = fx.sent[0];

    // 手表不应答，超过 RTO(1000ms) 后重传（IP ID/校验和会变，只比较 TCP 段）
    fx.clearSent();
    advance(1100);
    fx.gw.loop();
    check(fx.sent.size() == 1, "rto -> retransmit");
    if (fx.sent.size() == 1) {
        const std::vector<uint8_t>& a = fx.sent[0];
        const std::vector<uint8_t>& b = first;
        bool same = a.size() == b.size() && a.size() >= 20 &&
                    std::memcmp(a.data() + 20, b.data() + 20, a.size() - 20) == 0;
        check(same, "retransmitted tcp segment identical (modulo ip id/csum)");
    }

    // 手表 ACK 后不再重传
    ParsedTcp sa;
    parseTcp(first, sa);
    fx.clearSent();
    std::vector<uint8_t> ack1 = buildTcp(np(34567), np(443), 778, sa.seq + 1,
                                         xw::TCP_ACK, nullptr, 0);
    fx.feed(buildIp(xw::NETGW_WATCH_IP_BE, server, 6, ack1.data(), ack1.size()));
    advance(5000);
    fx.gw.loop();
    check(fx.sent.empty(), "no retransmit after ack");
}

static void testTcpConnectFail() {
    std::printf("[netgw: tcp upstream connect fail]\n");
    Fixture fx;
    fx.up.rejectConnect = true;
    uint32_t server = ip4(93, 184, 216, 34);

    std::vector<uint8_t> syn = buildTcp(np(45678), np(80), 42, 0, xw::TCP_SYN,
                                        nullptr, 0, 4096, 1400);
    fx.feed(buildIp(xw::NETGW_WATCH_IP_BE, server, 6, syn.data(), syn.size()));
    check(fx.sent.size() == 1, "connect fail -> rst");
    if (fx.sent.size() == 1) {
        ParsedTcp t;
        check(parseTcp(fx.sent[0], t) && (t.flags & xw::TCP_RST) != 0, "rst flag");
    }
    // 后续同四元组报文视为新连接（连接已释放）
    fx.clearSent();
    fx.feed(buildIp(xw::NETGW_WATCH_IP_BE, server, 6, syn.data(), syn.size()));
    check(fx.sent.size() == 1, "conn slot freed");
}

static void testTcpDelayedConnect() {
    std::printf("[netgw: tcp delayed upstream connect]\n");
    Fixture fx;
    fx.up.connectDelayed = true;
    uint32_t server = ip4(93, 184, 216, 34);
    uint32_t myIsn = 0;

    std::vector<uint8_t> syn = buildTcp(np(56789), np(80), 9000, 0, xw::TCP_SYN,
                                        nullptr, 0, 4096, 1400);
    fx.feed(buildIp(xw::NETGW_WATCH_IP_BE, server, 6, syn.data(), syn.size()));
    ParsedTcp sa;
    check(fx.sent.size() == 1 && parseTcp(fx.sent[0], sa), "syn -> syn-ack (optimistic)");
    if (fx.sent.size() != 1) return;
    myIsn = sa.seq;

    // 手表 ACK 并立即发数据（上游还没连上）
    std::vector<uint8_t> ack1 = buildTcp(np(56789), np(80), 9001, myIsn + 1,
                                         xw::TCP_ACK, nullptr, 0);
    fx.feed(buildIp(xw::NETGW_WATCH_IP_BE, server, 6, ack1.data(), ack1.size()));
    const char* body = "ping";
    std::vector<uint8_t> d1 = buildTcp(np(56789), np(80), 9001, myIsn + 1,
                                       xw::TCP_PSH | xw::TCP_ACK, (const uint8_t*)body, 4);
    fx.feed(buildIp(xw::NETGW_WATCH_IP_BE, server, 6, d1.data(), d1.size()));
    fx.gw.loop();
    check(fx.up.at(1).up2down.empty(), "data held while upstream connecting");

    fx.up.at(1).connected = true;
    fx.gw.loop();
    check(fx.up.at(1).up2down.size() == 4, "data flushed after upstream ready");
}

// ================= UDP =================
static void testDnsViaGateway() {
    std::printf("[netgw: dns via gateway]\n");
    Fixture fx;

    // 手表 -> 10.1.10.1:53 的 DNS 查询
    const uint8_t q[12] = {0xAB, 0xCD, 0x01, 0x00, 0, 1, 0, 0, 0, 0, 0, 0};
    std::vector<uint8_t> udp = buildUdp(np(40000), np(53), q, sizeof(q));
    fx.feed(buildIp(xw::NETGW_WATCH_IP_BE, xw::NETGW_LOCAL_IP_BE, 17, udp.data(), udp.size()));

    check(fx.up.socks.size() == 1, "dns flow created");
    if (!fx.up.socks.empty()) {
        check(!fx.up.socks[0].isTcp, "dns sock is udp");
        check(fx.up.socks[0].dstIp == ip4(223, 5, 5, 5), "dns forwarded to 223.5.5.5");
        check(fx.up.socks[0].dstPort == np(53), "dns dst port 53");
        check(fx.up.socks[0].up2down.size() == sizeof(q), "dns query forwarded");
    }
    check(fx.gw.udpFlows == 1, "udpFlows == 1");

    // 非 53 端口发往网关 IP：应被忽略
    fx.clearSent();
    std::vector<uint8_t> udp2 = buildUdp(np(40001), np(9999), q, sizeof(q));
    fx.feed(buildIp(xw::NETGW_WATCH_IP_BE, xw::NETGW_LOCAL_IP_BE, 17, udp2.data(), udp2.size()));
    check(fx.up.socks.size() == 1, "non-dns to gateway ignored");

    // 上游应答 -> 源改写回 10.1.10.1
    fx.clearSent();
    const uint8_t ans[16] = {0xAB, 0xCD, 0x81, 0x80, 0, 1, 0, 1, 0, 0, 0, 0,
                             1, 2, 3, 4};
    fx.up.at(1).down2up.assign(ans, ans + sizeof(ans));
    fx.gw.loop();
    check(fx.sent.size() == 1, "dns reply -> watch");
    if (fx.sent.size() == 1) {
        ParsedUdp u;
        check(parseUdp(fx.sent[0], u), "dns reply parse");
        ParsedIp ip;
        parseIp(fx.sent[0], ip);
        check(ip.src == xw::NETGW_LOCAL_IP_BE, "dns reply src = 10.1.10.1");
        check(u.sport == np(53) && u.dport == np(40000), "dns reply ports");
        check(u.len == sizeof(ans) && std::memcmp(u.data, ans, sizeof(ans)) == 0,
              "dns reply payload");
    }
}

static void testUdpRelay() {
    std::printf("[netgw: udp relay external]\n");
    Fixture fx;
    uint32_t server = ip4(8, 8, 8, 8);

    const uint8_t q[12] = {0x11, 0x22, 0x01, 0x00, 0, 1, 0, 0, 0, 0, 0, 0};
    std::vector<uint8_t> udp = buildUdp(np(40002), np(53), q, sizeof(q));
    fx.feed(buildIp(xw::NETGW_WATCH_IP_BE, server, 17, udp.data(), udp.size()));
    check(fx.up.socks.size() == 1, "udp flow created");
    if (!fx.up.socks.empty()) {
        check(fx.up.socks[0].dstIp == server, "udp dst = external");
        check(fx.up.socks[0].up2down.size() == sizeof(q), "udp payload forwarded");
    }

    fx.clearSent();
    const uint8_t ans[12] = {0x11, 0x22, 0x81, 0x80, 0, 1, 0, 0, 0, 0, 0, 0};
    fx.up.at(1).down2up.assign(ans, ans + sizeof(ans));
    fx.gw.loop();
    check(fx.sent.size() == 1, "udp reply -> watch");
    if (fx.sent.size() == 1) {
        ParsedIp ip;
        ParsedUdp u;
        check(parseIp(fx.sent[0], ip) && parseUdp(fx.sent[0], u), "udp reply parse");
        check(ip.src == server, "udp reply src = external server");
        check(ip.dst == xw::NETGW_WATCH_IP_BE, "udp reply dst = watch");
        check(u.sport == np(53) && u.dport == np(40002), "udp reply ports");
    }
}

// ================= 过滤 =================
static void testSourceFilter() {
    std::printf("[netgw: source filter]\n");
    Fixture fx;
    uint32_t server = ip4(93, 184, 216, 34);

    // 非手表来源的 TCP/UDP/ICMP 一律忽略
    std::vector<uint8_t> syn = buildTcp(np(1234), np(80), 1, 0, xw::TCP_SYN, nullptr, 0);
    fx.feed(buildIp(ip4(10, 1, 10, 3), server, 6, syn.data(), syn.size()));
    check(fx.sent.empty() && fx.up.socks.empty(), "tcp from non-watch ignored");

    std::vector<uint8_t> udp = buildUdp(np(1234), np(53), nullptr, 0);
    fx.feed(buildIp(ip4(10, 1, 10, 3), server, 17, udp.data(), udp.size()));
    check(fx.sent.empty() && fx.up.socks.empty(), "udp from non-watch ignored");

    uint8_t icmp[8] = {8, 0, 0, 0, 0, 1, 0, 1};
    fx.feed(buildIp(ip4(10, 1, 10, 3), server, 1, icmp, sizeof(icmp)));
    check(fx.sent.empty(), "icmp from non-watch ignored");

    // 太短/非 IPv4 的包不应崩溃
    uint8_t junk[10] = {0};
    fx.gw.onIpPacket(junk, sizeof(junk));
    uint8_t v6[20] = {0x60};
    fx.gw.onIpPacket(v6, sizeof(v6));
    check(true, "garbage packets do not crash");
}

int main() {
    testDhcp();
    testIcmp();
    testIcmpFragmentation();
    testTcpSynSynAck();
    testTcpFullFlow();
    testTcpWindowMss();
    testTcpRetransmit();
    testTcpConnectFail();
    testTcpDelayedConnect();
    testDnsViaGateway();
    testUdpRelay();
    testSourceFilter();
    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
