# 贡献指南

感谢你愿意为红米 Watch 5 蓝牙网关项目出力。动手前请先读完本页，能省彼此大量时间。

参与本项目即表示你同意遵守 [行为准则](CODE_OF_CONDUCT.md)。

## 报告问题 vs 报告漏洞

- **功能缺陷、使用问题**：开 [Issue](../../issues/new/choose)
- **安全漏洞**：**不要**开 Issue，按 [SECURITY.md](SECURITY.md) 的流程私下报告

## 开发环境

| 工具 | 要求 |
|---|---|
| PlatformIO | 6.x（VS Code 插件或 `pio` CLI 均可） |
| Python | PlatformIO 依赖 |
| g++ / make | 仅主机端单测需要 |
| 硬件 | ESP32 开发板（经典蓝牙 + WiFi），16MB Flash 为默认目标 |

## 本地开发

```bash
# 编译
pio run

# 烧录 + 串口监视
pio run -t upload monitor

# 主机端单元测试（无需 ESP32 工具链，纯 g++）
cd tests && make

# 只重新编译测试
cd tests && make clean && make
```

### 8MB 板构建

默认目标为 16MB 布局。8MB 板使用专用环境（自动切换到 `partitions-8mb.csv` + 8MB Flash）：

```bash
pio run -e esp32dev-8mb
pio run -e esp32dev-8mb -t upload monitor
```

> **4MB 板无法同时容纳双 OTA 与独立表盘分区**，不建议使用，详见 [DOCUMENTATION.md 第 7 节](DOCUMENTATION.md#7-flash-容量与分区表)。

提交前请确保：`pio run` 通过、`cd tests && make` 全部 PASS。

## 分支与提交规范

- 基于 `main` 拉功能分支：`feat/xxx`、`fix/xxx`、`docs/xxx`
- 提交信息使用 Conventional Commits 风格：`类型: 描述`

  | 类型 | 用途 |
  |---|---|
  | `feat` | 新功能 |
  | `fix` | 缺陷修复 |
  | `docs` | 仅文档变更 |
  | `test` | 仅测试变更 |
  | `refactor` | 不改变行为的重构 |
  | `chore` | 构建、配置、杂务 |

## Pull Request 要求

- 一个 PR 只做一件事，大改动请先开 Issue 讨论方案
- 说明**动机**（解决什么问题）和**方案**（怎么改的），不要只贴代码
- 附上验证证据：单测输出 / 实机行为截图 / 串口日志
- 涉及功能变更时，同步更新 [DOCUMENTATION.md](DOCUMENTATION.md)
- 涉及固件版本号时，修改 `platformio.ini` 的 `XW_FW_VERSION`
- 注意分区表约束：默认 16MB 布局；8MB 板需同时改 `partitions-8mb.csv` 与 `flash_size`；**4MB 板无法同时容纳双 OTA 与独立表盘分区**，详见 [DOCUMENTATION.md 第 7 节](DOCUMENTATION.md#7-flash-容量与分区表)

## 发布流程（仅维护者）

1. 更新 `platformio.ini` 的 `XW_FW_VERSION` 与文档版本记录
2. 打标签 `vX.Y.Z` 并推送
3. 生成 OTA / 整片固件与 `SHA256SUMS.txt`，创建 GitHub Release 并上传资产

## CI 说明

每次推送 / PR 会自动运行（`.github/workflows/ci.yml`）：主机端单元测试 + 16MB / 8MB 两种分区布局的固件编译。CI 绿灯是合并的基本要求。
