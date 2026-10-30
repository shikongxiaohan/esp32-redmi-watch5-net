# CHANGELOG

本项目遵循 [语义化版本](https://semver.org/lang/zh-CN/)：主版本不兼容变更、次版本新增功能、修订版本修 bug。

---

## v1.6.0 — 2026-10-30

> 这一版**没有新增功能**，全部是让"刷完起不来 / 起来了网页打不开"这条链能走通。
> 从 v1.5.0 升级**必须整片烧录**（`-full-16mb.bin` @ `0x0`）——
> v1.5.0 的固件根本跑不到 OTA 页面。

### 修复（Fixed）

按"崩溃暴露的先后顺序"排列。前一个不修，后一个永远不会暴露 —— 是连环坑。

#### 1. 启动即崩：`assert failed: tcpip_send_msg_wait_sem ... (Invalid mbox)`

**现象**：无限重启，串口无有效日志。

**根因**：`web.begin()` 被放在 `WiFi.mode(WIFI_STA)` **之前**。`WebServer::begin()`
内部要 `tcp_bind()`，而 lwIP 的 tcpip 线程（连带它的 mbox）要等
`esp_netif_init()` / `WiFi.mode()` 才创建。顺序反了就是硬断言，任何板子都中招。

**修复**：WiFi 初始化提前到 `web.begin()` 之前。

#### 2. 蓝牙初始化即崩：`abort()` at `coex_core_enable`

**现象**：修好 1 之后，日志能走到 `[xw] bt: init classic BT (master)`，
紧接着 `abort() was called at PC 0x401daf98 on core 1`。崩在预编译库里，没有错误日志。

**根因**：`WiFi.setSleep(false)`。它等价于 `esp_wifi_set_ps(WIFI_PS_NONE)`，
把 modem sleep 关掉了。而 **WiFi 与经典蓝牙共存时 modem sleep 必须保持开启**
（ESP-IDF 约束，见 espressif/esp-idf#8094 / IDFGH-8094）：先关 sleep 再
`esp_bt_controller_enable()`，就会 abort 在 `coex_core_enable()`。
原代码注释写的是「BT/WiFi 共存时降低延迟」—— **方向写反了**。

**修复**：删除该调用，原位留长注释防回归。默认值 `WIFI_PS_MIN_MODEM` 就是共存所需状态。
**验证**：反汇编新固件，`setSleep` 符号已完全消失（`nm` 无结果）。

#### 3. 网页拿到 IP 却打不开 / 换页就卡死

三层原因叠加，缺一层都还会犯：

**3a. PSRAM 根本没启用。** `platformio.ini` 用 `esp32dev` 板定义，它不带 PSRAM，
板载 WROVER-E 的 8MB 完全闲置，网页内嵌 HTML/JS 只能挤 320KB 内部 SRAM。

```ini
-DBOARD_HAS_PSRAM
-mfix-esp32-psram-cache-issue
-mfix-esp32-psram-cache-strategy=memw
```

**3b. 就算 PSRAM 能用了，页面 `String` 也不会去用。** ESP-IDF 规则是
「单次分配 > `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL`(4096) 才走外部内存」，
而 `String` 从 SSO 小块逐步 `realloc` 增长，永远够不到阈值 —— 被锁死在内部堆里。
实测刷几次页面就把内部堆打穿（heap 从 23KB 掉到 9KB），4MB PSRAM 纹丝不动。

新增 `newPageStr(reserve = 65536)`：页面串一次 reserve 足够大，首次分配直接落 PSRAM；
所有页面 handler 改用它。

> `reserve` 必须一次给足：CSS 14KB + 最大 JS 块 6KB + 动态内容，页面可能超 32KB。
> reserve 不够，中途 `realloc` 扩容会退回内部堆，前面的 PSRAM 分配就白费了。

**3c. 缺少可验证的信号。** `setup()` 里加一条"一锤定音"的探测：

```cpp
heap_caps_malloc_extmem_enable(4096);
void* probe = malloc(32769);
// [xw] malloc32K probe: ok, psramDelta=32768
```

> `psramDelta` 是这版的**指纹**：约 32768 说明大块分配真的落到 PSRAM；
> 为 0 说明 PSRAM 没生效，网页迟早还会打不开；低于 32768 属于碎片化，正常。

#### 4. 纯经典蓝牙场景下内部 RAM 不够用（`BLE release failed 0x103`）

**现象**：`esp_bt_controller_mem_release(ESP_BT_MODE_BLE)` 返回 `0x103`；
后来改成更早调用，又报 `0x102`。

**根因**：IDF v4.4 `bt.c` 的状态机约束 —— 该函数**必须在 controller 处于 IDLE
状态时调用**，非 IDLE 直接返回 `ESP_ERR_INVALID_STATE`。

**修复**：把 BLE 内存回收 **前置到 `esp_bt_controller_init()` 之前**：

```cpp
esp_bt_controller_mem_release(ESP_BT_MODE_BLE);   // IDLE 状态下归还 DRAM
esp_bt_controller_init(&btCfg);                   // 此后不再为 BLE 预留
```

之后 `BluetoothSerial::begin()` 内部的 `btStart()` 检测到 `INITED` 会直接 `enable`，
时序正好衔接。纯 SPP 场景下把 BLE 预留的 DRAM 还给系统堆，缓解网页/DNS 的 ENOMEM。

#### 5. GPIO2 上电电平会干扰 strapping 采样

**现象**：偶发启动异常；换板子后 LED 极性/行为不一致。

**根因**：`Led::begin()` 为了"上电先灭灯"直接 `pinMode(OUTPUT)` 并写高电平。
但 **GPIO2 是 strapping 脚**，上电/复位瞬间的电平参与启动模式判定；
低电平点亮的板子（`3.3V -> 电阻 -> LED -> GPIO2` 灌电流接法）拉高虽能灭灯，
却把 strapping 采样窗口的拉低态改成了拉高态。

**修复**：`begin()` 改为置**高阻输入**（外部上拉电阻按原设计保持熄灭），
等到第一次真正需要点亮时才切推挽输出（`_ensureOutput()`），切换瞬间先写熄灭电平避免闪一下。

#### 6. LED 闪烁看起来"没反应"，且故障灯与正常状态无法区分

**现象**：切换状态后灯要等一两秒才亮；故障时灯常亮，和"已联网正常"一模一样。

**根因**：
- 相位直接用 `millis() % period`，相位由**上电时刻**决定，与何时进入该状态无关。
- `Error` 的灯型是 `{-1, 0}`（常亮），与 `Online` 完全相同。
- 启动快闪是 200ms/100ms 的"闪两下停一下"节奏，**和 bootloader 崩溃重启的节奏一样**，
  排查时极易把正常启动误判成刷机失败。

**修复**：
- 新增 `ledPhaseFor()`：相位按"该状态已持续时长"计算，进状态立刻从"亮"开始；
- `setState()` 切换时重置 `_stateSinceMs`；
- 灯型重设计（"8/2 法"，灭的时间占大头）：启动中 1600/80（单眨）、配网 1000/120、
  故障 2400/100×3（**三连眨 + 停近 2 秒**），与任何普通闪烁和重启循环都能一眼分开；
- 新增 `Led::selftest(700ms)`：阻塞播一遍「亮-灭-亮」，换板子后肉眼确认极性。

#### 7. 表盘安装失败的提示几乎看不见

**根因**：故障灯型是"三连眨"，周期才是语义（一轮 1.8 秒），而代码只给"亮 3 秒"，
刚播完就灭了；且清理条件写错 —— 只要进 `updateLed()` 就把截止时刻清零，等于提示被截断。

**修复**：延长到 6 秒（够看清两轮以上），清理条件改为**只在真正过期时**才清。

#### 8. 烧录文档把 Flash 模式写反了（文档级 bug，但会直接刷砖）

**现象**：按 README 表格用 **QIO** 烧录，板子进 RTCWDT 复位循环。

**根因**：文档写「SPI MODE 选 DIO → WROVER-E 读不到 Flash，无限重启」。实际相反：
固件按 **DIO + 40MHz** 编译，选 QIO 才会挂（`load:0x3fffff30,len:1`）。
镜像头实测 `e9 03 02 40`：byte2 `0x02`=DIO，byte3 `0x40`=16MB | 40MHz。

顺带修正 `platformio.ini` 一条错误注释（原写「0x2f = 40MHz，0x20 = 80MHz」）。
按 esptool `targets/esp32.py` 的 `FLASH_FREQUENCY`：**80m=0xF、40m=0x0**，
所以 16MB+40MHz 是 `0x40`，不是 `0x2f`。

#### 9. 主机端 `test_btlink` 编译不过（CI 红）

**根因**：`btlink.cpp` 的真机分支引用 `<esp_bt.h>`，主机上没有；`SerialStub` 也没有
可变参 `printf`，而 `btlink.cpp` 里到处在用 `Serial.printf`。

**修复**：
- 新增 `tests/esp_bt.h` + `tests/esp_bt.cpp` + `tests/esp_bt_hooks.h` 桩，
  签名对齐 IDF 4.x/5.x（含 `esp_bt_gap_get_bond_device_list` 在 IDF 5 改成
  `uint16_t*` 的差异），主机编译也能发现接口漂移；
- `SerialStub` 补 `printf`；
- `Makefile` 的 `test_btlink` 目标链入桩文件；
- `tests/test_led.cpp` 按新灯型更新断言，并补 `Error` 三连眨的时序用例。

### 变更（Changed）

| 项 | v1.5.0 | v1.6.0 |
| --- | --- | --- |
| 版本号 | 1.5.0 | **1.6.0** |
| 主机测试 | 33,781 项 | **33,813 项**（9 套全通过） |
| OTA 包 | 1,892,560 B (1.81MB) | **1,915,392 B (1.83MB)** |
| Flash 占用 | — | 45.5%（1,908,813 / 4,194,304 B） |
| RAM 占用 | — | 27.9%（91,280 / 327,680 B） |
| 启动日志 | 无版本/内存行 | 新增 `fw=`、`mem boot:`、`malloc32K probe:` |
| 运行日志 | 无 | 每 20 秒一行 `mem: heap/minHeap/psramFree/apClients` |
| 页面日志 | 无 | 每个页面入口打印 `enter /xxx heap=... psramFree=...` |

### 固件校验值

```
d305fc04bf34acbfac188c0721b86667cfd644e2cd4eb84a9fe173250ccea04c  watch5-net-v1.6.0-full-16mb.bin
6b6e7b10b86757ce743842690a47c0c029db2a0516fa0cb419b436a2e4e63d16  watch5-net-v1.6.0-ota.bin
```

| 文件 | 大小 | 烧录方式 |
| --- | --- | --- |
| `watch5-net-v1.6.0-full-16mb.bin` | 16,777,216 B | 整片，地址 `0x0`，**DIO / 40MHz / 16MB** |
| `watch5-net-v1.6.0-ota.bin` | 1,915,392 B | Web「固件升级」页上传 |

### 构建可复现

同一份源码删掉 `.pio/build` 全量重建两次，`firmware.bin` 与合并后的整片镜像
SHA256 **完全一致**。所以上面的校验值可被任何人复核：

```bash
rm -rf .pio/build && pio run -e esp32dev
sha256sum .pio/build/esp32dev/firmware.bin   # 应等于 6b6e7b10…
```

### 已知限制（Unchanged）

- **手表连续上网的长跑没做完整回归**：DHCP / DNS / TCP / UDP 中继与 MiWear SPP V2
  握手已过主机端单测与部分真机联调，但"连续挂机一整天"这类稳定性验证尚未跑完。
- 构建可复现性**绑定当前工具链**（PlatformIO espressif32@6.10.0 / esptool 4.11.0），
  换版本不保证一致。
- 8MB 板需换 `partitions-8mb.csv`；4MB 板无法同时容纳双 OTA 与独立表盘分区。

---

## v1.5.0

首个公开发布版本，功能见 README「功能」表。

> ⚠️ **v1.5.0 的固件在多数板子上无法启动**（见 v1.6.0 的修复 1、2），
> 仓库内已用 v1.6.0 的产物替换。仅作历史版本记录，不建议下载。
