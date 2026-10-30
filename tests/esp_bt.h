/*
 * 主机端 esp_bt.h 桩（btlink.cpp 只在「经典蓝牙控制器预初始化」路径里用它）。
 *
 * 真实头文件来自 ESP-IDF，主机上没有工具链。这里只提供 btlink.cpp 引用到的
 * 最小符号集，签名与 IDF 4.x / 5.x 保持一致，这样主机编译就能发现签名漂移：
 *   - esp_bt_controller_mem_release(mode)
 *   - esp_bt_controller_config_t + BT_CONTROLLER_INIT_CONFIG_DEFAULT()
 *   - esp_bt_controller_init()
 *   - esp_bt_gap_get_bond_device_num() / esp_bt_gap_get_bond_device_list()
 *
 * 行为由测试钩子控制（见 esp_bt.cpp 与 test_btlink.cpp）。
 */
#pragma once

#include <cstdint>
#include <esp_spp_api.h>

typedef int esp_err_t;
#define ESP_OK              0
#define ESP_FAIL            -1
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_INVALID_STATE 0x103

/* 蓝牙模式位（esp_bt_mode_t） */
#define ESP_BT_MODE_IDLE  0x00
#define ESP_BT_MODE_BLE   0x01
#define ESP_BT_MODE_CLASSIC_BT 0x02
#define ESP_BT_MODE_BTDM  0x03
typedef uint8_t esp_bt_mode_t;

/* 控制器配置：真实结构体很大，这里只保留 btlink.cpp 会读写的字段名 */
typedef struct {
    uint16_t controller_task_stack_size;
    uint8_t  controller_task_prio;
    uint8_t  hci_uart_no;
    uint32_t hci_uart_baudrate;
    uint8_t  controller_run_mode;
    uint8_t  reserved[32];
} esp_bt_controller_config_t;

/* IDT 的默认配置宏展开后是一串初始化列表，主机桩用零初始化即可 */
#define BT_CONTROLLER_INIT_CONFIG_DEFAULT() \
    { 0, 0, 0, 0, 0, { 0 } }

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t esp_bt_controller_mem_release(esp_bt_mode_t mode);
esp_err_t esp_bt_controller_init(esp_bt_controller_config_t* cfg);

int esp_bt_gap_get_bond_device_num(void);
#ifdef ESP_IDF_VERSION_MAJOR
#if ESP_IDF_VERSION_MAJOR >= 5
esp_err_t esp_bt_gap_get_bond_device_list(uint16_t* dev_num, esp_bd_addr_t* dev_list);
#else
esp_err_t esp_bt_gap_get_bond_device_list(int* dev_num, esp_bd_addr_t* dev_list);
#endif
#else
/* 主机测试按 IDF 4.x 原型编译（tests/Makefile 未定义 ESP_IDF_VERSION_MAJOR） */
esp_err_t esp_bt_gap_get_bond_device_list(int* dev_num, esp_bd_addr_t* dev_list);
#endif

#ifdef __cplusplus
}
#endif
