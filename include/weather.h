/*
 * 天气服务（Open-Meteo，无需 API Key）
 *
 * 流程（与 Gadgetbridge XiaomiWeatherService 对齐）：
 *   手表请求天气 (Command{type=10, subtype=3, weather{location{...}}})
 *     -> HTTP 拉取 Open-Meteo 当前 + 7 日预报
 *     -> 推送 Command{type=10, subtype=0} 当前天气
 *     -> 推送 Command{type=10, subtype=1} 每日预报
 *   另支持周期刷新（weatherRefreshMin）。
 *
 * 仅 ESP32 平台（依赖 HTTPClient / ArduinoJson）。
 */
#pragma once

#include "appconfig.h"
#include "miwear.h"
#include "proto.h"

#include <cstdint>
#include <string>
#include <vector>

namespace xw {

struct WeatherData {
    WeatherNow now;
    std::vector<ForecastDay> days;
};

class WeatherService {
public:
    void begin(MiWear& wear, const AppConfig& cfg);

    // Web 界面修改配置后调用
    void setConfig(const AppConfig& cfg) { _cfg = cfg; }
    const AppConfig& config() const { return _cfg; }

    // 主循环：周期刷新
    void loop();

    // 处理手表命令（仅关注 type=10）；返回 true 表示已处理
    bool onCommand(const MiCommand& cmd);

    // 立即拉取并推送；成功返回 true
    bool refreshNow();

    // 状态（供 Web 页面展示）
    uint32_t lastFetchMs = 0;
    int lastFetchCount = 0;   // 成功推送次数
    std::string lastError;

private:
    MiWear* _wear = nullptr;
    AppConfig _cfg{};
    bool _busy = false;
    // 上次刷新时刻（millis），用无符号差值算间隔，抗 49.7 天回绕
    uint32_t _lastFetchMs = 0;
    // 失败退避截止时刻
    uint32_t _failUntilMs = 0;

    uint32_t intervalMs() const {
        uint32_t m = _cfg.weatherRefreshMin;
        if (m == 0) m = 1;
        return m * 60000UL;
    }

    bool _httpGet(const char* url, std::string& body);
    bool _fetch(WeatherData& out);
    bool _push(const WeatherData& d);
};

} // namespace xw
