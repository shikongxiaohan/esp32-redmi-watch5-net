/*
 * 天气服务实现（Open-Meteo）
 * 见 weather.h 的流程说明。
 */
#include "weather.h"

#include <Arduino.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

#include <cmath>
#include <cstdio>
#include <cstring>

namespace xw {

// km/h -> 蒲福风级（阈值表）
static int kmhToBeaufort(float kmh) {
    static const float th[] = {1, 5, 11, 19, 28, 38, 49, 61, 74, 88, 102, 117};
    for (int i = 0; i < 12; i++) {
        if (kmh < th[i]) return i;
    }
    return 12;
}

// Open-Meteo 返回的 ISO 本地时间（"2026-10-24T06:12" 或带秒）
// 规范化为手表要求的 "YYYY-MM-DDTHH:MM:SS"
static std::string normalizeIso(const char* s) {
    std::string r(s ? s : "");
    if (r.size() == 16) r += ":00";
    return r;
}

// 与 Gadgetbridge getLocationKey 一致：accu:<name hash % 1000000>
static std::string locationKeyOf(const std::string& name) {
    uint32_t h = 0;
    for (char c : name) h = h * 31 + uint8_t(c);
    char buf[24];
    std::snprintf(buf, sizeof(buf), "accu:%u", h % 1000000u);
    return std::string(buf);
}

void WeatherService::begin(MiWear& wear, const AppConfig& cfg) {
    _wear = &wear;
    _cfg = cfg;
    _nextFetchMs = 0;
}

bool WeatherService::_httpGet(const char* url, std::string& body) {
    HTTPClient http;
    http.setTimeout(8000);
    http.setConnectTimeout(5000);
    if (!http.begin(url)) {
        lastError = "http begin failed";
        return false;
    }
    int code = http.GET();
    if (code != 200) {
        lastError = "http " + std::to_string(code);
        http.end();
        return false;
    }
    body = http.getString().c_str();
    http.end();
    if (body.empty()) {
        lastError = "empty body";
        return false;
    }
    return true;
}

bool WeatherService::_fetch(WeatherData& out) {
    char url[256];
    // 当前天气 + 7 日预报（timezone=auto 让日出日落为当地时间）
    std::snprintf(url, sizeof(url),
                  "http://api.open-meteo.com/v1/forecast"
                  "?latitude=%.4f&longitude=%.4f"
                  "&current=temperature_2m,relative_humidity_2m,weather_code,"
                  "pressure_msl,wind_speed_10m,wind_direction_10m"
                  "&daily=weather_code,temperature_2m_max,temperature_2m_min,sunrise,sunset"
                  "&timezone=auto&forecast_days=7",
                  _cfg.latitude, _cfg.longitude);

    std::string body;
    if (!_httpGet(url, body)) return false;

    // 只保留需要的字段，降低内存占用
    // （ArduinoJson 7 的 Filter 只接受 JsonVariant/JsonDocument，
    //  需先把过滤描述解析成文档）
    static const char FILTER[] =
        "{\"current\":[\"temperature_2m\",\"relative_humidity_2m\",\"weather_code\","
        "\"pressure_msl\",\"wind_speed_10m\",\"wind_direction_10m\"],"
        "\"daily\":[\"time\",\"weather_code\",\"temperature_2m_max\","
        "\"temperature_2m_min\",\"sunrise\",\"sunset\"]}";

    JsonDocument doc;
    JsonDocument filterDoc;
    if (deserializeJson(filterDoc, FILTER) != DeserializationError::Ok) {
        lastError = "filter json";
        return false;
    }
    DeserializationError err =
        deserializeJson(doc, body, DeserializationOption::Filter(filterDoc));
    if (err) {
        lastError = std::string("json: ") + err.c_str();
        return false;
    }

    JsonObject cur = doc["current"];
    if (cur.isNull()) {
        lastError = "no current";
        return false;
    }

    out.now.locationName = _cfg.locationName;
    out.now.locationKey = locationKeyOf(_cfg.locationName);
    out.now.isCurrentLocation = true;
    out.now.condition = wmoToXiaomiCondition(cur["weather_code"] | 0);
    out.now.temperature = int32_t(std::lround(cur["temperature_2m"] | 0.0f));
    out.now.humidity = int32_t(cur["relative_humidity_2m"] | 0);
    out.now.pressure = cur["pressure_msl"] | 1013.25f;
    out.now.windBeaufort = kmhToBeaufort(cur["wind_speed_10m"] | 0.0f);
    out.now.windDirDeg = int32_t(cur["wind_direction_10m"] | 0);
    out.now.uvIndex = 0;    // Open-Meteo 免费接口不直接给 UV 指数
    out.now.aqi = 0;        // 下面尽力补齐

    // AQI（尽力而为）：独立的空气质量接口
    {
        char aqiUrl[192];
        std::snprintf(aqiUrl, sizeof(aqiUrl),
                      "http://air-quality-api.open-meteo.com/v1/air-quality"
                      "?latitude=%.4f&longitude=%.4f&current=us_aqi&timezone=auto",
                      _cfg.latitude, _cfg.longitude);
        std::string abody;
        if (_httpGet(aqiUrl, abody)) {
            JsonDocument adoc;
            if (!deserializeJson(adoc, abody)) {
                JsonObject ac = adoc["current"];
                if (!ac.isNull()) out.now.aqi = int32_t(ac["us_aqi"] | 0);
            }
        }
    }

    // 每日预报
    out.days.clear();
    JsonObject daily = doc["daily"];
    if (!daily.isNull()) {
        JsonArray codes = daily["weather_code"].as<JsonArray>();
        JsonArray tmax = daily["temperature_2m_max"].as<JsonArray>();
        JsonArray tmin = daily["temperature_2m_min"].as<JsonArray>();
        JsonArray sunrise = daily["sunrise"].as<JsonArray>();
        JsonArray sunset = daily["sunset"].as<JsonArray>();
        size_t n = codes.size();
        if (n > 7) n = 7;
        for (size_t i = 0; i < n; i++) {
            ForecastDay d;
            d.condition = wmoToXiaomiCondition(codes[i] | 0);
            d.tempMax = int32_t(std::lround(tmax[i] | 0.0f));
            d.tempMin = int32_t(std::lround(tmin[i] | 0.0f));
            d.sunrise = normalizeIso(sunrise[i] | "");
            d.sunset = normalizeIso(sunset[i] | "");
            out.days.push_back(d);
        }
    }
    if (out.days.empty()) {
        lastError = "no daily";
        return false;
    }
    return true;
}

bool WeatherService::_push(const WeatherData& d) {
    if (!_wear || !_wear->authenticated()) {
        lastError = "not authenticated";
        return false;
    }
    // 首日用当天实况拼一条预报项（Gadgetbridge 同样处理：
    // 首条预报应对应当天）
    std::vector<ForecastDay> entries;
    {
        ForecastDay today;
        today.condition = d.now.condition;
        today.tempMin = d.now.temperature;
        today.tempMax = d.now.temperature;
        if (!d.days.empty()) {
            today.tempMin = d.days[0].tempMin;
            today.tempMax = d.days[0].tempMax;
            today.sunrise = d.days[0].sunrise;
            today.sunset = d.days[0].sunset;
        }
        entries.push_back(today);
    }
    for (size_t i = 1; i < d.days.size(); i++) entries.push_back(d.days[i]);

    bool ok = true;
    auto cur = buildWeatherCurrentCmd(d.now);
    ok = _wear->sendCommand(cur.data(), cur.size()) && ok;
    auto fc = buildWeatherDailyForecastCmd(d.now, entries);
    ok = _wear->sendCommand(fc.data(), fc.size()) && ok;
    return ok;
}

bool WeatherService::refreshNow() {
    if (_busy) return false;
    _busy = true;
    WeatherData d;
    bool ok = _fetch(d);
    if (ok) ok = _push(d);
    lastFetchMs = millis();
    if (ok) {
        lastFetchCount++;
        lastError.clear();
    }
    _busy = false;
    return ok;
}

bool WeatherService::onCommand(const MiCommand& cmd) {
    if (!cmd.ok || cmd.type != cmd::TYPE_WEATHER) return false;
    if (cmd.subtype == cmd::WX_REQUEST_CONDITIONS) {
        // 手表请求天气（可能带 location{code,name}）。
        // 简化处理：始终推送 Web 配置的位置。
        if (_wear && _wear->authenticated()) {
            refreshNow();
        }
        return true;
    }
    return false;
}

void WeatherService::loop() {
    if (!_wear || !_wear->authenticated()) return;
    if (_cfg.weatherRefreshMin == 0) return; // 关闭周期刷新
    uint32_t now = millis();
    if (now < _nextFetchMs) return;
    if (refreshNow()) {
        _nextFetchMs = now + uint32_t(_cfg.weatherRefreshMin) * 60000UL;
    } else {
        _nextFetchMs = now + 30000; // 失败退避
    }
}

} // namespace xw
