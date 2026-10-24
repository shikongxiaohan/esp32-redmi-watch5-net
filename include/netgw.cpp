/*
 * 手表网络网关实现（TCP/UDP/ICMP 中继 + IP 分片）
 * 见 netgw.h 的设计说明。
 */
#include "netgw.h"
#include "iputil.h"
#include "dhcp.h"

#include <cstring>
#include <vector>

#if defined(ESP32) || defined(ESP_PLATFORM)
#include <Arduino.h>
#include <esp_random.h>
#define XW_NOW_MS() millis()
static inline uint32_t xwRandom() { return esp_random(); }
#else
#include <chrono>
#include <random>
static uint32_t xwHostNowMs() {
    static auto start = std::chrono::steady_clock::now();
    return uint32_t(std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - start).count());
}
#define XW_NOW_MS() xwHostNowMs()
static uint32_t xwRandom() {
    static std::mt19937 rng(0x5A5A5A5A);
    return rng();
}
#endif

namespace xw {

// DNS 服务器（手表 DHCP 未请求或查询网关时使用）
static const uint32_t DNS_SERVER_BE = 0x050505DF; // 223.5.5.5（网络字节小端读法）

// 空闲回收与重传参数
static const uint32_t TCP_IDLE_TIMEOUT_MS = 300000;
static const uint32_t UDP_IDLE_TIMEOUT_MS = 120000;
static const uint32_t TCP_RTO_MS = 1000;
static const uint32_t TCP_FIN_WAIT_MS = 60000;
static const uint8_t  TCP_MAX_TRIES = 8;
// 上游->手表 缓冲上限（超过则暂停读取上游，形成背压）
static const size_t UP2WATCH_HIGH_WATER = 16384;

// 序列号比较（处理回绕）
static inline bool seqLt(uint32_t a, uint32_t b) { return int32_t(a - b) < 0; }
static inline bool seqLeq(uint32_t a, uint32_t b) { return int32_t(a - b) <= 0; }

void NetGw::_log(const char* msg) {
    if (onLog) onLog(msg);
}

uint32_t NetGw::_nowMs() {
    return nowMs ? nowMs() : uint32_t(XW_NOW_MS());
}

void NetGw::_resetAll() {
    for (auto& c : _conns) {
        if (c.used && c.sock >= 0) _up.closeFd(c.sock);
        c = TcpConn();
    }
    for (auto& f : _udp) {
        if (f.used && f.sock >= 0) _up.closeFd(f.sock);
        f = UdpFlow();
    }
}

// ===================== IP 包构造 =====================

void NetGw::_sendIpPacket(uint32_t srcBe, uint32_t dstBe, uint8_t proto,
                          const uint8_t* payload, size_t len) {
    if (!_send) return;
    const size_t maxPayload = NETGW_MTU - 20; // 780
    if (20 + len <= NETGW_MTU) {
        // 单包
        uint8_t pkt[NETGW_MTU];
        size_t total = 20 + len;
        pkt[0] = 0x45;
        pkt[1] = 0x00;
        pkt[2] = uint8_t(total >> 8);
        pkt[3] = uint8_t(total);
        pkt[4] = uint8_t(_ipId >> 8);
        pkt[5] = uint8_t(_ipId);
        _ipId++;
        pkt[6] = 0x40; // DF
        pkt[7] = 0x00;
        pkt[8] = 64;   // TTL
        pkt[9] = proto;
        pkt[10] = 0; pkt[11] = 0; // checksum 占位
        std::memcpy(pkt + 12, &srcBe, 4);
        std::memcpy(pkt + 16, &dstBe, 4);
        if (len) std::memcpy(pkt + 20, payload, len);
        uint16_t csum = ipChecksum(pkt, 20);
        pkt[10] = uint8_t(csum >> 8);
        pkt[11] = uint8_t(csum);
        _send(pkt, total);
        txPackets++;
        return;
    }

    if (len == 0) return;

    // 分片（按 8 字节边界）
    size_t chunk = maxPayload & ~size_t(7); // 768
    size_t off = 0;
    bool more = true;
    while (more) {
        size_t remain = len - off;
        size_t n = remain <= chunk ? remain : chunk;
        if (remain <= chunk) more = false;
        uint8_t pkt[NETGW_MTU];
        size_t total = 20 + n;
        pkt[0] = 0x45;
        pkt[1] = 0x00;
        pkt[2] = uint8_t(total >> 8);
        pkt[3] = uint8_t(total);
        pkt[4] = uint8_t(_ipId >> 8);
        pkt[5] = uint8_t(_ipId);
        uint16_t fragField = uint16_t((off / 8) & 0x1FFF);
        if (more) fragField |= 0x2000; // MF
        pkt[6] = uint8_t(fragField >> 8);
        pkt[7] = uint8_t(fragField);
        pkt[8] = 64;
        pkt[9] = proto;
        pkt[10] = 0; pkt[11] = 0;
        std::memcpy(pkt + 12, &srcBe, 4);
        std::memcpy(pkt + 16, &dstBe, 4);
        std::memcpy(pkt + 20, payload + off, n);
        uint16_t csum = ipChecksum(pkt, 20);
        pkt[10] = uint8_t(csum >> 8);
        pkt[11] = uint8_t(csum);
        _send(pkt, total);
        txPackets++;
        off += n;
    }
    _ipId++;
}

// 构造完整 TCP 段（含头与校验和）到 out；返回段长（0 表示失败）
size_t NetGw::_buildTcpSegment(TcpConn& c, uint8_t flags, const uint8_t* data, size_t len,
                               uint8_t* out, size_t outCap) {
    size_t hdrLen = 20;
    if (flags & TCP_SYN) hdrLen = 24; // 携带 MSS 选项
    size_t total = hdrLen + len;
    if (total > outCap) return 0;

    // 回程段由"服务器侧"发给手表：src = 手表连接的目标端口，dst = 手表侧端口
    // （与 AstroBox/ipstack 的服务器伪装模型一致：手表看到的是与真实服务器的连接）
    std::memcpy(out, &c.dstPort, 2);      // src port（服务器侧端口）
    std::memcpy(out + 2, &c.watchPort, 2); // dst port（手表侧端口）
    std::memcpy(out + 4, &c.nextSnd, 4); // seq
    std::memcpy(out + 8, &c.nextRcv, 4); // ack
    out[12] = uint8_t(hdrLen / 4) << 4;
    out[13] = flags;
    uint16_t win = 4096;
    std::memcpy(out + 14, &win, 2);
    out[16] = 0; out[17] = 0; // checksum 占位
    out[18] = 0; out[19] = 0; // urgent
    if (flags & TCP_SYN) {
        out[20] = 0x02; out[21] = 0x04; // MSS option
        out[22] = uint8_t(NETGW_TCP_MSS >> 8);
        out[23] = uint8_t(NETGW_TCP_MSS);
    }
    if (len) std::memcpy(out + hdrLen, data, len);

    // TCP 校验和（伪首部：src=服务器 IP, dst=手表 IP）
    uint16_t csum = tcpChecksum(c.dstIp, NETGW_WATCH_IP_BE, out, total);
    std::memcpy(out + 16, &csum, 2);
    return total;
}

// 构造并发送一个 TCP 段（seq = c.nextSnd, ack = c.nextRcv）
void NetGw::_sendTcpSegment(TcpConn& c, uint8_t flags, const uint8_t* data, size_t len) {
    uint8_t seg[NETGW_TCP_MSS + 24];
    size_t total = _buildTcpSegment(c, flags, data, len, seg, sizeof(seg));
    if (total == 0) return;
    _sendIpPacket(c.dstIp, NETGW_WATCH_IP_BE, IP_PROTO_TCP, seg, total);
}

void NetGw::_sendUdpPacket(uint32_t srcBe, uint32_t dstBe, uint16_t sport, uint16_t dport,
                           const uint8_t* data, size_t len) {
    uint8_t udp[8 + 1528];
    if (8 + len > sizeof(udp)) return;
    std::memcpy(udp, &sport, 2);
    std::memcpy(udp + 2, &dport, 2);
    uint16_t ulen = uint16_t(8 + len);
    std::memcpy(udp + 4, &ulen, 2);
    udp[6] = 0; udp[7] = 0;
    if (len) std::memcpy(udp + 8, data, len);
    uint16_t csum = udpChecksum(srcBe, dstBe, udp, 8 + len);
    std::memcpy(udp + 6, &csum, 2);
    _sendIpPacket(srcBe, dstBe, IP_PROTO_UDP, udp, 8 + len);
}

// ===================== 收包处理 =====================

void NetGw::onIpPacket(const uint8_t* pkt, size_t len) {
    if (!_send || len < 20) return;
    rxPackets++;
    _handleIp(pkt, len);
}

void NetGw::_handleIp(const uint8_t* pkt, size_t len) {
    if ((pkt[0] >> 4) != 4) return;
    const uint8_t* payload;
    size_t plen;
    if (!ipPayload(pkt, len, payload, plen)) return;
    const IpHeader* ip = (const IpHeader*)pkt;
    uint32_t srcBe = ip->srcIp;
    uint32_t dstBe = ip->dstIp;

    // DHCP 优先代答（与 AstroBox 一致：先于协议栈处理）。
    // DISCOVER/REQUEST 源为 0.0.0.0，不能用手表 IP 过滤；端口 68<->67。
    if (ip->protocol == IP_PROTO_UDP && plen >= 8) {
        const UdpHeader* uh = (const UdpHeader*)payload;
        if (uh->srcPort == 0x4400 || uh->dstPort == 0x4300) {
            std::vector<uint8_t> reply;
            if (dhcpHandlePacket(pkt, len, reply) && !reply.empty()) {
                _send(reply.data(), reply.size());
                txPackets++;
                dhcpReplies++;
            }
            return;
        }
    }

    switch (ip->protocol) {
        case IP_PROTO_TCP: _handleTcp(srcBe, dstBe, payload, plen); break;
        case IP_PROTO_UDP: _handleUdp(srcBe, dstBe, payload, plen); break;
        case IP_PROTO_ICMP: _handleIcmp(srcBe, dstBe, payload, plen); break;
        default: break;
    }
}

void NetGw::_handleTcp(uint32_t srcBe, uint32_t dstBe, const uint8_t* tcp, size_t len) {
    if (len < 20) return;
    const TcpHeader* th = (const TcpHeader*)tcp;
    uint16_t sport = th->srcPort;   // 大端
    uint16_t dport = th->dstPort;   // 大端
    uint8_t flags = th->flags;
    size_t hdrLen = size_t(th->dataOffset >> 4) * 4;
    if (hdrLen < 20 || hdrLen > len) return;
    const uint8_t* data = tcp + hdrLen;
    size_t dlen = len - hdrLen;
    uint32_t seq = th->seq;
    uint32_t ack = th->ack;
    uint16_t win = th->window;

    // 仅接受来自手表的包
    if (srcBe != NETGW_WATCH_IP_BE) return;

    TcpConn* c = _findConn(sport, dstBe, dport);

    if (flags & TCP_RST) {
        if (c) _connTeardown(*c, false);
        return;
    }

    if (!c) {
        if (!(flags & TCP_SYN)) return; // 无连接的段，忽略
        c = _allocConn();
        if (!c) { dropped++; return; }
        c->used = true;
        c->dstIp = dstBe;
        c->watchPort = sport;
        c->dstPort = dport;
        c->watchIsn = seq;
        c->myIsn = (xwRandom() & 0x00FFFFFFu) << 8; // 任意 ISN
        c->nextRcv = seq + 1;
        c->nextSnd = c->myIsn;
        c->peerAcked = c->myIsn; // SYN 尚未被确认
        c->watchWin = win;
        c->lastActiveMs = _nowMs();

        // 解析手表 SYN 中的 MSS
        if (hdrLen > 20) {
            const uint8_t* opt = tcp + 20;
            size_t oleft = hdrLen - 20;
            while (oleft >= 2) {
                if (opt[0] == 0) break;        // EOL
                if (opt[0] == 1) { opt++; oleft--; continue; } // NOP
                uint8_t olen = opt[1];
                if (olen < 2 || size_t(olen) > oleft) break;
                if (opt[0] == 2 && olen == 4) {
                    uint16_t mss = uint16_t(opt[2] << 8) | opt[3];
                    if (mss >= 536 && mss < c->watchMss) c->watchMss = mss;
                }
                opt += olen;
                oleft -= olen;
            }
        }

        // 打开上游连接（非阻塞）
        c->sock = _up.tcpConnect(dstBe, dport);
        tcpConnsOpened++;
        if (c->sock < 0) {
            // 上游不可用：直接 RST
            _log("netgw: upstream tcp connect failed immediately");
            _connTeardown(*c, true);
            return;
        }
        // 立即 SYN-ACK（乐观：不等上游连上）。
        // 注意：SYN-ACK 段的 seq 必须是 myIsn 本身（SYN 占用该序号），
        // 发送完毕后再把 nextSnd 前移 1，否则手表对本端 ACK 会多算 1，
        // 导致 inFlight = nextSnd - peerAcked 下溢、窗口恒为 0。
        c->synAckSent = true;
        // 记录 SYN-ACK 段用于重传
        c->unacked.clear();
        {
            uint8_t seg[24];
            size_t n = _buildTcpSegment(*c, TCP_SYN | TCP_ACK, nullptr, 0, seg, sizeof(seg));
            if (n == 0) { _connTeardown(*c, true); return; }
            c->unacked.assign(seg, seg + n);
            _sendIpPacket(c->dstIp, NETGW_WATCH_IP_BE, IP_PROTO_TCP, seg, n);
            txPackets++;
        }
        c->nextSnd = c->myIsn + 1; // SYN 消耗一个序号
        c->lastTxMs = _nowMs();
        c->txTries = 1;
        return;
    }

    c->lastActiveMs = _nowMs();
    c->watchWin = win;

    // ACK 进展
    if ((flags & TCP_ACK) && seqLeq(c->peerAcked, ack)) {
        if (ack != c->peerAcked) {
            c->peerAcked = ack;
            c->dupAcks = 0;
            c->txTries = 0;
            if (seqLeq(c->nextSnd, ack)) c->unacked.clear();
        } else if (dlen == 0 && !c->unacked.empty()) {
            if (c->dupAcks < 255) c->dupAcks++;
        }
    }

    // 数据处理（仅接受按序数据）
    bool needAck = false;
    if (dlen > 0) {
        if (seq == c->nextRcv) {
            c->tx2up.insert(c->tx2up.end(), data, data + dlen);
            c->nextRcv = seq + dlen;
            tcpBytesUp += dlen;
            needAck = true;
        } else if (seqLt(seq, c->nextRcv)) {
            // 重传：裁剪已确认部分
            uint32_t skip = c->nextRcv - seq;
            if (skip < dlen) {
                const uint8_t* nd = data + skip;
                size_t nl = dlen - skip;
                c->tx2up.insert(c->tx2up.end(), nd, nd + nl);
                c->nextRcv = seq + dlen;
                tcpBytesUp += nl;
            }
            // 否则整段重复，丢弃
        } else {
            // 乱序间隙：丢弃，等待重传
            _sendTcpSegment(*c, TCP_ACK, nullptr, 0);
            return;
        }
    }

    // FIN 处理（数据之后）
    if (flags & TCP_FIN) {
        c->finFromWatch = true;
        c->nextRcv = c->nextRcv + 1;
        needAck = true;
    }

    // 仅对携带数据/FIN 的段回 ACK；纯 ACK（含握手完成 ACK）不应答
    if (needAck) _sendTcpSegment(*c, TCP_ACK, nullptr, 0);
}

void NetGw::_handleUdp(uint32_t srcBe, uint32_t dstBe, const uint8_t* udp, size_t len) {
    if (len < 8) return;
    if (srcBe != NETGW_WATCH_IP_BE) return;
    const UdpHeader* uh = (const UdpHeader*)udp;
    uint16_t sport = uh->srcPort;
    uint16_t dport = uh->dstPort;
    const uint8_t* data = udp + 8;
    size_t dlen = len - 8;

    UdpFlow* f = _findUdp(sport, dstBe, dport);
    if (!f) {
        bool viaGw = (dstBe == NETGW_LOCAL_IP_BE);
        // 网关 IP 上只代答 DNS（端口 53）
        if (viaGw && dport != 0x3500) return;
        f = _allocUdp();
        if (!f) { dropped++; return; }
        f->used = true;
        f->watchPort = sport;
        f->dstPort = dport;
        f->dnsViaGateway = viaGw;
        f->dstIp = viaGw ? DNS_SERVER_BE : dstBe;
        f->sock = _up.udpConnect(f->dstIp, dport);
        f->lastActiveMs = _nowMs();
        udpFlows++;
    }
    f->lastActiveMs = _nowMs();
    if (f->sock >= 0 && dlen > 0) {
        int w = _up.sendData(f->sock, data, dlen);
        if (w > 0) udpBytesUp += uint32_t(w);
    }
}

void NetGw::_handleIcmp(uint32_t srcBe, uint32_t dstBe, const uint8_t* icmp, size_t len) {
    if (len < 8) return;
    if (srcBe != NETGW_WATCH_IP_BE) return;
    if (icmp[0] != 8) return; // 仅 echo request
    uint8_t reply[1472];
    if (len > sizeof(reply)) return;
    std::memcpy(reply, icmp, len);
    reply[0] = 0; // echo reply
    reply[2] = 0; reply[3] = 0; // checksum 清零后重算
    uint16_t csum = ipChecksum(reply, len);
    reply[2] = uint8_t(csum >> 8);
    reply[3] = uint8_t(csum);
    _sendIpPacket(dstBe, NETGW_WATCH_IP_BE, IP_PROTO_ICMP, reply, len);
    icmpReplies++;
}

// ===================== 连接管理 =====================

NetGw::TcpConn* NetGw::_findConn(uint16_t watchPort, uint32_t dstIp, uint16_t dstPort) {
    for (auto& c : _conns) {
        if (c.used && c.watchPort == watchPort && c.dstIp == dstIp && c.dstPort == dstPort)
            return &c;
    }
    return nullptr;
}

NetGw::TcpConn* NetGw::_allocConn() {
    for (auto& c : _conns) if (!c.used) return &c;
    return nullptr;
}

NetGw::UdpFlow* NetGw::_findUdp(uint16_t watchPort, uint32_t dstIp, uint16_t dstPort) {
    for (auto& f : _udp) {
        if (f.used && f.watchPort == watchPort && f.dstIp == dstIp && f.dstPort == dstPort)
            return &f;
    }
    return nullptr;
}

NetGw::UdpFlow* NetGw::_allocUdp() {
    for (auto& f : _udp) if (!f.used) return &f;
    return nullptr;
}

void NetGw::_connTeardown(TcpConn& c, bool sendRst) {
    if (sendRst) {
        _sendTcpSegment(c, TCP_RST | TCP_ACK, nullptr, 0);
    }
    if (c.sock >= 0) { _up.closeFd(c.sock); c.sock = -1; }
    c.used = false;
}

// ===================== 主循环 =====================

void NetGw::loop() {
    uint32_t now = _nowMs();
    for (auto& c : _conns) {
        if (c.used) _connService(c);
    }
    for (auto& f : _udp) {
        if (f.used) _udpService(f);
    }
    // 空闲回收
    for (auto& c : _conns) {
        if (!c.used) continue;
        if (now - c.lastActiveMs > TCP_IDLE_TIMEOUT_MS) {
            _log("netgw: tcp conn idle timeout");
            _connTeardown(c, false);
        }
    }
    for (auto& f : _udp) {
        if (!f.used) continue;
        if (now - f.lastActiveMs > UDP_IDLE_TIMEOUT_MS) {
            if (f.sock >= 0) { _up.closeFd(f.sock); f.sock = -1; }
            f.used = false;
        }
    }
}

void NetGw::_connService(TcpConn& c) {
    if (c.sock < 0) { _connTeardown(c, true); return; }
    uint32_t now = _nowMs();

    // 1) 上游连接状态
    if (_up.hasError(c.sock)) {
        _log("netgw: upstream connect failed");
        _connTeardown(c, true);
        return;
    }
    bool upReady = _up.canSend(c.sock);

    // 2) 手表 -> 上游
    if (upReady && !c.tx2up.empty()) {
        size_t off = 0;
        while (off < c.tx2up.size()) {
            int w = _up.sendData(c.sock, c.tx2up.data() + off, c.tx2up.size() - off);
            if (w <= 0) break;
            off += size_t(w);
        }
        if (off > 0) c.tx2up.erase(c.tx2up.begin(), c.tx2up.begin() + off);
    }
    // 手表 FIN 且数据转发完毕：通知上游本方发送结束
    if (c.finFromWatch && c.tx2up.empty() && !c.upstreamWriteClosed) {
        c.upstreamWriteClosed = true;
        _up.shutdownWrite(c.sock);
    }

    // 3) 上游 -> 手表（读取，受背压限制）
    if (upReady && !c.upstreamClosed && c.up2watch.size() < UP2WATCH_HIGH_WATER) {
        uint8_t buf[1024];
        for (int i = 0; i < 4; i++) {
            int r = _up.recvData(c.sock, buf, sizeof(buf));
            if (r <= 0) {
                if (r == 0) c.upstreamClosed = true;
                break;
            }
            c.up2watch.insert(c.up2watch.end(), buf, buf + r);
            tcpBytesDown += r;
        }
    }

    // 4) 上游 -> 手表（发送，受窗口与 MSS 限制）
    if (!c.up2watch.empty()) {
        uint32_t inFlight = c.nextSnd - c.peerAcked;
        uint32_t win = c.watchWin > inFlight ? c.watchWin - inFlight : 0;
        if (win > 0) {
            size_t n = c.up2watch.size();
            if (n > c.watchMss) n = c.watchMss;
            if (n > win) n = win;
            // 构建完整段 -> 记录 -> 发送（重传时原样重发）
            uint8_t seg[NETGW_TCP_MSS + 24];
            size_t segLen = _buildTcpSegment(c, TCP_ACK | TCP_PSH, c.up2watch.data(), n,
                                             seg, sizeof(seg));
            if (segLen == 0) return;
            c.unacked.assign(seg, seg + segLen);
            _sendIpPacket(c.dstIp, NETGW_WATCH_IP_BE, IP_PROTO_TCP, seg, segLen);
            txPackets++;
            c.nextSnd += n;
            c.up2watch.erase(c.up2watch.begin(), c.up2watch.begin() + n);
            c.lastTxMs = now;
            c.txTries = 1;
            c.dupAcks = 0;
        }
    }

    // 5) 重传（RTO 或 3 次重复 ACK）
    if (!c.unacked.empty() && c.synAckSent) {
        bool timeout = (now - c.lastTxMs) > TCP_RTO_MS;
        bool fast = c.dupAcks >= 3;
        if ((timeout || fast) && c.txTries < TCP_MAX_TRIES) {
            _sendIpPacket(c.dstIp, NETGW_WATCH_IP_BE, IP_PROTO_TCP,
                          c.unacked.data(), c.unacked.size());
            c.lastTxMs = now;
            c.txTries++;
            c.dupAcks = 0;
        } else if (c.txTries >= TCP_MAX_TRIES) {
            _log("netgw: tcp retransmit limit, teardown");
            _connTeardown(c, true);
            return;
        }
    }

    // 6) 上游 EOF 且数据发完 -> 给手表发 FIN
    if (c.upstreamClosed && c.up2watch.empty() && !c.finToWatch) {
        uint8_t seg[24];
        size_t segLen = _buildTcpSegment(c, TCP_FIN | TCP_ACK, nullptr, 0, seg, sizeof(seg));
        if (segLen > 0) {
            c.unacked.assign(seg, seg + segLen);
            _sendIpPacket(c.dstIp, NETGW_WATCH_IP_BE, IP_PROTO_TCP, seg, segLen);
            txPackets++;
        }
        c.nextSnd += 1;
        c.finToWatch = true;
        c.lastTxMs = now;
        c.txTries = 1;
        c.dupAcks = 0;
    }

    // 7) 双向关闭且被确认 -> 回收
    if (c.finFromWatch && c.finToWatch) {
        if (seqLeq(c.nextSnd, c.peerAcked) || (now - c.lastActiveMs) > TCP_FIN_WAIT_MS) {
            _connTeardown(c, false);
        }
    }
}

void NetGw::_udpService(UdpFlow& f) {
    if (f.sock < 0) return;
    uint8_t buf[1536];
    for (int i = 0; i < 4; i++) {
        int r = _up.recvData(f.sock, buf, sizeof(buf));
        if (r <= 0) break;
        udpBytesDown += uint32_t(r);
        // 应答：源为"手表请求的目标"（网关代答时为 10.1.10.1）
        uint32_t srcBe = f.dnsViaGateway ? NETGW_LOCAL_IP_BE : f.dstIp;
        _sendUdpPacket(srcBe, NETGW_WATCH_IP_BE, f.dstPort, f.watchPort, buf, size_t(r));
    }
}

} // namespace xw
