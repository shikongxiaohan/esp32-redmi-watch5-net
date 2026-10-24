/*
 * ESP32 红米 Watch 5（蓝牙版）网络网关 - 主程序
 *
 * 功能：替代小米运动健康 App，为红米 Watch 5 提供：
 *   - 网络接入（DHCP/TCP/UDP/ICMP 中继，见 netgw）
 *   - 认证配对（MiWear SPP V2，见 miwear）
 *   - 天气推送（Open-Meteo，见 weather）
 *   - 对时（NTP，SYS_CLOCK 命令）
 *   - Web 管理界面 + 配网 AP（见 webui）
 *
 * 接线：
 *   手表 --BT SPP--> BtLink --> MiWear --onIpPacket--> NetGw --Upstream--> 互联网
 *                          \--onCommand--> WeatherService / 时钟推送
 *   WebUi 读写 AppConfig（NVS），驱动以上模块。
 */
#include <Arduino.h>
#include <WiFi.h>
#include <time.h> // struct tm / tm_gmtoff（configTime/getLocalTime 由 Arduino.h 提供）

#include <sys/socket.h>
#include <sys/select.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <cstdlib> // setenv

#include "appconfig.h"
#include "btlink.h"
#include "miwear.h"
#include "netgw.h"
#include "weather.h"
#include "webui.h"
#include "watchface.h"
#include "ota.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace xw;

// ==================== 全局对象 ====================
static AppConfig cfg;
static BtLink bt;
static MiWear wear;
static WeatherService wx;
static WebUi web;
static WatchfaceInstaller wfInstaller;
static OtaService ota;
static std::vector<WatchfaceEntry> g_faces; // 手表端表盘列表（type=4 subtype=0 响应）

// ==================== 上游 socket 实现 ====================
/*
 * 真实 BSD socket 的 Upstream。
 * 字节序约定同 netgw.h：dstIpBe/dstPortBe 是「网络字节的小端原生读法」，
 * 与 sin_addr.s_addr / sin_port 的内存布局一致，可直接赋值。
 */
class EspUpstream : public Upstream {
public:
    int tcpConnect(uint32_t dstIpBe, uint16_t dstPortBe) override {
        int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) return -1;
        int flags = ::fcntl(fd, F_GETFL, 0);
        ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        sockaddr_in sa;
        std::memset(&sa, 0, sizeof(sa));
        sa.sin_family = AF_INET;
        sa.sin_port = dstPortBe;
        sa.sin_addr.s_addr = dstIpBe;
        int r = ::connect(fd, (sockaddr*)&sa, sizeof(sa));
        if (r == 0) return fd;               // 立即连上（本地回环等）
        if (errno == EINPROGRESS) return fd; // 非阻塞进行中，稍后 canSend 检查
        ::close(fd);
        return -1;
    }

    int udpConnect(uint32_t dstIpBe, uint16_t dstPortBe) override {
        int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (fd < 0) return -1;
        int flags = ::fcntl(fd, F_GETFL, 0);
        ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        sockaddr_in sa;
        std::memset(&sa, 0, sizeof(sa));
        sa.sin_family = AF_INET;
        sa.sin_port = dstPortBe;
        sa.sin_addr.s_addr = dstIpBe;
        if (::connect(fd, (sockaddr*)&sa, sizeof(sa)) < 0 && errno != EINPROGRESS) {
            ::close(fd);
            return -1;
        }
        return fd;
    }

    void closeFd(int fd) override {
        if (fd >= 0) ::close(fd);
    }

    void shutdownWrite(int fd) override {
        if (fd >= 0) ::shutdown(fd, SHUT_WR);
    }

    int sendData(int fd, const uint8_t* data, size_t len) override {
        if (fd < 0) return -1;
        ssize_t r = ::send(fd, data, len, 0);
        if (r > 0) return int(r);
        return -1; // EAGAIN 或错误（错误由 hasError 捕获）
    }

    int recvData(int fd, uint8_t* buf, size_t cap) override {
        if (fd < 0) return -1;
        ssize_t r = ::recv(fd, buf, cap, 0);
        if (r > 0) return int(r);
        if (r == 0) return 0; // 对端 EOF
        return -1;            // EAGAIN 或错误
    }

    bool canSend(int fd) override {
        if (fd < 0) return false;
        // ESP-IDF 无 poll()，用 select()
        fd_set wfds;
        FD_ZERO(&wfds);
        FD_SET(fd, &wfds);
        timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 0;
        if (::select(fd + 1, nullptr, &wfds, nullptr, &tv) <= 0) return false;
        if (!FD_ISSET(fd, &wfds)) return false;
        int err = 0;
        socklen_t el = sizeof(err);
        if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el) != 0) return false;
        return err == 0;
    }

    bool hasError(int fd) override {
        if (fd < 0) return true;
        int err = 0;
        socklen_t el = sizeof(err);
        if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el) != 0) return true;
        return err != 0; // 连接失败 / RST 等；EOF 不算错误（recvData 返回 0）
    }
};

static EspUpstream upstream;
static NetGw gw(upstream);

// ==================== 状态 ====================
static bool authStarted = false;
static bool clockPushed = false;
static bool timeCrediblePushed = false;
static bool facesRequested = false;
static uint32_t lastClockMs = 0;
static bool apMode = false;
static bool wifiTimedOut = false;
static bool wifiWasUp = false;  // 上一轮 WiFi 是否已连接（用于检测掉线）
static uint32_t wifiStartMs = 0;
static uint32_t apStartMs = 0;  // 进入 AP 模式时刻
static uint32_t apRetryMs = 0;  // 上一次 STA 重试时刻
static uint32_t lastLinkMs = 0; // 最近一次「WiFi 或手表」在线的时刻（自动恢复出厂用）

// AP 配网模式下重试已保存 WiFi 的间隔；以及 AP 最短存活时间（避免刚进 AP 就被关掉）
static const uint32_t AP_RETRY_INTERVAL_MS = 60000;
static const uint32_t AP_MIN_LIVE_MS = 90000;

// ==================== 工具 ====================
static void authKeyBytes(const char* hex, uint8_t out[16]) {
    std::memset(out, 0, 16);
    if (!hex || !hex[0]) return; // 留空 = 全 0（开发默认，认证会失败）
    int n = 0;
    for (const char* p = hex; *p && n < 32; p++) {
        if (*p == ':' || *p == ' ') continue;
        char c = *p;
        int d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else return;
        if (n % 2 == 0) out[n / 2] = uint8_t(d << 4);
        else out[n / 2] = uint8_t(out[n / 2] | d);
        n++;
    }
}

static void normalizeMac(char* mac) {
    uint8_t b[6];
    if (BtLink::parseMac(mac, b)) {
        BtLink::macToString(b, mac);
    }
}

static void normalizeHexKey(char* hex) {
    // 去分隔符、转小写（webui 保存时已做，这里是加载后的保险）
    char* d = hex;
    char* w = hex;
    while (*d) {
        if (*d != ' ' && *d != ':') {
            *w++ = char(*d >= 'A' && *d <= 'F' ? *d + 32 : *d);
        }
        d++;
    }
    *w = 0;
}

// ==================== 回调 ====================
static void gwSend(const uint8_t* pkt, size_t len) {
    wear.sendIpPacket(pkt, len);
}

static void onCommandBytes(const uint8_t* data, size_t len) {
    MiCommand cmd = parseCommand(data, len);
    if (!cmd.ok) return;
    if (cmd.type == cmd::TYPE_WEATHER) {
        wx.onCommand(cmd);
    } else if (cmd.type == cmd::TYPE_WATCHFACE) {
        // 表盘列表响应 / 安装状态 / 激活与删除应答
        if (cmd.subtype == cmd::WATCHFACE_LIST) {
            parseWatchfaceList(cmd.watchface, g_faces);
            Serial.printf("[xw] watchface list: %u\n", (unsigned)g_faces.size());
        }
        wfInstaller.onCommand(cmd);
    } else if (cmd.type == cmd::TYPE_DATA_UPLOAD) {
        // 上传握手应答（DataUploadAck）
        wfInstaller.onCommand(cmd);
    } else if (cmd.type == cmd::TYPE_SYSTEM) {
        char m[56];
        std::snprintf(m, sizeof(m), "[xw] system cmd subtype=%u status=%u",
                      (unsigned)cmd.subtype, (unsigned)cmd.status);
        Serial.println(m);
    }
}

static void requestWatchfaceList() {
    if (!wear.authenticated()) return;
    auto cmd = buildWatchfaceListCmd();
    wear.sendCommand(cmd.data(), cmd.size());
}

static bool pushClock(const struct tm& t) {
    ClockSet c{};
    c.year = t.tm_year + 1900;
    c.month = t.tm_mon + 1;
    c.day = t.tm_mday;
    c.hour = t.tm_hour;
    c.minute = t.tm_min;
    c.second = t.tm_sec;
    // 时区偏移（秒）= 本地时间的 epoch - UTC epoch。
    // newlib 的 struct tm 无 tm_gmtoff 字段，用 mktime/time 差值可移植地算
    struct tm lt = t;
    time_t localEpoch = mktime(&lt);
    time_t utcNow = time(nullptr);
    long offSec = (localEpoch > 0 && utcNow > 0) ? long(localEpoch - utcNow) : 0;
    c.zoneOffset = int32_t((offSec / 60) / 15); // 15 分钟为单位（Gadgetbridge 一致）
    c.dstOffset = 0;
    c.tzName = cfg.tzName;
    c.is24hour = cfg.time24h;
    auto pkt = buildClockCmd(c);
    return wear.sendCommand(pkt.data(), pkt.size());
}

static void tickClock() {
    if (!wear.authenticated()) return;
    struct tm t;
    if (!getLocalTime(&t, 100)) return;
    bool credible = (t.tm_year + 1900) >= 2024; // NTP 同步后时钟才可信
    uint32_t now = millis();

    if (credible && !timeCrediblePushed) {
        // NTP 首次同步完成：立即校准手表
        if (pushClock(t)) {
            timeCrediblePushed = true;
            clockPushed = true;
            lastClockMs = now;
            Serial.println("[xw] clock pushed (ntp synced)");
        }
        return;
    }
    // 认证后先推一次（NTP 未就绪则推系统时间），之后每小时重推
    if (!clockPushed || now - lastClockMs >= 3600UL * 1000UL) {
        if (pushClock(t)) {
            clockPushed = true;
            lastClockMs = now;
            Serial.println("[xw] clock pushed");
        }
    }
}

static void onWearConnect() {
    // SPP 建立：重置协议层并重新握手
    wear.end();
    wear.begin(bt.stream());
    wear.onConnected();
    gw.end();
    gw.begin(gwSend);
    authStarted = false;
    clockPushed = false;
    timeCrediblePushed = false;
    facesRequested = false;
}

static void onWearDisconnect() {
    wear.end();
    wear.begin(bt.stream());
    gw.end();
    gw.begin(gwSend);
    authStarted = false;
    clockPushed = false;
    timeCrediblePushed = false;
    facesRequested = false;
    wfInstaller.cancel(); // 断连时终止进行中的表盘安装
    g_faces.clear();
    Serial.println("[xw] session reset");
}

static void manageWifi() {
    const uint32_t now = millis();

    if (WiFi.status() == WL_CONNECTED) {
        wifiWasUp = true;
        wifiTimedOut = false;
        // AP 至少存活一小段时间，避免刚进配网就因瞬连而关掉、打断用户操作
        if (apMode && now - apStartMs >= AP_MIN_LIVE_MS) {
            web.stopAp();
            apMode = false;
            Serial.println("[xw] station reconnected, AP closed");
        }
        return;
    }

    if (wifiWasUp) {
        // 运行中掉线：重新给满 20 秒重连窗口，而不是拿开机时刻判断
        wifiWasUp = false;
        wifiStartMs = now;
        wifiTimedOut = false;
        Serial.println("[xw] wifi lost, retry window restarted");
    }

    if (!cfg.hasWifi()) {
        if (!apMode) {
            web.startAp();
            apMode = true;
            apStartMs = now;
            apRetryMs = now;
        }
        return;
    }

    if (!apMode) {
        if (now - wifiStartMs > 20000) {
            Serial.println("[xw] wifi connect timeout -> AP mode");
            web.startAp();
            apMode = true;
            wifiTimedOut = true;
            apStartMs = now;
            apRetryMs = now;
        }
        return;
    }

    // AP 配网模式下仍每 60 秒重试一次已保存的 WiFi；连上后下一轮自动关闭 AP。
    // 这样路由器重启/换密码修好后，设备能自己回到 STA，不必手动重启。
    if (now - apRetryMs >= AP_RETRY_INTERVAL_MS) {
        apRetryMs = now;
        Serial.printf("[xw] retry STA while AP active: %s\n", cfg.wifiSsid);
        WiFi.begin(cfg.wifiSsid, cfg.wifiPass);
    }
}

// ==================== 自动恢复出厂（可选，默认关闭） ====================
/*
 * 仅在 cfg.autoResetEn 为真时生效：连续 autoResetMin 分钟内既没有连上 WiFi、
 * 也没有连上手表，则清空 WiFi / 配对密钥 / 手表 MAC 并重启进入配网模式。
 * 配网 AP 模式下有人在操作，暂停计时以免打断配置。
 */
static void manageAutoReset() {
    const uint32_t now = millis();
    if (!cfg.autoResetEn) {
        lastLinkMs = now;
        return;
    }
    if (web.apActive()) {
        lastLinkMs = now;
        return;
    }
    if (WiFi.status() == WL_CONNECTED || bt.connected()) {
        lastLinkMs = now;
        return;
    }
    const uint32_t limitMs = uint32_t(cfg.autoResetMin) * 60000UL;
    if (limitMs && now - lastLinkMs >= limitMs) {
        Serial.printf("[xw] no wifi/watch for %u min -> factory reset\n",
                      (unsigned)cfg.autoResetMin);
        cfg.wifiSsid[0] = 0;
        cfg.wifiPass[0] = 0;
        cfg.authKeyHex[0] = 0;
        cfg.watchMac[0] = 0;
        cfg.apPass[0] = 0;
        WebUi::saveConfig(cfg);
        delay(200);
        ESP.restart();
    }
}

// ==================== Arduino 入口 ====================
void setup() {
    Serial.begin(115200);
    delay(300);
    Serial.println("\n[xw] Redmi Watch 5 network gateway starting...");

    WebUi::loadConfig(cfg);
    normalizeMac(cfg.watchMac);
    normalizeHexKey(cfg.authKeyHex);

    // ---- 接线（先 begin，startAp 依赖 _cfg） ----
    wear.begin(bt.stream());
    wear.onIpPacket = [](const uint8_t* pkt, size_t len) { gw.onIpPacket(pkt, len); };
    wear.onCommand = onCommandBytes;

    gw.begin(gwSend);

    // 表盘安装器：命令走 Pb 通道（认证后加密），数据块走 Mass 通道（明文）
    wfInstaller.begin(
        [](const uint8_t* d, size_t n) { return wear.sendCommand(d, n); },
        [](const uint8_t* d, size_t n) { return wear.sendDataChunk(d, n); });
    wfInstaller.onLog = [](const char* m) { Serial.printf("[xw][wf] %s\n", m); };
    wfInstaller.onDone = [](bool ok, const std::string& err) {
        if (ok) {
            Serial.println("[xw][wf] install done");
            requestWatchfaceList();
        } else {
            Serial.printf("[xw][wf] install failed: %s\n", err.c_str());
        }
    };

    wx.begin(wear, cfg);
    web.begin(wear, gw, wx, cfg, bt, wfInstaller, g_faces, ota);

    // WiFi（STA；无配置或超时则 AP 配网）
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false); // BT/WiFi 共存时降低延迟
    if (cfg.hasWifi()) {
        Serial.printf("[xw] wifi connecting to %s\n", cfg.wifiSsid);
        WiFi.begin(cfg.wifiSsid, cfg.wifiPass);
        wifiStartMs = millis();
    } else {
        web.startAp();
        apMode = true;
        apStartMs = millis();
        apRetryMs = apStartMs;
    }

    // NTP + 时区
    configTime(0, 0, "ntp.aliyun.com", "ntp1.aliyun.com", "pool.ntp.org");
    setenv("TZ", cfg.tzPosix, 1);
    tzset();

    bt.onConnect = onWearConnect;
    bt.onDisconnect = onWearDisconnect;
    bt.begin(cfg.watchMac[0] ? cfg.watchMac : nullptr);

    Serial.printf("[xw] watch mac: %s\n",
                  bt.hasWatchMac() ? bt.watchMacStr() : "(auto: bonded)");
    lastLinkMs = millis();
    Serial.println("[xw] setup done");
}

void loop() {
    bt.loop();
    web.loop();

    // 蓝牙数据 -> MiWear（拆帧、认证、网络包分发）
    {
        uint8_t buf[1024];
        size_t n = bt.stream().readBulk(buf, sizeof(buf));
        if (n) wear.onBytes(buf, n);
    }

    wear.tick();

    // 会话就绪 -> 启动认证
    if (!authStarted && wear.state() == MwState::SESSION_OK) {
        uint8_t key[16];
        authKeyBytes(cfg.authKeyHex, key);
        wear.startAuth(key);
        authStarted = true;
        Serial.println("[xw] auth started");
    }

    // 认证完成 -> 请求一次表盘列表（Web 页面展示用）
    if (wear.authenticated() && !facesRequested) {
        requestWatchfaceList();
        facesRequested = true;
        Serial.println("[xw] watchface list requested");
    }

    wfInstaller.loop();
    tickClock();
    gw.loop();
    wx.loop();
    manageWifi();
    manageAutoReset();
}
