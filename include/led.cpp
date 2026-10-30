/*
 * 板载 LED 指示灯驱动实现
 */
#include "led.h"

namespace xw {

void Led::begin(int8_t pin, bool activeLow) {
    _pin = pin;
    _activeLow = activeLow;
    _outputMode = false;
    _testing = false;
    if (_pin < 0) {
        _state = LedState::Off;
        return;
    }
    // 关键：不要在这里把引脚拉高"先灭灯"。
    // GPIO2 是 strapping 脚，上电/复位时的电平参与启动模式判定；
    // 低电平点亮的板子（LED 阴极接 GPIO2）拉高虽能让灯灭，
    // 却会在 strapping 采样窗口把一个拉低态改成拉高态。
    // 改为高阻输入：外部电路（3.3V -> 电阻 -> LED -> GPIO2）自行保持熄灭，
    // 直到第一次需要点亮时才切换为推挽输出。
    pinMode(uint8_t(_pin), INPUT);
    _level = false;
    _lastMs = millis();
}

// 第一次真正要驱动 LED 时才切换为推挽输出
void Led::_ensureOutput() {
    if (_pin < 0 || _outputMode) return;
    pinMode(uint8_t(_pin), OUTPUT);
    // 切换为输出的瞬间先写"熄灭"电平，避免闪一下
    digitalWrite(uint8_t(_pin), _activeLow ? HIGH : LOW);
    _outputMode = true;
}

void Led::_apply(bool on) {
    if (_pin < 0) return;
    _ensureOutput();
    digitalWrite(uint8_t(_pin), _activeLow ? (on ? LOW : HIGH) : (on ? HIGH : LOW));
    _level = on;
}

void Led::loop() {
    if (_pin < 0) return;
    uint32_t now = millis();

    // 20ms 刷新粒度足够（最快模式 200ms/格），也省电
    if (now - _lastMs < 20) return;
    _lastMs = now;

    if (_testing) {
        // 测试模式超时后自动退出，回到正常状态指示
        if (ledDeadlineActive(_testUntilMs, now)) {
            if (!_level) _apply(true);
            return;
        }
        _testing = false;
    }
    // 用"状态内相位"而不是 millis() 绝对值，确保进状态即从亮开始
    uint32_t held = _hasStateSince ? (now - _stateSinceMs) : now;
    uint32_t phase = ledPhaseFor(held, ledPatternOf(_state));
    bool on = ledShouldBeOn(_state, phase);
    if (on != _level) _apply(on);
}

void Led::testOn(uint32_t timeoutMs) {
    _testing = true;
    _testUntilMs = millis() + timeoutMs;
    _apply(true);
}

void Led::testOff() {
    _testing = false;
    _testUntilMs = 0;
    _apply(false);
}

void Led::selftest(uint32_t stepMs) {
    if (_pin < 0) return;
    // 同步播完：亮 -> 灭 -> 亮，每步 stepMs（默认 700ms）。
    // 之所以阻塞而不是交给 loop() 分片执行：setup() 里紧接着就要
    // WiFi.mode() / 蓝牙初始化，那些动作会立刻把状态切到 Connecting，
    // 异步自检会被顶掉，用户只看到一闪而过，反而误判成"刷坏了"。
    // 这里总共只阻塞约 2.1 秒，且发生在 setup 阶段（此时还没有任何服务在跑）。
    _testing = false;
    _testUntilMs = 0;
    _apply(true);
    delay(stepMs);
    _apply(false);
    delay(stepMs);
    _apply(true);
    delay(stepMs);
    _apply(false);
    _lastMs = millis();
    // 交回状态机：把"状态起始时刻"重置为此刻，保证进状态即从亮开始
    _stateSinceMs = millis();
    _hasStateSince = true;
}

} // namespace xw
