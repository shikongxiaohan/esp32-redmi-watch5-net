/*
 * esp_bt.h 桩的测试钩子（供 test_btlink.cpp 注入 GAP bond 列表）。
 *
 * btlink.cpp 的 bonded 枚举在真机上走 IDF 的 GAP classic 接口，
 * 主机测试走 BluetoothSerial 桩的 getBondedDevices()。为了让 IDF 分支
 * 也能在主机上编译跑通，桩里提供同名钩子：
 *   g_fakeGapBondedCount / g_fakeGapBonded[]  → 被 GAP 桩函数读取
 *   g_fakeBleReleaseCalls / g_fakeBleReleaseResult → 控制内存释放返回值
 */
#pragma once

#include <cstdint>

extern int g_fakeGapBondedCount;
extern uint8_t g_fakeGapBonded[10][6];
extern int g_fakeBleReleaseCalls;
extern int g_fakeBleReleaseResult;
