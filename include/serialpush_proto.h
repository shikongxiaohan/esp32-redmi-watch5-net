/*
 * 表盘串口推送协议（纯逻辑层，可在主机端单测）
 *
 * 用途：PC 通过 USB 数据线（CH340 → UART0）把表盘安装包直接推到板子上，
 *       板子再通过蓝牙把表盘装进手表。整条链路的"控制"都在板子上，
 *       PC 端只是一个搬运工。
 *
 * 线路复用：UART0 同时承担固件日志输出（板子→PC）与推送命令（PC→板子），
 *           因此所有命令行必须带固定前缀 "XWFP "，日志行一律忽略。
 *
 * 命令格式（ASCII，一行一条，行尾 \n）：
 *   XWFP BEGIN <size> <name>   开始接收一个表盘包（size 为十进制字节数）
 *   XWFP DATA <base64>         追加一段 base64 编码的数据
 *   XWFP END                   接收结束，校验并开始安装
 *   XWFP ABORT                 放弃本次接收
 *   XWFP STATUS                查询接收状态
 *   XWFP BAUD <n>              建议切换串口波特率（PC 端先切换再发后续命令）
 *
 * 状态行（板子→PC，前缀 "XWFS "）：
 *   XWFS READY <size>          已就绪，等待数据
 *   XWFS PROGRESS <bytes>      已接收字节数
 *   XWFS OK <bytes>            接收完成并开始安装
 *   XWFS ERR <message>         出错
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace xw {

enum class PushCmd {
    NONE,      // 不是命令行
    BEGIN,
    DATA,
    END,
    ABORT,
    STATUS,
    BAUD,
    UNKNOWN,   // 带前缀但无法识别
};

struct PushLine {
    PushCmd cmd = PushCmd::NONE;
    uint32_t num = 0;      // BEGIN: 包大小；BAUD: 波特率
    std::string text;      // BEGIN: 文件名；DATA: base64 数据
    std::string raw;       // 原始行（不含行尾）
};

class PushParser {
public:
    // 命令行前缀
    static const char* prefix() { return "XWFP "; }

    // 判断一行是否为推送命令（是则必须以 prefix 开头）
    static bool isCommand(const std::string& line);

    // 解析一行命令
    static PushLine parse(const std::string& line);

    // 单行允许的最大长度（防止刷屏/内存爆）
    static const size_t MAX_LINE = 8192;
};

// Base64 解码：忽略空白与 '=' 填充；遇到非法字符返回 false。
// out 容量不足时返回 false。成功返回写入字节数（0 表示空输入）。
bool base64Decode(const std::string& in, uint8_t* out, size_t outCap, size_t& outLen);

// Base64 编码（供板子侧调试/测试使用）
std::string base64Encode(const uint8_t* in, size_t len);

} // namespace xw
