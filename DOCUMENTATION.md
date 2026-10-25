# ESP32 红米 Watch 5 网络网关 · 完整文档

> 当前版本 **v1.5.0** · 本文档为项目唯一完整文档，整合了原 `README.md`、
> `docs/protocol-notes.md`、`docs/ui-plan.md`、`firmware/使用说明.md` 的全部内容。

## 目录

- [1. 项目简介](#1-项目简介)
- [2. 功能特性](#2-功能特性)
- [3. 硬件与软件要求](#3-硬件与软件要求)
- [4. 快速开始](#4-快速开始)
  - [4.1 烧录固件](#41-烧录固件)
  - [4.2 配网](#42-配网)
  - [4.3 配对手表](#43-配对手表)
  - [4.4 配置配对密钥](#44-配置配对密钥认证必需)
  - [4.5 安装自定义表盘](#45-安装自定义表盘)
  - [4.6 固件升级（OTA）](#46-固件升级ota)
  - [4.7 自动恢复出厂（可选）](#47-自动恢复出厂可选默认关闭)
  - [4.8 板载 LED 状态指示](#48-板载-led-状态指示)
- [5. 性能监控](#5-性能监控)
- [6. Web 管理界面](#6-web-管理界面)
  - [6.1 页面与路由](#61-页面与路由)
  - [6.2 UI 设计与美化](#62-ui-设计与美化)
- [7. Flash 容量与分区表](#7-flash-容量与分区表)
- [8. 工作原理](#8-工作原理)
- [9. MiWear SPP V2 协议与实现细节](#9-miwear-spp-v2-协议与实现细节)
- [10. 测试](#10-测试)
- [11. 故障排查](#11-故障排查)
- [12. 提取配对密钥（auth key）](#12-提取配对密钥auth-key)
- [13. 已知限制](#13-已知限制)
- [14. 固件发布包](#14-固件发布包)
- [15. 目录结构](#15-目录结构)
- [16. 致谢与参考](#16-致谢与参考)
- [17. 免责声明](#17-免责声明)

---

## 1. 项目简介

运行在 ESP32 开发板上的独立固件，**替代小米运动健康（Mi Fitness）App**，为
**红米 Watch 5 蓝牙版（非 eSIM，无 WiFi）**提供完整的互联网接入能力。

```
红米 Watch 5 ──蓝牙 SPP──> ESP32 ──WiFi──> 互联网
 (仅蓝牙)      MiWear V2   协议中继+网络栈   DHCP/DNS/HTTP...
```

手表自身没有 WiFi 和蜂窝网络，平时靠手机的「小米运动健康」App 做蓝牙代理上网。
本项目用一块 ESP32 开发板（约 30 元）接管这个角色：手表连上 ESP32 后即可
DHCP 获取 IP、DNS 解析、TCP/UDP 联网，并额外获得**天气推送**与**网络对时**。

## 2. 功能特性

| 功能 | 说明 |
| --- | --- |
| 网络接入 | DHCP / ICMP(ping) / TCP 中继 / UDP 中继 / DNS 代答 |
| 协议认证 | MiWear SPP V2 完整握手（PhoneNonce/WatchNonce/AuthStep3，AES-128-CTR 加密通道） |
| 天气推送 | Open-Meteo 免费接口（无需 Key），支持手表主动请求 + 周期刷新 |
| 网络对时 | NTP 校准后推送 `System.Clock`，手表显示准确时间与时区 |
| 自定义表盘 | 上传 .bin 表盘文件（米坛社区等来源）→ 写入独立表盘分区 → 校验 MD5/CRC32 → 蓝牙分块安装 → 激活/卸载；**安装成功即自动删除缓存**，失败保留可重装，48 小时未用自动清理 |
| 表盘串口推送 | 用 USB 数据线把 .bin 直接推到板子上（`XWFP` 行协议），板子自行完成安装，无需 PC 端安装软件 |
| 固件 OTA | Web 上传 `firmware.bin`，A/B 双分区切换、启动确认、自动与手动回滚 |
| 性能监控 | 独立「性能监控」页：CPU / 内存 / 存储 / 网络上下行 / 蓝牙吞吐 / 网络延迟 / 芯片温度，Canvas 折线图（仿任务管理器）；全站右下角常驻**悬浮占用条**，点击可展开/收起、可拖动贴边并记忆位置 |
| Web 管理 | 状态页 / 配置页 / 蓝牙配对页 / 表盘管理页 / 性能监控页 / 固件升级页，自动保存到 NVS |
| 配网 | 无 WiFi 或连接失败时自动开 AP `RedmiWatch5Net-XXXX`（192.168.4.1） |
| 自动恢复出厂 | **可选，默认关闭**。连续 N 分钟既无 WiFi 也无手表连接时清空配置并重启进配网；网页可设开关与超时 |
| 板载 LED 指示 | 用板载 LED 指示运行状态（配网/联网/配对/装表盘/升级/故障）；**GPIO 引脚与点亮电平可在网页配置**，并提供点亮/熄灭测试按钮 |
| Web UI | 状态结论条、统一状态色、内联 SVG 图标、深色模式、移动端表格卡片化、表单就地校验、统一错误页、加载骨架屏、打印样式，无外部字体/脚本/样式依赖 |

## 3. 硬件与软件要求

- **开发板**：任何带经典蓝牙 + WiFi 的 ESP32（ESP32-WROOM-32 / WROVER / DevKitC 等）
  - 本版固件按 **16MB Flash** 构建（2×4MB OTA 槽 + 4MB 独立表盘分区）；
    8MB 板子需把 `platformio.ini` 的 `board_build.flash_size` 改为 `8MB` 并使用
    `partitions-8mb.csv`；4MB 板子无法同时容纳双 OTA 与独立表盘区
  - 注意：ESP32-S2/S3/C3 没有经典蓝牙，**不适用**；必须用原版 ESP32
- **手表**：红米 Watch 5 蓝牙版（非 eSIM 版）
- **工具链**：PlatformIO（VS Code 插件或 `pio` CLI），或 ESP-IDF + Arduino 组件

## 4. 快速开始

### 4.1 烧录固件

**方式 A：直接用发布固件（`firmware/` 目录，无需编译）**

| 文件 | 用途 |
| --- | --- |
| `firmware/watch5-net-v1.5.0-full-16mb.bin` | USB 整片烧录，**首次必须用这个**（16MB 板） |
| `firmware/watch5-net-v1.5.0-ota.bin` | Web「固件升级」页上传，后续免插线升级 |

```bash
# USB 整片烧录（首次 / 救砖）
esptool.py --chip esp32 --port /dev/ttyUSB0 --baud 921600 \
  write_flash -z 0x0 firmware/watch5-net-v1.5.0-full-16mb.bin
```

文件名规则：`watch5-net-<版本>-<用途>.bin`，详细说明与校验值见
[`firmware/SHA256SUMS.txt`](firmware/SHA256SUMS.txt) 与[第 14 节](#14-固件发布包)。

**方式 B：自己编译**

```bash
cd esp32-redmi-watch5-net
pio run -t erase           # 首次从旧分区表升级必须全擦
pio run -t upload          # 编译并烧录
pio device monitor         # 查看日志（115200）
```

编译产物：`.pio/build/esp32dev/firmware.bin`（约 1.85MB），等价发布的 `-ota.bin`；
后续可在 Web「固件升级」页直接上传，不需要 USB。生成整片合并镜像的命令见
[第 14 节](#14-固件发布包)。

> 本版为 **16MB 双 OTA + 独立表盘分区**：两个 4MB 应用槽 + 4MB 表盘区 + 64KB coredump，
> 不再使用 LittleFS。分区表变化后必须 `pio run -t erase`，否则 NVS 残留会导致
> 配置异常或启动分区选择错误。合并镜像 `-full-16mb.bin` 只能用于 USB
> 整片烧录，不能上传到 OTA 页面；8MB 板需另行生成对应容量的镜像。

### 4.2 配网

首次启动未配置 WiFi，设备自动开启配网热点：

- 连接 WiFi **`RedmiWatch5Net-XXXX`**（XXXX 为板子 MAC 后四位，默认开放无密码）
- 浏览器打开 **http://192.168.4.1** → 「配置」页填写家中/服务器 WiFi → 保存并重启

> 已配置 WiFi 但连不上（改密码、路由器重启、信号太弱）时，设备会在 20 秒后进入
> 同样的 AP 配网模式，**并且每 60 秒重试一次已保存的 WiFi**；一旦连上就自动关闭
> 热点、回到正常 STA 工作，无需手动重启。AP 最短保留 90 秒，避免刚进配网就被关掉。

> **连上 WiFi 后的访问地址**：STA 模式下 IP 由路由器 DHCP 动态分配，无固定地址，
> 也无 mDNS 主机名。查找方式：① 路由器后台「已连接设备 / DHCP 客户端列表」中找
> 设备名 `esp32`（固件未自定义主机名）或按 MAC 识别；② 内网扫描工具筛 80 端口
> 开放、主机名 esp32 的设备。「状态」页「WiFi 网络」行会显示当前 IP 与 RSSI。
> 串口日志（115200）不打印 IP。建议在路由器上为板子 MAC 绑定静态 IP，便于长期访问。

### 4.3 配对手表

> 前提：手表已恢复出厂设置或已在系统蓝牙设置里取消与旧手机的配对
> （长按手表表盘进入设置 → 系统 → 重置，或在旧手机上取消配对）。
> 操作时手表需亮屏并保持在蓝牙设置/配对界面附近。

1. 打开 Web 页 →「配对」
2. 点「开始扫描」（约 8 秒），列表中出现 **Redmi Watch 5** 后点「选择」
3. 设备重启；此时**手表端会弹出配对请求，在手表上确认**
4. 「状态」页蓝牙一栏显示 `已连接` 即 SPP 链路建立

### 4.4 配置配对密钥（认证必需）

SPP 连接后还需 MiWear 协议层的认证，密钥从小米运动健康 App 提取
（需 root 或备份，方法见[第 12 节](#12-提取配对密钥auth-key)）。在「配置」页填写
16 字节 hex 密钥：

```
00112233445566778899aabbccddeeff
```

保存重启后，「状态」页认证一栏显示 **已完成（加密通道就绪）** 即全部就绪。
此时手表端的天气、需要联网的应用即可通过 ESP32 上网。

> 未填密钥时协议握手能走到「会话就绪」，但认证无法通过（手表会拒绝），
> 网络功能不可用——这是小米的设备绑定机制，无法绕过。

### 4.5 安装自定义表盘

**方式 A：网页上传（最常用）**

1. 打开 Web 页 →「表盘」
2. 选择从米坛社区（BandBBS）等渠道下载的 `.bin` 表盘文件 →「上传并安装到手表」
3. 页面显示进度（校验 → 等待手表确认 → 分块上传 → 激活）
4. 安装完成后「手表端表盘」会刷新，新表盘标记为「使用中」；可随时点「启用」切换或「卸载」

**方式 B：USB 数据线推包（无需联网、无需 PC 端安装软件）**

板子自带串口推包通道：把 `.bin` 通过数据线推到板子上，板子自己完成校验与安装。

```bash
pip install pyserial
python3 tools/push_watchface.py 表盘.bin --port COM3
```

协议为行式 ASCII（命令前缀 `XWFP `，状态前缀 `XWFS `），
与固件日志共线互不干扰，详见 `include/serialpush_proto.h`。

**日志自动记录到供电的电脑（PC 侧，插线即记）**

板子的串口日志本来就走在供电那根 USB 线上（115200）。`tools/serial_logger.py`
在电脑侧自动识别板子串口并把日志落盘，无需手动开串口监视器，固件零改动：

```bash
pip install pyserial
python3 tools/serial_logger.py                 # 自动识别串口，日志存 ./logs
python3 tools/serial_logger.py --port COM3     # 指定串口（Windows）
python3 tools/serial_logger.py --list          # 只列出串口，确认板子被识别
```

- 自动识别 CH340 / CH341 / CP210x / FT232 / PL2303 等 USB 转串口桥
- 拔线、复位、掉线自动重连；每行带毫秒时间戳，按连接次分文件
- 常用参数：`--dir` 输出目录、`--only-xw` 只留 `[xw]`/`XWFS` 行、`--max-mb` 单文件上限滚动
- Windows 后台静默（无黑窗）：`pythonw tools\serial_logger.py --dir %USERPROFILE%\watch5-logs`，
  需要开机自动记录就把它加进「任务计划程序」的登录时触发
- 注意：串口同一时刻只能被一个程序打开，运行本工具前先关掉 Arduino IDE 的串口监视器

> **表盘安装包缓存规则**
> - 安装**成功** → 板子自动删除缓存包，不留残留
> - 安装**失败** → 缓存保留，「表盘管理」页可「重新安装此包」或「删除缓存」
> - 缓存包超过 **48 小时**未被使用 → 板子自动删除
>
> 16MB 布局下表盘写入**独立 4MB 分区**，与固件 OTA 槽互不影响，
> 装表盘**不会**破坏固件回滚镜像。
> 安装流程与米坛「表盘自定义工具箱」App 一致（MiWear type=4/type=22 + Mass 通道）。

### 4.6 固件升级（OTA）

首次通过 USB 全擦烧录带双 OTA 分区表的固件后，后续升级无需数据线。全擦会清除
NVS 配置和蓝牙绑定，烧录后需要重新配网并重新配对手表。

1. 打开 Web 页 →「固件升级」
2. 选择 `firmware/watch5-net-v1.5.0-ota.bin`（自编译则是
   `.pio/build/esp32dev/firmware.bin`；**不要**选 `-full-16mb.bin` 合并镜像）
3. 点「上传并升级」，页面会提示正在写入；保持供电和网络，设备完成后自动重启
4. 新固件启动后，**成功连上已配置的 WiFi** 并稳定 60 秒会自动确认；若 180 秒仍
   未连上 WiFi 且另一分区有有效固件，bootloader 会自动回滚。仅处于 AP 配网模式
   不算「联网成功」（设备本来就没有 WiFi 配置的纯 AP 用法除外）
5. 「固件升级」页可手动回滚到另一分区

> OTA 只接受 ESP32 应用镜像（0xE9 头 + 0x20 处应用描述符），会拒绝合并镜像、
> 表盘文件和截断文件。升级期间会禁止表盘安装、保存配置和重启操作。

### 4.7 自动恢复出厂（可选，默认关闭）

设备长时间离线（例如换了路由器、改了 WiFi 密码后自己回不来）时，可以让它在
约定时间后自我清理并重新进入配网模式，免去现场插线。

1. 打开 Web 页 →「配置」→「自动恢复出厂」
2. 「状态」选「开启」，填写超时分钟数（1–1440），保存并重启

判定规则：连续 N 分钟内 **WiFi 和手表都没有连接**（两者皆无）才触发；
设备处于配网 AP 模式时暂停计时，不会打断正在进行的配置。

触发后会清空 WiFi 名称/密码、配对密钥、手表 MAC 与 AP 密码，然后重启进入
`RedmiWatch5Net-XXXX` 热点等待重新配置；天气位置、时区等其余参数保持不变。

> 默认关闭。开启后只要 WiFi 正常就不会触发；但若 WiFi 本身长期不可用，
> 设备会按设定周期反复清空配置并重启，请确认场景合适再开启。

### 4.8 板载 LED 状态指示

用开发板上的 LED 就能看出设备在干什么，不用连电脑看串口日志。

1. 打开 Web 页 →「配置」→「板载 LED 指示」
2. 「GPIO 引脚」填实际接的引脚（多数 ESP32 开发板板载 LED 是 **GPIO2**，
   填 **-1** 表示禁用 LED）
3. 「点亮电平」按实际情况选（大多数板是**低电平点亮**）
4. 保存后点「LED 测试」里的「点亮 / 熄灭」按钮确认接对了

状态与灯型对应关系：

| 状态 | 灯型 | 含义 |
| --- | --- | --- |
| 启动中 | 快闪 | 固件刚起来 |
| 配网 AP 模式 | 慢闪 | 正在等用户连热点配网 |
| 连接 WiFi 中 | 单闪 | 正在握手 |
| 已联网且手表已连接 | **常亮** | 正常工作 |
| 已联网但手表未连接 | 极慢短闪 | 网关在线，等手表 |
| 蓝牙配对中 | 快速双闪 | 正在扫描/配对手表 |
| 正在安装表盘 | 短亮长灭 | 表盘在往手表里传 |
| 正在升级固件 | 方波 | OTA 写入或等待确认 |
| 故障 | 常亮 3 秒 | 表盘安装失败等 |

运行状态页也会显示当前 LED 状态，`/api/status` 接口里有 `led` 字段。

## 5. 性能监控

「性能监控」页（`/perf`）模仿 Windows 任务管理器，提供 4 张数字卡 + 4 张 Canvas
折线图；数据经 `/api/perf` 提供（当前读数 + 90 点历史，2 秒轮询）。
全站右下角另有一个常驻**悬浮占用条**，不打断当前操作即可掌握设备状态。

**指标与采样实现**（`include/perfdata.h/.cpp` 为纯逻辑层，可主机端单测；
`include/perfmon.h/.cpp` 为 ESP32 采样层）：

| 指标 | 采样方式 |
| --- | --- |
| CPU 占用 | 注册 CPU1 空闲钩子，用调用频率相对启动标定的基准估算 |
| 内存 | `heap_caps` 8bit 可分配堆 |
| 存储 | 应用镜像 + 表盘缓存 + 系统区，对整片 Flash |
| 网络上下行 | NetGw 的 TCP + UDP 累计字节 |
| 蓝牙吞吐 | BtLink SPP 收发累计字节 |
| 网络延迟 | 非阻塞 TCP 握手到 223.5.5.5:53，15 秒一次 |
| 芯片温度 | `temperatureRead()`，30 秒一次并缓存 |

**悬浮占用条**：显示 CPU/内存/存储百分比 + 网络上下行 + 蓝牙 + 延迟 + 温度 +
整体状态点；点击在精简/完整间切换，小屏默认收起；可拖动贴靠左右边缘，
位置与展开状态记入 `localStorage`，刷新后保持。

> 已知限制：WiFi 工作时 ESP32 内部温度传感器固定返回 128°F（≈53℃）假值，
> 固件已识别并标记为「不可用」，界面显示 –。

## 6. Web 管理界面

### 6.1 页面与路由

页面路由：

| 路径 | 用途 |
| --- | --- |
| `/` | 状态页（5s 自动刷新） |
| `/config` | 配置页（WiFi / 密钥 / MAC / 天气 / LED / 自动恢复出厂） |
| `/pair` | 蓝牙配对页（扫描 / 选择 MAC） |
| `/watchface` | 表盘管理页（上传 / 启用 / 卸载 / 缓存管理） |
| `/perf` | 性能监控页 |
| `/firmware` | 固件升级页（上传 / 回滚 / 确认） |

操作端点：

| 方法 | 路径 | 用途 |
| --- | --- | --- |
| POST | `/save` | 保存配置并重启 |
| POST | `/weather` | 立即刷新天气 |
| POST | `/reboot` | 重启设备 |
| POST | `/pair/scan` `/pair/stop` `/pair/save` | 扫描 / 停止 / 保存配对 |
| POST | `/watchface/upload` | multipart 上传表盘并立即安装 |
| POST | `/watchface/set` | 激活指定表盘 |
| POST | `/watchface/uninstall` | 删除手表端表盘 |
| POST | `/watchface/refresh` | 刷新手表端表盘列表 |
| POST | `/watchface/retry` | 重装缓存中的表盘包 |
| POST | `/watchface/cache/delete` | 删除缓存表盘包 |
| POST | `/update` | multipart OTA 上传（Update 库） |
| POST | `/firmware/rollback` | 回滚到另一分区 |
| POST | `/firmware/keep` | 确认当前固件 |
| POST | `/forget` | 忘记 WiFi 并重启进配网 |
| POST | `/led/test` | LED 点亮/熄灭测试 |

数据接口：

| 路径 | 内容 |
| --- | --- |
| `/api/status` | 运行状态 JSON（含 `led` 字段） |
| `/api/watchface` | 表盘列表与安装进度 |
| `/api/firmware` | 固件版本与分区信息 |
| `/api/perf` | 性能当前读数 + 90 点历史 |

其他说明：

- 配置持久化：NVS Preferences 命名空间 `xwnet`
- 配网：无 WiFi 配置或 STA 20s 超时 → AP `RedmiWatch5Net-XXXX`
  （192.168.4.1，DNSServer 劫持，captive portal）
- 配对页：8s 扫描（可配置 3–30s）→ 列表选择 → 保存重启 → 手表端确认
- 表盘安装进度由 `WatchfaceInstaller::progress()` 提供，页面 2 秒自刷新

### 6.2 UI 设计与美化

**硬约束**（决定实现边界）：

- 页面由 ESP32 在运行时拼接生成，**不能引入构建工具链**（无 npm / 打包 / 预处理器）
- 单页 HTML 已接近 10KB，**不能引入外部 CSS/JS 依赖**（设备常在无外网的配网环境）
- 浏览器多为手机（iOS Safari / Android Chrome），**移动端优先**
- 所有资源必须能从 Flash 直接发出，**字体只能用系统字体栈**

**设计原则**：

1. **一个主色**：品牌橙 `#ea580c` 只用于「主要操作 + 当前导航」。状态色独立成体系：
   正常 `#087a45`（绿）、注意 `#b45309`（琥珀）、异常 `#b42318`（红）、中性 `#526174`（灰）。
2. **信息分层**：每页只允许一个「主卡片」承载结论（能不能用），其余为支撑信息。
3. **移动端优先**：所有交互元素 ≥ 44×44px；表格在 <600px 时降级为键值卡片。
4. **零外部依赖**：CSS 内联、图标用内联 SVG、图表用 Canvas。
5. **渐进增强**：`prefers-color-scheme` 与 `prefers-reduced-motion` 都做，不支持则退回基础样式。

**问题与改进对照**（v1.5.0 已全部实施）：

| 编号 | 原问题 | 实施方案 |
| --- | --- | --- |
| U1 | 导航 6 项偏挤、移动端底部留白不齐 | 桌面 3 列网格，移动端 2 列；当前项加底部橙色指示条 |
| U2 | 状态页 6 张卡片平铺，主次不分 | 状态页顶部「结论条」：一句话总结（如「运行正常 · 已联网 · 手表已认证」），下方才是明细卡片 |
| U3 | 颜色语义不统一 | 抽出 `--ok / --warn / --bad / --neutral` 四个状态变量，全站状态色只走这套令牌；图表色同样令牌化 |
| U4 | 没有深色模式 | `prefers-color-scheme: dark` 覆盖整套变量；Canvas 图表运行时读取 CSS 变量跟随主题 |
| U5 | 表单校验反馈缺失（保存失败只返回 `409` 纯文本） | 保存/重启等失败改为带样式的统一错误结果页（含返回按钮）；配置表单 `noValidate` + 就地校验（密钥 32 位 hex、MAC 格式、AP 密码 ≥8 位） |
| U6 | 移动端表格横向滚动 | <600px 时 `thead` 隐藏，每行变为「字段名 / 值」两列卡片 |
| U7 | 数字刷新「跳变」 | 数值变化只做 0.2s 颜色过渡，不做位移动画 |
| U8 | 全站无图标 | 内联 SVG `<symbol>` + `<use>` 图标系统（导航与卡片标题，深色模式自动反色） |
| U9 | 没有加载态 | 表盘页空列表时显示骨架行而非「尚未获取」文字 |
| U10 | 无打印样式 | `@media print`：隐藏 nav、悬浮条、按钮与图表，去阴影，白底黑字 |

**验证结论**：六个页面在 1100px 桌面与 390px 手机视口下无横向溢出、无 JavaScript 错误；
深色模式图表颜色正确；配置表单错误提示出现与清除符合预期。
最终应用区 1.81MB（45.1%），仍远低于 4MB 单槽上限。

## 7. Flash 容量与分区表

**本版固件默认按 16MB Flash 构建**（`partitions.csv` = 16MB 布局）。
仓库同时提供三套分区表，按板子容量选用：

| 板子 Flash | 使用的分区表 | 构建方式 |
| --- | --- | --- |
| **16MB**（当前默认，推荐） | `partitions.csv`（= `partitions-16mb.csv`） | `pio run`（env `esp32dev`，无需改） |
| 8MB | `partitions-8mb.csv` | `pio run -e esp32dev-8mb`（专用 env，自动切换分区表与容量） |
| 4MB | 不建议 | 无法同时容纳双 OTA 与独立表盘分区 |

切换步骤：

1. 8MB 板直接使用专用环境 `pio run -e esp32dev-8mb`（无需改 `platformio.ini`；
   也可手动把 `board_build.flash_size` 改为 `8MB`、`board_build.partitions` 改为 `partitions-8mb.csv`）
2. **重新整片烧录**：`pio run -t erase`（新板第一次必须全擦，会清 WiFi 配置与蓝牙绑定），
   再烧对应容量的整片镜像（merge_bin 时 `--flash_size` 与 `--fill-flash-size` 同步改），
   或用 Flash Download Tool 选对应容量烧整片镜像

三档布局对比：

| 分区 | 4MB 版 | 8MB 版 | 16MB 版（当前） |
| --- | --- | --- | --- |
| ota_0 / ota_1 | 2 × 1,984KB | 2 × 2,560KB | **2 × 4MB** |
| 单个 OTA 槽剩余（固件 1.81MB） | 约 190KB ⚠️ | 约 767KB | **约 2.2MB** |
| 表盘暂存 | 与非活动 OTA 槽共用 | 独立 2MB | **独立 4MB** |
| 未分配保留 | 0 | 896KB | 3.9MB |
| 回滚镜像被表盘覆盖 | ⚠️ 会 | ✅ 不会 | ✅ 不会 |

> **不同容量的设备不能互相 OTA 升级**：分区布局不同，升级包必须与设备容量匹配。

## 8. 工作原理

### 协议栈（自底向上）

1. **蓝牙 SPP**：`btlink` 管理 RFCOMM 连接。手表是 SPP 服务端，ESP32 主动连接。
   RX 数据在蓝牙栈回调中写入 16KB 环形缓冲，主循环消费，避免丢包。
2. **MiWear SPP V2 帧**：`[0xA5A5][type][seq][len][crc16][payload]`。
   V1 版本查询 → SessionConfig → 认证握手 → 此后 Pb 通道 AES-128-CTR 加密。
3. **NetWork 通道**：认证后手表把原始 IPv4 包塞进 `channel=7` 的帧发给 ESP32。
4. **用户态网络栈**（`netgw`）：ESP32 给手表代答 DHCP（10.1.10.2/24，
   网关 10.1.10.1，DNS 223.5.5.5）、应答 ICMP，并为每个 TCP/UDP 连接在
   ESP32 上开一个真实 socket 双向搬运字节——与 AstroBox 的 ipstack 方案一致。

### 为什么不直接用 ESP32 的 lwIP + NAPT

Arduino 框架预编译的 lwIP 未开启 `IP_NAPT`，且自定义 netif 涉及 tcpip
线程安全问题；用户态中继行为与参考实现完全一致，且可以在 PC 上完整单测。

### 天气与对时

- 天气：手表请求 → Open-Meteo 拉取 → 按 Gadgetbridge 的
  `XiaomiWeatherService` 序列推送（当前 + 7 日预报）。
- 对时：NTP 同步后推送 `Command{type=2, subtype=3, System.Clock}`，
  认证成功时立即推送一次，之后每小时重推。

## 9. MiWear SPP V2 协议与实现细节

> 本节内容来自对 Gadgetbridge / AstroBox-Public 源码与 arduino-esp32 /
> ESP-IDF 官方源码的逐行核实。

### 9.1 SPP V2 帧格式

```
[0xA5 0xA5] [type:1] [seq:1] [len:2 LE] [crc16:2 LE] [payload:len]
```

- `type`：1=ACK，2=SessionConfig，3=Data
- `crc16`：CRC-16/ARC（poly 0xA001，反射，初值 0x0000），**只覆盖 payload**
- `seq`：发送方自增；收到 Data/SessionConfig 帧需回 ACK（seq 回显）
- Data payload：`[channel:1][opcode:1][data...]`

| channel | 用途 | 加密 |
| --- | --- | --- |
| 1 (Pb) | protobuf Command | type=1（认证）明文，其余认证后 AES-128-CTR |
| 2 (Mass) | 数据/文件传输（表盘安装） | 明文 |
| 7 (NetWork) | 原始 IPv4 包 | 明文 |

opcode：1=Plain，2=Encrypted。

### 9.2 V1 帧格式（版本协商阶段，易错点）

Gadgetbridge `XiaomiSppPacketV1.encode` 逐字节核实：

```
[BA DC FE] [ch:1] [flags:1] [size:2 LE] [opcode:1] [serial:1] [dtype:1] [payload] [EF]
                              ^^ size = payload.len + 3（不是 payload.len）
总长 = 11 + payload.len = size + 8
```

- payload 起点是偏移 **10**（10 字节头部），最后一个字节是 0xEF 结尾
- 实测版本查询 `BA DC FE 00 C0 03 00 00 01 00 EF`：size=3 → payload 为空，
  总长 11 ✓（早期实现把 size 读成偏移 6-7、payload 起点算成 8，导致版本
  响应永远解析不出 payload[0]，握手停在 CONNECTED；已修正并有主机测试覆盖）

### 9.3 连接与认证流程

```
SPP connect (ESP32 主动, RFCOMM, SDP 发现 SCN)
  -> ESP32 发 V1 版本查询 BA DC FE 00 C0 03 00 00 01 00 EF
  -> 手表回版本（>=2 走 V2）
  -> ESP32 发 SessionConfig(StartSession): opcode=1, VERSION=1.0.0,
     MAX_PACKET_SIZE=0xfc00, TX_WIN=32, SEND_TIMEOUT=10000ms
  -> 手表回 SessionConfig
  -> ESP32 发 Auth.PhoneNonce（16 字节随机，esp_random）
  -> 手表回 Auth.WatchNonce（HMAC-SHA256(authKey, phoneNonce||watchNonce) 校验）
  -> ESP32 发 Auth.AuthStep3：
       AuthDeviceInfo{unknown1=0, phoneApiLevel, phoneName, unknown3=224, region}
       AES-128-CCM(key=authKey, nonce=12B) 加密 -> encryptedDeviceInfo
       encryptedNonces = HMAC-SHA256(encKey, phoneNonce||watchNonce) 32 字节
       （Gadgetbridge XiaomiAuthService: hmacSHA256(encryptionKey, nonce||watchNonce)）
  -> 手表回 Auth{status=1}（subtype=27 或 AUTH_SEND_USERID）
  -> 此后 Pb 通道 AES-128-CTR 加密
```

### 9.4 KDF（miwear-auth）

```
hmac_key = HMAC-SHA256(key = phoneNonce || watchNonce, data = authKey)
okm      = T1 || T2,  T_i = HMAC(hmac_key, T_{i-1} || "miwear-auth" || i)
decKey = okm[0:16]   encKey = okm[16:32]
decNonce = okm[32:36]  encNonce = okm[36:40]
```

AES-128-CTR 的 IV = key 本身（协议规定，每消息独立计数器）。
参考值与独立 Python 实现交叉验证一致。

### 9.5 字节序约定（重要）

全套代码使用「**网络字节的小端原生读法**」：

- IP/端口在网络帧里是大端字节流；小端机器上直接读成 `uint32_t`/`uint16_t`
  存放，`memcpy` 往返即网络序
- 例：`10.1.10.2` 的网络字节为 `0A 01 0A 02`，小端读作 `0x020A010A`，
  即 `NETGW_WATCH_IP_BE = 0x020A010A`
- 好处：赋给 `sin_addr.s_addr` / `sin_port` 时内存布局天然正确，无需转换
- DHCP 回复字段（yiaddr 等）需按「LSB-first 网络序」逐字节写
  （`putIp()`，见 dhcp.cpp）；纯大端整数（xid、flags、lease）用 `putBe32`

### 9.6 手表网络栈（NetGw）

```
手表 10.1.10.2/24 <--BT--> ESP32 网关 10.1.10.1 <--socket--> 互联网
                        MTU 800, MSS 700, DNS 223.5.5.5
```

- **DHCP**：手表 DISCOVER/OFFER/REQUEST/ACK 全代答，`siaddr=10.1.10.1`，
  DNS=223.5.5.5（阿里公共 DNS，国内快）
- **DNS 代答**：手表发给 10.1.10.1:53 的查询改写到真实 DNS 服务器，
  回程源地址改写回 10.1.10.1（dnsViaGateway 流标记）
- **ICMP**：echo request 直接应答；支持分片重组（手表可能分片）
- **TCP**：为每个 4 元组维护状态机 + 一个上游非阻塞 socket：
  - SYN → 立即 SYN-ACK（seq=myIsn，SYN 消耗一个序号，发送后再前移 nextSnd）
  - 回程段采用「服务器伪装」模型：src=手表连接的目标端口，dst=手表侧端口
    （与 AstroBox ipstack accept 语义一致）
  - 仅携带数据/FIN 的段回 ACK；纯 ACK 不应答
  - RTO 重传（unacked 段缓存）、watchWin/watchMss 限制、半关闭
    （手表 FIN → shutdownWrite(上游)；上游 EOF → FIN 给手表）
- **UDP**：每个 (watchPort, dstIp, dstPort) 一个 connected socket 中继

**线程模型**：`onIpPacket` 在主循环上下文触发（BT 数据经环形缓冲读出后分发），
只做纯内存操作；上游 socket 均为非阻塞，实际收发在 `loop()` 中完成。
手表端并发连接超限时 `netgw.dropped` 计数会增加。

### 9.7 蓝牙链路（BtLink）实现要点

依据 arduino-esp32 `BluetoothSerial`（master）源码核实：

- `onData` 注册后，库内部的 512 字节 `_spp_rx_queue` **不再被填充**。
  手表单帧可达 ~800B 且突发，故自建 **16KB 固定环形缓冲**
  （`portENTER_CRITICAL` 保护，BT 回调上下文无动态分配）
- `connect(addr, channel=0)` 内部先 SDP 发现 SCN 再连接，**阻塞最多 10s**，
  且会先 `disconnect()`——必须在独立 FreeRTOS 任务中调用
- `write()` 入 TX 队列（32 包，满时阻塞 ≤1s），实际 `esp_spp_write`
  在库的 TX 任务；`loop()` 中周期 `flush()`（队列空时零开销）
- `connected(timeout=0)` 读事件位，非阻塞；断开重连退避 5s
- SSP：`onConfirmRequest` → `confirmReply(true)` 自动接受（Just Works，
  手表端用户确认）
- **MAC 字节序**：ESP-IDF `MAC2STR` 为正序，`esp_bd_addr_t` 与人类可读
  MAC 顺序一致；`BTAddress::toString()` 亦为正序，无需反序
- **bonded 枚举**：arduino-esp32 3.20017 的 `BluetoothSerial` 没有
  `getBondedDevices()`（3.1.x+ 才加入），改用 ESP-IDF GAP classic 接口
  `esp_bt_gap_get_bond_device_num()` + `esp_bt_gap_get_bond_device_list()`
- 扫描：`discoverAsync(cb, timeoutMs)` 每设备回调一次，去重后存入固定数组

### 9.8 天气与对时命令

**天气**（与 Gadgetbridge XiaomiWeatherService 对齐）：

手表请求：`Command{type=10, subtype=3, weather{location{code,name}}}`

应答序列：

1. `Command{type=10, subtype=0, weather{current{...}}}` 当前天气
2. `Command{type=10, subtype=1, weather{forecast{metadata, entries}}}` 每日预报

- 数据源 Open-Meteo forecast API（免费无 Key）
- `locationKey` = `"accu:" + hash % 1000000`（与 Gadgetbridge 一致）
- 天气码映射 WMO → Xiaomi（见 proto.cpp `wmoToXiaomiCondition`）
- 首日用当天实况拼预报项；推送后 `refreshNow` 周期可配

**对时**：`Command{type=2, subtype=3, system{clock{date,time,timezone,isNot24hour}}}`

- `timezone`：**15 分钟为单位**的 sint32（zigzag）。CST-8 = -32
- `isNot24hour`：0=24 小时制，1=12 小时制
- 推送时机：认证成功后立即一次（NTP 未就绪则先推系统时间），
  NTP 首次同步后立即校准，之后每小时重推
- NTP：`ntp.aliyun.com` / `ntp1.aliyun.com` / `pool.ntp.org`，
  时区 `setenv("TZ", tzPosix) + tzset()`
- **偏移计算**：xtensa newlib 的 `struct tm` 无 `tm_gmtoff` 字段
  （需 `__TM_GMTOFF` 宏，默认未定义），改用
  `mktime(localTm) - time(nullptr)` 的差值算时区偏移秒数（可移植）

### 9.9 ESP-IDF 平台适配要点（真机编译核实）

- **AES-CCM**：ESP-IDF 5.x（mbedtls 3.x）移除了 mbedtls 2.x 的一次性
  `mbedtls_ccm_encrypt()`，必须 `mbedtls_ccm_setkey()` +
  `mbedtls_ccm_encrypt_and_tag()` 组合（tag 4 字节附在密文后）
- **poll() 不存在**：ESP-IDF lwIP 只提供 `select()`，socket 可写检查用
  `select(fd+1, nullptr, &wfds, nullptr, &tv)` + `SO_ERROR`
- **ArduinoJson 7**：`DeserializationOption::Filter` 只接受
  `JsonVariantConst`/`JsonDocument&`（v6 的 `const char*` 构造已移除），
  过滤描述需先 `deserializeJson(filterDoc, FILTER)` 再传入
- **configTime/getLocalTime**：由 `Arduino.h` 直接声明，无需额外头文件
- **WebServer/DNMServer**：全局命名空间类（勿在 namespace xw 内前向声明）
- **分区表**：16MB flash，双 OTA 槽各 4MB + 独立表盘分区 4MB + nvs + otadata
  + coredump 64KB，见 `partitions.csv`
- **表盘暂存**：优先写入独立 `watchface` 分区（16MB 布局，subtype 0x40），
  4MB 兼容布局退化为非活动 OTA 槽；安装成功即删除，与 OTA 回滚镜像互不影响
- **OTA 校验**：应用镜像首字节 `0xE9`，偏移 `0x20` 为 app descriptor magic
  `0xABCD5432`；写入后再执行 `esp_image_verify`，合并镜像不能用于 OTA

### 9.10 常量表

| 常量 | 值 | 说明 |
| --- | --- | --- |
| 网关 IP | 10.1.10.1 | ESP32 侧 |
| 手表 IP | 10.1.10.2 | DHCP 分配 |
| MTU | 800 | NetWork 通道（表端 buffer 限制） |
| MSS | 700 | TCP 载荷上限 |
| DNS | 223.5.5.5 | 阿里公共 DNS |
| 并发 TCP | 6 | `NETGW_MAX_CONNS` |
| 并发 UDP | 8 | `NETGW_MAX_UDP_FLOWS` |
| RX 环形缓冲 | 16KB | btlink SppStream |
| 默认 auth key | 16×0x00 | 未配置时占位，认证会失败 |

### 9.11 自定义表盘安装（对标米坛社区表盘工具箱）

实现依据：Gadgetbridge `XiaomiWatchfaceService` + `XiaomiDataUploadService` +
`XiaomiFWHelper.parseAsWatchface`（逐行核实）。

**文件格式（vela .bin，全部小端）**：

| 偏移 | 内容 |
| --- | --- |
| 0x00 | magic `5A A5` |
| 0x20 | previewOffset (u32)，0 = 无预览图 |
| 0x28 | id（ASCII 纯数字，NUL 结尾；Gadgetbridge 校验 `^\d+$`） |
| 0x68 | name（NUL 结尾）或 `FF FF FF FF` = 本地化 |
| 0x74 | tableOffset (u32)，仅本地化时有效 |
| 0x78 | tableSize (u32)，仅本地化时有效 |

i18n 表：`[locale 位图 u64:8][置位 locale 的 size:4 × N][名称数据拼接]`。
locale 索引与 Gadgetbridge `FACE_LOCALE_BITMAP_LIST` 一致：
0=en_US、1=zh_CN、2=zh_TW、3=ja_JP……30=zh_HK。
选择顺序（用户语言 zh_CN）：**zh_CN 精确 > zh_TW/zh_HK > en_US > 第一个可用**
（对应 Gadgetbridge findBestI18n 的「精确 > 语言匹配 > fallback」）。

**安装流程（7 步）**：

```
1. ESP32 解析文件头 + 两遍流式扫描（4KB/片，主循环分步执行）:
   pass1: MD5(全文件)      pass2: CRC32([00][type][md5][size] || 文件数据)
2. Pb  Command{type=4, subtype=4, watchface{installStart{id, size}}}
3. 手表 Command{type=4, subtype=4, watchface{installStatus}}
   installStatus 0=可传；非 0（如 2=已安装）→ 中止
4. Pb  Command{type=22, subtype=0, dataUpload{request{type=16, md5sum, size}}}
5. 手表 Command{type=22, subtype=0, dataUpload{ack{unknown2, resumePosition, chunkSize}}}
   unknown2/resumePosition 必须为 0；chunkSize 缺省 2048（本项目钳制在 64–16384）
6. Mass 通道（channel=2 明文）逐块:
   [totalParts:2 LE][currentPart:2 LE][payload 分片]
   payload = [0x00][type=16][md5:16][size:4][文件数据][crc32:4]   共 22 + N + 4 字节
   partSize = chunkSize - 4；currentPart 从 1 开始；totalParts = ceil(payloadLen/partSize)
7. 末块发完 → Pb Command{type=4, subtype=1, watchface{watchfaceId}} 激活
```

- 与 Gadgetbridge 一致：**不做 per-chunk 重传**（V2 SPP 的 ACK 仅日志，
  GB 亦为 fire-and-forget），可靠性由 SPP/L2CAP + 尾部 md5/crc32 校验保证
- 上传 payload 的 CRC32 覆盖前缀（含 md5），所以必须两遍扫描；
  1MB 文件两遍约 0.2–0.5s（ESP32@240MHz），按 4KB 分片不会卡住网关
- 命令行协议帧走 `sendCommand`（认证后 AES-CTR 加密），数据块走
  `sendDataChunk`（channel 2 明文）——两者都是完整 V2 帧
  `[A5A5][03][seq][len][crc16]` + `[channel][opcode]` + payload

**存储与 Web**：

- 表盘上传先写入暂存区：16MB 布局为独立 4MB `watchface` 分区，
  4MB 兼容布局为非活动 OTA 槽（首个 4KB 先擦除）
- 上传走 WebServer multipart（`HTTP_UPLOAD_BUFLEN=1436` 分块回调）；
  上传期间 `handleClient()` 会阻塞主循环，故每 8 个 chunk 主动驱动一次
  蓝牙/协议/网关栈，避免长时间上传把网关业务饿死
- 仅认证后允许上传/安装/激活/卸载；断连自动取消安装
- OTA 使用 `Update` 写入另一应用槽；`Update.end()` 后再做 `esp_image_verify`，
  失败时恢复原启动分区

## 10. 测试

```bash
cd tests && make        # 全部 33,781 项检查，无需 ESP32 工具链
```

| 测试文件 | 检查项 | 覆盖 |
| --- | --- | --- |
| `test_protocol.cpp` | 134 | 协议 / KDF / CRC32 / MD5 / 表盘命令 / 校验和 |
| `test_netgw.cpp` | 106 | 网络栈全流程 |
| `test_btlink.cpp` | 32,962 | 蓝牙链路 / 环形缓冲 / 扫描 |
| `test_miwear.cpp` | 37 | 帧格式 / 握手 / ACK（XW_HOST_TEST 编译状态机） |
| `test_watchface.cpp` | 179 | 表盘解析 / 完整安装流程 / 失败路径 |
| `test_ota.cpp` | 6 | OTA 应用镜像头校验 |
| `test_serialpush.cpp` | 121 | 串口推包协议 / Base64 / 缓存 TTL |
| `test_led.cpp` | 164 | LED 状态闪烁模式（含 millis 回绕） |
| `test_perf.cpp` | 72 | 性能纯逻辑（百分比/速率/健康度/环形历史） |

覆盖范围：CRC-16/CRC-32/SHA-256/MD5/HMAC/KDF（标准向量 + 流式一致性）、protobuf 编解码、
DHCP、IP/TCP/UDP 校验和、TCP 全流程（握手/窗口/MSS/重传/半关闭）、ICMP 分片重组、
DNS 代答、MiWear 帧格式与握手推进、表盘文件解析（含 i18n 本地化表）、
表盘安装全流程（分块重组后与 Python zlib/hashlib 参考值逐字节比对）、
OTA 应用镜像头校验、蓝牙环形缓冲回绕与丢弃、扫描去重、MAC 解析、
LED 状态闪烁模式（含 millis 回绕）、protobuf 长度溢出防护、认证确认判定、
串口 BAUD 溢出防护、超长行丢弃等。

编译已验证（ESP32 Arduino 3.20017.0 / ESP-IDF 4.4 / PlatformIO 6）：
Flash 占用 45.1%（1.81MB / 4MB OTA 槽），RAM 27.8%。

## 11. 故障排查

| 现象 | 排查 |
| --- | --- |
| 扫描不到手表 | 手表需亮屏且在蓝牙设置页附近；确认手表已取消旧配对 |
| 一直「认证中」后失败 | 配对密钥错误或未填；确认手表端没有残留绑定 |
| 手表显示已连接但无法上网 | 看状态页「网关包」计数是否为 0；检查 WiFi 是否连通 |
| WiFi 连不上 | 20 秒后自动进 AP 配网模式，重新配置 |
| 日志出现 `watch hmac mismatch` | auth key 不对，重新提取 |
| OTA 页上传被拒绝 | 确认上传的是 `-ota.bin`（如 `watch5-net-v1.5.0-ota.bin`），不是 `-full-16mb.bin` 合并镜像或表盘文件 |
| OTA 后反复重启 | 新固件未通过 180 秒健康检查会自动回滚；也可在升级页手动回滚 |
| OTA 页显示无可用回滚 | 另一分区被表盘临时文件覆盖（仅 4MB 兼容布局），或首次线刷尚无旧版本 |
| LED 不亮 | 「配置」页的 GPIO 引脚填错（常见是 GPIO2），或点亮电平选反了；用「LED 测试」按钮确认 |
| 性能页温度显示 – | WiFi 工作时 ESP32 内部温度传感器返回假值，属已知限制，其余指标不受影响 |
| 板子偶发重启/掉线，事后无从查证 | 用 `tools/serial_logger.py` 挂后台自动落盘（插线即记、断线重连），日志文件带毫秒时间戳 |

## 12. 提取配对密钥（auth key）

红米 Watch 5 与小米运动健康首次绑定时会生成 16 字节密钥，用于 SPP 通道的
挑战应答。提取方式（需要 root 或 App 备份）：

1. root 手机或 `adb backup` 导出 `com.xiaomi.hm.health` 数据
2. 在其 SharedPreferences / 数据库中查找 32 位 hex 字符串的 auth key
3. 填入 Web「配置」页

具体提取工具与步骤请参考 Gadgetbridge 社区 Wiki（本项目不提供绕过设备
绑定机制的方案，密钥需用户自行从自己的账号/设备获取）。

## 13. 已知限制

- 仅支持 IPv4；HTTPS 流量的 TCP 中继透明转发（不解密）
- 手表侧 DNS 固定走 223.5.5.5（阿里公共 DNS）代答
- 并发 TCP 连接 6 个、UDP 流 8 个（够手表日常使用，见 `netgw.h` 常量）
- 蓝牙与 WiFi 共用 2.4G 射频，大流量时互相有性能影响
- 表盘预览图不在 Web 页渲染（LVGL RLE 解码未实现，仅解析 previewOffset）
- 表盘安装包**不做长期存储**：安装成功即自动删除缓存；失败时保留直到重装、手动删除或 48 小时超时
- WiFi 工作时 ESP32 内部温度传感器返回固定假值（≈53℃），性能页温度标记为不可用
- 串口推包需 PC 端有 `pyserial`（`pip install pyserial`）；仅推包不需联网
- 不提供 ArduinoOTA / espota 网络烧录，只支持网页 OTA、USB 线刷与串口推表盘
- 未实现手表固件升级（OTA 通道）；Mass 通道目前只用于表盘安装
- 本固件按 16MB Flash 构建；4MB 板无法容纳双 OTA + 独立表盘分区

## 14. 固件发布包

`firmware/` 目录存放编译产出的可直接烧录固件，文件名格式为
`watch5-net-<版本>-<用途>.bin`。

### 文件说明

| 文件 | 大小 | 用途 | 烧录方式 |
| --- | --- | --- | --- |
| `watch5-net-v1.5.0-ota.bin` | 1,892,560 B (1.81MB) | 应用 OTA 镜像 | Web「固件升级」页上传，或 esptool 写 `0x10000` |
| `watch5-net-v1.5.0-full-16mb.bin` | 16,777,216 B (16MB) | 整片合并镜像 | 仅 USB：`esptool write_flash 0x0`（首次烧录 / 救砖 / 安卓 OTG 线刷） |

> 两者**不能混用**：合并镜像包含 bootloader 与分区表，上传到 OTA 页会被拒绝；
> OTA 镜像用于已烧录过双 OTA 分区表的设备。
>
> **整片镜像按 Flash 容量区分**：16MB 板用 `-full-16mb.bin`；
> 8MB 板需把下述命令的 `--flash_size 16MB` 改为 `8MB`、`--fill-flash-size` 改为 `8MB`
> 并使用 `partitions-8mb.csv` 重新生成。

### 当前版本

- 版本号 **1.5.0**（定义于 `platformio.ini` 的 `XW_FW_VERSION`）
- 目标板：ESP32（经典蓝牙 + WiFi），**16MB Flash**
- 分区表：`partitions.csv`（双 OTA 槽 4MB + **独立表盘分区 4MB** + nvs + otadata + coredump）
- 资源占用：Flash 45.1%（应用区 1.81MB / 4MB 槽），RAM 27.8%
- 新增：**Web 管理页 UI 美化**：状态结论条、统一状态色、内联 SVG 图标、深色模式、
  移动端表格卡片化、配置表单就地校验、统一错误结果页、表盘加载骨架屏、
  打印样式；悬浮占用条可拖动贴靠左右边缘并记忆位置
- 修复：配置页 LED 测试表单误嵌套在保存表单内、性能页温度单位拼接错误、
  缓存表盘重装失败时错误后仍追加 302 跳转、性能图表不可用数据仍画末端点

### 烧录命令

```bash
# 首次 / 换机 / 分区表变更后：整片烧录（会清除 WiFi 配置和蓝牙绑定）
esptool.py --chip esp32 --port /dev/ttyUSB0 --baud 921600 \
  write_flash -z 0x0 watch5-net-v1.5.0-full-16mb.bin

# 后续增量升级：只写应用分区
esptool.py --chip esp32 --port /dev/ttyUSB0 --baud 921600 \
  write_flash -z 0x10000 watch5-net-v1.5.0-ota.bin
```

更推荐的方式：整片烧录成功后，后续升级直接在 Web「固件升级」页上传 `-ota.bin`，
不用再插数据线。

### 校验

```bash
sha256sum -c SHA256SUMS.txt
```

### 自行编译

```bash
pio run                    # 产出 .pio/build/esp32dev/firmware.bin，等同 -ota.bin
pio run -t erase           # 分区表变更前必须先全擦
pio run -t upload          # 编译并 USB 烧录
```

生成整片合并镜像：

```bash
python3 ~/.platformio/packages/tool-esptoolpy/esptool.py --chip esp32 merge_bin \
  -o watch5-net-v1.5.0-full-16mb.bin \
  --flash_mode dio --flash_freq 40m --flash_size 16MB --fill-flash-size 16MB \
  0x1000  .pio/build/esp32dev/bootloader.bin \
  0x8000  .pio/build/esp32dev/partitions.bin \
  0xe000  ~/.platformio/packages/framework-arduinoespressif32/tools/partitions/boot_app0.bin \
  0x10000 .pio/build/esp32dev/firmware.bin
```

然后更新 `firmware/SHA256SUMS.txt`。

## 15. 目录结构

```
esp32-redmi-watch5-net/
├── platformio.ini          # PlatformIO 工程配置
├── partitions.csv          # 16MB 双 OTA + 独立表盘分区（当前默认）
├── partitions-8mb.csv      # 8MB 板分区表（2×2.5MB OTA + 2MB 独立表盘区）
├── partitions-16mb.csv     # 16MB 板分区表（= partitions.csv，保留副本）
├── sdkconfig.defaults      # ESP-IDF 配置（经典蓝牙/SPP）
├── README.md               # 项目入口（简介 + 快速指引）
├── DOCUMENTATION.md        # 完整文档（本文件）
├── LICENSE
├── firmware/               # 已编译的可烧录固件
│   ├── watch5-net-v1.5.0-full-16mb.bin   # USB 整片烧录（首次）
│   ├── watch5-net-v1.5.0-ota.bin        # Web OTA 升级
│   └── SHA256SUMS.txt      # 校验值
├── .gitignore              # 忽略 .pio 构建产物与测试可执行文件
├── tools/
│   ├── push_watchface.py   # 串口推包脚本（把表盘 .bin 推到板子上）
│   └── serial_logger.py    # 串口日志自动接收器（插线即记，存到电脑）
├── src/
│   └── main.cpp            # 入口：对象接线、WiFi/NTP/时钟推送、上游 socket
├── include/
│   ├── appconfig.h         # 配置结构（NVS 持久化）
│   ├── btlink.h/.cpp       # 蓝牙 SPP 链路管理（连接/重连/扫描/配对）
│   ├── led.h/.cpp          # 板载 LED GPIO 驱动
│   ├── ledlogic.h/.cpp     # LED 状态闪烁模式（纯逻辑，可主机端单测）
│   ├── perfdata.h/.cpp     # 性能监控纯逻辑（百分比/速率/健康度/环形历史）
│   ├── perfmon.h/.cpp      # 性能采样（空闲钩子/堆/Flash/吞吐/延迟/温度）
│   ├── miwear.h/.cpp       # MiWear SPP V2 协议（拆帧/认证/加密）
│   ├── proto.h/.cpp        # protobuf 编解码（Command/Auth/Weather/Clock）
│   ├── crc16/crc32/md5/sha256  # 协议原语（表盘上传校验用 CRC-32/MD5）
│   ├── iputil/dhcp/netgw   # 手表侧用户态网络栈（IP/TCP/UDP/ICMP/DHCP）
│   ├── weather.h/.cpp      # 天气服务（Open-Meteo）
│   ├── watchface.h/.cpp    # 表盘文件解析 + 安装状态机（Mass 通道上传）
│   ├── staging.h/.cpp      # 表盘暂存区（独立分区优先，兼容非活动 OTA 槽）
│   ├── facecache.h/.cpp    # 安装包缓存元数据 + 48h TTL 判定
│   ├── serialpush.h/.cpp   # 串口推包接收器（UART0 行协议）
│   ├── serialpush_proto.h/.cpp  # 推包协议解析 + Base64（纯逻辑，可单测）
│   ├── ota.h/.cpp          # Web OTA、镜像校验、启动确认、回滚
│   ├── ota_image.h         # 应用镜像头纯函数校验
│   └── webui.h/.cpp        # Web 管理界面 + 配网 AP + 表盘/固件/性能页
└── tests/                  # 主机端单元测试（无需 ESP32，make 运行）
    ├── test_protocol.cpp   #   协议/KDF/CRC32/MD5/表盘命令/校验和（134 项）
    ├── test_netgw.cpp      #   网络栈全流程（106 项）
    ├── test_btlink.cpp     #   蓝牙链路/环形缓冲/扫描（32,962 项）
    ├── test_miwear.cpp     #   帧格式/握手/ACK（37 项，XW_HOST_TEST 编译状态机）
    ├── test_watchface.cpp  #   表盘解析/完整安装流程/失败路径（179 项）
    ├── test_ota.cpp        #   OTA 应用镜像头校验（6 项）
    ├── test_serialpush.cpp #   串口推包协议/Base64/缓存 TTL（121 项）
    ├── test_led.cpp        #   LED 闪烁模式（164 项）
    ├── test_perf.cpp       #   性能纯逻辑（72 项）
    └── *.h                 #   主机桩（Arduino/BluetoothSerial/FreeRTOS）
```

## 16. 致谢与参考

- [Gadgetbridge](https://github.com/Freeyourgadget/Gadgetbridge)（AGPL-3.0）——
  MiWear 协议、天气/时钟命令序列的参考实现
- [AstroBox-Public](https://github.com/hnczlab/AstroBox-Public)（AGPL-3.0）——
  网络栈（ipstack）与 DHCP 方案的参考实现
- ESP-IDF / arduino-esp32 的 BluetoothSerial 与 lwIP
- Kimi K3.1 Pro（Moonshot AI 模型）—— Web 管理界面美术支持

## 17. 免责声明

本项目为非官方第三方实现，与小米公司无关。「红米 Watch 5」「小米运动健康」
商标归其所有者。使用本项目可能影响手表电池续航；请仅在你自己的设备上使用，
并遵守当地法律法规。
