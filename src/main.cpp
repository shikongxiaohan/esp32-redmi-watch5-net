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
#include <esp_heap_caps.h> // heap_caps_malloc_extmem_enable（大分配走 PSRAM 的阈值）
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
#include "led.h"
#include "miwear.h"
#include "netgw.h"
#include "perfmon.h"
#include "weather.h"
#include "webui.h"
#include "watchface.h"
#include "ota.h"
#include "serialpush.h"

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
static SerialPush serialPush;
static Led led;
static PerfMon perf;
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

// ==================== 板载 LED 状态指示 ====================
/*
 * 状态优先级（高覆盖低）：
 *   固件升级 > 表盘安装 > 配网 AP > 蓝牙配对 > 连接中 > 已联网未连手表 > 已联网
 * 判定全部来自 ledlogic.h 的纯函数，便于主机端单测。
 */
static uint32_t ledFaultUntilMs = 0; // 故障提示的截止时刻（0 = 无故障提示）

// 表盘安装失败：让 LED 三连眨 3 秒提示一下，用户不用看网页也知道失败了
// 亮 3 秒的写法对「3 连眨」没有意义（闪烁周期才是语义），这里给 6 秒，
// 足够让用户看到至少两轮「三连眨 + 停顿」，又不会长期盖住正常状态。
static void ledSignalFault() {
    ledFaultUntilMs = millis() + 6000UL;
}

static void updateLed() {
    if (!cfg.ledEnabled || !cfg.ledPinValid()) {
        led.setState(LedState::Off);
        return;
    }
    // 故障提示优先（3 秒内）
    if (ledFaultActive(ledFaultUntilMs, millis())) {
        led.setState(LedState::Error);
        return;
    }
    if (ledFaultUntilMs != 0 && !ledFaultActive(ledFaultUntilMs, millis())) {
        ledFaultUntilMs = 0; // 提示期已过，复位标记（只在真正过期时清，不能在期内清）
    }
    LedState s;
    if (apMode) {
        s = LedState::ApMode;
    } else if (ota.active() || ota.pendingVerify()) {
        // 升级中：既包括接收固件，也包括「已写入待确认」阶段
        s = LedState::FirmwareBusy;
    } else if (wfInstaller.busy()) {
        s = LedState::WatchfaceBusy;
    } else if (bt.scanning()) {
        s = LedState::Pairing;
    } else if (WiFi.status() != WL_CONNECTED) {
        s = LedState::Connecting;
    } else if (bt.connected() && wear.authenticated()) {
        s = LedState::Online;
    } else {
        s = LedState::OnlineNoWatch;
    }
    led.setState(s);
}

// ==================== 性能监控数据源 ====================
/*
 * Flash 占用口径（供性能页显示）：
 *   已用 = 应用镜像（分区表中已写部分）+ 表盘缓存 + 系统区（bootloader /
 *          分区表 / NVS / otadata / coredump 的固定预留）
 *   总量 = 整片 Flash 容量
 * 注意：双 OTA 结构下另一个 OTA 槽也占满 4MB，但它不算"已用数据"，
 * 这里只统计真正写入了内容的部分，避免百分比虚高。
 */
static uint32_t perfFlashTotal() {
    return uint32_t(ESP.getFlashChipSize());
}

static uint32_t perfFlashUsed() {
    // 应用镜像：当前运行分区已写入大小
    uint32_t appBytes = uint32_t(ESP.getSketchSize());

    // 表盘缓存：分区里实际存了安装包才算占用
    uint32_t cacheBytes = 0;
    const FaceCacheMeta& fc = web.faceCache();
    if (fc.present) cacheBytes = fc.size;

    // 系统区：0x0 ~ 0x10000（bootloader + 分区表 + NVS + otadata）固定预留
    const uint32_t SYSTEM_BYTES = 0x10000;

    uint32_t used = appBytes + cacheBytes + SYSTEM_BYTES;
    uint32_t total = perfFlashTotal();
    return used > total ? total : used;
}

static void beginPerfMon() {
    PerfMon::Sources src;
    src.netUpBytes = []() { return gw.totalBytesUp(); };
    src.netDownBytes = []() { return gw.totalBytesDown(); };
    src.btUpBytes = []() { return bt.stream().txBytes; };
    src.btDownBytes = []() { return bt.stream().rxBytes; };
    src.flashUsedBytes = []() { return perfFlashUsed(); };
    src.flashTotalBytes = []() { return perfFlashTotal(); };
    src.wifiOk = []() { return WiFi.status() == WL_CONNECTED; };
    src.authOk = []() { return wear.authenticated(); };
    perf.begin(src);
}

// ==================== Arduino 入口 ====================
void setup() {
    Serial.begin(115200);
    delay(300);
    Serial.println("\n[xw] Redmi Watch 5 network gateway starting...");
    // 诊断：固件版本 + 内存水位（排查网页打不开用）
    // psram=0 说明 PSRAM 没初始化成功；heap 低说明内部 RAM 紧张。
    Serial.printf("[xw] fw=%s\n", XW_FW_VERSION);
    // 保险：显式启用「大分配走 PSRAM」的 malloc 阈值。
    // 预编译库的 psramInit() 理论上已调用，这里显式再调一次确保生效。
    heap_caps_malloc_extmem_enable(4096);
    // 一锤定音的验证：malloc 大块是否真的落到 PSRAM。
    // 这行日志也是固件版本指纹——看到它说明刷的是含 PSRAM 页面修复的固件。
    {
        uint32_t psramBefore = (unsigned)ESP.getFreePsram();
        void* probe = malloc(32769);
        int32_t delta = probe ? (int32_t)(psramBefore - (unsigned)ESP.getFreePsram()) : -1;
        Serial.printf("[xw] mem boot: heap=%u minHeap=%u psram=%u psramFree=%u\n",
                      (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMinFreeHeap(),
                      (unsigned)ESP.getPsramSize(), (unsigned)ESP.getFreePsram());
        Serial.printf("[xw] malloc32K probe: %s, psramDelta=%d（期望约 32768，"
                      "若为 0 说明大分配没走 PSRAM）\n",
                      probe ? "ok" : "FAIL", delta);
        free(probe);
    }

    WebUi::loadConfig(cfg);
    normalizeMac(cfg.watchMac);
    normalizeHexKey(cfg.authKeyHex);

    // ---- 板载 LED ----
    // 注意上电时序：自检必须在 updateLed() 第一次跑之前完成。
    // 否则 setup 里后续的 WiFi.mode()/蓝牙初始化会把状态机切到 Connecting，
    // 自检还没播完就被"状态指示"顶掉，用户只看到零星的闪，反而更迷惑。
    if (cfg.ledEnabled && cfg.ledPinValid()) {
        led.begin(cfg.ledPin, cfg.ledActiveLow);
        led.setState(LedState::Booting);
        // 启动时打一条自检提示：换板子后对照这条日志就能判断接线是否匹配。
        // 随后 LED 会「亮 700ms -> 灭 700ms -> 亮 700ms」，肉眼确认即可。
        Serial.printf("[xw][led] pin=GPIO%d active=%s（期望：亮-灭-亮 各 700ms）\n",
                      int(cfg.ledPin), cfg.ledActiveLow ? "LOW=点亮" : "HIGH=点亮");
        Serial.printf("[xw][led] 若灯型相反（灭-亮-灭）说明极性反了，"
                      "把「点亮电平」改成%s\n",
                      cfg.ledActiveLow ? "高电平点亮" : "低电平点亮");
        // selftest() 是同步阻塞的：内部自己 delay 走完 2.1 秒再返回，
        // 这样它不会被后面 WiFi/蓝牙初始化引发的状态切换顶掉。
        led.selftest();
        Serial.println("[xw][led] 自检结束，转入状态指示");
    } else {
        led.begin(-1, cfg.ledActiveLow);
        Serial.println("[xw][led] 未启用（ledEnabled 关闭或引脚无效）");
    }

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
            ledSignalFault(); // LED 亮 3 秒提示失败
        }
        // 安装成功 → 删除板子上的安装包缓存；失败 → 保留以便重装
        web.watchfaceInstallDone(ok, err);
    };

    wx.begin(wear, cfg);

    // 关键：WiFi 必须在 web.begin() 之前初始化。
    // WebServer::begin() 内部会 tcp_bind()，而 lwIP 的 tcpip 线程（和它的 mbox）
    // 要等 WiFi.mode() / esp_netif_init() 才创建。顺序反了会直接断言失败：
    //   assert failed: tcpip_send_msg_wait_sem ... (Invalid mbox)
    // 这个是硬崩溃（不是警告），任何板子都会中招。
    WiFi.mode(WIFI_STA);

    // 注意：这里绝不能调用 WiFi.setSleep(false)！
    // setSleep(false) = esp_wifi_set_ps(WIFI_PS_NONE)，把 WiFi modem sleep 关掉。
    // 而 WiFi 与经典蓝牙共存时 modem sleep 必须保持开启（IDF 官方约束，
    // espressif/esp-idf issue IDFGH-8094）：先关 sleep 再 esp_bt_controller_enable()，
    // 会直接 abort 在 coex_core_enable()（Backtrace: coex_enable -> esp_bt_controller_enable）。
    // 实测这块板子就是被这一行炸掉的：bt: init classic BT (master) 后立刻 abort。
    // 默认值 WIFI_PS_MIN_MODEM 就是共存要的正确状态，什么都不用做。

    web.begin(wear, gw, wx, cfg, bt, wfInstaller, g_faces, ota);
    web.setLed(&led);
    web.setPerf(&perf);
    serialPush.begin(web);
    Serial.printf("[xw] mem after web.begin: heap=%u psramFree=%u\n",
                  (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getFreePsram());

    // 性能监控必须在 web.begin 之后（需要访问 faceCache）
    beginPerfMon();

    // WiFi 已在 web.begin() 之前初始化，这里只做连接/开 AP
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
    Serial.printf("[xw] mem after bt.begin: heap=%u psramFree=%u\n",
                  (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getFreePsram());

    Serial.printf("[xw] watch mac: %s\n",
                  bt.hasWatchMac() ? bt.watchMacStr() : "(auto: bonded)");
    lastLinkMs = millis();
    Serial.println("[xw] setup done");
}

void loop() {
    // 诊断：周期打印内存水位（每 20 秒，排查网页/DNS 打不开用）
    {
        static uint32_t lastMemLog = 0;
        uint32_t nowMs = millis();
        if (nowMs - lastMemLog >= 20000) {
            lastMemLog = nowMs;
            Serial.printf("[xw] mem: heap=%u minHeap=%u psramFree=%u apClients=%d\n",
                          (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMinFreeHeap(),
                          (unsigned)ESP.getFreePsram(), WiFi.softAPgetStationNum());
        }
    }

    bt.loop();
    web.loop();
    serialPush.loop();

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
    perf.loop();
    updateLed();
    led.loop();
}
