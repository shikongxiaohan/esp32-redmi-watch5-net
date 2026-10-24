/*
 * 手表网络网关（NetGw）
 *
 * 红米 Watch 5 通过 NetWork 通道（明文）收发原始 IPv4 包。
 * 本模块在 ESP32 上实现一个最小用户态网络栈，替代 AstroBox 的 ipstack：
 *
 *   - DHCP 应答（见 dhcp.cpp）：手表拿到 10.1.10.2，网关 10.1.10.1
 *   - ICMP echo：直接应答（连通性探测）
 *   - TCP 中继：为手表的每个 TCP 连接维护一个上游 socket，双向搬运字节
 *   - UDP 中继：DNS 等无连接流量按流转发
 *
 * 为什么不直接用 lwIP netif + NAPT：Arduino 框架预编译的 lwIP 未开启
 * IP_NAPT，且自定义 netif 涉及 tcpip 线程安全问题；用户态中继行为
 * 与 AstroBox 参考实现完全一致，且可在主机端完整单测。
 *
 * 线程模型：onIpPacket 在蓝牙回调上下文被调用，只做纯内存操作
 * （拆包、状态更新、缓冲），不做阻塞 IO；上游 socket 均为非阻塞，
 * 实际的收发在 loop() 中完成。
 */
#pragma once

#include <cstdint>
#include <cstddef>
#include <functional>
#include <vector>

namespace xw {

// 网关侧参数（与 AstroBox 网络栈一致）。
// 字节序约定同 iputil.h：网络字节的小端原生读法。10.1.10.1 -> 0x010A010A。
static const uint32_t NETGW_LOCAL_IP_BE = 0x010A010A;   // 10.1.10.1
static const uint32_t NETGW_WATCH_IP_BE = 0x020A010A;   // 10.1.10.2
static const uint16_t NETGW_MTU = 800;                  // 通道 MTU（含 IP 头）
static const uint16_t NETGW_TCP_MSS = 700;              // TCP 载荷上限
static const size_t   NETGW_MAX_CONNS = 6;              // 并发 TCP 连接数
static const size_t   NETGW_MAX_UDP_FLOWS = 8;          // 并发 UDP 流数

// 上游 socket 抽象：ESP32 用真实 BSD socket；主机测试注入假实现
class Upstream {
public:
    virtual ~Upstream() = default;
    // 打开到 dstIp:dstPort 的 TCP 连接（非阻塞；立即返回 fd 或 -1）
    virtual int tcpConnect(uint32_t dstIpBe, uint16_t dstPortBe) = 0;
    // 打开 UDP socket 并连接 dst（返回 fd 或 -1）
    virtual int udpConnect(uint32_t dstIpBe, uint16_t dstPortBe) = 0;
    virtual void closeFd(int fd) = 0;
    // 关闭 socket 的写方向（通知上游本方发送结束，如 HTTP EOF）
    virtual void shutdownWrite(int fd) = 0;
    // 返回发送的字节数；<0 表示 would-block 或错误
    virtual int sendData(int fd, const uint8_t* data, size_t len) = 0;
    // 返回接收的字节数；0=对端关闭；<0=would-block 或错误
    virtual int recvData(int fd, uint8_t* buf, size_t cap) = 0;
    // TCP: 连接已建立（可写/可读）；UDP: 恒 true
    virtual bool canSend(int fd) = 0;
    // TCP: 连接失败或被重置
    virtual bool hasError(int fd) = 0;
};

class NetGw {
public:
    using SendFn = std::function<void(const uint8_t* pkt, size_t len)>;

    NetGw(Upstream& up) : _up(up) {}

    void begin(SendFn sendFn) { _send = std::move(sendFn); }
    void end() { _send = nullptr; _resetAll(); }

    // 时间源（主机测试可注入假时钟）；默认 millis()
    std::function<uint32_t()> nowMs;

    // 蓝牙上下文调用：处理一个 IP 包（纯内存操作）
    void onIpPacket(const uint8_t* pkt, size_t len);

    // 主循环：驱动上游 socket 收发、重传、超时回收
    void loop();

    // 统计
    uint32_t rxPackets = 0;
    uint32_t txPackets = 0;
    uint32_t dhcpReplies = 0;
    uint32_t icmpReplies = 0;
    uint32_t tcpConnsOpened = 0;
    uint32_t tcpBytesUp = 0;    // 手表 -> 上游
    uint32_t tcpBytesDown = 0;  // 上游 -> 手表
    uint32_t udpBytesUp = 0;    // UDP 上行（性能监控统计总吞吐用）
    uint32_t udpBytesDown = 0;  // UDP 下行
    uint32_t udpFlows = 0;
    uint32_t dropped = 0;

    // 总字节（TCP + UDP），供性能监控读取
    uint32_t totalBytesUp() const { return tcpBytesUp + udpBytesUp; }
    uint32_t totalBytesDown() const { return tcpBytesDown + udpBytesDown; }

    std::function<void(const char*)> onLog;

private:
    Upstream& _up;
    SendFn _send;
    uint16_t _ipId = 0;

    // ---- TCP 连接 ----
    struct TcpConn {
        bool used = false;
        uint32_t dstIp = 0;      // 大端
        uint16_t watchPort = 0;  // 大端
        uint16_t dstPort = 0;    // 大端
        uint32_t watchIsn = 0;
        uint32_t myIsn = 0;
        uint32_t nextRcv = 0;    // 期望的手表 seq
        uint32_t nextSnd = 0;    // 我们的下一个发送 seq
        uint32_t peerAcked = 0;  // 手表已确认我们的数据到 seq
        uint16_t watchWin = 0;
        uint16_t watchMss = 700; // 手表 SYN 通告的 MSS（限制我们的段长）
        uint8_t dupAcks = 0;
        bool synAckSent = false;
        bool finFromWatch = false;
        bool finToWatch = false;
        bool upstreamClosed = false;   // 上游已 EOF（不再有数据到来）
        bool upstreamWriteClosed = false; // 已通知上游本方发送结束
        int sock = -1;
        // 手表 -> 上游 待写数据
        std::vector<uint8_t> tx2up;
        // 上游 -> 手表 待发数据（受 watchWin 限制）
        std::vector<uint8_t> up2watch;
        // 已发出但未被确认的段（用于重传）
        std::vector<uint8_t> unacked;
        uint32_t lastTxMs = 0;
        uint8_t txTries = 0;
        uint32_t lastActiveMs = 0;
    };
    TcpConn _conns[NETGW_MAX_CONNS];

    // ---- UDP 流 ----
    struct UdpFlow {
        bool used = false;
        uint32_t dstIp = 0;     // 大端（0.0.0.0 表示"网关代答"占位）
        uint16_t watchPort = 0; // 大端
        uint16_t dstPort = 0;   // 大端
        bool dnsViaGateway = false; // 目标是 10.1.10.1:53，需改写
        int sock = -1;
        uint32_t lastActiveMs = 0;
    };
    UdpFlow _udp[NETGW_MAX_UDP_FLOWS];

    void _resetAll();
    void _log(const char* msg);
    // 当前毫秒数：注入了 nowMs 则用之，否则回落到平台时钟（主机测试可完全控制时间）
    uint32_t _nowMs();

    // IP 包构造与发送（自动分片）
    void _sendIpPacket(uint32_t srcBe, uint32_t dstBe, uint8_t proto,
                       const uint8_t* payload, size_t len);
    // 构建完整 TCP 段（含头与校验和）到 out；返回段长
    size_t _buildTcpSegment(TcpConn& c, uint8_t flags, const uint8_t* data, size_t len,
                            uint8_t* out, size_t outCap);
    void _sendTcpSegment(TcpConn& c, uint8_t flags, const uint8_t* data, size_t len);
    void _sendUdpPacket(uint32_t srcBe, uint32_t dstBe, uint16_t sport, uint16_t dport,
                        const uint8_t* data, size_t len);

    // 处理入口
    void _handleIp(const uint8_t* pkt, size_t len);
    void _handleTcp(uint32_t srcBe, uint32_t dstBe, const uint8_t* tcp, size_t len);
    void _handleUdp(uint32_t srcBe, uint32_t dstBe, const uint8_t* udp, size_t len);
    void _handleIcmp(uint32_t srcBe, uint32_t dstBe, const uint8_t* icmp, size_t len);

    TcpConn* _findConn(uint16_t watchPort, uint32_t dstIp, uint16_t dstPort);
    TcpConn* _allocConn();
    UdpFlow* _findUdp(uint16_t watchPort, uint32_t dstIp, uint16_t dstPort);
    UdpFlow* _allocUdp();

    void _connService(TcpConn& c);   // loop() 中调用
    void _udpService(UdpFlow& f);
    void _connTeardown(TcpConn& c, bool sendRst);
};

} // namespace xw
