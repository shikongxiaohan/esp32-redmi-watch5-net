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
 *
 * 关于 GPIO2（重要）：
 *   GPIO2 是 ESP32 的 strapping 引脚，上电瞬间的电平参与决定启动模式。
 *   官方 DevKitC / NodeMCU-32 的板载 LED 是「3.3V -> 电阻 -> LED -> GPIO2」
 *   的灌电流接法（低电平点亮），因此 begin() 里**不能**为了"先灭灯"就把
 *   GPIO2 拉高——那会在下一个复位周期的 strapping 采样窗口拉高该脚。
 *   本实现改为先把引脚置为高阻输入（外部上拉电阻按原设计把 LED 关掉），
 *   等第一次真正需要点亮时才切换为推挽输出。
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
    // 切状态时重置"状态起始时刻"，让新状态的闪烁相位从 0 起算，
    // 保证每次进入状态都立刻可见（详见 ledlogic.h 的 ledPhaseFor）。
    void setState(LedState s) {
        if (s != _state) {
            _state = s;
            _stateSinceMs = millis();
            _hasStateSince = true;
        }
    }
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

    // 触发一次极性自检：按「亮 - 灭 - 亮」各 stepMs 播一遍，便于肉眼确认。
    // 阻塞执行（共约 2.1 秒），仅应在 setup() 阶段调用：
    // 否则后续初始化切状态会把自检顶掉，用户只看到一闪而过。
    // 结束后把状态起始时刻重置为当前，交回状态机由 loop() 接管。
    void selftest(uint32_t stepMs = 700);

private:
    void _apply(bool on);
    void _ensureOutput();

    int8_t _pin = -1;
    bool _activeLow = false;
    LedState _state = LedState::Off;
    bool _level = false;   // 当前实际输出电平（已按 activeLow 归一化为"是否点亮"）
    bool _outputMode = false; // 是否已切换为推挽输出（false = 高阻态）
    bool _testing = false; // 测试常亮模式
    uint32_t _testUntilMs = 0; // 测试模式截止时刻（0 = 未在测试）
    uint32_t _stateSinceMs = 0; // 当前状态的起始时刻
    bool _hasStateSince = false;
    uint32_t _lastMs = 0;
};

} // namespace xw
