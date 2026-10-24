/*
 * Web 管理界面实现
 * 见 webui.h 的页面与模式说明。
 */
#include "webui.h"
#include "miwear.h"
#include "btlink.h"
#include "led.h"

#include <Arduino.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <WiFi.h>
#include <time.h>

#include <cstdio>
#include <cstring>

namespace xw {

// ---------------- HTML 转义 ----------------
static String htmlEsc(const char* s) {
    String r;
    if (!s) return r;
    for (const char* p = s; *p; p++) {
        switch (*p) {
            case '&': r += "&amp;"; break;
            case '<': r += "&lt;"; break;
            case '>': r += "&gt;"; break;
            case '"': r += "&quot;"; break;
            case '\'': r += "&#39;"; break;
            default: r += *p;
        }
    }
    return r;
}

// ---------------- 配置持久化 ----------------
void WebUi::loadConfig(AppConfig& cfg) {
    Preferences p;
    p.begin("xwnet", true);
    String s;
    s = p.getString("ssid", cfg.wifiSsid); s.toCharArray(cfg.wifiSsid, sizeof(cfg.wifiSsid));
    s = p.getString("pass", cfg.wifiPass); s.toCharArray(cfg.wifiPass, sizeof(cfg.wifiPass));
    s = p.getString("authkey", cfg.authKeyHex); s.toCharArray(cfg.authKeyHex, sizeof(cfg.authKeyHex));
    s = p.getString("watchmac", cfg.watchMac); s.toCharArray(cfg.watchMac, sizeof(cfg.watchMac));
    s = p.getString("locname", cfg.locationName); s.toCharArray(cfg.locationName, sizeof(cfg.locationName));
    cfg.latitude = p.getFloat("lat", cfg.latitude);
    cfg.longitude = p.getFloat("lon", cfg.longitude);
    s = p.getString("tzposix", cfg.tzPosix); s.toCharArray(cfg.tzPosix, sizeof(cfg.tzPosix));
    s = p.getString("tzname", cfg.tzName); s.toCharArray(cfg.tzName, sizeof(cfg.tzName));
    cfg.time24h = p.getBool("h24", cfg.time24h);
    cfg.weatherRefreshMin = uint16_t(p.getUShort("wxmin", cfg.weatherRefreshMin));
    s = p.getString("appass", cfg.apPass); s.toCharArray(cfg.apPass, sizeof(cfg.apPass));
    cfg.autoResetEn = p.getBool("aren", cfg.autoResetEn);
    cfg.autoResetMin = uint16_t(p.getUShort("armin", cfg.autoResetMin));
    if (cfg.autoResetMin < 1) cfg.autoResetMin = 1;
    if (cfg.autoResetMin > 1440) cfg.autoResetMin = 1440;
    cfg.ledEnabled = p.getBool("led_en", cfg.ledEnabled);
    cfg.ledActiveLow = p.getBool("led_low", cfg.ledActiveLow);
    cfg.ledPin = int8_t(p.getChar("led_pin", char(cfg.ledPin)));
    if (cfg.ledPin < -1 || cfg.ledPin > 48) cfg.ledPin = -1; // 越界视为禁用
    p.end();
}

void WebUi::saveConfig(const AppConfig& cfg) {
    Preferences p;
    p.begin("xwnet", false);
    p.putString("ssid", cfg.wifiSsid);
    p.putString("pass", cfg.wifiPass);
    p.putString("authkey", cfg.authKeyHex);
    p.putString("watchmac", cfg.watchMac);
    p.putString("locname", cfg.locationName);
    p.putFloat("lat", cfg.latitude);
    p.putFloat("lon", cfg.longitude);
    p.putString("tzposix", cfg.tzPosix);
    p.putString("tzname", cfg.tzName);
    p.putBool("h24", cfg.time24h);
    p.putUShort("wxmin", cfg.weatherRefreshMin);
    p.putString("appass", cfg.apPass);
    p.putBool("aren", cfg.autoResetEn);
    p.putUShort("armin", cfg.autoResetMin);
    p.putBool("led_en", cfg.ledEnabled);
    p.putBool("led_low", cfg.ledActiveLow);
    p.putChar("led_pin", char(cfg.ledPin));
    p.end();
}

// ---------------- 表盘安装包缓存（NVS） ----------------
// 键：fc_present / fc_name / fc_size / fc_at
void WebUi::_fcLoad() {
    Preferences p;
    p.begin("xwnet", true);
    _fcMeta.present = p.getBool("fc_present", false);
    _fcMeta.name = p.getString("fc_name", "").c_str();
    _fcMeta.size = (uint32_t)p.getULong("fc_size", 0);
    _fcMeta.uploadedAt = (uint32_t)p.getULong("fc_at", 0);
    p.end();
}

void WebUi::_fcSave() {
    Preferences p;
    p.begin("xwnet", false);
    p.putBool("fc_present", _fcMeta.present);
    p.putString("fc_name", _fcMeta.name.c_str());
    p.putULong("fc_size", (unsigned long)_fcMeta.size);
    p.putULong("fc_at", (unsigned long)_fcMeta.uploadedAt);
    p.end();
}

void WebUi::_fcSet(const std::string& name, uint32_t size) {
    _fcMeta.present = true;
    _fcMeta.name = name;
    _fcMeta.size = size;
    _fcMeta.uploadedAt = (uint32_t)time(nullptr);
    _fcSave();
}

void WebUi::_fcClear() {
    _fcMeta.clear();
    _fcSave();
}

// 擦除暂存区中的缓存包
bool WebUi::_fcErase() {
    if (_wfStage.busy()) return false;
    std::string err;
    if (!_wfStage.eraseAll(err)) return false;
    _fcClear();
    return true;
}

// 缓存包超过 TTL 未使用 → 自动删除
void WebUi::_fcCheckExpiry() {
    if (!_fcMeta.present) return;
    uint32_t now = (uint32_t)time(nullptr);
    if (!faceCacheExpired(_fcMeta, now, FACE_CACHE_TTL_SEC)) return;
    if (_wfStage.busy() || (_wf && _wf->busy())) return;  // 安装中不动
    Serial.println("[xw] face cache expired (48h) -> erase");
    _fcErase();
}

bool WebUi::faceCacheBusy() const {
    return _wfStage.busy() || (_wf && _wf->busy());
}

// ---------------- 页面 ----------------
static const char CSS[] =
    "<style>"
    ":root{color-scheme:light;--brand:#c2410c;--brand-soft:#fff7ed;--ink:#172033;"
    "--muted:#64748b;--line:#e2e8f0;--bg:#f4f6fa;--card:#fff;--danger:#b42318}"
    "*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--ink);"
    "font-family:-apple-system,BlinkMacSystemFont,'Segoe UI','PingFang SC','Microsoft YaHei',sans-serif;"
    "font-size:15px;line-height:1.55;-webkit-text-size-adjust:100%}"
    "header{background:#172033;color:#fff;border-bottom:3px solid #ea580c}"
    ".head{max-width:960px;margin:auto;padding:22px 20px;display:flex;align-items:center;gap:13px}"
    ".logo{width:42px;height:42px;border-radius:12px;background:#ea580c;display:grid;place-items:center;"
    "font-size:20px;font-weight:800;flex:none}.brand{font-size:19px;font-weight:700;letter-spacing:.2px}"
    ".subtitle{font-size:12px;color:#cbd5e1;margin-top:1px}"
    "main{max-width:960px;margin:0 auto;padding:20px 20px 96px}"
    "nav{display:flex;gap:6px;margin:0 0 22px;padding:5px;background:#e9edf3;border-radius:12px}"
    "nav a{flex:1;text-align:center;padding:9px 12px;border-radius:8px;color:#475569;"
    "text-decoration:none;font-size:14px;font-weight:600;white-space:nowrap}"
    "nav a:hover{background:#fff;color:var(--brand)}nav a.active{background:#fff;color:var(--brand);"
    "box-shadow:0 1px 4px #0f172a14}"
    ".pagehead{margin:0 0 18px}.pagehead h1{font-size:25px;line-height:1.3;margin:0 0 5px}"
    ".pagehead p{margin:0;color:var(--muted);font-size:14px}"
    ".card{background:var(--card);border:1px solid var(--line);border-radius:14px;padding:20px;"
    "margin:0 0 16px;box-shadow:0 2px 8px #0f172a06}"
    ".card h2{font-size:16px;margin:0 0 16px;display:flex;align-items:center;gap:8px}"
    ".card h2:before{content:'';width:4px;height:17px;border-radius:3px;background:#ea580c}"
    ".cardhead{display:flex;align-items:center;justify-content:space-between;gap:12px;margin-bottom:14px}"
    ".cardhead h2{margin:0}.cardhead .meta{color:var(--muted);font-size:13px;text-align:right}"
    ".kv{margin:0;display:grid;grid-template-columns:minmax(145px,32%) 1fr}"
    ".kv dt,.kv td,.kv th{padding:11px 0;border-bottom:1px solid #edf0f4;vertical-align:top}"
    ".kv dt{color:var(--muted)}.kv dd{margin:0;font-weight:550;overflow-wrap:anywhere}"
    ".kv dt:last-of-type,.kv dd:last-of-type{border-bottom:0}.kv tr:last-child th,.kv tr:last-child td{border:0}"
    "table.kv{width:100%;border-collapse:collapse}.kv th{text-align:left;color:var(--muted);font-weight:400;width:32%}"
    ".kv td{text-align:right;font-weight:550;overflow-wrap:anywhere}"
    ".ok,.bad,.badge{display:inline-flex;align-items:center;gap:5px;font-weight:650}"
    ".ok{color:#087a45}.bad{color:var(--danger)}.badge{border-radius:999px;padding:3px 9px;font-size:12px}"
    ".badge.ok{background:#eaf8f0}.badge.bad{background:#fff0ee}.badge.neutral{background:#eef2f7;color:#526174}"
    ".grid{display:grid;grid-template-columns:1fr 1fr;gap:0 18px}.span2{grid-column:1/-1}"
    ".field{margin-bottom:15px}.field:last-child{margin-bottom:0}"
    "label{display:block;margin:0 0 6px;font-size:13px;font-weight:600;color:#3e4b60}"
    "input,select{width:100%;min-height:44px;padding:10px 12px;border:1px solid #cbd5e1;background:#fff;"
    "border-radius:9px;color:var(--ink);font:inherit;font-size:14px;outline:none}"
    "input:focus,select:focus{border-color:#ea580c;box-shadow:0 0 0 3px #ea580c1c}"
    "input[type=file]{padding:8px;background:#f8fafc}input[type=file]::file-selector-button{margin-right:10px;"
    "border:0;border-radius:6px;padding:7px 12px;background:#ffedd5;color:#9a3412;font-weight:600}"
    "button,.button{display:inline-flex;align-items:center;justify-content:center;min-height:42px;padding:9px 18px;"
    "border:1px solid transparent;border-radius:9px;background:var(--brand);color:#fff;font:inherit;font-size:14px;"
    "font-weight:650;cursor:pointer;text-decoration:none;white-space:nowrap}"
    "button:hover,.button:hover{background:#9a3412}button.secondary{background:#fff;color:#354156;border-color:#cbd5e1}"
    "button.secondary:hover{background:#f8fafc}button.danger{background:#fff;color:var(--danger);border-color:#f0b4ae}"
    "button.danger:hover{background:#fff5f4}button:disabled{cursor:not-allowed;background:#e2e8f0;color:#7b8798;border-color:#e2e8f0}"
    "button:focus-visible,a:focus-visible{outline:3px solid #fb923c;outline-offset:3px}"
    ".actions{display:flex;align-items:center;gap:10px;flex-wrap:wrap}.actions.between{justify-content:space-between}"
    "form{margin:0}form.inline{display:inline-flex;margin:0}.stack{display:grid;gap:10px}"
    ".table-wrap{overflow-x:auto;border:1px solid var(--line);border-radius:10px}"
    ".data{width:100%;border-collapse:collapse;font-size:14px}.data th,.data td{padding:12px 14px;"
    "border-bottom:1px solid #edf0f4;text-align:left;vertical-align:middle}.data th{background:#f8fafc;"
    "color:#526174;font-size:12px;font-weight:650;white-space:nowrap}.data tr:last-child td{border:0}"
    ".data td:last-child{text-align:right}.data .actions{justify-content:flex-end;flex-wrap:nowrap}"
    ".filetd{font-family:ui-monospace,SFMono-Regular,Consolas,monospace;font-size:13px;overflow-wrap:anywhere}"
    ".help,small{display:block;color:var(--muted);font-size:13px;line-height:1.55}.help{margin:10px 0 0}.flush{margin:0}"
    ".notice{padding:12px 14px;border-radius:9px;background:#fff7ed;color:#9a3412;margin:0 0 16px;font-size:13px}"
    ".danger-zone{border-color:#fecaca}.danger-zone h2:before{background:#dc2626}"
    ".scanline{display:flex;align-items:end;gap:10px}.scanline .field{width:110px;margin:0;flex:none}"
    "progress{display:block;width:100%;height:10px;margin:10px 0;border:0;border-radius:8px;overflow:hidden;background:#e8edf3}"
    "progress::-webkit-progress-bar{background:#e8edf3}progress::-webkit-progress-value{background:#ea580c}"
    "progress::-moz-progress-bar{background:#ea580c}.progress-label{font-size:22px;font-weight:750;color:var(--brand)}"
    ".empty{padding:22px;text-align:center;color:var(--muted);background:#f8fafc;border-radius:10px}"
    ".footer{text-align:center;color:#7b8798;font-size:12px;margin-top:22px}"
    "@media(min-width:700px){.split{display:grid;grid-template-columns:1fr 1fr;gap:16px}.split .card{margin-bottom:16px}}"
    "@media(max-width:600px){.head{padding:17px 16px}.logo{width:38px;height:38px}.brand{font-size:17px}"
    "main{padding:16px 12px 104px}nav{display:grid;grid-template-columns:1fr 1fr;gap:4px;margin-bottom:18px}"
    "nav a{padding:9px 6px}nav a:last-child:nth-child(odd){grid-column:1/-1}.pagehead h1{font-size:22px}.card{padding:16px;border-radius:12px}.grid{grid-template-columns:1fr}"
    ".kv{grid-template-columns:minmax(110px,38%) 1fr}.kv th{width:38%}.data th,.data td{padding:11px 12px}"
    "input,select{font-size:16px}button{min-height:44px}.actions:not(.between)>button{flex:1}.cardhead{align-items:flex-start}"
    ".scanline{align-items:end}.scanline button{flex:1}.footer{margin-top:16px}}"
    "@media(prefers-reduced-motion:reduce){*{scroll-behavior:auto!important}}"
    // ---- 性能监控 ----
    ".stats{display:grid;grid-template-columns:repeat(4,1fr);gap:12px;margin:0 0 16px}"
    ".stat{background:#fff;border:1px solid var(--line);border-radius:12px;padding:14px 16px;box-shadow:0 2px 8px #0f172a06}"
    ".stat .k{font-size:12px;color:var(--muted);font-weight:600;letter-spacing:.2px}"
    ".stat .v{font-size:26px;font-weight:750;line-height:1.2;margin-top:4px;font-variant-numeric:tabular-nums}"
    // 全局 small 是 display:block，这里必须显式改回 inline，否则 % / ℃ 会换行
    ".stat .v small{display:inline;font-size:14px;font-weight:600;color:var(--muted);margin-left:2px}"
    ".stat .sub{font-size:12px;color:var(--muted);margin-top:2px}"
    ".chartbox{position:relative;height:190px;margin:2px 0 0}"
    ".chartbox canvas{width:100%;height:100%;display:block}"
    ".legend{display:flex;gap:16px;flex-wrap:wrap;font-size:12.5px;color:#4a5768;margin-top:10px}"
    ".legend i{display:inline-block;width:10px;height:10px;border-radius:3px;margin-right:6px;vertical-align:-1px}"
    ".legend b{font-weight:650;font-variant-numeric:tabular-nums}"
    // ---- 快捷悬浮占用条（模仿汽水音乐任务栏播控）----
    ".perfdock{position:fixed;right:14px;bottom:14px;z-index:50;display:flex;align-items:center;gap:0;"
    "background:#172033f2;color:#e8eef7;border-radius:14px;padding:8px 12px;"
    "box-shadow:0 8px 26px #0f172a3d;backdrop-filter:blur(8px);font-size:12px;"
    "font-variant-numeric:tabular-nums;cursor:pointer;user-select:none;transition:padding .18s}"
    ".perfdock:hover{padding:9px 14px}"
    ".perfdock .seg{display:flex;flex-direction:column;align-items:center;padding:0 9px;min-width:46px}"
    ".perfdock .seg+.seg{border-left:1px solid #ffffff1f}"
    ".perfdock .lab{font-size:10px;color:#8fa0b6;letter-spacing:.3px}"
    ".perfdock .num{font-size:13px;font-weight:700;line-height:1.3}"
    ".perfdock .num.warn{color:#fbbf24}.perfdock .num.bad{color:#f87171}"
    ".perfdock .dot{width:8px;height:8px;border-radius:50%;background:#34d399;flex:none;margin-left:4px}"
    ".perfdock .dot.warn{background:#fbbf24}.perfdock .dot.bad{background:#f87171}"
    ".perfdock .bar{width:34px;height:3px;background:#ffffff26;border-radius:2px;margin-top:4px;overflow:hidden}"
    ".perfdock .bar i{display:block;height:100%;background:#60a5fa;border-radius:2px;transition:width .4s}"
    ".perfdock .bar i.warn{background:#fbbf24}.perfdock .bar i.bad{background:#f87171}"
    ".perfdock .chev{color:#7d8ea6;margin-left:4px;font-size:11px}"
    ".perfdock.mini .exp{display:none}"
    ".perfdock .exp{display:flex;align-items:center}"
    "@media(max-width:600px){.stats{grid-template-columns:1fr 1fr}.chartbox{height:160px}"
    ".perfdock{right:8px;bottom:8px;padding:6px 8px;font-size:11px}.perfdock .seg{padding:0 6px;min-width:38px}"
    ".perfdock .seg.hide-m{display:none}}"
    "</style>";

enum class UiPage { None, Status, Perf, Config, Pair, Watchface, Firmware };

static void pageBegin(String& h, const char* title, UiPage active, const char* refresh = nullptr) {
    h += "<!DOCTYPE html><html lang='zh-CN'><head><meta charset='utf-8'>"
         "<meta name='viewport' content='width=device-width,initial-scale=1'>";
    h += "<meta name='theme-color' content='#172033'><title>";
    h += title;
    h += " · Watch 5 网关</title>";
    if (refresh) {
        h += "<meta http-equiv='refresh' content='";
        h += refresh;
        h += "'>";
    }
    h += CSS;
    h += "</head><body><header><div class='head'><div class='logo' aria-hidden='true'>W5</div>"
         "<div><div class='brand'>Watch 5 网关</div><div class='subtitle'>REDMI WATCH 5 · ESP32</div></div></div></header><main>";
    if (active != UiPage::None) {
        h += "<nav aria-label='主导航'>";
        const char* paths[] = {"/", "/perf", "/config", "/pair", "/watchface", "/firmware"};
        const char* names[] = {"运行状态", "性能监控", "网关配置", "手表配对", "表盘管理", "固件升级"};
        UiPage pages[] = {UiPage::Status, UiPage::Perf, UiPage::Config, UiPage::Pair,
                          UiPage::Watchface, UiPage::Firmware};
        for (size_t i = 0; i < 6; i++) {
            h += "<a href='";
            h += paths[i];
            h += active == pages[i] ? "' class='active' aria-current='page'>" : "'>";
            h += names[i];
            h += "</a>";
        }
        h += "</nav>";
    }
    h += "<div class='pagehead'><h1>";
    h += title;
    h += "</h1><p>";
    switch (active) {
        case UiPage::Status: h += "查看手表连接、网络服务与设备运行状态"; break;
        case UiPage::Perf: h += "实时 CPU、内存、存储、网络与蓝牙占用"; break;
        case UiPage::Config: h += "管理网络、手表、天气和配网参数"; break;
        case UiPage::Pair: h += "扫描并选择要连接的 Redmi Watch 5"; break;
        case UiPage::Watchface: h += "上传后临时暂存并直接安装到手表"; break;
        case UiPage::Firmware: h += "通过网页升级固件，支持启动确认与回滚"; break;
        case UiPage::None: h += "Redmi Watch 5 网络网关"; break;
    }
    h += "</p></div>";
}

/*
 * 悬浮占用条（所有页面共用）
 *
 * 形态参考汽水音乐的任务栏播控：常驻右下角、一行胶囊、信息密度高但不挡内容，
 * 点一下在「精简 / 完整」之间切换。
 *
 * 显示内容：
 *   CPU / 内存 / 存储 百分比 + 三条迷你进度条
 *   网络下行、上行（复合单位）
 *   蓝牙收发（复合单位）
 *   延迟（ms）、芯片温度（℃）
 *   整体状态点（绿/黄/红）
 * 数据源：/api/perf，2 秒轮询（与后端采样间隔一致）
 */
static void perfDock(String& h) {
    h += "<div class='perfdock' id='pd' role='button' tabindex='0' "
         "aria-label='系统占用快捷查看，点击展开或收起'>"
         "<span class='dot' id='pd-dot'></span>"
         "<div class='seg'><span class='lab'>CPU</span><span class='num' id='pd-cpu'>–</span>"
         "<span class='bar'><i id='pd-cpu-b'></i></span></div>"
         "<div class='seg'><span class='lab'>内存</span><span class='num' id='pd-mem'>–</span>"
         "<span class='bar'><i id='pd-mem-b'></i></span></div>"
         "<div class='seg'><span class='lab'>存储</span><span class='num' id='pd-sto'>–</span>"
         "<span class='bar'><i id='pd-sto-b'></i></span></div>"
         "<div class='exp'>"
         "<div class='seg'><span class='lab'>↓ 网络</span><span class='num' id='pd-down'>–</span></div>"
         "<div class='seg'><span class='lab'>↑ 网络</span><span class='num' id='pd-up'>–</span></div>"
         "<div class='seg hide-m'><span class='lab'>蓝牙</span><span class='num' id='pd-bt'>–</span></div>"
         "<div class='seg'><span class='lab'>延迟</span><span class='num' id='pd-ping'>–</span></div>"
         "<div class='seg hide-m'><span class='lab'>温度</span><span class='num' id='pd-temp'>–</span></div>"
         "</div>"
         "<span class='chev' id='pd-chev'>◀</span></div>";

    h += "<script>";
    h += R"JS(
(function(){
  var dock=document.getElementById('pd');
  if(!dock) return;
  // 小屏默认收起：悬浮条在 390px 宽度下会遮住图表
  var mini=(window.innerWidth<600);
  if(mini){ dock.classList.add('mini'); }
  var chev=document.getElementById('pd-chev');
  if(chev) chev.textContent=mini?'▶':'◀';
  function fmt(b){
    if(b===null||b===undefined||b<0) return '–';
    if(b<1024) return b+' B/s';
    if(b<1048576) return (b<10240?(b/1024).toFixed(1):Math.round(b/1024))+' KB/s';
    return (b/1048576).toFixed(2)+' MB/s';
  }
  function cls(v,warn,bad){
    return v>=bad?'bad':(v>=warn?'warn':'');
  }
  function bar(id,v){
    var el=document.getElementById(id);
    if(!el) return;
    el.style.width=Math.max(2,Math.min(100,v))+'%';
    el.className=cls(v,70,90);
  }
  function num(id,txt,c){
    var el=document.getElementById(id);
    if(!el) return;
    el.textContent=txt;
    el.className='num'+(c?(' '+c):'');
  }
  function tick(){
    fetch('/api/perf').then(function(r){return r.json();}).then(function(d){
      var c=d.c||{};
      num('pd-cpu', c.cpu<0?'–':c.cpu+'%', c.cpu<0?'':cls(c.cpu,70,90));
      num('pd-mem',c.mem+'%',cls(c.mem,60,90));
      num('pd-sto',c.sto+'%',cls(c.sto,85,95));
      bar('pd-cpu-b', c.cpu<0?0:c.cpu);
      bar('pd-mem-b',c.mem); bar('pd-sto-b',c.sto);
      num('pd-down',fmt(c.netDown));
      num('pd-up',fmt(c.netUp));
      var bt=(c.btUp||0)+(c.btDown||0);
      num('pd-bt',fmt(bt));
      num('pd-ping',(c.ping===null||c.ping===undefined||c.ping<0)?'–':c.ping+' ms',
          (c.ping>=300?'bad':(c.ping>=150?'warn':'')));
      num('pd-temp',(c.temp===null||c.temp===undefined||c.temp<-100)?'–':c.temp+'℃',
          c.temp>=75?'bad':(c.temp>=60?'warn':''));
      var dot=document.getElementById('pd-dot');
      if(dot) dot.className='dot'+(c.health===2?' bad':(c.health===1?' warn':''));
    }).catch(function(){});
  }
  dock.addEventListener('click',function(){
    mini=!mini;
    dock.classList.toggle('mini',mini);
    var ch=document.getElementById('pd-chev');
    if(ch) ch.textContent=mini?'▶':'◀';
  });
  dock.addEventListener('keydown',function(e){
    if(e.key==='Enter'||e.key===' '){ e.preventDefault(); dock.click(); }
  });
  tick();
  setInterval(tick,2000);
})();
)JS";
    h += "</script>";
}

static void pageEnd(String& h, bool withDock = true) {
    h += "<div class='footer'>ESP32 · Redmi Watch 5 网络网关</div></main>";
    // 性能页本身已有完整图表，不再叠加悬浮条（省一次 2 秒轮询）
    if (withDock) perfDock(h);
    h += "</body></html>";
}

static const char* stateName(xw::MwState s) {
    switch (s) {
        case xw::MwState::DISCONNECTED: return "未连接";
        case xw::MwState::CONNECTED: return "已连接(握手)";
        case xw::MwState::VERSION_OK: return "版本就绪";
        case xw::MwState::SESSION_SENT: return "会话配置中";
        case xw::MwState::SESSION_OK: return "会话就绪";
        case xw::MwState::AUTH_NONCE_SENT: return "认证中";
        case xw::MwState::AUTHENTICATED: return "已认证";
    }
    return "?";
}

void WebUi::_handleRoot() {
    String h;
    h.reserve(8192);
    pageBegin(h, "运行状态", UiPage::Status, "5");

    bool bt = _wear && _wear->state() != xw::MwState::DISCONNECTED;
    h += "<section class='card'><h2>连接状态</h2><table class='kv'>";
    h += "<tr><th scope='row'>蓝牙 SPP</th><td>";
    h += bt ? "<span class='badge ok'>已连接</span>" : "<span class='badge bad'>未连接</span>";
    if (bt) {
        h += " · ";
        h += stateName(_wear->state());
    }
    h += "</td></tr><tr><th scope='row'>安全认证</th><td>";
    h += (_wear && _wear->authenticated())
             ? "<span class='badge ok'>加密通道就绪</span>"
             : "<span class='badge bad'>未完成认证</span>";
    h += "</td></tr><tr><th scope='row'>WiFi 网络</th><td>";
    if (_apActive) {
        h += "<span class='badge neutral'>AP 配网模式</span><br>192.168.4.1";
    } else if (WiFi.status() == WL_CONNECTED) {
        h += htmlEsc(_cfg->wifiSsid);
        h += "<br><span class='help'>" + WiFi.localIP().toString() + " · " +
             String(WiFi.RSSI()) + " dBm</span>";
    } else {
        h += "<span class='badge bad'>未连接</span>";
    }
    h += "</td></tr><tr><th scope='row'>配对密钥</th><td>";
    h += _cfg->hasAuthKey() ? "<span class='ok'>已配置</span>"
                            : "<span class='bad'>未配置，无法认证</span>";
    h += "</td></tr>";
    if (_led && _led->enabled()) {
        h += "<tr><th scope='row'>LED 指示</th><td>";
        h += "<span class='badge neutral'>" + String(ledStateName(_led->state())) + "</span>";
        h += "<br><span class='help'>GPIO" + String(_led->pin()) + " · " +
             (_led->activeLow() ? "低电平点亮" : "高电平点亮") + "</span>";
        h += "</td></tr>";
    }
    h += "</table></section>";

    uint32_t up = millis() / 1000;
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%lu 天 %02lu:%02lu:%02lu",
                  (unsigned long)(up / 86400), (unsigned long)(up / 3600 % 24),
                  (unsigned long)(up / 60 % 60), (unsigned long)(up % 60));
    h += "<section class='card'><div class='cardhead'><h2>运行统计</h2>"
         "<span class='meta'>每 5 秒自动刷新</span></div><table class='kv'>";
    h += "<tr><th scope='row'>运行时间</th><td>" + String(buf) + "</td></tr>";
    h += "<tr><th scope='row'>固件版本</th><td>" + String(_ota ? _ota->version() : "dev") +
         " · " + String(_ota ? _ota->runningLabel() : "?") + "</td></tr>";
    if (_wear) {
        h += "<tr><th scope='row'>协议帧（收 / 发）</th><td>" + String(_wear->rxFrames) + " / " +
             String(_wear->txFrames) + "</td></tr>";
        h += "<tr><th scope='row'>IP 包（收 / 发）</th><td>" + String(_wear->rxIpPackets) + " / " +
             String(_wear->txIpPackets) + "</td></tr>";
    }
    if (_gw) {
        h += "<tr><th scope='row'>网关包（收 / 发）</th><td>" + String(_gw->rxPackets) + " / " +
             String(_gw->txPackets) + "</td></tr>";
        h += "<tr><th scope='row'>DHCP / ICMP 应答</th><td>" + String(_gw->dhcpReplies) + " / " +
             String(_gw->icmpReplies) + "</td></tr>";
        h += "<tr><th scope='row'>TCP 连接 / 字节（↑ / ↓）</th><td>" + String(_gw->tcpConnsOpened) + " · " +
             String(_gw->tcpBytesUp) + " / " + String(_gw->tcpBytesDown) + "</td></tr>";
        h += "<tr><th scope='row'>UDP 流 / 丢弃</th><td>" + String(_gw->udpFlows) + " / " +
             String(_gw->dropped) + "</td></tr>";
    }
    h += "</table></section>";

    if (_wx) {
        h += "<section class='card'><h2>天气服务</h2><table class='kv'><tr><th scope='row'>当前位置</th><td>";
        h += htmlEsc(_cfg->locationName);
        h += "</td></tr><tr><th scope='row'>服务状态</th><td>";
        if (!_wx->lastError.empty()) {
            h += "<span class='bad'>" + htmlEsc(_wx->lastError.c_str()) + "</span>";
        } else {
            h += "<span class='badge ok'>运行正常</span>";
        }
        h += "</td></tr><tr><th scope='row'>成功刷新</th><td>" + String(_wx->lastFetchCount) + " 次</td></tr>";
        h += "<tr><th scope='row'>刷新策略</th><td>" +
             String(_cfg->weatherRefreshMin == 0 ? "仅手表请求" :
                    String(_cfg->weatherRefreshMin) + " 分钟") + "</td></tr></table></section>";
    }

    h += "<section class='card'><h2>设备操作</h2><div class='actions between'>"
         "<form method='post' action='/weather'><button type='submit' class='secondary'>立即刷新天气</button></form>"
         "<form method='post' action='/reboot'><button type='submit' class='danger'>重启设备</button></form>"
         "</div></section>";
    pageEnd(h);
    _server->send(200, "text/html; charset=utf-8", h);
}

void WebUi::_handleConfig() {
    AppConfig& c = *_cfg;
    String h;
    h.reserve(12288);
    pageBegin(h, "网关配置", UiPage::Config);
    h += "<form method='post' action='/save'>";

    h += "<section class='card'><h2>WiFi 网络</h2><div class='grid'>";
    h += "<div class='field'><label for='ssid'>WiFi 名称（SSID）</label>"
         "<input id='ssid' name='ssid' maxlength='32' autocomplete='off' value='" +
         htmlEsc(c.wifiSsid) + "'></div>";
    h += "<div class='field'><label for='pass'>WiFi 密码</label>"
         "<input id='pass' name='pass' maxlength='64' type='password' autocomplete='new-password' value='" +
         htmlEsc(c.wifiPass) + "'></div></div></section>";

    h += "<section class='card'><h2>手表连接</h2>";
    h += "<div class='field'><label for='authkey'>配对密钥</label>"
         "<input id='authkey' name='authkey' maxlength='32' pattern='[0-9a-fA-F]*' "
         "aria-describedby='authkey-help' placeholder='16 字节十六进制密钥' value='" +
         htmlEsc(c.authKeyHex) + "'><small id='authkey-help' class='help'>"
         "从小米运动健康提取；留空使用默认密钥。</small></div>";
    h += "<div class='field'><label for='watchmac'>手表蓝牙 MAC</label>"
         "<input id='watchmac' name='watchmac' class='filetd' maxlength='17' "
         "aria-describedby='watchmac-help' placeholder='AA:BB:CC:DD:EE:FF' value='" +
         htmlEsc(c.watchMac) + "'><small id='watchmac-help' class='help'>"
         "留空时自动尝试已配对设备，也可在「手表配对」页扫描选择。</small></div></section>";

    h += "<section class='card'><h2>天气与时间</h2><div class='grid'>";
    h += "<div class='field span2'><label for='locname'>天气位置名称</label>"
         "<input id='locname' name='locname' maxlength='39' value='" + htmlEsc(c.locationName) + "'></div>";
    h += "<div class='field'><label for='lat'>纬度</label><input id='lat' name='lat' inputmode='decimal' value='" +
         String(c.latitude, 4) + "'></div>";
    h += "<div class='field'><label for='lon'>经度</label><input id='lon' name='lon' inputmode='decimal' value='" +
         String(c.longitude, 4) + "'></div>";
    h += "<div class='field'><label for='tzposix'>POSIX 时区</label>"
         "<input id='tzposix' name='tzposix' maxlength='47' aria-describedby='tz-help' value='" +
         htmlEsc(c.tzPosix) + "'><small id='tz-help' class='help'>中国大陆使用 CST-8</small></div>";
    h += "<div class='field'><label for='tzname'>手表显示时区名</label>"
         "<input id='tzname' name='tzname' maxlength='39' value='" + htmlEsc(c.tzName) + "'></div>";
    h += "<div class='field'><label for='h24'>时间格式</label><select id='h24' name='h24'>";
    h += c.time24h ? "<option value='1' selected>24 小时</option><option value='0'>12 小时</option>"
                   : "<option value='1'>24 小时</option><option value='0' selected>12 小时</option>";
    h += "</select></div>";
    h += "<div class='field'><label for='wxmin'>天气刷新间隔</label>"
         "<input id='wxmin' name='wxmin' type='number' min='0' max='1440' aria-describedby='wx-help' value='" +
         String(c.weatherRefreshMin) + "'><small id='wx-help' class='help'>分钟；0 表示仅手表请求时推送</small></div>";
    h += "</div></section>";

    h += "<section class='card'><h2>配网 AP</h2><div class='field'>"
         "<label for='appass'>AP 密码</label><input id='appass' name='appass' maxlength='64' "
         "type='password' aria-describedby='ap-help' value='" + htmlEsc(c.apPass) + "'>"
         "<small id='ap-help' class='help'>留空为开放网络；设置密码时至少 8 位。</small></div></section>";

    h += "<section class='card'><h2>自动恢复出厂</h2><div class='grid'>";
    h += "<div class='field'><label for='aren'>状态</label><select id='aren' name='aren'>";
    h += c.autoResetEn ? "<option value='1' selected>开启</option><option value='0'>关闭（推荐）</option>"
                       : "<option value='1'>开启</option><option value='0' selected>关闭（推荐）</option>";
    h += "</select></div>";
    h += "<div class='field'><label for='armin'>超时（分钟）</label>"
         "<input id='armin' name='armin' type='number' min='1' max='1440' aria-describedby='ar-help' value='" +
         String(c.autoResetMin) + "'><small id='ar-help' class='help'>1–1440 分钟</small></div>";
    h += "</div><small class='help flush'>开启后，若连续这段时间内 <strong>WiFi 和手表都没有连接</strong>，"
         "设备会清空 WiFi、配对密钥与手表 MAC，然后重启进入配网模式。仅建议无人值守场景开启，默认关闭；"
         "设备处于配网 AP 模式时不计时。</small></section>";
    h += "<section class='card'><h2>板载 LED 指示</h2><div class='grid'>";
    h += "<div class='field'><label for='led_en'>状态</label><select id='led_en' name='led_en'>";
    h += c.ledEnabled ? "<option value='1' selected>开启</option><option value='0'>关闭</option>"
                      : "<option value='1'>开启</option><option value='0' selected>关闭</option>";
    h += "</select></div>";
    h += "<div class='field'><label for='led_pin'>GPIO 引脚</label>"
         "<input id='led_pin' name='led_pin' type='number' min='-1' max='48' "
         "aria-describedby='led-pin-help' value='" + String(c.ledPin) + "'>"
         "<small id='led-pin-help' class='help'>-1 表示禁用；多数开发板板载 LED 在 GPIO2</small></div>";
    h += "<div class='field'><label for='led_low'>点亮电平</label><select id='led_low' name='led_low'>";
    h += c.ledActiveLow ? "<option value='1' selected>低电平点亮（常见）</option><option value='0'>高电平点亮</option>"
                        : "<option value='1'>低电平点亮（常见）</option><option value='0' selected>高电平点亮</option>";
    h += "</select></div>";
    h += "</div><small class='help flush'>指示含义："
         "<strong>快闪</strong>启动中 · <strong>慢闪</strong>配网 AP · "
         "<strong>常亮</strong>已联网并连接手表 · <strong>极慢短闪</strong>已联网但手表未连接 · "
         "<strong>快速双闪</strong>蓝牙配对中 · <strong>短亮长灭</strong>正在安装表盘 · "
         "<strong>方波</strong>正在升级固件。</small></section>";

    h += "<section class='card'><h2>LED 测试</h2><div class='actions between'>"
         "<div>手动点亮 / 熄灭板载 LED<small>用于确认引脚与点亮电平是否正确</small></div>"
         "<div class='actions'>"
         "<form method='post' action='/led/test'><input type='hidden' name='on' value='1'>"
         "<button type='submit' class='secondary'>点亮</button></form>"
         "<form method='post' action='/led/test'><input type='hidden' name='on' value='0'>"
         "<button type='submit' class='secondary'>熄灭</button></form>"
         "</div></div></section>";

    h += "<div class='actions between card'><span class='help flush'>保存后设备将自动重启</span>"
         "<button type='submit'>保存配置并重启</button></div></form>";

    h += "<section class='card danger-zone'><h2>重置网络</h2>"
         "<div class='actions between'><div>忘记当前 WiFi<small>清除配置并重新进入 AP 配网模式</small></div>"
         "<form method='post' action='/forget' onsubmit='return confirm(\"确定清除 WiFi 配置并进入配网模式？\")'>"
         "<button type='submit' class='danger'>忘记 WiFi</button></form></div></section>";
    pageEnd(h);
    _server->send(200, "text/html; charset=utf-8", h);
}

void WebUi::_handleSave() {
    if (_otaBusy() || (_wf && _wf->busy())) {
        _server->send(409, "text/plain; charset=utf-8", "固件升级或表盘安装进行中");
        return;
    }
    AppConfig& c = *_cfg;
    auto copyArg = [&](const char* name, char* dst, size_t cap) {
        if (_server->hasArg(name)) {
            String v = _server->arg(name);
            v.trim();
            std::strncpy(dst, v.c_str(), cap - 1);
            dst[cap - 1] = 0;
        }
    };
    copyArg("ssid", c.wifiSsid, sizeof(c.wifiSsid));
    copyArg("pass", c.wifiPass, sizeof(c.wifiPass));
    copyArg("authkey", c.authKeyHex, sizeof(c.authKeyHex));
    copyArg("watchmac", c.watchMac, sizeof(c.watchMac));
    copyArg("locname", c.locationName, sizeof(c.locationName));
    copyArg("tzposix", c.tzPosix, sizeof(c.tzPosix));
    copyArg("tzname", c.tzName, sizeof(c.tzName));
    copyArg("appass", c.apPass, sizeof(c.apPass));
    if (_server->hasArg("lat")) c.latitude = _server->arg("lat").toFloat();
    if (_server->hasArg("lon")) c.longitude = _server->arg("lon").toFloat();
    if (_server->hasArg("wxmin")) {
        int m = _server->arg("wxmin").toInt();
        if (m < 0) m = 0;
        if (m > 1440) m = 1440;
        c.weatherRefreshMin = uint16_t(m);
    }
    c.time24h = !_server->hasArg("h24") || _server->arg("h24") != "0";
    c.autoResetEn = _server->hasArg("aren") && _server->arg("aren") == "1";
    if (_server->hasArg("armin")) {
        int m = _server->arg("armin").toInt();
        if (m < 1) m = 1;
        if (m > 1440) m = 1440;
        c.autoResetMin = uint16_t(m);
    }
    if (c.autoResetMin < 1) c.autoResetMin = 1;
    if (c.autoResetMin > 1440) c.autoResetMin = 1440;
    c.ledEnabled = _server->hasArg("led_en") && _server->arg("led_en") == "1";
    c.ledActiveLow = !_server->hasArg("led_low") || _server->arg("led_low") == "1";
    if (_server->hasArg("led_pin")) {
        int pin = _server->arg("led_pin").toInt();
        if (pin < -1) pin = -1;
        if (pin > 48) pin = 48;
        c.ledPin = int8_t(pin);
    }

    // 密钥规范化：去空格、转小写
    {
        char* d = c.authKeyHex;
        char* w = c.authKeyHex;
        while (*d) {
            if (*d != ' ' && *d != ':') {
                *w++ = char(*d >= 'A' && *d <= 'F' ? *d + 32 : *d);
            }
            d++;
        }
        *w = 0;
    }

    saveConfig(c);
    String h;
    pageBegin(h, "配置已保存", UiPage::None, "4;url=/");
    h += "<section class='card'><span class='badge ok'>保存成功</span>"
         "<p>设备即将重启，4 秒后自动返回运行状态页。</p></section>";
    pageEnd(h);
    _server->send(200, "text/html; charset=utf-8", h);
    delay(500);
    ESP.restart();
}

void WebUi::_handleWeather() {
    bool ok = _wx ? _wx->refreshNow() : false;
    String h;
    pageBegin(h, "天气刷新", UiPage::None, "2;url=/");
    h += "<section class='card'>";
    h += ok ? "<span class='badge ok'>推送成功</span><p>最新天气已推送至手表。</p>"
            : "<span class='badge bad'>推送失败</span><p>请在运行状态页查看详细错误信息。</p>";
    h += "<small>2 秒后自动返回</small></section>";
    pageEnd(h);
    _server->send(200, "text/html; charset=utf-8", h);
}

void WebUi::_handleReboot() {
    if (_otaBusy() || (_wf && _wf->busy())) {
        _server->send(409, "text/plain; charset=utf-8", "固件升级或表盘安装进行中");
        return;
    }
    String h;
    pageBegin(h, "正在重启", UiPage::None, "10");
    h += "<section class='card'><span class='badge neutral'>设备重启中</span>"
         "<p>请稍候，重启完成后可重新打开运行状态页。</p></section>";
    pageEnd(h);
    _server->send(200, "text/html; charset=utf-8", h);
    delay(300);
    ESP.restart();
}

// ---------------- 性能监控 ----------------
/*
 * 页面结构：
 *   1) 四个大数字卡片（CPU / 内存 / 存储 / 温度）
 *   2) CPU·内存·存储 百分比折线图（三条线，0-100 固定纵轴）
 *   3) 网络上下行折线图（自动纵轴，复合单位）
 *   4) 蓝牙吞吐 + 网络延迟折线图
 * 数据全部由 /api/perf 提供，前端每 2 秒拉一次，不依赖外部 CDN。
 */
void WebUi::_handlePerf() {
    String h;
    h.reserve(9000);
    pageBegin(h, "性能监控", UiPage::Perf);

    h += "<div class='stats'>"
         "<div class='stat'><div class='k'>CPU 占用</div><div class='v' id='s-cpu'>–<small>%</small></div>"
         "<div class='sub'>空闲钩子频率估算</div></div>"
         "<div class='stat'><div class='k'>内存占用</div><div class='v' id='s-mem'>–<small>%</small></div>"
         "<div class='sub' id='s-mem-sub'>–</div></div>"
         "<div class='stat'><div class='k'>存储占用</div><div class='v' id='s-sto'>–<small>%</small></div>"
         "<div class='sub' id='s-sto-sub'>–</div></div>"
         "<div class='stat'><div class='k'>芯片温度</div><div class='v' id='s-temp'>–<small>℃</small></div>"
         "<div class='sub'>ESP32 内部传感器</div></div>"
         "</div>";

    h += "<section class='card'><div class='cardhead'><h2>CPU / 内存 / 存储</h2>"
         "<span class='meta' id='meta-a'>最近 3 分钟</span></div>"
         "<div class='chartbox'><canvas id='c-a'></canvas></div>"
         "<div class='legend'>"
         "<span><i style='background:#ea580c'></i>CPU <b id='l-cpu'>–</b></span>"
         "<span><i style='background:#2563eb'></i>内存 <b id='l-mem'>–</b></span>"
         "<span><i style='background:#059669'></i>存储 <b id='l-sto'>–</b></span>"
         "</div></section>";

    h += "<section class='card'><div class='cardhead'><h2>网络上下行</h2>"
         "<span class='meta'>手表经网关的 TCP + UDP 流量</span></div>"
         "<div class='chartbox'><canvas id='c-b'></canvas></div>"
         "<div class='legend'>"
         "<span><i style='background:#7c3aed'></i>下行 <b id='l-down'>–</b></span>"
         "<span><i style='background:#0891b2'></i>上行 <b id='l-up'>–</b></span>"
         "</div></section>";

    h += "<section class='card'><div class='cardhead'><h2>蓝牙吞吐</h2>"
         "<span class='meta'>SPP 链路实际收发</span></div>"
         "<div class='chartbox'><canvas id='c-c'></canvas></div>"
         "<div class='legend'>"
         "<span><i style='background:#db2777'></i>蓝牙下行 <b id='l-btd'>–</b></span>"
         "<span><i style='background:#65a30d'></i>蓝牙上行 <b id='l-btu'>–</b></span>"
         "</div></section>";

    h += "<section class='card'><div class='cardhead'><h2>网络延迟</h2>"
         "<span class='meta'>每 15 秒探测一次</span></div>"
         "<div class='chartbox'><canvas id='c-d'></canvas></div>"
         "<div class='legend'>"
         "<span><i style='background:#475569'></i>TCP 握手往返 <b id='l-ping'>–</b></span>"
         "</div></section>";

    h += "<section class='card'><h2>数据说明</h2><table class='kv'>"
         "<tr><th scope='row'>CPU 占用</th><td>空闲任务钩子调用频率的相对估算值，不是硬件计数；"
         "只反映趋势与相对高低，绝对值可能有偏差</td></tr>"
         "<tr><th scope='row'>内存占用</th><td>8 位可分配堆（不含 PSRAM）的已用比例</td></tr>"
         "<tr><th scope='row'>存储占用</th><td>应用镜像 + 表盘缓存 + 系统区，对总 Flash 容量</td></tr>"
         "<tr><th scope='row'>蓝牙吞吐</th><td>SPP 链路实际收发字节速率，不是射频占空比</td></tr>"
         "<tr><th scope='row'>网络延迟</th><td>到 223.5.5.5:53 的 TCP 握手往返时间，"
         "失败显示为 –</td></tr>"
         "<tr><th scope='row'>芯片温度</th><td>ESP32 内部温度传感器，精度约 ±10℃，"
         "仅表示芯片温度趋势</td></tr>"
         "</table></section>";

    // ---- 图表与轮询脚本 ----
    h += "<script>";
    h += R"JS(
(function(){
  var hist=[];
  function fmtRate(b){
    if(b<1024) return b+' B/s';
    if(b<1048576) return (b<10240?(b/1024).toFixed(1):Math.round(b/1024))+' KB/s';
    return (b/1048576).toFixed(2)+' MB/s';
  }
  function fmtBytes(b){
    if(b<1024) return b+' B';
    if(b<1048576) return (b/1024).toFixed(1)+' KB';
    if(b<1073741824) return (b/1048576).toFixed(1)+' MB';
    return (b/1073741824).toFixed(2)+' GB';
  }
  // 纵轴刻度用统一单位：以轴上限决定单位，避免"0 B/s"和"47 KB/s"混排
  function axisRate(max){
    var unit='B/s', div=1;
    if(max>=1048576){ unit='MB/s'; div=1048576; }
    else if(max>=1024){ unit='KB/s'; div=1024; }
    return function(v){
      var x=v/div;
      var s=(x>=10||x===0)?Math.round(x).toString():x.toFixed(1);
      return s+' '+unit;
    };
  }
  // 画一张多线折线图
  function draw(cv, series, opt){
    var dpr=window.devicePixelRatio||1;
    var w=cv.clientWidth||600, hh=cv.clientHeight||180;
    if(cv.width!==Math.round(w*dpr)||cv.height!==Math.round(hh*dpr)){
      cv.width=Math.round(w*dpr); cv.height=Math.round(hh*dpr);
    }
    var g=cv.getContext('2d');
    g.setTransform(dpr,0,0,dpr,0,0);
    g.clearRect(0,0,w,hh);
    // 左侧留给纵轴标签：百分比刻度较窄，复合单位（"12.3 KB/s"）需要更宽
    var padL=opt.padL||34,padR=8,padT=8,padB=16;
    var iw=w-padL-padR, ih=hh-padT-padB;
    // 网格
    g.strokeStyle='#eef2f7'; g.lineWidth=1;
    for(var i=0;i<=4;i++){
      var y=padT+ih*i/4;
      g.beginPath(); g.moveTo(padL,y); g.lineTo(w-padR,y); g.stroke();
    }
    // 纵轴刻度
    g.fillStyle='#93a1b3'; g.font='10px -apple-system,sans-serif'; g.textAlign='right';
    for(var i=0;i<=4;i++){
      var y=padT+ih*i/4;
      var v=opt.max*(1-i/4);
      g.fillText(opt.fmt? opt.fmt(v): Math.round(v)+'%', padL-6, y+3);
    }
    if(!hist.length) return;
    var n=hist.length;
    var stepX=n>1? iw/(n-1):0;
    for(var s=0;s<series.length;s++){
      var key=series[s].key, color=series[s].color;
      g.beginPath(); g.strokeStyle=color; g.lineWidth=2; g.lineJoin='round';
      var started=false;
      for(var i=0;i<n;i++){
        var v=hist[i][key];
        // 负值 / null 表示该项不可用：断线，不画成 0（否则会被误读成"占用为 0"）
        if(v===null||v===undefined||v<0){ started=false; continue; }
        var x=padL+stepX*i;
        var y=padT+ih*(1-Math.min(v,opt.max)/opt.max);
        if(!started){ g.moveTo(x,y); started=true; } else g.lineTo(x,y);
      }
      g.stroke();
      // 末端点
      var lv=hist[n-1][key];
      if(lv!==null&&lv!==undefined){
        var lx=padL+stepX*(n-1), ly=padT+ih*(1-Math.min(lv,opt.max)/opt.max);
        g.beginPath(); g.arc(lx,ly,2.6,0,Math.PI*2); g.fillStyle=color; g.fill();
      }
    }
  }
  var ca=document.getElementById('c-a');
  var cb=document.getElementById('c-b');
  var cc=document.getElementById('c-c');
  var cd=document.getElementById('c-d');
  function bump(el,cls){
    el.className = cls || '';
  }
  function setNum(id,txt){
    var el=document.getElementById(id);
    if(el) el.innerHTML=txt;
  }
  function refresh(){
    fetch('/api/perf').then(function(r){return r.json();}).then(function(d){
      hist=d.h||[];
      var cur=d.c||{};
      // 数字卡片
      setNum('s-cpu', (cur.cpu<0?'–':cur.cpu)+'<small>%</small>');
      setNum('s-mem', cur.mem+'<small>%</small>');
      setNum('s-sto', cur.sto+'<small>%</small>');
      setNum('s-temp', (cur.temp===null||cur.temp===undefined||cur.temp<-100)?'–':cur.temp)+'<small>℃</small>';
      if(document.getElementById('s-mem-sub')) document.getElementById('s-mem-sub').textContent=
        fmtBytes(cur.memFree)+' 可用 / '+fmtBytes(cur.memTotal);
      if(document.getElementById('s-sto-sub')) document.getElementById('s-sto-sub').textContent=
        fmtBytes(cur.stoUsed)+' / '+fmtBytes(cur.stoTotal);
      // 图例
      setNum('l-cpu', (cur.cpu<0?'–':cur.cpu+'%'));
      setNum('l-mem', cur.mem+'%');
      setNum('l-sto', cur.sto+'%');
      setNum('l-down', fmtRate(cur.netDown||0));
      setNum('l-up', fmtRate(cur.netUp||0));
      setNum('l-btd', fmtRate(cur.btDown||0));
      setNum('l-btu', fmtRate(cur.btUp||0));
      setNum('l-ping', (cur.ping===null||cur.ping===undefined||cur.ping<0)?'–':cur.ping+' ms');
      // 图表
      draw(ca,[{key:'cpu',color:'#ea580c'},{key:'mem',color:'#2563eb'},{key:'sto',color:'#059669'}],
           {max:100,fmt:function(v){return Math.round(v)+'%';}});
      var netMax=1;
      for(var i=0;i<hist.length;i++){
        netMax=Math.max(netMax,hist[i].netDown||0,hist[i].netUp||0);
      }
      netMax=netMax*1.15;
      draw(cb,[{key:'netDown',color:'#7c3aed'},{key:'netUp',color:'#0891b2'}],
           {max:netMax,padL:58,fmt:axisRate(netMax)});
      var cMax=1;
      for(var i=0;i<hist.length;i++){
        cMax=Math.max(cMax,hist[i].btDown||0,hist[i].btUp||0);
      }
      cMax=cMax*1.15;
      draw(cc,[{key:'btDown',color:'#db2777'},{key:'btUp',color:'#65a30d'}],
           {max:cMax,padL:58,fmt:axisRate(cMax)});
      // 延迟单独一张图：单位是 ms，与蓝牙吞吐（B/s）量纲不同，不能共用一个纵轴
      var pMax=100;
      for(var i=0;i<hist.length;i++){
        var pv=hist[i].ping;
        if(pv!==null&&pv!==undefined&&pv>=0) pMax=Math.max(pMax,pv);
      }
      pMax=Math.ceil(pMax*1.2/50)*50; // 取到 50ms 的整数倍，刻度好读
      draw(cd,[{key:'ping',color:'#475569'}],
           {max:pMax,padL:48,fmt:function(v){return Math.round(v)+' ms';}});
    }).catch(function(){});
  }
  refresh();
  setInterval(refresh,2000);
  window.addEventListener('resize',function(){ refresh(); });
})();
)JS";
    h += "</script>";

    pageEnd(h, false); // 本页已有完整图表，不叠加悬浮占用条
    _server->send(200, "text/html; charset=utf-8", h);
}

void WebUi::_handlePerfApi() {
    if (!_perf) {
        _server->send(503, "application/json", "{\"err\":\"perf unavailable\"}");
        return;
    }
    const PerfSample& c = _perf->latest();
    const PerfHistory& hst = _perf->history();

    String j;
    j.reserve(3000 + hst.count() * 60);
    j += "{\"c\":{";
    // CPU 不可用（255）时输出 -1，前端显示为 –
    j += "\"cpu\":" + String(c.cpu == PERF_CPU_UNAVAILABLE ? -1 : int(c.cpu));
    j += ",\"mem\":" + String(c.mem);
    j += ",\"sto\":" + String(c.sto);
    // 供前端显示"已用/总量"（取同一份采样，避免与百分比不一致）
    j += ",\"memFree\":" + String((unsigned)c.memFree);
    j += ",\"memTotal\":" + String((unsigned)c.memTotal);
    j += ",\"stoUsed\":" + String((unsigned)c.stoUsed);
    j += ",\"stoTotal\":" + String((unsigned)c.stoTotal);
    j += ",\"netUp\":" + String((unsigned)c.netUp);
    j += ",\"netDown\":" + String((unsigned)c.netDown);
    j += ",\"btUp\":" + String((unsigned)c.btUp);
    j += ",\"btDown\":" + String((unsigned)c.btDown);
    j += ",\"ping\":" + String((int)c.ping);
    j += ",\"temp\":" + String((int)c.temp);
    j += ",\"health\":" + String(c.health);
    j += "},\"h\":[";
    for (int i = 0; i < hst.count(); i++) {
        const PerfSample& s = hst.at(i);
        if (i) j += ",";
        j += "{\"cpu\":" + String(s.cpu == PERF_CPU_UNAVAILABLE ? -1 : int(s.cpu));
        j += ",\"mem\":" + String(s.mem);
        j += ",\"sto\":" + String(s.sto);
        j += ",\"netUp\":" + String((unsigned)s.netUp);
        j += ",\"netDown\":" + String((unsigned)s.netDown);
        j += ",\"btUp\":" + String((unsigned)s.btUp);
        j += ",\"btDown\":" + String((unsigned)s.btDown);
        j += ",\"ping\":" + String((int)s.ping);
        j += "}";
    }
    j += "]}";
    _server->send(200, "application/json", j);
}

void WebUi::_handleApi() {
    String j = "{";
    j += "\"uptime\":" + String(millis() / 1000);
    j += ",\"btConnected\":" + String(_wear && _wear->state() != xw::MwState::DISCONNECTED ? 1 : 0);
    j += ",\"authenticated\":" + String(_wear && _wear->authenticated() ? 1 : 0);
    j += ",\"state\":\"" + String(stateName(_wear ? _wear->state() : xw::MwState::DISCONNECTED)) + "\"";
    if (WiFi.status() == WL_CONNECTED) {
        j += ",\"wifi\":\"" + WiFi.localIP().toString() + "\"";
    } else if (_apActive) {
        j += ",\"wifi\":\"ap:192.168.4.1\"";
    } else {
        j += ",\"wifi\":\"down\"";
    }
    if (_gw) {
        j += ",\"gwRx\":" + String(_gw->rxPackets);
        j += ",\"gwTx\":" + String(_gw->txPackets);
        j += ",\"dhcp\":" + String(_gw->dhcpReplies);
        j += ",\"icmp\":" + String(_gw->icmpReplies);
        j += ",\"tcpConns\":" + String(_gw->tcpConnsOpened);
        j += ",\"udpFlows\":" + String(_gw->udpFlows);
    }
    if (_led && _led->enabled()) {
        j += ",\"led\":\"" + String(ledStateName(_led->state())) + "\"";
        j += ",\"ledPin\":" + String(_led->pin());
    } else {
        j += ",\"led\":\"disabled\"";
    }
    j += "}";
    _server->send(200, "application/json", j);
}

// ---------------- 配对页 ----------------
void WebUi::_handlePair() {
    String h;
    h.reserve(12288);
    pageBegin(h, "手表配对", UiPage::Pair, (_bt && _bt->scanning()) ? "3" : nullptr);

    h += "<section class='card'><h2>当前设备</h2><table class='kv'>";
    h += "<tr><th scope='row'>手表 MAC</th><td class='filetd'>";
    if (_bt && _bt->hasWatchMac()) {
        h += htmlEsc(_bt->watchMacStr());
    } else {
        h += "<span class='badge neutral'>自动模式</span><br><span class='help'>自动尝试已配对设备</span>";
    }
    h += "</td></tr><tr><th scope='row'>连接状态</th><td>";
    if (_wear && _wear->state() != xw::MwState::DISCONNECTED) {
        h += "<span class='badge ok'>已连接</span><br><span class='help'>";
        h += stateName(_wear->state());
        h += "</span>";
    } else {
        h += "<span class='badge bad'>未连接</span>";
    }
    h += "</td></tr></table></section>";

    h += "<section class='card'><div class='cardhead'><h2>扫描蓝牙设备</h2>";
    if (_bt && _bt->scanning()) h += "<span class='badge ok'>扫描中</span>";
    h += "</div>";
    if (_bt && _bt->scanning()) {
        h += "<div class='actions between'><span>正在搜索附近设备，页面每 3 秒刷新</span>"
             "<form method='post' action='/pair/stop'><button type='submit' class='secondary'>停止扫描</button></form></div>";
    } else {
        h += "<form method='post' action='/pair/scan'><div class='scanline'>"
             "<div class='field'><label for='scan-sec'>扫描时长（秒）</label>"
             "<input id='scan-sec' type='number' name='sec' value='8' min='3' max='30'></div>"
             "<button type='submit'>开始扫描</button></div></form>";
        h += "<small class='help'>扫描时请让手表保持可发现状态，或将手表放在旁边并点亮屏幕。</small>";
    }
    h += "</section>";

    if (_bt && _bt->scanCount() > 0) {
        h += "<section class='card'><div class='cardhead'><h2>扫描结果</h2><span class='meta'>共 " +
             String(_bt->scanCount()) + " 个设备</span></div><div class='table-wrap'><table class='data'>"
             "<thead><tr><th scope='col'>设备名称</th><th scope='col'>MAC 地址</th>"
             "<th scope='col'>信号</th><th scope='col'>操作</th></tr></thead><tbody>";
        for (size_t i = 0; i < _bt->scanCount(); i++) {
            const BtLink::Device& d = _bt->scanResult(i);
            bool isTarget = _bt->hasWatchMac() && String(d.mac) == String(_bt->watchMacStr());
            h += "<tr><td>" + htmlEsc(d.name[0] ? d.name : "(未广播名称)");
            if (isTarget) h += " <span class='badge ok'>当前设备</span>";
            h += "</td><td class='filetd'>" + String(d.mac) + "</td><td>" + String(d.rssi) + " dBm</td>";
            h += "<td><form method='post' action='/pair/save'><input type='hidden' name='mac' value='" +
                 String(d.mac) + "'><button type='submit'" + String(isTarget ? " disabled" : "") +
                 ">选择</button></form></td></tr>";
        }
        h += "</tbody></table></div></section>";
    }

    if (_bt) {
        // 用快照读取，避免与 BT 任务写入 _bonded 竞态
        BtLink::Device bondedSnap[10];
        size_t bondedN = _bt->copyBonded(bondedSnap, 10);
        if (bondedN > 0) {
            h += "<section class='card'><div class='cardhead'><h2>已绑定设备</h2><span class='meta'>共 " +
                 String(bondedN) + " 个</span></div><div class='table-wrap'><table class='data'>"
                 "<thead><tr><th scope='col'>MAC 地址</th><th scope='col'>操作</th></tr></thead><tbody>";
            for (size_t i = 0; i < bondedN; i++) {
                const BtLink::Device& d = bondedSnap[i];
                h += "<tr><td class='filetd'>" + String(d.mac) + "</td>";
                h += "<td><form method='post' action='/pair/save'><input type='hidden' name='mac' value='" +
                     String(d.mac) + "'><button type='submit' class='secondary'>选择</button></form></td></tr>";
            }
            h += "</tbody></table></div><small class='help'>已绑定设备无需再次确认；若手表已恢复出厂设置，"
                 "请先在手表上取消配对后重新扫描。</small></section>";
        }
    }

    h += "<section class='card danger-zone'><h2>自动配对模式</h2><div class='actions between'>"
         "<div>清除指定手表<small>改为自动尝试已绑定设备</small></div>"
         "<form method='post' action='/pair/save' onsubmit='return confirm(\"确定清除手表 MAC，改为自动尝试已配对设备？\")'>"
         "<input type='hidden' name='mac' value=''><button type='submit' class='danger'>清除手表 MAC</button>"
         "</form></div></section>";
    h += "<div class='notice'>选择设备后网关会重启。手表若弹出配对请求，请在手表上确认；"
         "使用非默认密钥时，请先在「网关配置」页填写 16 字节配对密钥。</div>";
    pageEnd(h);
    _server->send(200, "text/html; charset=utf-8", h);
}

void WebUi::_handlePairScan() {
    uint32_t sec = 8;
    if (_server->hasArg("sec")) {
        int s = _server->arg("sec").toInt();
        if (s < 3) s = 3;
        if (s > 30) s = 30;
        sec = uint32_t(s);
    }
    if (_bt) _bt->startScan(sec * 1000);
    _server->sendHeader("Location", "/pair", true);
    _server->send(302, "text/plain", "");
}

void WebUi::_handlePairStop() {
    if (_bt) _bt->stopScan();
    _server->sendHeader("Location", "/pair", true);
    _server->send(302, "text/plain", "");
}

void WebUi::_handlePairSave() {
    if (_otaBusy() || (_wf && _wf->busy())) {
        _server->send(409, "text/plain; charset=utf-8", "固件升级或表盘安装进行中");
        return;
    }
    AppConfig& c = *_cfg;
    if (_server->hasArg("mac")) {
        String v = _server->arg("mac");
        v.trim();
        std::strncpy(c.watchMac, v.c_str(), sizeof(c.watchMac) - 1);
        c.watchMac[sizeof(c.watchMac) - 1] = 0;
        // 规范化 "AA:BB:..." 大写格式
        uint8_t b[6];
        if (BtLink::parseMac(c.watchMac, b)) {
            BtLink::macToString(b, c.watchMac);
        }
    }
    saveConfig(c);
    String h;
    pageBegin(h, "配对已保存", UiPage::None, "4;url=/");
    h += "<section class='card'><span class='badge ok'>设置成功</span><table class='kv'>"
         "<tr><th scope='row'>手表 MAC</th><td class='filetd'>" + htmlEsc(c.watchMac) + "</td></tr>"
         "<tr><th scope='row'>下一步</th><td>设备即将重启</td></tr></table>"
         "<small class='help'>4 秒后自动返回运行状态页</small></section>";
    pageEnd(h);
    _server->send(200, "text/html; charset=utf-8", h);
    delay(500);
    ESP.restart();
}

// ---------------- 表盘管理页 ----------------

static String jsonEsc(const char* s) {
    String r;
    if (!s) return r;
    for (const char* p = s; *p; p++) {
        unsigned char c = (unsigned char)*p;
        switch (c) {
            case '"': r += "\\\""; break;
            case '\\': r += "\\\\"; break;
            case '\n': r += "\\n"; break;
            case '\r': r += "\\r"; break;
            case '\t': r += "\\t"; break;
            default:
                if (c < 0x20) {
                    char b[8];
                    std::snprintf(b, sizeof(b), "\\u%04x", c);
                    r += b;
                } else {
                    r += char(c);
                }
        }
    }
    return r;
}

const char* WebUi::_wfStateName(WfState s) const {
    switch (s) {
        case WfState::IDLE: return "空闲";
        case WfState::SCANNING: return "校验文件";
        case WfState::INSTALL_START: return "等待手表确认";
        case WfState::UPLOAD_REQUEST: return "等待手表接收";
        case WfState::UPLOADING: return "上传中";
        case WfState::DONE: return "完成";
        case WfState::FAILED: return "失败";
    }
    return "?";
}

void WebUi::_requestWatchfaceList() {
    if (!_wear || !_wear->authenticated()) return;
    uint32_t now = millis();
    if (now - _wfLastListReqMs < 3000) return; // 限流 3s
    _wfLastListReqMs = now;
    auto cmd = buildWatchfaceListCmd();
    _wear->sendCommand(cmd.data(), cmd.size());
}

void WebUi::_pumpDuringUpload() {
    // WebServer::handleClient 处理 multipart 上传时会一直阻塞到上传结束；
    // 期间周期性驱动蓝牙/协议/网关栈，避免大文件上传把网关业务卡死。
    if (_bt) _bt->loop();
    if (_bt && _wear) {
        uint8_t buf[512];
        size_t n = _bt->stream().readBulk(buf, sizeof(buf));
        if (n) _wear->onBytes(buf, n);
    }
    if (_wear) _wear->tick();
    if (_gw) _gw->loop();
}

void WebUi::_handleWatchface() {
    String h;
    h.reserve(16384);
    bool busy = _wf && _wf->busy();
    bool auth = _wear && _wear->authenticated();
    bool pending = _ota && _ota->pendingVerify();
    pageBegin(h, "表盘管理", UiPage::Watchface, busy ? "2" : nullptr);

    if (!auth) {
        h += "<div class='notice'><strong>手表尚未完成认证</strong><br>"
             "安装和管理表盘前，请先连接手表并在「手表配对」页完成认证。</div>";
    }
    if (pending) {
        h += "<div class='notice'>新固件正在等待启动确认，暂时不能安装表盘，以免覆盖回滚镜像。</div>";
    }

    if (_wf) {
        h += "<section class='card'><div class='cardhead'><h2>安装状态</h2>";
        if (busy) {
            h += "<span class='badge neutral'>";
            h += _wfStateName(_wf->state());
            h += "</span></div><div class='actions between'><div>";
            if (!_wf->info().name.empty()) h += "<strong>" + htmlEsc(_wf->info().name.c_str()) + "</strong>";
            h += "<small class='filetd'>ID " + htmlEsc(_wf->info().id.c_str()) + "</small></div>"
                 "<span class='progress-label'>" + String(_wf->progress()) + "%</span></div>";
            h += "<progress max='100' value='" + String(_wf->progress()) + "' aria-label='表盘安装进度'>" +
                 String(_wf->progress()) + "%</progress>";
            h += "<small class='help'>安装期间请保持手表连接，页面每 2 秒自动刷新。</small>";
        } else if (_wf->state() == WfState::DONE) {
            h += "<span class='badge ok'>安装完成</span></div><span class='ok'>表盘已安装，激活命令已发送。</span>";
        } else if (_wf->state() == WfState::FAILED) {
            h += "<span class='badge bad'>安装失败</span></div><span class='bad'>" +
                 htmlEsc(_wf->error().c_str()) + "</span>";
        } else {
            h += "<span class='badge neutral'>空闲</span></div><span class='help flush'>"
                 "选择表盘文件上传后会自动开始安装。</span>";
        }
        h += "</section>";
    }

    h += "<section class='card'><h2>上传并安装</h2>"
         "<form method='post' action='/watchface/upload' enctype='multipart/form-data'>"
         "<div class='field'><label for='fw'>选择表盘文件</label>"
         "<input id='fw' type='file' name='fw' accept='.bin' required aria-describedby='wf-upload-help'>"
         "<small id='wf-upload-help' class='help'>上传后直接安装到手表；支持米坛社区等渠道的 .bin 文件。"
         "也可用 USB 数据线把安装包推到板子上（串口协议，见 README）。</small></div>"
         "<button type='submit'" + String((auth && !busy && !_otaBusy() && !pending) ? "" : " disabled") +
         ">上传并安装到手表</button></form></section>";

    // 缓存包卡片：安装成功自动删除；失败保留可重装；48 小时未用自动清理
    h += "<section class='card'><div class='cardhead'><h2>安装包缓存</h2><span class='meta'>";
    if (_fcMeta.present) {
        h += String(_fcMeta.size / 1024) + " KB</span></div>";
        h += "<p class='help flush'>已缓存：<strong>" + htmlEsc(_fcMeta.name.c_str()) + "</strong>（" +
             String(_fcMeta.size) + " 字节）<br>保留剩余：<strong>" +
             faceCacheRemainingText(faceCacheRemaining(_fcMeta, (uint32_t)time(nullptr),
                                                        FACE_CACHE_TTL_SEC)).c_str() +
             "</strong>（超过 48 小时未使用会自动删除）</p>";
        h += "<div class='actions'>";
        h += "<form method='post' action='/watchface/retry'>"
             "<button type='submit'" + String((auth && !busy && !_otaBusy() && !pending) ? "" : " disabled") +
             ">重新安装此包</button></form>";
        h += "<form method='post' action='/watchface/cache/delete' "
             "onsubmit='return confirm(\"删除缓存的安装包？\")'>"
             "<input type='hidden' name='id' value='1'>"
             "<button type='submit' class='danger'" + String((auth && !busy) ? "" : " disabled") +
             ">删除缓存</button></form>";
        h += "</div>";
    } else {
        h += "无</span></div><p class='help flush'>当前没有缓存的安装包。安装成功后缓存会自动清理；"
             "若安装失败则保留在此，可重新安装或手动删除。</p>";
    }
    h += "</section>";

    const esp_partition_t* stage = _wfStage.partition();
    h += "<section class='card'><div class='cardhead'><h2>临时安装区</h2><span class='meta'>";
    if (stage) h += String(stage->size / 1024) + " KB 可用";
    else h += "不可用";
    h += "</span></div><p class='help flush'>表盘先写入临时安装区。";
    if (_wfStage.dedicated()) {
        h += "该区是<strong>独立表盘分区</strong>，与固件 OTA 槽互不影响，"
             "安装表盘不会破坏固件回滚镜像。";
    } else {
        h += "当前为 4MB 布局，临时区借用非活动 OTA 分区："
             "<strong>安装表盘会覆盖固件回滚镜像</strong>，直到下一次固件升级成功。"
             "升级到 16MB 板子后将使用独立表盘分区。";
    }
    h += "</p></section>";

    h += "<section class='card'><div class='cardhead'><div><h2>手表端表盘</h2></div>"
         "<form method='post' action='/watchface/refresh'><button type='submit' class='secondary'" +
         String(auth ? "" : " disabled") + ">刷新列表</button></form></div>";
    if (_faces && !_faces->empty()) {
        h += "<div class='table-wrap'><table class='data'><thead><tr><th scope='col'>名称</th>"
             "<th scope='col'>ID</th><th scope='col'>操作</th></tr></thead><tbody>";
        for (const WatchfaceEntry& e : *_faces) {
            h += "<tr><td>" + htmlEsc(e.name.empty() ? e.id.c_str() : e.name.c_str());
            if (e.active) h += " <span class='badge ok'>使用中</span>";
            h += "</td><td class='filetd'>" + htmlEsc(e.id.c_str()) + "</td><td><div class='actions'>";
            if (!e.active) {
                h += "<form method='post' action='/watchface/set'>"
                     "<input type='hidden' name='id' value='" + htmlEsc(e.id.c_str()) + "'>"
                     "<button type='submit'" + String(auth ? "" : " disabled") + ">启用</button></form>";
            }
            if (e.canDelete) {
                h += "<form method='post' action='/watchface/uninstall' onsubmit='return confirm(\"从手表卸载该表盘？\")'>"
                     "<input type='hidden' name='id' value='" + htmlEsc(e.id.c_str()) + "'>"
                     "<button type='submit' class='danger'" + String(auth ? "" : " disabled") +
                     ">卸载</button></form>";
            }
            h += "</div></td></tr>";
        }
        h += "</tbody></table></div>";
    } else {
        h += "<div class='empty'>尚未获取手表表盘列表，连接并认证后点击「刷新列表」。</div>";
    }
    h += "</section><div class='notice'>安装流程：校验文件（MD5 / CRC32）→ 手表确认 → 分块上传 → 激活。"
         "表盘只临时暂存，不占用长期存储空间。</div>";
    // 安装进行中：页面每 2 秒整页刷新，且要把 CPU 让给分块上传，
    // 此时不叠加悬浮占用条（避免额外的 2 秒轮询与整页刷新互相干扰）
    pageEnd(h, !busy);
    _server->send(200, "text/html; charset=utf-8", h);

    if (auth && _faces && _faces->empty() && !busy) _requestWatchfaceList();
}

void WebUi::_handleWfUploadChunk() {
    HTTPUpload& up = _server->upload();
    if (up.status == UPLOAD_FILE_START) {
        _wfUploadBad = false;
        _wfUploadBytes = 0;
        _wfUploadPump = 0;
        _wfLastDataMs = millis();
        _wfPushMode = false;
        // 头部状态在 START 最开头重置：下面有多个提前 return 的分支，
        // 放在末尾会导致「上一次上传失败后的残留头部」带到下一次。
        _wfHeadLen = 0;
        _wfHead[0] = _wfHead[1] = 0;
        String fn = up.filename;
        fn.toLowerCase();
        if (!fn.endsWith(".bin")) {
            _wfUploadBad = true;
            return;
        }
        if (!_wear || !_wear->authenticated() || (_wf && _wf->busy()) || _otaBusy() ||
            (_ota && _ota->pendingVerify())) {
            _wfUploadBad = true;
            return;
        }
        std::string err;
        if (!_wfStage.begin(0, err)) {
            _wfUploadBad = true;
            return;
        }
        _wfUploadName = up.filename.c_str();
        return;
    }

    if (up.status == UPLOAD_FILE_WRITE) {
        // 表盘文件魔数校验：必须等累计到 2 字节才能判。
        // 原实现只在「首包就 >= 2 字节」时检查，若首包恰好 1 字节，
        // 后续包因 _wfUploadBytes != 0 而永久跳过校验，非法文件得以进入。
        if (!_wfUploadBad) {
            while (_wfHeadLen < 2 && _wfHeadLen < up.currentSize) {
                _wfHead[_wfHeadLen++] = up.buf[_wfHeadLen];
            }
            if (_wfHeadLen == 2 && (_wfHead[0] != 0x5A || _wfHead[1] != 0xA5)) {
                _wfUploadBad = true;
                _wfStage.abort();
            }
        }
        if (!_wfUploadBad) {
            _wfLastDataMs = millis();
            if (_wfStage.write(up.buf, up.currentSize) != up.currentSize) {
                _wfUploadBad = true;
                _wfStage.abort();
            } else {
                _wfUploadBytes += up.currentSize;
                if (++_wfUploadPump >= 8) { // 每 ~16KB 让蓝牙/网关栈跑一步
                    _wfUploadPump = 0;
                    _pumpDuringUpload();
                }
            }
        }
        return;
    }

    if (up.status == UPLOAD_FILE_END) {
        if (_wfUploadBad) {
            _wfStage.abort();
            return;
        }
        uint32_t bytes = 0;
        if (!_wfStage.finish(bytes)) {
            _wfUploadBad = true;
            _wfStage.abort();
            return;
        }
        // 头部必须完整（2 字节魔数），否则文件被截断
        if (bytes < 2 || _wfHeadLen < 2) {
            _wfUploadBad = true;
            _wfStage.abort();
            return;
        }
        _wfSrc.open(_wfStage.partition(), 0, bytes);
        if (_wf) {
            _wf->setMaxBytes(_wfStage.capacity());
            if (!_wf->start(_wfSrc)) {
                _wfUploadBad = true;
                _wfStage.abort();
            } else {
                // 记录缓存元数据：安装成功后自动删除，失败则保留
                _fcSet(_wfUploadName.empty() ? std::string("watchface.bin") : _wfUploadName, bytes);
            }
        }
        return;
    }

    if (up.status == UPLOAD_FILE_ABORTED) {
        _wfUploadBad = true;
        _wfStage.abort();
    }
}

void WebUi::_handleWfUploadDone() {
    if (_wfUploadBad) {
        String h;
        pageBegin(h, "表盘上传失败", UiPage::None, "4;url=/watchface");
        h += "<section class='card'><span class='badge bad'>安装未开始</span>"
             "<p>请确认手表已认证、文件为有效 .bin 表盘，且升级/安装任务未在进行中。</p>"
             "<small>4 秒后自动返回表盘管理页</small></section>";
        pageEnd(h);
        _server->send(200, "text/html; charset=utf-8", h);
        return;
    }
    _server->sendHeader("Location", "/watchface", true);
    _server->send(302, "text/plain", "");
}

// ==================== 串口推送通道（与网页上传共用暂存区/安装器） ====================

bool WebUi::wfPushBegin(uint32_t size, const std::string& name, std::string& err) {
    err.clear();
    if (size == 0) {
        err = "包大小为 0";
        return false;
    }
    if (_wfStage.busy() || (_wf && _wf->busy())) {
        err = "设备忙（正在安装或上传）";
        return false;
    }
    if (_otaBusy() || (_ota && _ota->pendingVerify())) {
        err = "固件升级进行中";
        return false;
    }
    if (!_wear || !_wear->authenticated()) {
        err = "手表未连接或未认证";
        return false;
    }
    std::string e2;
    if (!_wfStage.begin(0, e2)) {
        err = e2;
        return false;
    }
    if (size > _wfStage.capacity()) {
        _wfStage.abort();
        err = "安装包超过暂存区容量";
        return false;
    }
    _wfUploadName = name;
    _wfUploadBytes = 0;
    _wfUploadPump = 0;
    _wfHeadLen = 0;
    _wfHead[0] = _wfHead[1] = 0;
    _wfLastDataMs = millis();
    _wfPushMode = true;
    return true;
}

size_t WebUi::wfPushData(const uint8_t* data, size_t len) {
    if (!_wfPushMode) return 0;
    if (!data || !len) return 0;
    // 与网页上传走同一套头部魔数校验（表盘文件以 5A A5 开头）
    while (_wfHeadLen < 2 && _wfHeadLen < len) {
        _wfHead[_wfHeadLen++] = data[_wfHeadLen];
    }
    if (_wfHeadLen == 2 && (_wfHead[0] != 0x5A || _wfHead[1] != 0xA5)) {
        Serial.println("[xw] watchface push rejected: bad magic");
        _wfStage.abort();
        _wfPushMode = false;
        return 0;
    }
    _wfLastDataMs = millis();
    size_t n = _wfStage.write(data, len);
    if (n != len) {
        _wfStage.abort();
        _wfPushMode = false;
        return 0;
    }
    _wfUploadBytes += uint32_t(len);
    if (++_wfUploadPump >= 8) {
        _wfUploadPump = 0;
        _pumpDuringUpload();
    }
    return len;
}

bool WebUi::wfPushEnd(std::string& err) {
    err.clear();
    if (!_wfPushMode) {
        err = "没有进行中的推送";
        return false;
    }
    uint32_t bytes = 0;
    if (!_wfStage.finish(bytes)) {
        err = _wfStage.error();
        _wfStage.abort();
        _wfPushMode = false;
        return false;
    }
    _wfPushMode = false;
    // 表盘文件头必须是 5A A5；头部不足 2 字节说明数据被截断
    if (bytes < 2 || _wfHeadLen < 2) {
        err = "安装包过短";
        _wfStage.abort();
        return false;
    }
    _wfSrc.open(_wfStage.partition(), 0, bytes);
    if (_wf) {
        _wf->setMaxBytes(_wfStage.capacity());
        if (!_wf->start(_wfSrc)) {
            err = "安装器忙";
            _wfStage.abort();
            return false;
        }
        _fcSet(_wfUploadName.empty() ? std::string("watchface.bin") : _wfUploadName, bytes);
    }
    return true;
}

void WebUi::wfPushAbort() {
    _wfPushMode = false;
    _wfStage.abort();
}

// 安装结束：成功 → 删除缓存包；失败 → 保留（可在网页重装或手动删除）
void WebUi::watchfaceInstallDone(bool ok, const std::string& err) {
    if (ok) {
        Serial.println("[xw] face cache cleared (install ok)");
        _fcErase();
    } else {
        Serial.printf("[xw] face cache kept (install failed: %s)\n", err.c_str());
    }
}

void WebUi::_handleWfSet() {
    if (_wear && _wear->authenticated() && _server->hasArg("id")) {
        String id = _server->arg("id");
        id.trim();
        if (!id.isEmpty() && id.length() <= 32) {
            auto cmd = buildWatchfaceSetCmd(id.c_str());
            _wear->sendCommand(cmd.data(), cmd.size());
        }
    }
    _server->sendHeader("Location", "/watchface", true);
    _server->send(302, "text/plain", "");
}

void WebUi::_handleWfUninstall() {
    if (_wear && _wear->authenticated() && _server->hasArg("id")) {
        String id = _server->arg("id");
        id.trim();
        if (!id.isEmpty() && id.length() <= 32) {
            auto cmd = buildWatchfaceDeleteCmd(id.c_str());
            _wear->sendCommand(cmd.data(), cmd.size());
        }
    }
    _server->sendHeader("Location", "/watchface", true);
    _server->send(302, "text/plain", "");
}

void WebUi::_handleWfRefresh() {
    _wfLastListReqMs = 0; // 手动刷新不限流
    _requestWatchfaceList();
    _server->sendHeader("Location", "/watchface", true);
    _server->send(302, "text/plain", "");
}

// 用缓存的安装包重新安装
void WebUi::_handleWfRetry() {
    if (!_fcMeta.present) {
        _server->sendHeader("Location", "/watchface", true);
        _server->send(302, "text/plain", "");
        return;
    }
    if (_wfStage.busy() || (_wf && _wf->busy()) || _otaBusy()) {
        _server->send(409, "text/plain; charset=utf-8", "设备忙，请稍后重试");
        return;
    }
    if (!_wear || !_wear->authenticated()) {
        _server->send(409, "text/plain; charset=utf-8", "手表未连接或未认证");
        return;
    }
    if (!_wfStage.openCached(_fcMeta.size)) {
        _server->send(409, "text/plain; charset=utf-8", "缓存包不可用");
        return;
    }
    _wfSrc.open(_wfStage.partition(), 0, _fcMeta.size);
    if (_wf) {
        _wf->setMaxBytes(_wfStage.capacity());
        if (!_wf->start(_wfSrc)) {
            _server->send(409, "text/plain; charset=utf-8", "安装器忙");
        }
    }
    _server->sendHeader("Location", "/watchface", true);
    _server->send(302, "text/plain", "");
}

// 手动删除缓存的安装包
void WebUi::_handleWfCacheDelete() {
    if (_wfStage.busy() || (_wf && _wf->busy())) {
        _server->send(409, "text/plain; charset=utf-8", "设备忙，请稍后再删除");
        return;
    }
    _fcErase();
    _server->sendHeader("Location", "/watchface", true);
    _server->send(302, "text/plain", "");
}

void WebUi::_handleWfApi() {
    String j = "{";
    if (_wf) {
        j += "\"state\":\"" + String(_wfStateName(_wf->state())) + "\"";
        j += ",\"busy\":" + String(_wf->busy() ? 1 : 0);
        j += ",\"progress\":" + String(_wf->progress());
        j += ",\"error\":\"" + jsonEsc(_wf->error().c_str()) + "\"";
        j += ",\"faceId\":\"" + jsonEsc(_wf->info().id.c_str()) + "\"";
        j += ",\"faceName\":\"" + jsonEsc(_wf->info().name.c_str()) + "\"";
    }
    const esp_partition_t* stage = _ota ? _ota->inactivePartition() : nullptr;
    j += ",\"stagingUsed\":" + String((unsigned long)_wfStage.bytes());
    j += ",\"stagingCap\":" + String((unsigned long)(stage ? stage->size : 0));
    // 缓存包信息
    j += ",\"cachePresent\":" + String(_fcMeta.present ? 1 : 0);
    j += ",\"cacheName\":\"" + jsonEsc(_fcMeta.name.c_str()) + "\"";
    j += ",\"cacheSize\":" + String((unsigned long)_fcMeta.size);
    j += ",\"cacheRemain\":" + String((unsigned long)faceCacheRemaining(
        _fcMeta, (uint32_t)time(nullptr), FACE_CACHE_TTL_SEC));
    j += ",\"cacheDedicated\":" + String(_wfStage.dedicated() ? 1 : 0);

    j += ",\"faces\":[";
    if (_faces) {
        for (size_t i = 0; i < _faces->size(); i++) {
            const WatchfaceEntry& e = (*_faces)[i];
            if (i) j += ",";
            j += "{\"id\":\"" + jsonEsc(e.id.c_str()) + "\",\"name\":\"" +
                 jsonEsc(e.name.c_str()) + "\",\"active\":" + String(e.active ? 1 : 0) +
                 ",\"canDelete\":" + String(e.canDelete ? 1 : 0) + "}";
        }
    }
    j += "]}";
    _server->send(200, "application/json", j);
}

// ---------------- 固件升级页 ----------------
bool WebUi::_otaBusy() const {
    return _ota && (_ota->active() || _ota->verified());
}

void WebUi::_handleFirmware() {
    String h;
    h.reserve(12288);
    bool wfBusy = _wf && _wf->busy();
    bool pending = _ota && _ota->pendingVerify();
    bool blocked = wfBusy || _otaBusy() || pending;
    pageBegin(h, "固件升级", UiPage::Firmware);

    h += "<section class='card'><h2>当前固件</h2><table class='kv'>";
    h += "<tr><th scope='row'>版本</th><td>" + String(_ota ? _ota->version() : "dev") + "</td></tr>";
    h += "<tr><th scope='row'>运行分区</th><td class='filetd'>" +
         String(_ota ? _ota->runningLabel() : "?") + "</td></tr>";
    h += "<tr><th scope='row'>镜像状态</th><td>";
    if (pending) {
        h += "<span class='badge neutral'>待确认</span> 新固件将在联网稳定 60 秒后自动确认";
    } else if (_ota && _ota->rollbackAvailable()) {
        h += "<span class='badge ok'>运行正常</span> · 可回滚到上一版本";
    } else {
        h += "<span class='badge ok'>运行正常</span>";
    }
    h += "</td></tr></table></section>";

    if (pending) {
        h += "<div class='notice'>新固件正在等待启动确认。确认后旧固件将不再作为自动回滚目标；"
             "若设备无法联网，180 秒后会自动回滚。</div>";
        h += "<section class='card'><h2>确认新固件</h2><div class='actions between'>"
             "<span class='help flush'>当前版本工作正常时，可立即确认。</span>"
             "<form method='post' action='/firmware/keep'><button type='submit' class='secondary'>"
             "保留当前固件</button></form></div></section>";
    }

    h += "<section class='card'><h2>上传固件</h2>"
         "<form method='post' action='/update' enctype='multipart/form-data' "
         "onsubmit=\"this.querySelector('button').disabled=true;this.querySelector('button').textContent='正在写入，请勿断电…'\">"
         "<div class='field'><label for='fwbin'>固件文件</label>"
         "<input id='fwbin' type='file' name='fw' accept='.bin' required aria-describedby='fw-help'>"
         "<small id='fw-help' class='help'>选择 PlatformIO 生成的 firmware.bin，"
         "不要上传 firmware-merged-4mb.bin（合并镜像只能通过 USB 整片烧录）。</small></div>"
         "<button type='submit'" + String(blocked ? " disabled" : "") + ">上传并升级</button></form>";
    if (wfBusy) h += "<div class='notice'>表盘正在安装，完成后再进行固件升级。</div>";
    else if (pending) h += "<div class='notice'>请先确认或等待当前新固件通过启动检查。</div>";
    else if (_otaBusy()) h += "<div class='notice'>固件正在写入，请勿关闭页面或断电。</div>";
    h += "</section>";

    if (_ota && _ota->rollbackAvailable()) {
        h += "<section class='card danger-zone'><h2>回滚固件</h2><div class='actions between'>"
             "<div>切换回另一分区<small>回滚后设备会重启，配置和表盘不受影响</small></div>"
             "<form method='post' action='/firmware/rollback' "
             "onsubmit='return confirm(\"确定回滚到上一版本固件？设备将重启。\")'>"
             "<button type='submit' class='danger'>回滚并重启</button></form></div></section>";
    }

    h += "<div class='notice'>升级过程中请保持供电和网络连接。表盘临时安装区会被固件升级覆盖；"
         "升级后若新固件无法联网，设备会自动回滚。升级页没有独立密码，请仅在可信局域网使用。</div>";
    pageEnd(h);
    _server->send(200, "text/html; charset=utf-8", h);
}

void WebUi::_handleFwUploadChunk() {
    HTTPUpload& up = _server->upload();
    if (up.status == UPLOAD_FILE_START) {
        _otaUploadBad = false;
        _otaUploadPump = 0;
        _otaLastDataMs = millis();
        if (!_ota || (_wf && _wf->busy()) || _wfStage.busy() || _otaBusy() ||
            _ota->pendingVerify()) {
            _otaUploadBad = true;
            return;
        }
        if (!_ota->start()) _otaUploadBad = true;
        return;
    }

    if (up.status == UPLOAD_FILE_WRITE) {
        if (!_otaUploadBad && _ota) {
            _otaLastDataMs = millis();
            if (!_ota->write(up.buf, up.currentSize)) {
                _otaUploadBad = true;
            } else if (++_otaUploadPump >= 16) {
                _otaUploadPump = 0;
                _pumpDuringUpload();
            }
        }
        return;
    }

    if (up.status == UPLOAD_FILE_ABORTED) {
        _otaUploadBad = true;
        if (_ota) _ota->abort();
    }
}

void WebUi::_handleFwUploadDone() {
    bool ok = !_otaUploadBad && _ota && _ota->finish();
    String h;
    pageBegin(h, ok ? "升级完成" : "升级失败", UiPage::None, ok ? nullptr : "6;url=/firmware");
    if (ok) {
        h += "<section class='card'><span class='badge ok'>写入成功</span>"
             "<p>新固件已写入并切换启动分区，设备即将重启。</p>"
             "<small>重启后 60 秒内保持网络可用，系统会自动确认新版本。</small></section>";
    } else {
        h += "<section class='card'><span class='badge bad'>升级未生效</span><p>";
        String why = _ota ? _ota->error().c_str() : "";
        if (why.isEmpty()) why = "当前有其他任务进行中，或 OTA 上传被中断";
        h += htmlEsc(why.c_str());
        h += "</p><small>原固件仍在运行，6 秒后返回升级页。</small></section>";
    }
    pageEnd(h);
    _server->send(200, "text/html; charset=utf-8", h);

    if (ok) {
        delay(800);
        ESP.restart();
    }
}

void WebUi::_handleFwRollback() {
    bool ok = _ota && !(_wf && _wf->busy()) && !_otaBusy() &&
              _ota->rollbackAvailable() && _ota->rollback();
    String h;
    pageBegin(h, ok ? "正在回滚" : "回滚失败", UiPage::None, ok ? nullptr : "5;url=/firmware");
    if (ok) {
        h += "<section class='card'><span class='badge ok'>已切换启动分区</span>"
             "<p>设备即将重启并运行上一版本固件。</p></section>";
    } else {
        h += "<section class='card'><span class='badge bad'>无法回滚</span><p>";
        h += _ota ? htmlEsc(_ota->error().c_str()) : "OTA 服务不可用";
        h += "</p><small>另一分区没有完整固件，5 秒后返回升级页。</small></section>";
    }
    pageEnd(h);
    _server->send(200, "text/html; charset=utf-8", h);
    if (ok) {
        delay(600);
        ESP.restart();
    }
}

void WebUi::_handleFwKeep() {
    if (_ota) _ota->keepCurrent();
    _server->sendHeader("Location", "/firmware", true);
    _server->send(302, "text/plain", "");
}

void WebUi::_handleFwApi() {
    String j = "{";
    j += "\"version\":\"" + String(_ota ? _ota->version() : "dev") + "\"";
    j += ",\"partition\":\"" + String(_ota ? _ota->runningLabel() : "?") + "\"";
    j += ",\"runningVersion\":\"" + String(_ota ? _ota->runningVersion() : "?") + "\"";
    j += ",\"state\":\"" + String(_ota ? _ota->stateName() : "?") + "\"";
    j += ",\"pendingVerify\":" + String(_ota && _ota->pendingVerify() ? 1 : 0);
    j += ",\"rollbackAvailable\":" + String(_ota && _ota->rollbackAvailable() ? 1 : 0);
    j += ",\"written\":" + String((unsigned long)(_ota ? _ota->written() : 0));
    j += ",\"error\":\"" + jsonEsc(_ota ? _ota->error().c_str() : "") + "\"";
    j += "}";
    _server->send(200, "application/json", j);
}

void WebUi::_handleLedTest() {
    // on=1 常亮，on=0 熄灭；未指定 on 则切换
    bool on = !(_server->hasArg("on") && _server->arg("on") == "0");
    if (_server->hasArg("on")) on = (_server->arg("on") == "1");
    bool ok = false;
    if (_led && _led->enabled()) {
        if (on) _led->testOn();
        else _led->testOff();
        ok = true;
    }
    _server->send(ok ? 200 : 409, "text/plain; charset=utf-8",
                  ok ? (on ? "ON" : "OFF") : "LED 未启用");
}

void WebUi::_handleNotFound() {
    // AP 配网模式：任意域名都导回配置页（captive portal 行为）
    if (_apActive) {
        _server->sendHeader("Location", "http://192.168.4.1/config", true);
        _server->send(302, "text/plain", "");
    } else {
        _server->send(404, "text/plain; charset=utf-8", "404");
    }
}

// ---------------- 生命周期 ----------------
void WebUi::begin(MiWear& wear, NetGw& gw, WeatherService& wx, AppConfig& cfg, BtLink& bt,
                  WatchfaceInstaller& wf, const std::vector<WatchfaceEntry>& faces, OtaService& ota) {
    _wear = &wear;
    _gw = &gw;
    _wx = &wx;
    _cfg = &cfg;
    _bt = &bt;
    _wf = &wf;
    _faces = &faces;
    _ota = &ota;
    if (_started) return;
    _started = true;
    _ota->begin();
    _fcLoad();

    _server = new WebServer(80);
    _server->on("/", [this]() { _handleRoot(); });
    _server->on("/config", [this]() { _handleConfig(); });
    _server->on("/pair", [this]() { _handlePair(); });
    _server->on("/watchface", [this]() { _handleWatchface(); });
    _server->on("/perf", [this]() { _handlePerf(); });
    _server->on("/firmware", [this]() { _handleFirmware(); });
    _server->on("/save", HTTP_POST, [this]() { _handleSave(); });
    _server->on("/weather", HTTP_POST, [this]() { _handleWeather(); });
    _server->on("/reboot", HTTP_POST, [this]() { _handleReboot(); });
    _server->on("/pair/scan", HTTP_POST, [this]() { _handlePairScan(); });
    _server->on("/pair/stop", HTTP_POST, [this]() { _handlePairStop(); });
    _server->on("/pair/save", HTTP_POST, [this]() { _handlePairSave(); });
    _server->on("/watchface/upload", HTTP_POST,
                [this]() { _handleWfUploadDone(); },
                [this]() { _handleWfUploadChunk(); });
    _server->on("/watchface/set", HTTP_POST, [this]() { _handleWfSet(); });
    _server->on("/watchface/uninstall", HTTP_POST, [this]() { _handleWfUninstall(); });
    _server->on("/watchface/refresh", HTTP_POST, [this]() { _handleWfRefresh(); });
    _server->on("/watchface/retry", HTTP_POST, [this]() { _handleWfRetry(); });
    _server->on("/watchface/cache/delete", HTTP_POST, [this]() { _handleWfCacheDelete(); });
    _server->on("/update", HTTP_POST,
                [this]() { _handleFwUploadDone(); },
                [this]() { _handleFwUploadChunk(); });
    _server->on("/firmware/rollback", HTTP_POST, [this]() { _handleFwRollback(); });
    _server->on("/firmware/keep", HTTP_POST, [this]() { _handleFwKeep(); });
    _server->on("/forget", HTTP_POST, [this]() {
        if (_otaBusy() || (_wf && _wf->busy())) {
            _server->send(409, "text/plain; charset=utf-8", "升级或安装进行中，无法重置网络");
            return;
        }
        _cfg->wifiSsid[0] = 0;
        _cfg->wifiPass[0] = 0;
        saveConfig(*_cfg);
        _server->send(200, "text/html; charset=utf-8",
                      "<!DOCTYPE html><html><head><meta charset='utf-8'></head><body>"
                      "WiFi 已清除，正在重启进入配网模式…</body></html>");
        delay(500);
        ESP.restart();
    });
    _server->on("/api/status", [this]() { _handleApi(); });
    _server->on("/api/watchface", [this]() { _handleWfApi(); });
    _server->on("/api/firmware", [this]() { _handleFwApi(); });
    _server->on("/api/perf", [this]() { _handlePerfApi(); });
    _server->on("/led/test", HTTP_POST, [this]() { _handleLedTest(); });
    _server->onNotFound([this]() { _handleNotFound(); });
    _server->begin();
}

void WebUi::startAp() {
    if (_apActive) return;
    uint32_t mac = (ESP.getEfuseMac() >> 32) & 0xFFFF;
    char suffix[5];
    std::snprintf(suffix, sizeof(suffix), "%04X", (unsigned)mac);
    String ssid = String("RedmiWatch5Net-") + suffix;
    const char* pass = _cfg->apPass[0] ? _cfg->apPass : nullptr;
    if (pass && std::strlen(pass) < 8) pass = nullptr; // WPA2 要求至少 8 位
    WiFi.softAP(ssid.c_str(), pass);
    delay(200);
    _dns = new DNSServer();
    _dns->setErrorReplyCode(DNSReplyCode::NoError);
    _dns->start(53, "*", WiFi.softAPIP());
    _apActive = true;
}

void WebUi::stopAp() {
    if (!_apActive) return;
    if (_dns) {
        _dns->stop();
        delete _dns;
        _dns = nullptr;
    }
    WiFi.softAPdisconnect(true);
    _apActive = false;
}

void WebUi::loop() {
    if (_server) _server->handleClient();
    if (_apActive && _dns) _dns->processNextRequest();
    const uint32_t now = millis();
    if (_wfStage.busy() && now - _wfLastDataMs > 30000) {
        _wfStage.abort();
        _wfUploadBad = true;
    }
    if (_ota && _ota->active() && now - _otaLastDataMs > 30000) {
        _ota->abort();
        _otaUploadBad = true;
    }
    // 缓存包 TTL 清理（每 5 分钟检查一次）
    if (now - _fcLastCheckMs > 300000) {
        _fcLastCheckMs = now;
        _fcCheckExpiry();
    }
    // 健康检查只认「已接入用户 WiFi」；仅当设备本来就没有 WiFi 配置（纯 AP 使用）时，
    // AP 模式才算网络就绪，否则新固件连不上 WiFi 会被误判为健康而不回滚。
    const bool netReady = (WiFi.status() == WL_CONNECTED) || (_apActive && _cfg && !_cfg->hasWifi());
    if (_ota) _ota->loop(netReady);
}

} // namespace xw
