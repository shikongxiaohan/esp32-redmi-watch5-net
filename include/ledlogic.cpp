/*
 * 板载 LED 指示灯 —— 纯逻辑实现
 */
#include "ledlogic.h"

namespace xw {

LedPattern ledPatternOf(LedState s) {
    switch (s) {
        // 闪烁节奏约定（8/2 法）：每个周期只亮一小段，灭的时间占大头。
        // 周期短到 1.2s 时，肉眼会把它数成"闪两下停一下"，
        // 看起来和 bootloader 崩溃重启的节奏一模一样 —— 排查时极易误判。
        // 因此除「启动中 / 配网 / 配对」这类"明显是活的"状态外，
        // 一律用 2.4s 长周期，语义统一为「亮的次数 / 停顿长度」。
        case LedState::Off:          return {0, 0};
        case LedState::Booting:      return {1600, 80};   // 单眨：亮80ms 灭1.5s
        case LedState::ApMode:       return {1000, 120};  // 每秒一眨（配网中）
        case LedState::Connecting:   return {1000, 250};  // 单闪（连 WiFi）
        case LedState::Online:       return {-1, 0};      // 常亮
        case LedState::OnlineNoWatch:return {2000, 100};  // 很慢的短闪
        case LedState::Pairing:      return {400, 200};   // 较快闪（配对中）
        case LedState::WatchfaceBusy:return {300, 150};   // 短亮长灭（装表盘）
        case LedState::FirmwareBusy: return {1000, 500};  // 慢速方波（升级中）
        case LedState::Error:        return {2400, 100};  // 三连眨 + 停顿（见 ledShouldBeOn）
    }
    return {0, 0};
}

const char* ledStateName(LedState s) {
    switch (s) {
        case LedState::Off:           return "已关闭";
        case LedState::Booting:       return "启动中（单眨）";
        case LedState::ApMode:        return "配网 AP 模式（每秒一眨）";
        case LedState::Connecting:    return "连接 WiFi 中（单闪）";
        case LedState::Online:        return "已联网且手表已连接（常亮）";
        case LedState::OnlineNoWatch: return "已联网但手表未连接（极慢短闪）";
        case LedState::Pairing:       return "蓝牙配对中（快速双闪）";
        case LedState::WatchfaceBusy: return "正在安装表盘（短亮长灭）";
        case LedState::FirmwareBusy:  return "正在升级固件（方波）";
        case LedState::Error:         return "故障（三连眨 + 停顿）";
    }
    return "未知";
}

// 对 Error 做特殊处理：一个周期 = 眨3下 + 长停顿。
//   |<-- 100ms on/100ms off  x3 -->|<------ 1900ms 全灭 ------>|  = 2400ms
// 这样「3 连眨 + 停」与「1 眨 + 停」「2 眨/秒」在肉眼和手机上都能区分开。
// 0..500ms   ：on 100 / off 100 / on 100 / off 100 / on 100  （第 3 次亮到 500ms 结束）
// 500..2400  ：全灭
static bool ledErrorPulse(uint32_t phase) {
    // 每 100ms 一个格子：0 on, 1 off, 2 on, 3 off, 4 on, 5+ off
    uint32_t slot = phase / 100u;
    return slot <= 4 && (slot % 2u) == 0u;
}

bool ledShouldBeOn(LedState state, uint32_t nowMs) {
    LedPattern p = ledPatternOf(state);
    if (p.periodMs == 0) return false;   // 常灭
    if (p.periodMs < 0) return true;     // 常亮
    // 无符号减法：millis() 回绕时依然正确
    uint32_t phase = nowMs % (uint32_t)p.periodMs;
    if (state == LedState::Error) return ledErrorPulse(phase);
    return phase < (uint32_t)p.onMs;
}

uint32_t ledPhaseFor(uint32_t stateHeldMs, const LedPattern& p) {
    if (p.periodMs <= 0) return 0;            // 常亮/常灭不看相位
    uint32_t period = (uint32_t)p.periodMs;
    if (stateHeldMs >= period) {
        // 已超过一个周期：按周期回绕，且回绕后仍从"亮"开始，
        // 保证每秒/每两秒重复时的节奏与首次一致
        return stateHeldMs % period;
    }
    // 首个周期：从 0 起算（由亮开始），进状态立刻可见
    return stateHeldMs;
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
