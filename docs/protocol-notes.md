# MiWear SPP V2 协议与实现笔记

本文档记录「ESP32 红米 Watch 5 网络网关」所实现的协议细节与设计决策，
所有结论均来自对 Gadgetbridge / AstroBox-Public 源码与 arduino-esp32 /
ESP-IDF 官方源码的逐行核实（2026-10，master 分支）。

## 1. SPP V2 帧格式

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

### V1 帧格式（版本协商阶段，易错点）

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

## 2. 连接与认证流程

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

### KDF（miwear-auth）

```
hmac_key = HMAC-SHA256(key = phoneNonce || watchNonce, data = authKey)
okm      = T1 || T2,  T_i = HMAC(hmac_key, T_{i-1} || "miwear-auth" || i)
decKey = okm[0:16]   encKey = okm[16:32]
decNonce = okm[32:36]  encNonce = okm[36:40]
```

AES-128-CTR 的 IV = key 本身（协议规定，每消息独立计数器）。
参考值与独立 Python 实现交叉验证一致。

## 3. 字节序约定（重要）

全套代码使用「**网络字节的小端原生读法**」：

- IP/端口在网络帧里是大端字节流；小端机器上直接读成 `uint32_t`/`uint16_t`
  存放，`memcpy` 往返即网络序
- 例：`10.1.10.2` 的网络字节为 `0A 01 0A 02`，小端读作 `0x020A010A`，
  即 `NETGW_WATCH_IP_BE = 0x020A010A`
- 好处：赋给 `sin_addr.s_addr` / `sin_port` 时内存布局天然正确，无需转换
- DHCP 回复字段（yiaddr 等）需按「LSB-first 网络序」逐字节写
  （`putIp()`，见 dhcp.cpp）；纯大端整数（xid、flags、lease）用 `putBe32`

## 4. 手表网络栈（NetGw）

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

### 为什么不直接用 lwIP NAPT

Arduino 框架预编译 lwIP 未开 `IP_NAPT`；自定义 netif 涉及 tcpip 线程安全。
用户态中继与参考实现行为一致，且可完整单测（见 tests/test_netgw.cpp，
12 个场景 106 项检查）。

### 线程模型

`onIpPacket` 在主循环上下文触发（BT 数据经环形缓冲读出后分发），
只做纯内存操作；上游 socket 均为非阻塞，实际收发在 `loop()` 中完成。

## 5. 蓝牙链路（BtLink）实现要点

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

## 6. 天气与对时命令

### 天气（与 Gadgetbridge XiaomiWeatherService 对齐）

手表请求：`Command{type=10, subtype=3, weather{location{code,name}}}`
应答序列：

1. `Command{type=10, subtype=0, weather{current{...}}}` 当前天气
2. `Command{type=10, subtype=1, weather{forecast{metadata, entries}}}` 每日预报

- 数据源 Open-Meteo forecast API（免费无 Key）
- `locationKey` = `"accu:" + hash % 1000000`（与 Gadgetbridge 一致）
- 天气码映射 WMO → Xiaomi（见 proto.cpp `wmoToXiaomiCondition`）
- 首日用当天实况拼预报项；推送后 `refreshNow` 周期可配

### 对时

`Command{type=2, subtype=3, system{clock{date,time,timezone,isNot24hour}}}`

- `timezone`：**15 分钟为单位**的 sint32（zigzag）。CST-8 = -32
- `isNot24hour`：0=24 小时制，1=12 小时制
- 推送时机：认证成功后立即一次（NTP 未就绪则先推系统时间），
  NTP 首次同步后立即校准，之后每小时重推
- NTP：`ntp.aliyun.com` / `ntp1.aliyun.com` / `pool.ntp.org`，
  时区 `setenv("TZ", tzPosix) + tzset()`
- **偏移计算**：xtensa newlib 的 `struct tm` 无 `tm_gmtoff` 字段
  （需 `__TM_GMTOFF` 宏，默认未定义），改用
  `mktime(localTm) - time(nullptr)` 的差值算时区偏移秒数（可移植）

## 6.5 ESP-IDF 平台适配要点（真机编译核实）

- **AES-CCM**：ESP-IDF 5.x（mbedtls 3.x）移除了 mbedtls 2.x 的一次性
  `mbedtls_ccm_encrypt()`，必须 `mbedtls_ccm_setkey()` +
  `mbedtls_ccm_encrypt_and_tag()` 组合（tag 4 字节附在密文后）
- **poll() 不存在**：ESP-IDF lwIP 只提供 `select()`，socket 可写检查用
  `select(fd+1, nullptr, &wfds, nullptr, &tv)` + `SO_ERROR`
- **ArduinoJson 7**：`DeserializationOption::Filter` 只接受
  `JsonVariantConst`/`JsonDocument&`（v6 的 `const char*` 构造已移除），
  过滤描述需先 `deserializeJson(filterDoc, FILTER)` 再传入
- **configTime/getLocalTime**：由 `Arduino.h` 直接声明，无需额外头文件
- **WebServer/DNSServer**：全局命名空间类（勿在 namespace xw 内前向声明）
- **分区表**：4MB flash，双 OTA 槽各 1.9375MB + coredump 64KB，见 `partitions.csv`
- **无 LittleFS**：表盘上传临时写入非活动 OTA 分区，安装后不保留；
  该槽在 OTA 时会被 `Update` 覆盖，因此表盘暂存会使旧版本回滚镜像失效
- **OTA 校验**：应用镜像首字节 `0xE9`，偏移 `0x20` 为 app descriptor magic
  `0xABCD5432`；写入后再执行 `esp_image_verify`，合并镜像不能用于 OTA

## 7. Web 界面与配网

- 路由：`/`（状态，5s 刷新）、`/config`、`/pair`（扫描/选择 MAC）、
  `/watchface`（表盘临时直装）、`/firmware`（OTA）、`/save`、`/weather`、
  `/reboot`、`/forget`、`/api/status`、`/api/watchface`、`/api/firmware`
- 表盘端点：`POST /watchface/upload`（multipart→非活动 OTA 槽→立即安装）、
  `set`（激活）、`uninstall`（手表端删除）、`refresh`（列表）
- OTA 端点：`POST /update`（multipart→Update 库）、`POST /firmware/rollback`、
  `POST /firmware/keep`（确认当前固件）
- 配置持久化：NVS Preferences 命名空间 `xwnet`
- 配网：无 WiFi 配置或 STA 20s 超时 → AP `RedmiWatch5Net-XXXX`
  （192.168.4.1，DNSServer 劫持，captive portal）
- 配对页：8s 扫描（可配置 3–30s）→ 列表选择 → 保存重启 → 手表端确认

## 8. 常量表

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

## 9. 自定义表盘安装（对标米坛社区表盘工具箱）

实现依据：Gadgetbridge `XiaomiWatchfaceService` + `XiaomiDataUploadService` +
`XiaomiFWHelper.parseAsWatchface`（逐行核实）。

### 文件格式（vela .bin，全部小端）

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

### 安装流程（7 步）

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

### 存储与 Web

- 表盘上传临时写入非活动 OTA 分区（首个 4KB 先擦除），安装后不保留；
  单文件上限为 OTA 槽大小约 1.9MB
- 上传走 WebServer multipart（`HTTP_UPLOAD_BUFLEN=1436` 分块回调）；
  上传期间 `handleClient()` 会阻塞主循环，故每 8 个 chunk 主动驱动一次
  蓝牙/协议/网关栈，避免长时间上传把网关业务饿死
- 表盘安装进度由 `WatchfaceInstaller::progress()` 提供，页面 2 秒自刷新
- 仅认证后允许上传/安装/激活/卸载；断连自动取消安装
- OTA 使用 `Update` 写入另一应用槽；`Update.end()` 后再做 `esp_image_verify`，
  失败时恢复原启动分区。非活动槽同时用于表盘暂存，因此 OTA 与表盘上传互斥

## 10. 已知限制

- 仅 IPv4；HTTPS 流量的 TCP 中继透明转发（不解密）
- ESP32 固件支持 Web OTA；手表固件 OTA 通道未实现，Mass 通道仅用于表盘安装
- OTA 应用槽约 1.9375MB，当前固件占用约 90%，后续功能空间有限
- 表盘不做长期存储；临时暂存会覆盖旧版本固件回滚镜像
- 表盘预览图不渲染（LVGL RLE 解码未实现，仅解析 previewOffset）
- 手表端并发连接超限时 `netgw.dropped` 计数会增加
