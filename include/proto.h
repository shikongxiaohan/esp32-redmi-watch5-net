/*
 * 小米 MiWear 协议 protobuf 编解码（proto2 语法子集）
 * 消息定义依据 Gadgetbridge app/src/main/proto/xiaomi.proto（AGPL-3.0）
 * 参考实现: nodomain.freeyourgadget.gadgetbridge.proto.xiaomi.XiaomiProto
 *
 * 只实现本项目所需的消息：Command / Auth / Weather / System(Clock)
 * 纯 C++ 无平台依赖，可在主机端单测。
 */
#pragma once

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <string>
#include <vector>

namespace xw {

// ===================== 命令类型 / 子类型常量 =====================
// 来源: Gadgetbridge XiaomiAuthService / XiaomiSystemService / XiaomiWeatherService
namespace cmd {
static const uint32_t TYPE_AUTH = 1;
static const uint32_t AUTH_SEND_USERID = 5;
static const uint32_t AUTH_NONCE = 26;
static const uint32_t AUTH_CONFIRM = 27;

static const uint32_t TYPE_SYSTEM = 2;
static const uint32_t SYS_BATTERY = 1;
static const uint32_t SYS_DEVICE_INFO = 2;
static const uint32_t SYS_CLOCK = 3;
static const uint32_t SYS_DEVICE_STATE = 79;

static const uint32_t TYPE_WEATHER = 10;
static const uint32_t WX_SET_CURRENT = 0;       // 设置当前天气
static const uint32_t WX_DAILY_FORECAST = 1;    // 每日预报
static const uint32_t WX_REQUEST_CONDITIONS = 3; // 手表请求天气(位置)
static const uint32_t WX_ADD_LOCATION = 7;      // 添加位置

static const uint32_t TYPE_NOTIFICATION = 7;
static const uint32_t NOTIF_SEND = 0;

// 表盘管理（Gadgetbridge XiaomiWatchfaceService）
static const uint32_t TYPE_WATCHFACE = 4;
static const uint32_t WATCHFACE_LIST = 0;    // 请求/接收表盘列表
static const uint32_t WATCHFACE_SET = 1;     // 激活表盘
static const uint32_t WATCHFACE_DELETE = 2;  // 删除表盘
static const uint32_t WATCHFACE_INSTALL = 4; // 安装表盘（开始）

// 数据上传（Gadgetbridge XiaomiDataUploadService）
static const uint32_t TYPE_DATA_UPLOAD = 22;
static const uint32_t UPLOAD_START = 0;
static const uint32_t UPLOAD_TYPE_WATCHFACE = 16; // 表盘（vela .bin）
} // namespace cmd

// ===================== wire format 写入 =====================
void pbPutVarint(std::vector<uint8_t>& b, uint64_t v);
void pbPutTag(std::vector<uint8_t>& b, uint32_t field, uint32_t wire);
void pbPutVarintField(std::vector<uint8_t>& b, uint32_t field, uint64_t v);
void pbPutSint32Field(std::vector<uint8_t>& b, uint32_t field, int32_t v); // zigzag
void pbPutLenField(std::vector<uint8_t>& b, uint32_t field, const uint8_t* d, size_t n);
void pbPutLenField(std::vector<uint8_t>& b, uint32_t field, const std::vector<uint8_t>& d);
void pbPutStringField(std::vector<uint8_t>& b, uint32_t field, const std::string& s);
void pbPutFloatField(std::vector<uint8_t>& b, uint32_t field, float f);    // wire type 5
void pbPutMessageField(std::vector<uint8_t>& b, uint32_t field, const std::vector<uint8_t>& msg);

// ===================== wire format 读取 =====================
struct PbField {
    uint32_t num = 0;
    uint32_t wire = 0;      // 0=varint 1=64bit 2=len-delim 5=32bit
    uint64_t varint = 0;
    const uint8_t* data = nullptr;
    size_t len = 0;
};

class PbReader {
public:
    PbReader(const uint8_t* p, size_t n) : _p(p), _n(n), _pos(0) {}
    bool next(PbField& f);
    void rewind() { _pos = 0; }
    // 便捷访问（线性查找）
    bool find(uint32_t num, PbField& out) const;
    bool has(uint32_t num) const;
    uint64_t varintOf(uint32_t num, uint64_t def = 0) const;
    int32_t sintOf(uint32_t num, int32_t def = 0) const;
    float floatOf(uint32_t num, float def = 0.0f) const;
    bool bytesOf(uint32_t num, std::vector<uint8_t>& out) const;
    bool stringOf(uint32_t num, std::string& out) const;
    bool subReader(uint32_t num, PbField& out) const; // 取子消息（字段本体）
private:
    const uint8_t* _p;
    size_t _n;
    size_t _pos;
};

// ===================== 顶层 Command 解析 =====================
struct MiCommand {
    uint32_t type = 0;
    uint32_t subtype = 0;
    bool hasStatus = false;
    uint32_t status = 0;
    PbField auth;      // field 3
    PbField weather;   // field 12
    PbField system;    // field 4
    PbField watchface; // field 6
    PbField dataUpload;// field 24
    bool ok = false;
};

MiCommand parseCommand(const uint8_t* data, size_t len);

/*
 * 认证确认判定（纯函数，可主机端单测）
 *
 * 语义：
 *   - Auth 子消息里带 status 字段时，以它为准：1 = 成功，其他值 = 失败
 *   - status 字段缺失时，回退到「命令子类型 == AUTH_CONFIRM 即视为确认」
 *
 * 关键点：不能用 varintOf(8, 1) 之类的「缺失即成功」默认值，
 * 也不能对 subtype 短路 —— 否则手表明确返回 status=0 时会被误判为认证成功，
 * 后续加密通道用错密钥，表现为连接随机中断且日志误报「认证成功」。
 */
bool authConfirmSucceeded(const MiCommand& cmd);

// ===================== 表盘安装响应解析 =====================
// Command{type=4, subtype=4, watchface{installStatus}} 的安装状态
struct WatchfaceInstallStatus {
    bool valid = false;
    uint32_t installStatus = 0; // 0=可上传 2=已安装
};
WatchfaceInstallStatus parseWatchfaceInstallStatus(const PbField& watchface);

// Command{type=22, subtype=0, dataUpload{dataUploadAck}} 的上传参数
struct DataUploadAck {
    bool valid = false;
    uint32_t unknown2 = 0;        // 必须为 0
    uint32_t resumePosition = 0;  // 必须为 0
    bool hasChunkSize = false;
    uint32_t chunkSize = 0;       // 缺省 2048（Redmi Watch 3 Active 为 4096）
};
DataUploadAck parseDataUploadAck(const PbField& dataUpload);

// 表盘列表条目（Command{type=4, subtype=0, watchface{watchfaceList{...}}}）
struct WatchfaceEntry {
    std::string id;
    std::string name;
    bool active = false;
    bool canDelete = false;
};
bool parseWatchfaceList(const PbField& watchface, std::vector<WatchfaceEntry>& out);

// ===================== 消息构造 =====================
// 认证: Command{type=1, subtype=26, auth{phoneNonce{nonce}}}
std::vector<uint8_t> buildAuthPhoneNonceCmd(const uint8_t nonce[16]);

// 认证: Command{type=1, subtype=27, auth{authStep3{encryptedNonces, encryptedDeviceInfo}}}
std::vector<uint8_t> buildAuthStep3Cmd(const uint8_t encSigns[32],
                                       const uint8_t* encDevInfo, size_t encDevInfoLen);

// AuthDeviceInfo 明文内容（unknown1=0, phoneApiLevel, phoneName, unknown3=224, region）
// 之后由 miwear 层做 AES-128-CCM 加密
std::vector<uint8_t> buildAuthDeviceInfoBytes(uint32_t phoneApiLevel,
                                              const std::string& phoneName,
                                              const std::string& region);

// 天气: Command{type=10, subtype=7, weather{location{code,name}}}  添加位置
std::vector<uint8_t> buildWeatherAddLocationCmd(const std::string& code, const std::string& name);

// 天气: Command{type=10, subtype=0, weather{current{...}}}  当前天气
struct WeatherNow {
    std::string locationName;   // 位置名（如 "北京"）
    std::string locationKey;    // accu:xxxxxx
    bool isCurrentLocation = true;
    uint32_t condition = 0;     // Xiaomi 天气码
    int32_t temperature = 0;    // ℃
    int32_t humidity = 0;       // %
    int32_t windBeaufort = 0;   // 蒲福风级
    int32_t windDirDeg = 0;     // 风向(度)
    int32_t uvIndex = 0;
    int32_t aqi = 0;
    float pressure = 0.0f;      // hPa
};
std::vector<uint8_t> buildWeatherCurrentCmd(const WeatherNow& w);

// 天气预报条目
struct ForecastDay {
    uint32_t condition = 0;
    int32_t tempMin = 0;
    int32_t tempMax = 0;
    std::string sunrise;  // ISO 时间
    std::string sunset;
};

// 天气: Command{type=10, subtype=1, weather{forecast{metadata, entries}}}  每日预报
std::vector<uint8_t> buildWeatherDailyForecastCmd(const WeatherNow& today,
                                                  const std::vector<ForecastDay>& days);

// 对时: Command{type=2, subtype=3, system{clock{date,time,timezone,isNot24hour}}}
struct ClockSet {
    int year, month, day, hour, minute, second;
    int32_t zoneOffset;  // 15 分钟为单位
    int32_t dstOffset;
    std::string tzName;
    bool is24hour = true;
};
std::vector<uint8_t> buildClockCmd(const ClockSet& c);

// 请求设备信息: Command{type=2, subtype=2}
std::vector<uint8_t> buildDeviceInfoRequestCmd();
// 请求电量: Command{type=2, subtype=1}
std::vector<uint8_t> buildBatteryRequestCmd();

// ===================== 表盘管理命令 =====================
// 请求表盘列表: Command{type=4, subtype=0}
std::vector<uint8_t> buildWatchfaceListCmd();

// 安装表盘开始: Command{type=4, subtype=4, watchface{watchfaceInstallStart{id, size}}}
std::vector<uint8_t> buildWatchfaceInstallStartCmd(const std::string& id, uint32_t size);

// 激活表盘: Command{type=4, subtype=1, watchface{watchfaceId}}
std::vector<uint8_t> buildWatchfaceSetCmd(const std::string& id);

// 删除表盘: Command{type=4, subtype=2, watchface{watchfaceId}}
std::vector<uint8_t> buildWatchfaceDeleteCmd(const std::string& id);

// 数据上传请求: Command{type=22, subtype=0,
//                       dataUpload{dataUploadRequest{type, md5sum, size}}}
std::vector<uint8_t> buildDataUploadRequestCmd(uint32_t type, const uint8_t md5[16], uint32_t size);

// ===================== 天气码映射 =====================
// WMO 天气解释码 (Open-Meteo / weather_code) -> Xiaomi 天气码
// Xiaomi 码表来源: Gadgetbridge XiaomiWeatherConditions
uint32_t wmoToXiaomiCondition(int wmoCode);

// ISO8601 时间（带冒号，秒级），与 Gadgetbridge unixTimestampToISOWithColons 一致
std::string unixToIsoWithColons(int64_t unixSeconds);

} // namespace xw
