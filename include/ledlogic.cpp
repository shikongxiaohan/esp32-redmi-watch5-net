/*
 * 板载 LED 状态指示 —— 纯逻辑实现
 */
#include "ledlogic.h"

namespace xw {

LedPattern ledPatternOf(LedState s) {
    switch (s) {
        case LedState::Off:          return {0, 0};
        case LedState::Booting:      return {200, 100};   // 快闪
        case LedState::ApMode:       return {2000, 200};  // 慢闪（配网中）
        case LedState::Connecting:   return {1000, 250};  // 单闪（连 WiFi）
        case LedState::Online:       return {-1, 0};      // 常亮
        case LedState::OnlineNoWatch:return {2000, 100};  // 很慢的短闪
        case LedState::Pairing:      return {400, 200};   // 较快闪（配对中）
        case LedState::WatchfaceBusy:return {300, 150};   // 短亮长灭（装表盘）
        case LedState::FirmwareBusy: return {1000, 500};  // 慢速方波（升级中）
        case LedState::Error:        return {-1, 0};      // 常亮
    }
    return {0, 0};
}

const char* ledStateName(LedState s) {
    switch (s) {
        case LedState::Off:           return "已关闭";
        case LedState::Booting:       return "启动中（快闪）";
        case LedState::ApMode:        return "配网 AP 模式（慢闪）";
        case LedState::Connecting:    return "连接 WiFi 中（单闪）";
        case LedState::Online:        return "已联网且手表已连接（常亮）";
        case LedState::OnlineNoWatch: return "已联网但手表未连接（极慢短闪）";
        case LedState::Pairing:       return "蓝牙配对中（快速双闪）";
        case LedState::WatchfaceBusy: return "正在安装表盘（短亮长灭）";
        case LedState::FirmwareBusy:  return "正在升级固件（方波）";
        case LedState::Error:         return "故障（常亮）";
    }
    return "未知";
}

bool ledShouldBeOn(LedState state, uint32_t nowMs) {
    LedPattern p = ledPatternOf(state);
    if (p.periodMs == 0) return false;   // 常灭
    if (p.periodMs < 0) return true;     // 常亮
    // 无符号减法：millis() 回绕时依然正确
    uint32_t phase = nowMs % (uint32_t)p.periodMs;
    return phase < (uint32_t)p.onMs;
}

int ledPriority(LedState s) {
    switch (s) {
        case LedState::Off:           return 0;
        case LedState::Online:        return 10;
        case LedState::OnlineNoWatch: return 20;
        case LedState::Connecting:    return 30;
        case LedState::Pairing:       return 40;
        case LedState::ApMode:        return 50;
        case LedState::FirmwareBusy:  return 60;
        case LedState::WatchfaceBusy: return 70;
        case LedState::Error:         return 80;
        case LedState::Booting:       return 90;
    }
    return 0;
}

LedState ledMerge(LedState a, LedState b) {
    return ledPriority(a) >= ledPriority(b) ? a : b;
}

bool ledDeadlineActive(uint32_t deadline, uint32_t nowMs) {
    if (deadline == 0) return false;
    // 无符号差值：now < deadline 时差值回绕成大值（>= 0x80000000）。
    return (uint32_t)(nowMs - deadline) >= 0x80000000UL;
}

bool ledFaultActive(uint32_t faultUntilMs, uint32_t nowMs) {
    return ledDeadlineActive(faultUntilMs, nowMs);
}

} // namespace xw
