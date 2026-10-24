/*
 * 主机端 freertos/task.h 桩：
 * 记录任务创建参数，不真正起线程（测试直接驱动 public 接口）。
 */
#pragma once

#include <freertos/FreeRTOS.h>
#include <cstddef>

// 测试钩子
struct FakeTaskInfo {
    bool created = false;
    TaskFunction_t fn = nullptr;
    const char* name = nullptr;
    uint32_t stack = 0;
    UBaseType_t prio = 0;
    BaseType_t core = 0;
};
extern FakeTaskInfo g_fakeTask;

inline BaseType_t xTaskCreatePinnedToCore(TaskFunction_t fn, const char* name,
                                          uint32_t stack, void* arg,
                                          UBaseType_t prio,
                                          TaskHandle_t* handle, BaseType_t core) {
    (void)arg;
    g_fakeTask.created = true;
    g_fakeTask.fn = fn;
    g_fakeTask.name = name;
    g_fakeTask.stack = stack;
    g_fakeTask.prio = prio;
    g_fakeTask.core = core;
    if (handle) *handle = (TaskHandle_t)&g_fakeTask;
    return pdTRUE;
}

inline void vTaskDelay(TickType_t) {}
inline void vTaskDelete(TaskHandle_t) {}
