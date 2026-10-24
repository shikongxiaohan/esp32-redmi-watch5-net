/*
 * 应用配置（NVS 持久化，Web 界面可改）
 */
#pragma once

#include <cstdint>
#include <cstring>

namespace xw {

struct AppConfig {
    // WiFi（STA）
    char wifiSsid[33] = {0};
    char wifiPass[65] = {0};
    // 手表配对密钥（16 字节，hex 字符串形式存储，如 "00112233..."）
    // 留空则使用默认开发密钥（仅用于测试，正式使用需从小米运动健康提取）
    char authKeyHex[33] = {0};
    // 手表蓝牙 MAC（"AA:BB:CC:DD:EE:FF" 或 "AABBCCDDEEFF"）；留空则尝试已配对设备
    char watchMac[18] = {0};
    // 天气位置（Open-Meteo，无 Key）
    char locationName[40] = "北京";
    float latitude = 39.9042f;
    float longitude = 116.4074f;
    // 时区（POSIX TZ 字符串，用于 NTP 对时；中国大陆为 CST-8）
    char tzPosix[48] = "CST-8";
    char tzName[40] = "CST";
    bool time24h = true;
    // 天气自动刷新间隔（分钟）；0 = 仅手表请求时推送
    uint16_t weatherRefreshMin = 30;
    // Web 配网 AP 密码（留空 = 开放网络）
    char apPass[65] = {0};
    // 自动恢复出厂：连续 N 分钟「WiFi 与手表都没有连接」时清空配置并重启。
    // 默认关闭；开启后适合无人值守部署，误开会导致离线即清配置。
    bool autoResetEn = false;
    uint16_t autoResetMin = 10; // 1..1440 分钟

    bool hasWifi() const { return wifiSsid[0] != 0; }
    bool hasAuthKey() const { return authKeyHex[0] != 0; }
};

} // namespace xw
