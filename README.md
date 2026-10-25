# ESP32 红米 Watch 5 网络网关

[![CI](https://github.com/shikongxiaohan/esp32-redmi-watch5-net/actions/workflows/ci.yml/badge.svg)](https://github.com/shikongxiaohan/esp32-redmi-watch5-net/actions/workflows/ci.yml)
[![License](https://img.shields.io/github/license/shikongxiaohan/esp32-redmi-watch5-net.svg)](LICENSE)
[![Release](https://img.shields.io/github/v/release/shikongxiaohan/esp32-redmi-watch5-net.svg)](https://github.com/shikongxiaohan/esp32-redmi-watch5-net/releases/latest)

> **完整文档请见 [DOCUMENTATION.md](DOCUMENTATION.md)**（协议细节、使用手册、固件发布、测试说明等全部内容）。
> 当前版本 **v1.5.0** · ESP32 / Arduino · 16MB Flash

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
| 自定义表盘 | 上传 .bin 表盘文件 → 写入独立表盘分区 → 校验 MD5/CRC32 → 蓝牙分块安装 → 激活/卸载；安装成功即自动删除缓存，失败保留可重装，48 小时未用自动清理 |
| 表盘串口推送 | 用 USB 数据线把 .bin 直接推到板子上（`XWFP` 行协议），无需 PC 端安装软件 |
| 固件 OTA | Web 上传 `firmware.bin`，A/B 双分区切换、启动确认、自动与手动回滚 |
| 性能监控 | 独立「性能监控」页：CPU / 内存 / 存储 / 网络上下行 / 蓝牙吞吐 / 网络延迟 / 芯片温度，Canvas 折线图（仿任务管理器）；全站右下角常驻**悬浮占用条**，可拖动贴边并记忆位置 |
| Web 管理 | 状态 / 配置 / 蓝牙配对 / 表盘管理 / 性能监控 / 固件升级，自动保存到 NVS |
| 配网 | 无 WiFi 或连接失败时自动开 AP `RedmiWatch5Net-XXXX`（192.168.4.1） |
| 自动恢复出厂 | **可选，默认关闭**。连续 N 分钟既无 WiFi 也无手表连接时清空配置并重启进配网 |
| 板载 LED 指示 | GPIO 引脚与点亮电平可在网页配置，并提供点亮/熄灭测试按钮 |
| Web UI | 状态结论条、统一状态色、内联 SVG 图标、深色模式、移动端表格卡片化、表单就地校验、统一错误页、加载骨架屏、打印样式，无外部字体/脚本/样式依赖 |

## 快速开始

1. **烧录**：用 `firmware/watch5-net-v1.5.0-full-16mb.bin` 整片烧录（16MB 板，首次必须）

   ```bash
   esptool.py --chip esp32 --port /dev/ttyUSB0 --baud 921600 \
     write_flash -z 0x0 firmware/watch5-net-v1.5.0-full-16mb.bin
   ```

2. **配网**：连接热点 `RedmiWatch5Net-XXXX` → 浏览器打开 <http://192.168.4.1> → 「配置」页填 WiFi
3. **配对**：Web 页 →「配对」→ 扫描约 8 秒 → 选择 **Redmi Watch 5** → 在手表上确认
4. **密钥**：从小米运动健康 App 提取 16 字节 hex auth key 填入「配置」页，保存重启
5. **完成**：「状态」页显示 *运行正常 · 已联网 · 手表已认证*，手表即可上网

后续升级无需数据线：Web「固件升级」页上传 `firmware/watch5-net-v1.5.0-ota.bin`。

> 手表需先恢复出厂或取消旧手机配对；密钥提取需 root 或 App 备份，详见完整文档。

> **连上 WiFi 后从哪个 IP 进管理页**：板子以 STA 模式工作，IP 由路由器 DHCP
> 动态分配，没有固定地址（也没有 mDNS 主机名）。在路由器后台「已连接设备 /
> DHCP 客户端列表」中找设备名 `esp32` 或按 MAC 认出板子，浏览器打开
> `http://该IP`（80 端口）即可；「状态」页会显示当前 IP。想固定访问，就在
> 路由器里给板子 MAC 绑定静态 IP。串口日志（115200）不打印 IP。

## 硬件要求

- **开发板**：任何带**经典蓝牙** + WiFi 的原版 ESP32（WROOM-32 / WROVER / DevKitC）；
  ESP32-S2/S3/C3 没有经典蓝牙，**不适用**
- **Flash**：本版按 **16MB** 构建（2×4MB OTA 槽 + 4MB 独立表盘分区）；
  8MB 板需换 `partitions-8mb.csv`，4MB 板无法同时容纳双 OTA 与独立表盘区
- **手表**：红米 Watch 5 蓝牙版（非 eSIM 版）

## 固件下载

| 文件 | 大小 | 用途 |
| --- | --- | --- |
| [`firmware/watch5-net-v1.5.0-full-16mb.bin`](firmware/watch5-net-v1.5.0-full-16mb.bin) | 16MB | USB 整片烧录（首次 / 救砖） |
| [`firmware/watch5-net-v1.5.0-ota.bin`](firmware/watch5-net-v1.5.0-ota.bin) | 1.81MB | Web「固件升级」页上传 |

校验值见 [`firmware/SHA256SUMS.txt`](firmware/SHA256SUMS.txt)。

## 自行编译

```bash
pio run -t erase    # 首次 / 分区表变更后必须全擦
pio run -t upload   # 编译并烧录
pio device monitor  # 查看日志（115200）
```

主机端测试（无需 ESP32 工具链，**33,781 项检查**）：

```bash
cd tests && make
```

## 文档导航

完整内容（含 MiWear 协议细节、表盘安装流程、Web UI 设计、故障排查等）见
[**DOCUMENTATION.md**](DOCUMENTATION.md)：

- [快速开始](DOCUMENTATION.md#4-快速开始)（烧录 / 配网 / 配对 / 密钥 / 表盘 / OTA / LED）
- [性能监控](DOCUMENTATION.md#5-性能监控)
- [Web 管理界面与路由](DOCUMENTATION.md#6-web-管理界面)
- [MiWear SPP V2 协议与实现细节](DOCUMENTATION.md#9-miwear-spp-v2-协议与实现细节)
- [测试](DOCUMENTATION.md#10-测试) · [故障排查](DOCUMENTATION.md#11-故障排查)
- [固件发布包](DOCUMENTATION.md#14-固件发布包)

## 致谢

- [Gadgetbridge](https://github.com/Freeyourgadget/Gadgetbridge)（AGPL-3.0）—— MiWear 协议、天气/时钟命令序列参考实现
- [AstroBox-Public](https://github.com/hnczlab/AstroBox-Public)（AGPL-3.0）—— 网络栈（ipstack）与 DHCP 方案参考实现

## 免责声明

本项目为非官方第三方实现，与小米公司无关。「红米 Watch 5」「小米运动健康」
商标归其所有者。使用本项目可能影响手表电池续航；请仅在你自己的设备上使用，
并遵守当地法律法规。
