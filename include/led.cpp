/*
 * 板载 LED 指示灯驱动实现
 */
#include "led.h"

namespace xw {

void Led::begin(int8_t pin, bool activeLow) {
    _pin = pin;
    _activeLow = activeLow;
    if (_pin < 0) {
        _state = LedState::Off;
        return;
    }
    pinMode(uint8_t(_pin), OUTPUT);
    // 上电先灭
    digitalWrite(uint8_t(_pin), _activeLow ? HIGH : LOW);
    _level = false;
    _lastMs = millis();
}

void Led::_apply(bool on) {
    if (_pin < 0) return;
    digitalWrite(uint8_t(_pin), _activeLow ? (on ? LOW : HIGH) : (on ? HIGH : LOW));
    _level = on;
}

void Led::loop() {
    if (_pin < 0) return;
    uint32_t now = millis();
    // 20ms 刷新粒度足够（最快模式周期 200ms），也省电
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
    bool on = ledShouldBeOn(_state, now);
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

} // namespace xw
