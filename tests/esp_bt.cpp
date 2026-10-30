/*
 * esp_bt.h 桩实现（主机端）。见 esp_bt.h 头部说明。
 *
 * 只有 btlink.cpp 的「经典蓝牙控制器预初始化」与「GAP bond 枚举」两条
 * 真机分支会调到这里。行为全部由测试钩子驱动：
 *   - g_fakeBleReleaseResult 控制 esp_bt_controller_mem_release 返回值
 *   - g_fakeGapBondedCount / g_fakeGapBonded 提供 bond 列表
 */
#include <esp_bt.h>
#include <esp_bt_hooks.h>

#include <cstring>

int g_fakeGapBondedCount = 0;
uint8_t g_fakeGapBonded[10][6] = {};

/* 记录调用次数，测试可断言「只释放了一次 BLE 内存」 */
int g_fakeBleReleaseCalls = 0;
int g_fakeBleReleaseResult = ESP_OK;

extern "C" {

esp_err_t esp_bt_controller_mem_release(esp_bt_mode_t mode) {
    (void)mode;
    ++g_fakeBleReleaseCalls;
    return g_fakeBleReleaseResult;
}

esp_err_t esp_bt_controller_init(esp_bt_controller_config_t* cfg) {
    (void)cfg;
    return ESP_OK;
}

int esp_bt_gap_get_bond_device_num(void) { return g_fakeGapBondedCount; }

esp_err_t esp_bt_gap_get_bond_device_list(int* dev_num, esp_bd_addr_t* dev_list) {
    if (!dev_num || !dev_list) return ESP_ERR_INVALID_ARG;
    int cap = *dev_num;
    int n = g_fakeGapBondedCount < cap ? g_fakeGapBondedCount : cap;
    for (int i = 0; i < n; i++) std::memcpy(dev_list[i], g_fakeGapBonded[i], 6);
    *dev_num = n;
    return n > 0 ? ESP_OK : ESP_FAIL;
}

} // extern "C"