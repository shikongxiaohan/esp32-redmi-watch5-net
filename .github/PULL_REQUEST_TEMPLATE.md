# Pull Request

## 变更类型

- [ ] 缺陷修复
- [ ] 新功能
- [ ] 重构（无行为变化）
- [ ] 文档
- [ ] 测试
- [ ] 构建 / 配置 / 杂务

## 关联 Issue

Closes #

## 动机与方案

<!-- 解决什么问题（为什么改）、怎么改的（改了什么） -->

## 验证证据

- [ ] `pio run` 编译通过
- [ ] `cd tests && make` 全部 PASS
- [ ] 实机验证（如适用，附串口日志 / 页面截图）

## 检查项

- [ ] 已同步更新 [DOCUMENTATION.md](../blob/main/DOCUMENTATION.md)（如涉及功能 / 分区表 / 接口变更）
- [ ] 已更新 `platformio.ini` 的 `XW_FW_VERSION`（如需要发版）
- [ ] 不涉及安全敏感变更（涉及则先读 [SECURITY.md](../blob/main/SECURITY.md)）
