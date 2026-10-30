/*
 * 板载 LED 状态指示（纯逻辑层，可在主机端单测）
 *
 * 职责划分：
 *   - 本文件（ledlogic.h）：状态 -> 闪烁模式的纯函数判定，无硬件依赖
 *   - include/led.h/.cpp：ESP32 侧 GPIO 驱动，调用本判定结果
 *
 * 设计要点：
 *   - 状态优先级：配网 AP > 表盘安装 > 固件升级 > 蓝牙配对 > 故障 > 正常运行
 *     高优先级状态会覆盖低优先级（表盘安装时比"已联网"更重要）。
 *   - 闪烁模式用「周期内亮多久」表示，避免各状态各写一套定时逻辑：
 *       0 = 常灭，-1 = 常亮，>0 = 周期(ms)内亮 ms
 *   - 毫秒时间差全部用无符号减法，天然抗 millis() 49.7 天回绕。
 */
#pragma once

#include <cstdint>

namespace xw {

// LED 指示状态
enum class LedState {
    Off,          // 关闭（用户禁用或无 LED）
    Booting,      // 启动中：快速闪烁
    ApMode,       // 配网 AP 模式：慢速双闪
    Connecting,   // 正在连接 WiFi：单闪
    Online,       // 已联网且手表已连接：常亮（呼吸太耗资源，用常亮）
    OnlineNoWatch,// 已联网但手表未连接：慢闪
    Pairing,      // 蓝牙配对中：快速闪烁
    WatchfaceBusy,// 表盘安装中：连续快闪（短亮长灭）
    FirmwareBusy, // 固件升级中：慢速快闪
    Error         // 故障：长亮
};

// 闪烁模式
struct LedPattern {
    int32_t periodMs;  // >0：周期；0：常灭；-1：常亮
    uint16_t onMs;     // 周期内亮起的时长
};

// 各状态的闪烁模式
LedPattern ledPatternOf(LedState s);

// 状态的中文名称（供 Web 状态页显示）
const char* ledStateName(LedState s);

// 计算某状态下、给定时刻的 LED 输出（true=亮）
// nowMs 为"相对该状态开始时刻"的偏移，而不是 millis() 绝对值。
// 否则所有状态共用上电时刻的相位：进状态那一刻灯可能是灭的，
// 用户看不到任何变化，于是所有闪烁看起来一模一样。
bool ledShouldBeOn(LedState state, uint32_t nowMs);

/*
 * 把"状态保持"折算成状态内的相位偏移。
 *
 * 背景：若直接用 millis() % period 判定，闪烁相位由设备上电时刻决定，
 * 与用户何时触发该状态无关。例如上电 500ms 后进入 OnlineNoWatch
 * (周期 2000ms)，相位 500 落在"灭"区间，灯要再等 1.5 秒才亮——
 * 用户看到的是"没反应"。
 *
 * 做法：相位 = min(该状态已持续时长, period)，即每次进入状态都从
 * "由亮开始"重新计时，第一个周期完整走完，之后按周期回绕。
 * 用 min 而不是取模，保证状态持续很久时相位仍在 [0, period) 内。
 */
uint32_t ledPhaseFor(uint32_t stateHeldMs, const LedPattern& p);

// 状态优先级：数值越大优先级越高，用于合并多个并发事件
int ledPriority(LedState s);

// 按优先级合并两个状态（a、b 都可能是 0 表示"无事件"）
LedState ledMerge(LedState a, LedState b);

/*
 * 故障提示是否仍在有效期内（纯函数，可主机端单测）
 *
 * main 里用它实现「表盘安装失败 -> LED 常亮 3 秒 -> 自动恢复」。
 * 用无符号差值判断，millis() 回绕时依然正确：
 *   faultUntilMs == 0        -> 无故障提示
 *   now < faultUntilMs       -> 还在提示期内
 *   否则                     -> 提示已结束
 */
bool ledFaultActive(uint32_t faultUntilMs, uint32_t nowMs);

/*
 * 某个「截止时刻」是否仍在有效期内（纯函数，可主机端单测）
 *
 * 语义：deadline == 0 -> 无有效期；now < deadline -> 仍在有效期内。
 * 无符号差值写法，millis() 回绕时依然正确。
 */
bool ledDeadlineActive(uint32_t deadline, uint32_t nowMs);

} // namespace xw
