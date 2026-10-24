/*
 * CRC-16/ARC（MiWear SPP V2 帧校验）
 * 参数: poly=0x8005 (反射后 0xA001), init=0x0000, refin=true, refout=true, xorout=0x0000
 * 与 Gadgetbridge XiaomiSppPacketV2.calculatePayloadChecksum 及
 * AstroBox miwear/packet.rs crc16_arc 完全一致
 */
#pragma once

#include <cstdint>
#include <cstddef>

namespace xw {

uint16_t crc16Arc(const uint8_t* data, size_t len);

} // namespace xw
