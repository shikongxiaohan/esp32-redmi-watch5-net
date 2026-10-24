/*
 * 主机端 freertos/FreeRTOS.h 桩。
 */
#pragma once

#include <cstdint>

typedef uint32_t TickType_t;
typedef int BaseType_t;
typedef unsigned int UBaseType_t;
typedef void (*TaskFunction_t)(void*);
typedef void* TaskHandle_t;
#define pdTRUE 1
#define pdFALSE 0
#define pdMS_TO_TICKS(ms) (TickType_t)(ms)
