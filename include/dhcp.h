/*
 * DHCP 应答器（手表侧）
 *
 * 手表通过 NetWork 通道广播 DHCP DISCOVER/REQUEST（UDP 68 -> 67）。
 * 本模块直接构造 OFFER/ACK 应答（不经过 lwIP 的 UDP 栈）。
 *
 * 配置与 AstroBox 网络栈一致：
 *   手表 IP  10.1.10.2
 *   网关/本机 10.1.10.1
 *   掩码     255.255.255.0
 *   租约     269352960s
 *   DNS      223.5.5.5（AliDNS；同时中继层也会处理发给网关的 DNS 查询）
 *
 * 实现为纯函数：输入原始 IP 包，输出应答 IP 包（vector）。
 * 无平台依赖，可主机单测。
 */
#pragma once

#include <cstdint>
#include <cstddef>
#include <vector>

namespace xw {

// 尝试处理一个 IP 包。若是 DHCP 请求且构造应答成功，返回 true，
// outPacket 为可直接经 NetWork 通道发回的完整 IPv4 包。
bool dhcpHandlePacket(const uint8_t* pkt, size_t len,
                      std::vector<uint8_t>& outPacket);

} // namespace xw
