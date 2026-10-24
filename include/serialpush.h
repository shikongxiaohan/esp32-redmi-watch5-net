/*
 * 串口表盘推送接收器（ESP32 侧）
 *
 * 用途：PC 通过 USB 数据线（CH340 → UART0）把表盘安装包直接推到板子上，
 *       板子再通过蓝牙把表盘装进手表。控制逻辑全部在板子上，
 *       PC 端只负责把字节搬到串口。
 *
 * 与日志共线：UART0 同时输出固件日志（板子→PC）与接收命令（PC→板子），
 *             因此所有命令以 "XWFP " 开头，日志行一律忽略。
 *
 * 协议见 serialpush_proto.h。
 */
#pragma once

#include "webui.h"

#include <Arduino.h>

#include <string>

namespace xw {

class SerialPush {
public:
    void begin(WebUi& web);
    // 主循环调用：读取串口命令并驱动推送
    void loop();

    bool receiving() const { return _receiving; }
    uint32_t received() const { return _received; }

private:
    WebUi* _web = nullptr;
    std::string _line;
    bool _receiving = false;
    uint32_t _expected = 0;
    uint32_t _received = 0;
    uint32_t _lastDataMs = 0;

    void _reply(const char* fmt, ...);
    void _handleLine(const std::string& line);
    void _reset();
};

} // namespace xw
