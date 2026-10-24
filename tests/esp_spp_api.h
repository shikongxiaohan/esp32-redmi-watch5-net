/*
 * 主机端 esp_spp_api.h 桩（btlink.cpp 只需要 esp_bd_addr_t；
 * BluetoothSerial.h 桩另需 esp_spp_sec_t / esp_spp_role_t）。
 * 类型定义与 esp-idf master 一致：esp_spp_sec_t 是 uint16_t（宏常量），
 * esp_spp_role_t 是 enum。
 */
#pragma once

#include <cstdint>

typedef uint8_t esp_bd_addr_t[6];

/* Security Setting Mask（esp-idf: ESP_BT_SEC_* 宏） */
#define ESP_SPP_SEC_NONE            0x0000
#define ESP_SPP_SEC_AUTHORIZE       0x0001
#define ESP_SPP_SEC_AUTHENTICATE    0x0002
#define ESP_SPP_SEC_ENCRYPT         0x0004
#define ESP_SPP_SEC_MODE4_LEVEL4    0x0008
#define ESP_SPP_SEC_MITM            0x0010
#define ESP_SPP_SEC_IN_16_DIGITS    0x0020
typedef uint16_t esp_spp_sec_t;

typedef enum {
    ESP_SPP_ROLE_SLAVE = 1,
    ESP_SPP_ROLE_MASTER = 0,
} esp_spp_role_t;
