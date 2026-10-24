/*
 * 板载 LED 指示灯驱动（ESP32 侧）
 *
 * 用法：
 *   Led led;
 *   led.begin(pin, activeLow);      // pin = -1 表示禁用
 *   led.setState(LedState::Online);
 *   led.loop();                      // 主循环里周期调用（约 20ms 一次）
 *
 * 实现要点：
 *   - 闪烁时序用 ledlogic.h 的纯函数判定（可主机单测）
 *   - 只在输出电平真正变化时才写 GPIO，避免高频无谓操作
 *   - activeLow 支持：多数开发板板载 LED 是低电平点亮
 */
#pragma once

#include "ledlogic.h"

#include <Arduino.h>

namespace xw {

class Led {
public:
    // pin：GPIO 号；-1 表示不启用 LED
    // activeLow：true = 低电平点亮（多数板载 LED），false = 高电平点亮
    void begin(int8_t pin, bool activeLow);
    void setState(LedState s) { _state = s; }
    LedState state() const { return _state; }
    bool enabled() const { return _pin >= 0; }
    int8_t pin() const { return _pin; }
    bool activeLow() const { return _activeLow; }

    // 主循环调用：按当前状态刷新输出
    void loop();

    // 立即把 LED 点亮（常亮），忽略状态机；用于网页"测试灯"按钮
    // 测试最长持续 testTimeoutMs（默认 10 秒）后自动恢复正常状态指示，
    // 避免用户点了"点亮"忘记点"熄灭"导致 LED 永久常亮。
    void testOn(uint32_t timeoutMs = 10000);
    void testOff();
    bool testing() const { return _testing; }

private:
    void _apply(bool on);

    int8_t _pin = -1;
    bool _activeLow = false;
    LedState _state = LedState::Off;
    bool _level = false;   // 当前实际输出电平（已按 activeLow 归一化为"是否点亮"）
    bool _testing = false; // 测试常亮模式
    uint32_t _testUntilMs = 0; // 测试模式截止时刻（0 = 未在测试）
    uint32_t _lastMs = 0;
};

} // namespace xw
