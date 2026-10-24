# ESP32 红米 Watch 5 网络网关

运行在 ESP32 开发板上的独立固件，**替代小米运动健康（Mi Fitness）App**，为
**红米 Watch 5 蓝牙版（非 eSIM，无 WiFi）**提供完整的互联网接入能力。

```
红米 Watch 5 ──蓝牙 SPP──> ESP32 ──WiFi──> 互联网
 (仅蓝牙)      MiWear V2   协议中继+网络栈   DHCP/DNS/HTTP...
```

手表自身没有 WiFi 和蜂窝网络，平时靠手机的「小米运动健康」App 做蓝牙代理上网。
本项目用一块 ESP32 开发板（约 30 元）接管这个角色：手表连上 ESP32 后即可
DHCP 获取 IP、DNS 解析、TCP/UDP 联网，并额外获得**天气推送**与**网络对时**。

## 功能

| 功能 | 说明 |
| --- | --- |
| 网络接入 | DHCP / ICMP(ping) / TCP 中继 / UDP 中继 / DNS 代答 |
| 协议认证 | MiWear SPP V2 完整握手（PhoneNonce/WatchNonce/AuthStep3，AES-128-CTR 加密通道） |
| 天气推送 | Open-Meteo 免费接口（无需 Key），支持手表主动请求 + 周期刷新 |
| 网络对时 | NTP 校准后推送 `System.Clock`，手表显示准确时间与时区 |
| 自定义表盘 | 上传 .bin 表盘文件（米坛社区等来源）→ 临时写入非活动 OTA 区 → 校验 MD5/CRC32 → 蓝牙分块安装 → 激活/卸载；不在设备内长期保存 |
| 固件 OTA | Web 上传 `firmware.bin`，A/B 双分区切换、启动确认、自动与手动回滚 |
| Web 管理 | 状态页 / 配置页 / 蓝牙配对页 / 表盘管理页 / 固件升级页，自动保存到 NVS |
| 配网 | 无 WiFi 或连接失败时自动开 AP `RedmiWatch5Net-XXXX`（192.168.4.1） |
| 自动恢复出厂 | **可选，默认关闭**。连续 N 分钟既无 WiFi 也无手表连接时清空配置并重启进配网；网页可设开关与超时 |

## 硬件与软件要求

- **开发板**：任何带经典蓝牙 + WiFi 的 ESP32（ESP32-WROOM-32 / WROVER / DevKitC 等）
  - 需要 **4MB Flash**；本工程使用双 OTA 分区，低于 4MB 无法同时保留升级能力
  - 注意：ESP32-S2/S3/C3 没有经典蓝牙，**不适用**；必须用原版 ESP32
- **手表**：红米 Watch 5 蓝牙版（非 eSIM 版）
- **工具链**：PlatformIO（VS Code 插件或 `pio` CLI），或 ESP-IDF + Arduino 组件

## 快速开始

### 1. 烧录固件

**方式 A：直接用发布固件（`firmware/` 目录，无需编译）**

| 文件 | 用途 |
| --- | --- |
| `firmware/watch5-net-v1.1.2-full-4mb.bin` | USB 整片烧录，**首次必须用这个** |
| `firmware/watch5-net-v1.1.2-ota.bin` | Web「固件升级」页上传，后续免插线升级 |

```bash
# USB 整片烧录（首次 / 救砖）
esptool.py --chip esp32 --port /dev/ttyUSB0 --baud 921600 \
  write_flash -z 0x0 firmware/watch5-net-v1.1.2-full-4mb.bin
```

文件名规则：`watch5-net-<版本>-<用途>.bin`，详细说明与校验值见
[`firmware/使用说明.md`](firmware/使用说明.md)（含 `SHA256SUMS.txt`）。

**方式 B：自己编译**

```bash
cd esp32-redmi-watch5-net
pio run -t erase           # 首次从旧分区表升级必须全擦
pio run -t upload          # 编译并烧录
pio device monitor         # 查看日志（115200）
```

编译产物：`.pio/build/esp32dev/firmware.bin`（约 1.83MB），等价发布的 `-ota.bin`；
后续可在 Web「固件升级」页直接上传，不需要 USB。生成 4MB 合并镜像的命令见
[`firmware/使用说明.md`](firmware/使用说明.md)。

> 本版改用 4MB 双 OTA 分区：两个 1.9375MB 应用槽 + 64KB coredump，不再使用
> LittleFS。分区表变化后必须 `pio run -t erase`，否则 NVS/旧文件系统残留会导致
> 配置异常或启动分区选择错误。合并镜像 `-full-4mb.bin` 只能用于 USB
> 整片烧录，不能上传到 OTA 页面。

### 2. 配网

首次启动未配置 WiFi，设备自动开启配网热点：

- 连接 WiFi **`RedmiWatch5Net-XXXX`**（XXXX 为板子 MAC 后四位，默认开放无密码）
- 浏览器打开 **http://192.168.4.1** → 「配置」页填写家中/服务器 WiFi → 保存并重启

> 已配置 WiFi 但连不上（改密码、路由器重启、信号太弱）时，设备会在 20 秒后进入
> 同样的 AP 配网模式，**并且每 60 秒重试一次已保存的 WiFi**；一旦连上就自动关闭
> 热点、回到正常 STA 工作，无需手动重启。AP 最短保留 90 秒，避免刚进配网就被关掉。

### 3. 配对手表

> 前提：手表已恢复出厂设置或已在系统蓝牙设置里取消与旧手机的配对
> （长按手表表盘进入设置 → 系统 → 重置，或在旧手机上取消配对）。
> 操作时手表需亮屏并保持在蓝牙设置/配对界面附近。

1. 打开 Web 页 →「配对」
2. 点「开始扫描」（约 8 秒），列表中出现 **Redmi Watch 5** 后点「选择」
3. 设备重启；此时**手表端会弹出配对请求，在手表上确认**
4. 「状态」页蓝牙一栏显示 `已连接` 即 SPP 链路建立

### 4. 配置配对密钥（认证必需）

SPP 连接后还需 MiWear 协议层的认证，密钥从小米运动健康 App 提取
（需 root 或备份，方法见下文）。在「配置」页填写 16 字节 hex 密钥：

```
00112233445566778899aabbccddeeff
```

保存重启后，「状态」页认证一栏显示 **已完成（加密通道就绪）** 即全部就绪。
此时手表端的天气、需要联网的应用即可通过 ESP32 上网。

> 未填密钥时协议握手能走到「会话就绪」，但认证无法通过（手表会拒绝），
> 网络功能不可用——这是小米的设备绑定机制，无法绕过。

### 5. 安装自定义表盘

1. 打开 Web 页 →「表盘」
2. 选择从米坛社区（BandBBS）等渠道下载的 `.bin` 表盘文件 →「上传并安装到手表」
3. 页面显示进度（校验 → 等待手表确认 → 分块上传 → 激活）
4. 安装完成后「手表端表盘」会刷新，新表盘标记为「使用中」；可随时点「启用」切换或「卸载」

> 要求：手表已完成认证；单个文件约 1.9MB 以内。表盘只写入非活动 OTA 分区的
> 临时暂存区，安装后不在设备内保留，也不提供本地表盘列表。下一次固件升级或
> 表盘上传会覆盖该临时区。
> **注意**：该临时区与「上一版本固件的回滚镜像」是同一块空间，安装表盘会清除
> 回滚镜像——此后若新固件启动失败将无法自动/手动回滚，直到下一次固件升级成功。
> 安装流程与米坛「表盘自定义工具箱」App 一致（MiWear type=4/type=22 + Mass 通道）。

### 6. 固件升级（OTA）

首次通过 USB 全擦烧录带双 OTA 分区表的固件后，后续升级无需数据线。全擦会清除
NVS 配置和蓝牙绑定，烧录后需要重新配网并重新配对手表。

1. 打开 Web 页 →「固件升级」
2. 选择 `firmware/watch5-net-v1.1.2-ota.bin`（自编译则是
   `.pio/build/esp32dev/firmware.bin`；**不要**选 `-full-4mb.bin` 合并镜像）
3. 点「上传并升级」，页面会提示正在写入；保持供电和网络，设备完成后自动重启
4. 新固件启动后，**成功连上已配置的 WiFi** 并稳定 60 秒会自动确认；若 180 秒仍
   未连上 WiFi 且另一分区有有效固件，bootloader 会自动回滚。仅处于 AP 配网模式
   不算「联网成功」（设备本来就没有 WiFi 配置的纯 AP 用法除外）
5. 「固件升级」页可手动回滚到另一分区；表盘临时暂存会覆盖旧回滚镜像，
   页面会在回滚不可用时隐藏按钮

> OTA 只接受 ESP32 应用镜像（0xE9 头 + 0x20 处应用描述符），会拒绝合并镜像、
> 表盘文件和截断文件。升级期间会禁止表盘安装、保存配置和重启操作。

### 7. 自动恢复出厂（可选，默认关闭）

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


## 大容量 Flash 分区（换 8MB / 16MB 板子时使用）

仓库内置两套备用分区表：`partitions-8mb.csv`、`partitions-16mb.csv`。
大容量板子到手后切换步骤（以 8MB 为例，16MB 同理）：

1. **改 `platformio.ini`**：`board_build.flash_size = 8MB`，
   `board_build.partitions = partitions-8mb.csv`
2. **改名/替换分区表**：`cp partitions-8mb.csv partitions.csv`
3. **重新整片烧录**：`pio run -t erase`（新板第一次必须全擦），
   再烧整片镜像（merge_bin 时把 `--flash_size 4MB` 改成对应容量），
   或用 Flash Download Tool 选对应容量烧整片镜像

三档布局对比：

| 分区 | 4MB 版（当前） | 8MB 版 | 16MB 版 |
| --- | --- | --- | --- |
| ota_0 / ota_1 | 2 × 1,984KB | **2 × 2,560KB** | **2 × 4MB** |
| 单个 OTA 槽剩余（固件 1.83MB） | 约 190KB ⚠️ | **约 767KB** | **约 2.2MB** |
| 表盘暂存 | 与非活动 OTA 槽共用 | **独立 2MB** | **独立 4MB** |
| 未分配保留 | 0 | 896KB | 3.9MB |
| 回滚镜像被表盘覆盖 | ⚠️ 会（有 UI 警告） | ✅ 不会 | ✅ 不会 |

> 注意：换 16MB 板后首次烧录必须整片（bootloader/分区表全部重写），
> 且旧 4MB 设备的 OTA 升级包不能直接传给 16MB 设备（分区布局不同，需按
> 上表重新出固件）。

## 目录结构

```
esp32-redmi-watch5-net/
├── platformio.ini          # PlatformIO 工程配置
├── partitions.csv          # 4MB 双 OTA 分区（2×1.9375MB + coredump）
├── partitions-8mb.csv      # 8MB 板备用分区表（2×2.5MB OTA + 2MB 独立表盘区）
├── partitions-16mb.csv     # 16MB 板备用分区表（2×4MB OTA + 4MB 独立表盘区）
├── sdkconfig.defaults      # ESP-IDF 配置（经典蓝牙/SPP）
├── firmware/               # 已编译的可烧录固件
│   ├── watch5-net-v1.1.2-full-4mb.bin   # USB 整片烧录（首次）
│   ├── watch5-net-v1.1.2-ota.bin        # Web OTA 升级
│   ├── SHA256SUMS.txt      # 校验值
│   └── 使用说明.md          # 命名规则 / 烧录命令 / 自行编译
├── .gitignore              # 忽略 .pio 构建产物与测试可执行文件
├── src/
│   └── main.cpp            # 入口：对象接线、WiFi/NTP/时钟推送、上游 socket
├── include/
│   ├── appconfig.h         # 配置结构（NVS 持久化）
│   ├── btlink.h/.cpp       # 蓝牙 SPP 链路管理（连接/重连/扫描/配对）
│   ├── miwear.h/.cpp       # MiWear SPP V2 协议（拆帧/认证/加密）
│   ├── proto.h/.cpp        # protobuf 编解码（Command/Auth/Weather/Clock）
│   ├── crc16/crc32/md5/sha256  # 协议原语（表盘上传校验用 CRC-32/MD5）
│   ├── iputil/dhcp/netgw   # 手表侧用户态网络栈（IP/TCP/UDP/ICMP/DHCP）
│   ├── weather.h/.cpp      # 天气服务（Open-Meteo）
│   ├── watchface.h/.cpp    # 表盘文件解析 + 安装状态机（Mass 通道上传）
│   ├── staging.h/.cpp      # 非活动 OTA 分区临时暂存 + 表盘读取源
│   ├── ota.h/.cpp          # Web OTA、镜像校验、启动确认、回滚
│   ├── ota_image.h         # 应用镜像头纯函数校验
│   └── webui.h/.cpp        # Web 管理界面 + 配网 AP + 表盘/固件页
├── tests/                  # 主机端单元测试（无需 ESP32，make 运行）
│   ├── test_protocol.cpp   #   协议/KDF/CRC32/MD5/表盘命令/校验和（134 项）
│   ├── test_netgw.cpp      #   网络栈全流程（106 项）
│   ├── test_btlink.cpp     #   蓝牙链路/环形缓冲/扫描（3.3 万项）
│   ├── test_miwear.cpp     #   帧格式/握手/ACK（37 项，XW_HOST_TEST 编译状态机）
│   ├── test_watchface.cpp  #   表盘解析/完整安装流程/失败路径（179 项）
│   ├── test_ota.cpp        #   OTA 应用镜像头校验（6 项）
│   └── *.h                 #   主机桩（Arduino/BluetoothSerial/FreeRTOS）
└── docs/
    └── protocol-notes.md   # 协议与实现笔记
```

## 工作原理

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

## 测试

```bash
cd tests && make        # 全部 33,423 项检查，无需 ESP32 工具链
```

覆盖：CRC-16/CRC-32/SHA-256/MD5/HMAC/KDF（标准向量 + 流式一致性）、protobuf 编解码、
DHCP、IP/TCP/UDP 校验和、TCP 全流程（握手/窗口/MSS/重传/半关闭）、ICMP 分片重组、
DNS 代答、MiWear 帧格式与握手推进、表盘文件解析（含 i18n 本地化表）、
表盘安装全流程（分块重组后与 Python zlib/hashlib 参考值逐字节比对）、
OTA 应用镜像头校验、蓝牙环形缓冲回绕与丢弃、扫描去重、MAC 解析等。

编译已验证（ESP32 Arduino 3.20017.0 / ESP-IDF 5.1 / PlatformIO 6）：
Flash 占用 90.1%（1.83MB / 1.9375MB OTA 槽），RAM 26.5%（约 87KB / 320KB）。

## 故障排查

| 现象 | 排查 |
| --- | --- |
| 扫描不到手表 | 手表需亮屏且在蓝牙设置页附近；确认手表已取消旧配对 |
| 一直「认证中」后失败 | 配对密钥错误或未填；确认手表端没有残留绑定 |
| 手表显示已连接但无法上网 | 看状态页「网关包」计数是否为 0；检查 WiFi 是否连通 |
| WiFi 连不上 | 20 秒后自动进 AP 配网模式，重新配置 |
| 日志出现 `watch hmac mismatch` | auth key 不对，重新提取 |
| OTA 页上传被拒绝 | 确认上传的是 `-ota.bin`（如 `watch5-net-v1.1.2-ota.bin`），不是 `-full-4mb.bin` 合并镜像或表盘文件 |
| OTA 后反复重启 | 新固件未通过 180 秒健康检查会自动回滚；也可在升级页手动回滚 |
| OTA 页显示无可用回滚 | 另一分区被表盘临时文件覆盖，或首次线刷尚无旧版本 |

## 提取配对密钥（auth key）

红米 Watch 5 与小米运动健康首次绑定时会生成 16 字节密钥，用于 SPP 通道的
挑战应答。提取方式（需要 root 或 App 备份）：

1. root 手机或 `adb backup` 导出 `com.xiaomi.hm.health` 数据
2. 在其 SharedPreferences / 数据库中查找 32 位 hex 字符串的 auth key
3. 填入 Web「配置」页

具体提取工具与步骤请参考 Gadgetbridge 社区 Wiki（本项目不提供绕过设备
绑定机制的方案，密钥需用户自行从自己的账号/设备获取）。

## 已知限制

- 仅支持 IPv4；手表侧 DNS 固定走 223.5.5.5（阿里公共 DNS）代答
- 并发 TCP 连接 6 个、UDP 流 8 个（够手表日常使用，见 `netgw.h` 常量）
- 蓝牙与 WiFi 共用 2.4G 射频，大流量时互相有性能影响
- 表盘预览图不在 Web 页渲染（LVGL RLE 解码未实现，仅解析 previewOffset）
- 表盘不做长期存储：上传时占用非活动 OTA 分区作为临时区，安装后不保留文件；
  这会覆盖上一版本固件的回滚镜像
- 不提供 ArduinoOTA / espota 网络烧录，只支持网页 OTA 和 USB 线刷
- 未实现手表固件升级（OTA 通道）；Mass 通道目前只用于表盘安装
- OTA 应用槽约 1.9375MB，当前固件已占 90%，后续功能增长空间有限

## 致谢与参考

- [Gadgetbridge](https://github.com/Freeyourgadget/Gadgetbridge)（AGPL-3.0）——
  MiWear 协议、天气/时钟命令序列的参考实现
- [AstroBox-Public](https://github.com/hnczlab/AstroBox-Public)（AGPL-3.0）——
  网络栈（ipstack）与 DHCP 方案的参考实现
- ESP-IDF / arduino-esp32 的 BluetoothSerial 与 lwIP

## 免责声明

本项目为非官方第三方实现，与小米公司无关。「红米 Watch 5」「小米运动健康」
商标归其所有者。使用本项目可能影响手表电池续航；请仅在你自己的设备上使用，
并遵守当地法律法规。
