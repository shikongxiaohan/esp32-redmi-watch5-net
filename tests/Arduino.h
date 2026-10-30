/*
 * 主机端 Arduino.h 桩（仅用于单测编译 miwear.h/btlink.h 等）。
 * 只提供测试所需的最小接口；ESP32 真机编译时不会包含本文件
 * （真机 include 路径不含 tests/）。
 */
#pragma once

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <cstdio>
#include <cstdarg>
#include <string>

// 测试可控制的假时钟
uint32_t xw_test_millis();
#define millis() xw_test_millis()

// ---------------- FreeRTOS 基础类型（esp-idf 风格） ----------------
typedef uint32_t TickType_t;
typedef int BaseType_t;
typedef unsigned int UBaseType_t;
typedef void (*TaskFunction_t)(void*);
typedef void* TaskHandle_t;
#define pdTRUE 1
#define pdFALSE 0
#define pdMS_TO_TICKS(ms) (TickType_t)(ms)

// ---------------- 临界区（主机端：单线程测试，空操作） ----------------
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(mux) \
    do {                         \
    } while (0)
#define portEXIT_CRITICAL(mux) \
    do {                       \
    } while (0)

// ---------------- String 桩 ----------------
class String {
public:
    String() = default;
    String(const char* s) : _s(s ? s : "") {}
    String(const std::string& s) : _s(s) {}
    String(int v) {
        char b[32];
        std::snprintf(b, sizeof(b), "%d", v);
        _s = b;
    }
    String(unsigned v) {
        char b[32];
        std::snprintf(b, sizeof(b), "%u", v);
        _s = b;
    }
    const char* c_str() const { return _s.c_str(); }
    size_t length() const { return _s.size(); }
    bool operator==(const String& o) const { return _s == o._s; }
    bool operator==(const char* o) const { return _s == (o ? o : ""); }

private:
    std::string _s;
};

// ---------------- Serial 桩（收集输出供断言） ----------------
class SerialStub {
public:
    std::string out;
    void print(const char* s) { out += s ? s : ""; }
    void print(const String& s) { out += s.c_str(); }
    void println(const char* s) {
        out += s ? s : "";
        out += "\n";
    }
    void println(const String& s) {
        out += s.c_str();
        out += "\n";
    }
    void clear() { out.clear(); }
    /* 可变参版本：真机 Serial.printf 的常用形态，固件代码里到处在用。
     * 桩里累积到 out，同时按需要可以照常断言日志内容。 */
    void printf(const char* fmt, ...) {
        if (!fmt) return;
        char buf[512];
        va_list ap;
        va_start(ap, fmt);
        int n = std::vsnprintf(buf, sizeof(buf), fmt, ap);
        va_end(ap);
        if (n > 0) out.append(buf, std::size_t(n) < sizeof(buf) ? std::size_t(n) : sizeof(buf) - 1);
    }
};
extern SerialStub Serial;

// ---------------- Stream ----------------
class Stream {
public:
    virtual ~Stream() = default;
    virtual size_t write(const uint8_t*, size_t) { return 0; }
    virtual size_t write(uint8_t) { return 0; }
    virtual void flush() {}
    virtual int available() { return 0; }
    virtual int read() { return -1; }
    virtual int peek() { return -1; }
};

// ---------------- esp_random 桩（miwear 认证 nonce；主机端固定序列） ----------------
inline uint32_t esp_random() {
    static uint32_t s = 0x12345678u;
    s = s * 1103515245u + 12345u; // 与 glibc rand 同族的 LCG，仅保证可复现
    return s;
}
