#include "proto.h"

#include <cmath>
#include <ctime>

namespace xw {

// ===================== 写入侧 =====================

void pbPutVarint(std::vector<uint8_t>& b, uint64_t v) {
    while (v >= 0x80) {
        b.push_back(uint8_t(v) | 0x80);
        v >>= 7;
    }
    b.push_back(uint8_t(v));
}

void pbPutTag(std::vector<uint8_t>& b, uint32_t field, uint32_t wire) {
    pbPutVarint(b, (uint64_t(field) << 3) | wire);
}

void pbPutVarintField(std::vector<uint8_t>& b, uint32_t field, uint64_t v) {
    pbPutTag(b, field, 0);
    pbPutVarint(b, v);
}

static uint32_t zigzag32(int32_t v) {
    return (uint32_t(v) << 1) ^ uint32_t(v >> 31);
}

void pbPutSint32Field(std::vector<uint8_t>& b, uint32_t field, int32_t v) {
    pbPutTag(b, field, 0);
    pbPutVarint(b, zigzag32(v));
}

void pbPutLenField(std::vector<uint8_t>& b, uint32_t field, const uint8_t* d, size_t n) {
    pbPutTag(b, field, 2);
    pbPutVarint(b, n);
    b.insert(b.end(), d, d + n);
}

void pbPutLenField(std::vector<uint8_t>& b, uint32_t field, const std::vector<uint8_t>& d) {
    pbPutLenField(b, field, d.data(), d.size());
}

void pbPutStringField(std::vector<uint8_t>& b, uint32_t field, const std::string& s) {
    pbPutLenField(b, field, reinterpret_cast<const uint8_t*>(s.data()), s.size());
}

void pbPutFloatField(std::vector<uint8_t>& b, uint32_t field, float f) {
    pbPutTag(b, field, 5);
    uint32_t bits;
    std::memcpy(&bits, &f, 4);
    b.push_back(uint8_t(bits));
    b.push_back(uint8_t(bits >> 8));
    b.push_back(uint8_t(bits >> 16));
    b.push_back(uint8_t(bits >> 24));
}

void pbPutMessageField(std::vector<uint8_t>& b, uint32_t field, const std::vector<uint8_t>& msg) {
    pbPutLenField(b, field, msg.data(), msg.size());
}

// ===================== 读取侧 =====================

bool PbReader::next(PbField& f) {
    if (_pos >= _n) return false;
    size_t p = _pos;
    uint64_t tag = 0;
    // 读取 tag varint
    for (int i = 0; i < 10; i++) {
        if (p >= _n) return false;
        uint8_t byte = _p[p++];
        tag |= uint64_t(byte & 0x7f) << (7 * i);
        if (!(byte & 0x80)) goto tag_ok;
    }
    return false;
tag_ok:;
    f.num = uint32_t(tag >> 3);
    f.wire = uint32_t(tag & 7);
    switch (f.wire) {
        case 0: { // varint
            uint64_t v = 0;
            for (int i = 0; i < 10; i++) {
                if (p >= _n) return false;
                uint8_t byte = _p[p++];
                v |= uint64_t(byte & 0x7f) << (7 * i);
                if (!(byte & 0x80)) break;
            }
            f.varint = v;
            break;
        }
        case 1: { // 64-bit
            if (p + 8 > _n) return false;
            f.data = _p + p;
            f.len = 8;
            p += 8;
            break;
        }
        case 2: { // length-delimited
            uint64_t len = 0;
            for (int i = 0; i < 10; i++) {
                if (p >= _n) return false;
                uint8_t byte = _p[p++];
                len |= uint64_t(byte & 0x7f) << (7 * i);
                if (!(byte & 0x80)) break;
            }
            // 先比较再相加：若写成 p + len > _n，len 接近 UINT64_MAX 时加法会回绕，
            // 使越界检查被绕过（后续按巨大长度读越界内存）。
            if (len > uint64_t(_n - p)) return false;
            f.data = _p + p;
            f.len = size_t(len);
            p += size_t(len);
            break;
        }
        case 5: { // 32-bit
            if (p + 4 > _n) return false;
            f.data = _p + p;
            f.len = 4;
            p += 4;
            break;
        }
        default:
            return false;
    }
    _pos = p;
    return true;
}

bool PbReader::find(uint32_t num, PbField& out) const {
    PbReader tmp(_p, _n);
    PbField f;
    while (tmp.next(f)) {
        if (f.num == num) {
            out = f;
            return true;
        }
    }
    return false;
}

bool PbReader::has(uint32_t num) const {
    PbField f;
    return find(num, f);
}

uint64_t PbReader::varintOf(uint32_t num, uint64_t def) const {
    PbField f;
    if (find(num, f) && f.wire == 0) return f.varint;
    return def;
}

static int32_t unzigzag32(uint32_t v) {
    return int32_t(v >> 1) ^ -int32_t(v & 1);
}

int32_t PbReader::sintOf(uint32_t num, int32_t def) const {
    PbField f;
    if (find(num, f) && f.wire == 0) return unzigzag32(uint32_t(f.varint));
    return def;
}

float PbReader::floatOf(uint32_t num, float def) const {
    PbField f;
    if (find(num, f) && f.wire == 5 && f.len == 4) {
        uint32_t bits = uint32_t(f.data[0]) | (uint32_t(f.data[1]) << 8) |
                        (uint32_t(f.data[2]) << 16) | (uint32_t(f.data[3]) << 24);
        float out;
        std::memcpy(&out, &bits, 4);
        return out;
    }
    return def;
}

bool PbReader::bytesOf(uint32_t num, std::vector<uint8_t>& out) const {
    PbField f;
    if (find(num, f) && f.wire == 2) {
        out.assign(f.data, f.data + f.len);
        return true;
    }
    return false;
}

bool PbReader::stringOf(uint32_t num, std::string& out) const {
    PbField f;
    if (find(num, f) && f.wire == 2) {
        out.assign(reinterpret_cast<const char*>(f.data), f.len);
        return true;
    }
    return false;
}

bool PbReader::subReader(uint32_t num, PbField& out) const {
    return find(num, out) && out.wire == 2;
}

// ===================== Command 解析 =====================

MiCommand parseCommand(const uint8_t* data, size_t len) {
    MiCommand c;
    PbReader r(data, len);
    PbField f;
    while (r.next(f)) {
        switch (f.num) {
            case 1:
                if (f.wire == 0) c.type = uint32_t(f.varint);
                break;
            case 2:
                if (f.wire == 0) c.subtype = uint32_t(f.varint);
                break;
            case 3:
                c.auth = f;
                break;
            case 4:
                c.system = f;
                break;
            case 6:
                c.watchface = f;
                break;
            case 12:
                c.weather = f;
                break;
            case 24:
                c.dataUpload = f;
                break;
            case 100:
                if (f.wire == 0) {
                    c.hasStatus = true;
                    c.status = uint32_t(f.varint);
                }
                break;
            default:
                break;
        }
    }
    c.ok = (c.type != 0);
    return c;
}

bool authConfirmSucceeded(const MiCommand& cmd) {
    if (!cmd.ok) return false;
    PbReader authR(cmd.auth.data, cmd.auth.len);
    // status 存在时以它为准；缺失才回退到子类型判定
    if (authR.has(8)) {
        return authR.varintOf(8, 0) == 1;
    }
    return cmd.subtype == cmd::AUTH_CONFIRM;
}

// ===================== 消息构造 =====================

std::vector<uint8_t> buildAuthPhoneNonceCmd(const uint8_t nonce[16]) {
    std::vector<uint8_t> phoneNonce;
    pbPutLenField(phoneNonce, 1, nonce, 16);
    std::vector<uint8_t> auth;
    pbPutMessageField(auth, 30, phoneNonce); // PhoneNonce
    std::vector<uint8_t> out;
    pbPutVarintField(out, 1, cmd::TYPE_AUTH);
    pbPutVarintField(out, 2, cmd::AUTH_NONCE);
    pbPutMessageField(out, 3, auth);
    return out;
}

std::vector<uint8_t> buildAuthStep3Cmd(const uint8_t encSigns[32],
                                       const uint8_t* encDevInfo, size_t encDevInfoLen) {
    std::vector<uint8_t> step3;
    pbPutLenField(step3, 1, encSigns, 32);                    // encryptedNonces
    pbPutLenField(step3, 2, encDevInfo, encDevInfoLen);       // encryptedDeviceInfo
    std::vector<uint8_t> auth;
    pbPutMessageField(auth, 32, step3);                       // authStep3
    std::vector<uint8_t> out;
    pbPutVarintField(out, 1, cmd::TYPE_AUTH);
    pbPutVarintField(out, 2, cmd::AUTH_CONFIRM);
    pbPutMessageField(out, 3, auth);
    return out;
}

static std::vector<uint8_t> buildAuthDeviceInfo(uint32_t phoneApiLevel,
                                                const std::string& phoneName,
                                                const std::string& region) {
    std::vector<uint8_t> di;
    pbPutVarintField(di, 1, 0);                 // unknown1 = 0（proto2 必须显式序列化）
    pbPutFloatField(di, 2, float(phoneApiLevel)); // phoneApiLevel
    pbPutStringField(di, 3, phoneName);         // phoneName
    pbPutVarintField(di, 4, 224);               // unknown3 = 224
    pbPutStringField(di, 5, region);            // region
    return di;
}

// 暴露给 miwear.cpp（AuthDeviceInfo 内容）
std::vector<uint8_t> buildAuthDeviceInfoBytes(uint32_t phoneApiLevel,
                                              const std::string& phoneName,
                                              const std::string& region) {
    return buildAuthDeviceInfo(phoneApiLevel, phoneName, region);
}

static std::vector<uint8_t> buildWeatherMetadata(const std::string& publicationTimestamp,
                                                 const std::string& cityName,
                                                 const std::string& locationName,
                                                 const std::string& locationKey,
                                                 bool isCurrentLocation) {
    std::vector<uint8_t> md;
    pbPutStringField(md, 1, publicationTimestamp);
    pbPutStringField(md, 2, cityName);
    pbPutStringField(md, 3, locationName);
    pbPutStringField(md, 4, locationKey);
    pbPutVarintField(md, 5, isCurrentLocation ? 1 : 0);
    return md;
}

static std::vector<uint8_t> buildUnitValue(int32_t value, const std::string& unit) {
    std::vector<uint8_t> uv;
    pbPutStringField(uv, 1, unit);
    pbPutSint32Field(uv, 2, value);
    return uv;
}

std::vector<uint8_t> buildWeatherAddLocationCmd(const std::string& code, const std::string& name) {
    std::vector<uint8_t> loc;
    pbPutStringField(loc, 1, code);
    pbPutStringField(loc, 2, name);
    std::vector<uint8_t> weather;
    pbPutMessageField(weather, 5, loc); // location
    std::vector<uint8_t> out;
    pbPutVarintField(out, 1, cmd::TYPE_WEATHER);
    pbPutVarintField(out, 2, cmd::WX_ADD_LOCATION);
    pbPutMessageField(out, 12, weather);
    return out;
}

std::vector<uint8_t> buildWeatherCurrentCmd(const WeatherNow& w) {
    std::vector<uint8_t> cur;
    pbPutMessageField(cur, 1, buildWeatherMetadata(unixToIsoWithColons(time(nullptr)),
                                                   "", w.locationName, w.locationKey,
                                                   w.isCurrentLocation));
    pbPutVarintField(cur, 2, w.condition);
    pbPutMessageField(cur, 3, buildUnitValue(w.temperature, "\xe2\x84\x83")); // ℃
    pbPutMessageField(cur, 4, buildUnitValue(w.humidity, "%"));
    pbPutMessageField(cur, 5, buildUnitValue(w.windBeaufort, std::to_string(w.windDirDeg)));
    pbPutMessageField(cur, 6, buildUnitValue(w.uvIndex, ""));
    pbPutMessageField(cur, 7, buildUnitValue(w.aqi, "Unknown"));
    // warning: 空 WeatherWarnings 消息（与 Gadgetbridge 一致）
    pbPutLenField(cur, 8, nullptr, 0);
    pbPutFloatField(cur, 9, w.pressure * 100.0f); // pressure 字段 9（float，单位 Pa）
    std::vector<uint8_t> weather;
    pbPutMessageField(weather, 1, cur); // current
    std::vector<uint8_t> out;
    pbPutVarintField(out, 1, cmd::TYPE_WEATHER);
    pbPutVarintField(out, 2, cmd::WX_SET_CURRENT);
    pbPutMessageField(out, 12, weather);
    return out;
}

std::vector<uint8_t> buildWeatherDailyForecastCmd(const WeatherNow& today,
                                                  const std::vector<ForecastDay>& days) {
    std::vector<uint8_t> entries;
    auto putEntry = [&](uint32_t cond, int32_t tMin, int32_t tMax,
                        const std::string& sunrise, const std::string& sunset) {
        std::vector<uint8_t> e;
        pbPutMessageField(e, 1, buildUnitValue(today.aqi, "Unknown")); // aqi
        {
            std::vector<uint8_t> range;
            pbPutSint32Field(range, 1, cond);
            pbPutSint32Field(range, 2, cond);
            pbPutMessageField(e, 2, range); // conditionRange
        }
        {
            std::vector<uint8_t> range;
            pbPutSint32Field(range, 1, tMin);
            pbPutSint32Field(range, 2, tMax);
            pbPutMessageField(e, 3, range); // temperatureRange
        }
        pbPutStringField(e, 4, "\xe2\x84\x83"); // temperatureSymbol
        {
            std::vector<uint8_t> ss;
            pbPutStringField(ss, 1, sunrise);
            pbPutStringField(ss, 2, sunset);
            pbPutMessageField(e, 5, ss); // sunriseSunset
        }
        pbPutLenField(entries, 1, e.data(), e.size());
    };
    putEntry(today.condition, today.temperature, today.temperature, "", "");
    for (const auto& d : days) {
        putEntry(d.condition, d.tempMin, d.tempMax, d.sunrise, d.sunset);
    }
    std::vector<uint8_t> forecast;
    pbPutMessageField(forecast, 1, buildWeatherMetadata(unixToIsoWithColons(time(nullptr)),
                                                        "", today.locationName, today.locationKey,
                                                        today.isCurrentLocation));
    pbPutMessageField(forecast, 2, entries); // entries
    std::vector<uint8_t> weather;
    pbPutMessageField(weather, 2, forecast); // forecast
    std::vector<uint8_t> out;
    pbPutVarintField(out, 1, cmd::TYPE_WEATHER);
    pbPutVarintField(out, 2, cmd::WX_DAILY_FORECAST);
    pbPutMessageField(out, 12, weather);
    return out;
}

std::vector<uint8_t> buildClockCmd(const ClockSet& c) {
    std::vector<uint8_t> date;
    pbPutVarintField(date, 1, uint32_t(c.year));
    pbPutVarintField(date, 2, uint32_t(c.month));
    pbPutVarintField(date, 3, uint32_t(c.day));
    std::vector<uint8_t> tm;
    pbPutVarintField(tm, 1, uint32_t(c.hour));
    pbPutVarintField(tm, 2, uint32_t(c.minute));
    pbPutVarintField(tm, 3, uint32_t(c.second));
    std::vector<uint8_t> tz;
    pbPutSint32Field(tz, 1, c.zoneOffset);
    pbPutSint32Field(tz, 2, c.dstOffset);
    pbPutStringField(tz, 3, c.tzName);
    std::vector<uint8_t> clock;
    pbPutMessageField(clock, 1, date);
    pbPutMessageField(clock, 2, tm);
    pbPutMessageField(clock, 3, tz);
    pbPutVarintField(clock, 4, c.is24hour ? 0 : 1);
    std::vector<uint8_t> system;
    pbPutMessageField(system, 4, clock); // Clock 在 System 中字段号 4
    std::vector<uint8_t> out;
    pbPutVarintField(out, 1, cmd::TYPE_SYSTEM);
    pbPutVarintField(out, 2, cmd::SYS_CLOCK);
    pbPutMessageField(out, 4, system);
    return out;
}

std::vector<uint8_t> buildDeviceInfoRequestCmd() {
    std::vector<uint8_t> out;
    pbPutVarintField(out, 1, cmd::TYPE_SYSTEM);
    pbPutVarintField(out, 2, cmd::SYS_DEVICE_INFO);
    return out;
}

std::vector<uint8_t> buildBatteryRequestCmd() {
    std::vector<uint8_t> out;
    pbPutVarintField(out, 1, cmd::TYPE_SYSTEM);
    pbPutVarintField(out, 2, cmd::SYS_BATTERY);
    return out;
}

// ===================== 表盘安装响应解析 =====================

WatchfaceInstallStatus parseWatchfaceInstallStatus(const PbField& watchface) {
    WatchfaceInstallStatus r;
    if (watchface.wire != 2 || watchface.data == nullptr) return r;
    PbReader wf(watchface.data, watchface.len);
    PbField f;
    while (wf.next(f)) {
        if (f.num == 5 && f.wire == 0) { // installStatus
            r.installStatus = uint32_t(f.varint);
            r.valid = true;
        }
    }
    return r;
}

DataUploadAck parseDataUploadAck(const PbField& dataUpload) {
    DataUploadAck r;
    if (dataUpload.wire != 2 || dataUpload.data == nullptr) return r;
    PbReader du(dataUpload.data, dataUpload.len);
    PbField ack;
    if (!du.subReader(2, ack)) return r; // dataUploadAck 字段 2
    PbReader a(ack.data, ack.len);
    PbField f;
    while (a.next(f)) {
        switch (f.num) {
            case 2: // unknown2
                if (f.wire == 0) r.unknown2 = uint32_t(f.varint);
                break;
            case 4: // resumePosition
                if (f.wire == 0) r.resumePosition = uint32_t(f.varint);
                break;
            case 5: // chunkSize
                if (f.wire == 0) {
                    r.hasChunkSize = true;
                    r.chunkSize = uint32_t(f.varint);
                }
                break;
            default:
                break;
        }
    }
    r.valid = true;
    return r;
}

bool parseWatchfaceList(const PbField& watchface, std::vector<WatchfaceEntry>& out) {
    out.clear();
    if (watchface.wire != 2 || watchface.data == nullptr) return false;
    PbReader wf(watchface.data, watchface.len);
    PbField list;
    if (!wf.subReader(1, list)) return false; // watchfaceList 字段 1
    // WatchfaceList{repeated WatchfaceInfo watchface = 1}
    PbReader lr(list.data, list.len);
    PbField info;
    while (lr.next(info)) {
        if (info.num != 1 || info.wire != 2) continue;
        PbReader ir(info.data, info.len);
        WatchfaceEntry e;
        PbField f;
        while (ir.next(f)) {
            switch (f.num) {
                case 1: // id
                    if (f.wire == 2) e.id.assign(reinterpret_cast<const char*>(f.data), f.len);
                    break;
                case 2: // name
                    if (f.wire == 2) e.name.assign(reinterpret_cast<const char*>(f.data), f.len);
                    break;
                case 3: // active
                    if (f.wire == 0) e.active = (f.varint != 0);
                    break;
                case 4: // canDelete
                    if (f.wire == 0) e.canDelete = (f.varint != 0);
                    break;
                default:
                    break;
            }
        }
        out.push_back(e);
    }
    return true;
}

// ===================== 表盘管理命令构造 =====================

std::vector<uint8_t> buildWatchfaceListCmd() {
    std::vector<uint8_t> out;
    pbPutVarintField(out, 1, cmd::TYPE_WATCHFACE);
    pbPutVarintField(out, 2, cmd::WATCHFACE_LIST);
    return out;
}

std::vector<uint8_t> buildWatchfaceInstallStartCmd(const std::string& id, uint32_t size) {
    std::vector<uint8_t> start;
    pbPutStringField(start, 1, id);       // WatchfaceInstallStart.id
    pbPutVarintField(start, 2, size);     // WatchfaceInstallStart.size
    std::vector<uint8_t> wf;
    pbPutMessageField(wf, 6, start);      // Watchface.watchfaceInstallStart
    std::vector<uint8_t> out;
    pbPutVarintField(out, 1, cmd::TYPE_WATCHFACE);
    pbPutVarintField(out, 2, cmd::WATCHFACE_INSTALL);
    pbPutMessageField(out, 6, wf);
    return out;
}

std::vector<uint8_t> buildWatchfaceSetCmd(const std::string& id) {
    std::vector<uint8_t> wf;
    pbPutStringField(wf, 2, id);          // Watchface.watchfaceId
    std::vector<uint8_t> out;
    pbPutVarintField(out, 1, cmd::TYPE_WATCHFACE);
    pbPutVarintField(out, 2, cmd::WATCHFACE_SET);
    pbPutMessageField(out, 6, wf);
    return out;
}

std::vector<uint8_t> buildWatchfaceDeleteCmd(const std::string& id) {
    std::vector<uint8_t> wf;
    pbPutStringField(wf, 2, id);          // Watchface.watchfaceId
    std::vector<uint8_t> out;
    pbPutVarintField(out, 1, cmd::TYPE_WATCHFACE);
    pbPutVarintField(out, 2, cmd::WATCHFACE_DELETE);
    pbPutMessageField(out, 6, wf);
    return out;
}

std::vector<uint8_t> buildDataUploadRequestCmd(uint32_t type, const uint8_t md5[16], uint32_t size) {
    std::vector<uint8_t> req;
    pbPutVarintField(req, 1, type);       // DataUploadRequest.type
    pbPutLenField(req, 2, md5, 16);       // DataUploadRequest.md5sum
    pbPutVarintField(req, 3, size);       // DataUploadRequest.size
    std::vector<uint8_t> du;
    pbPutMessageField(du, 1, req);        // DataUpload.dataUploadRequest
    std::vector<uint8_t> out;
    pbPutVarintField(out, 1, cmd::TYPE_DATA_UPLOAD);
    pbPutVarintField(out, 2, cmd::UPLOAD_START);
    pbPutMessageField(out, 24, du);
    return out;
}

// ===================== 天气码映射 =====================
// Xiaomi 码表: Gadgetbridge XiaomiWeatherConditions
// WMO 码: Open-Meteo weather_code 文档
uint32_t wmoToXiaomiCondition(int wmoCode) {
    switch (wmoCode) {
        case 0:  return 0;  // CLEAR_SKY 晴
        case 1:  return 0;  // 主要晴 -> 晴
        case 2:  return 1;  // CLOUDY 多云
        case 3:  return 2;  // OVERCAST 阴
        case 45: return 32; // MODERATE_FOG 雾
        case 48: return 32; // 冻雾
        case 51: return 3;  // SHOWER 小毛毛雨
        case 53: return 7;  // LIGHT_RAIN 毛毛雨
        case 55: return 8;  // MODERATE_RAIN 浓毛毛雨
        case 56: return 6;  // SLEET 冻毛毛雨
        case 57: return 6;
        case 61: return 7;  // LIGHT_RAIN 小雨
        case 63: return 8;  // MODERATE_RAIN 中雨
        case 65: return 9;  // HEAVY_RAINFALL 大雨
        case 66: return 19; // FREEZING_RAIN 冻雨
        case 67: return 19;
        case 71: return 14; // LIGHT_SNOW 小雪
        case 73: return 15; // MODERATE_SNOW 中雪
        case 75: return 16; // HEAVY_SNOW 大雪
        case 77: return 33; // SNOW 雪粒
        case 80: return 3;  // SHOWER 小阵雨
        case 81: return 8;  // MODERATE_RAIN 阵雨
        case 82: return 9;  // HEAVY_RAINFALL 强阵雨
        case 85: return 13; // SNOW_SHOWERS 小阵雪
        case 86: return 16; // HEAVY_SNOW 大阵雪
        case 95: return 4;  // THUNDERSTORM 雷雨
        case 96: return 4;  // 雷雨伴冰雹
        case 99: return 5;  // HAIL 冰雹
        default: return 0;
    }
}

std::string unixToIsoWithColons(int64_t unixSeconds) {
    time_t t = time_t(unixSeconds);
    struct tm tmv;
    gmtime_r(&t, &tmv);
    char buf[40];
    snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d",
             tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
             tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
    return std::string(buf);
}

} // namespace xw
