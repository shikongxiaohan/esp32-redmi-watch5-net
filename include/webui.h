/*
 * Web 管理界面
 *
 *  - STA 模式：设备 IP 上提供状态页 / 配置页
 *  - AP 模式（配网）：SSID "RedmiWatch5Net"，192.168.4.1，DNS 劫持（ captive portal ）
 *  - 配置持久化：NVS（Preferences，命名空间 "xwnet"）
 *
 * 页面：
 *   GET  /            状态（自动刷新）
 *   GET  /config      配置表单
 *   GET  /pair        手表配对（扫描 / 选择 MAC）
 *   GET  /watchface   表盘管理（上传后临时暂存并直接安装）
 *   GET  /firmware    OTA 固件升级
 *   POST /save        保存并重启
 *   POST /weather     立即刷新天气
 *   GET  /reboot      重启
 *   GET  /api/status  JSON 状态
 *   GET  /api/watchface  JSON 表盘状态（安装进度 / 临时区 / 手表端列表）
 *   GET  /api/firmware   JSON OTA 状态
 *   POST /watchface/upload       上传表盘并直接安装（multipart，临时写入非活动 OTA 槽）
 *   POST /watchface/set          激活手表端表盘
 *   POST /watchface/uninstall    从手表卸载表盘
 *   POST /watchface/refresh      刷新手表端表盘列表
 *   POST /update                上传固件并切换启动分区
 *   POST /firmware/rollback     回滚到另一分区
 *   POST /firmware/keep         确认保留当前固件
 *   POST /pair/scan   开始扫描（参数 sec=秒）
 *   POST /pair/stop   停止扫描
 *   POST /pair/save   保存手表 MAC 并重启
 */
#pragma once

#include "appconfig.h"
#include "netgw.h"
#include "ota.h"
#include "staging.h"
#include "weather.h"
#include "watchface.h"

#include <IPAddress.h>
#include <Preferences.h>
#include <WebServer.h>
#include <DNSServer.h>

namespace xw {

class BtLink;

class WebUi {
public:
    void begin(class MiWear& wear, NetGw& gw, WeatherService& wx, AppConfig& cfg, BtLink& bt,
               WatchfaceInstaller& wf, const std::vector<WatchfaceEntry>& faces, OtaService& ota);
    void loop();

    // AP 配网模式相关
    void startAp();
    void stopAp();
    bool apActive() const { return _apActive; }

    // 配置加载/保存（NVS）
    static void loadConfig(AppConfig& cfg);
    static void saveConfig(const AppConfig& cfg);

private:
    MiWear* _wear = nullptr;
    NetGw* _gw = nullptr;
    WeatherService* _wx = nullptr;
    AppConfig* _cfg = nullptr;
    BtLink* _bt = nullptr;
    WatchfaceInstaller* _wf = nullptr;
    const std::vector<WatchfaceEntry>* _faces = nullptr;
    OtaService* _ota = nullptr;

    bool _apActive = false;
    bool _started = false;

    WebServer* _server = nullptr;
    DNSServer* _dns = nullptr;

    // 表盘上传 / 临时暂存状态
    WatchfaceStaging _wfStage;
    PartitionSource _wfSrc;
    bool _wfUploadBad = false;    // 上传被拒（格式/空间/超限）
    uint32_t _wfUploadBytes = 0;  // 已接收字节
    uint32_t _wfUploadPump = 0;   // 距上次驱动蓝牙栈的 chunk 数
    uint32_t _wfLastDataMs = 0;
    uint32_t _wfLastListReqMs = 0;

    // OTA 上传状态
    bool _otaUploadBad = false;
    uint32_t _otaUploadPump = 0;
    uint32_t _otaLastDataMs = 0;

    void _handleRoot();
    void _handleConfig();
    void _handleSave();
    void _handleWeather();
    void _handleReboot();
    void _handleApi();
    void _handleNotFound();
    void _handlePair();
    void _handlePairScan();
    void _handlePairStop();
    void _handlePairSave();

    // 表盘管理
    void _handleWatchface();
    void _handleWfUploadChunk();
    void _handleWfUploadDone();
    void _handleWfSet();
    void _handleWfUninstall();
    void _handleWfRefresh();
    void _handleWfApi();
    void _pumpDuringUpload(); // 大文件上传期间保持蓝牙/网络栈运转
    void _requestWatchfaceList();
    const char* _wfStateName(WfState s) const;

    // OTA 固件升级
    void _handleFirmware();
    void _handleFwUploadChunk();
    void _handleFwUploadDone();
    void _handleFwRollback();
    void _handleFwKeep();
    void _handleFwApi();
    bool _otaBusy() const;
};

} // namespace xw
