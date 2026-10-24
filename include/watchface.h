/*
 * 自定义表盘安装（对标米坛社区「表盘自定义工具箱」tech.pingx.watchface）
 *
 * 依据（见 docs/protocol-notes.md 第 10 节）：
 *  - Gadgetbridge XiaomiWatchfaceService / XiaomiDataUploadService（AGPL-3.0）
 *  - Gadgetbridge XiaomiFWHelper.parseAsWatchface（vela .bin 文件头解析）
 *
 * 文件格式（vela .bin，全部偏移小端）:
 *   0x00  magic 5A A5
 *   0x20  previewOffset (u32)
 *   0x28  id（ASCII 数字，NUL 结尾）
 *   0x68  name（NUL 结尾）或 FFFFFFFF（本地化）
 *   0x74  tableOffset (u32)   ┐ 仅 name == FFFFFFFF 时有效
 *   0x78  tableSize   (u32)   ┘
 *   i18n 表: [locale 位图:8][每 locale size:4]... + 各 locale 名称数据
 *
 * 安装流程（7 步，与 Gadgetbridge 一致）:
 *   1. 解析文件头 + MD5/CRC32 全文件扫描（分片进行，不阻塞主循环）
 *   2. Pb 通道 Command{type=4, subtype=4, watchface{installStart{id, size}}}
 *   3. 手表回 installStatus（0=可传，2=已装）
 *   4. Pb 通道 Command{type=22, subtype=0, dataUpload{request{type=16, md5, size}}}
 *   5. 手表回 DataUploadAck{unknown2=0, resumePosition=0, chunkSize(缺省2048)}
 *   6. Mass 通道（channel 2 明文）分块传
 *      [totalParts:2 LE][currentPart:2 LE][payload 分片]
 *      payload = [0x00][type=16][md5:16][size:4][文件数据][crc32:4]
 *      （前缀 22 字节；partSize = chunkSize - 4）
 *   7. 全部发完后 Pb 通道 Command{type=4, subtype=1, watchface{watchfaceId}} 激活
 */
#pragma once

#include "proto.h"
#include "md5.h"
#include "crc32.h"

#include <Arduino.h>

#include <cstdint>
#include <cstddef>
#include <functional>
#include <string>
#include <vector>

namespace xw {

// 表盘文件源抽象（ESP32: OTA 临时分区；主机测试: 内存桩）
class WatchfaceSource {
public:
    virtual ~WatchfaceSource() = default;
    virtual uint32_t size() const = 0;
    // 读 [offset, offset+len)，返回实际读到的字节数（0 表示失败/EOF）
    virtual size_t readAt(uint32_t offset, uint8_t* buf, size_t len) = 0;
};

// 文件头解析结果
struct WatchfaceInfo {
    bool valid = false;
    std::string id;            // 纯数字字符串
    std::string name;          // 本地化表盘名（zh_CN 优先）
    uint32_t size = 0;         // 文件总大小
    uint32_t previewOffset = 0; // 0 = 无预览图
};

// 解析 vela 表盘文件头（只读头部 256 字节 + i18n 表）
WatchfaceInfo parseWatchfaceHeader(WatchfaceSource& src);

// ===================== 安装状态机 =====================
enum class WfState {
    IDLE,           // 空闲
    SCANNING,       // 解析文件头 + MD5/CRC32 扫描（分片进行）
    INSTALL_START,  // 已发 installStart，等待手表 installStatus
    UPLOAD_REQUEST, // 已发 dataUploadRequest，等待 DataUploadAck
    UPLOADING,      // Mass 通道分块上传中
    DONE,           // 完成
    FAILED,         // 失败（见 error()）
};

class WatchfaceInstaller {
public:
    using SendCmdFn = std::function<bool(const uint8_t*, size_t)>;   // -> MiWear::sendCommand
    using SendChunkFn = std::function<bool(const uint8_t*, size_t)>; // -> MiWear::sendDataChunk
    using ProgressFn = std::function<void(int percent, const char* stage)>;
    using DoneFn = std::function<void(bool success, const std::string& error)>;
    using LogFn = std::function<void(const char*)>;

    void begin(SendCmdFn sendCmd, SendChunkFn sendChunk);

    // 单文件大小上限（默认 4MB；Web 上传时设为临时分区容量）
    void setMaxBytes(uint32_t maxBytes) { _maxBytes = maxBytes; }

    // 开始安装（src 生命周期须覆盖整个安装过程；返回 false 表示已在进行中）
    bool start(WatchfaceSource& src);

    // 取消当前安装（进行中才有效）
    void cancel();

    // 主循环驱动：扫描推进、分块发送（每次一块）、超时检查
    void loop();

    // 分发手表命令（main 的 onCommand 中对 type=4 / type=22 调用）
    void onCommand(const MiCommand& cmd);

    WfState state() const { return _state; }
    bool busy() const {
        return _state != WfState::IDLE && _state != WfState::DONE && _state != WfState::FAILED;
    }
    int progress() const { return _progress; }
    const std::string& error() const { return _error; }
    const WatchfaceInfo& info() const { return _info; }
    uint32_t chunkSize() const { return _chunkSize; }
    uint32_t totalParts() const { return _totalParts; }

    ProgressFn onProgress;
    DoneFn onDone;
    LogFn onLog;

private:
    SendCmdFn _sendCmd;
    SendChunkFn _sendChunk;
    WatchfaceSource* _src = nullptr;
    uint32_t _maxBytes = 4u * 1024u * 1024u;

    WfState _state = WfState::IDLE;
    int _progress = 0;
    std::string _error;
    WatchfaceInfo _info;

    // 扫描阶段
    Md5 _md5;
    Crc32 _crc;
    uint8_t _md5Digest[16] = {0};
    uint32_t _crc32v = 0;
    uint32_t _scanPos = 0; // 已扫描的文件偏移
    int _scanPhase = 0;    // 0=md5, 1=crc32
    static const uint32_t SCAN_SLICE = 4096;
    uint8_t _scanBuf[SCAN_SLICE];
    uint8_t _prefix[22] = {0}; // [0x00][type][md5][size] 上传流前缀

    // 上传阶段
    uint32_t _chunkSize = 2048;  // 手表上报（缺省 2048）
    uint32_t _partSize = 2044;   // chunkSize - 4
    uint32_t _totalParts = 0;
    uint32_t _currentPart = 0;   // 0 基；已发块数
    uint32_t _payloadLen = 0;    // 22 + fileSize + 4
    std::vector<uint8_t> _chunkBuf;
    uint8_t _crcBytes[4] = {0};  // 上传流尾部 crc32（小端）
    uint32_t _waitStartMs = 0;   // 等待手表响应的起始时刻（elapsed 判超时）

    void _log(const char* msg);
    void _fail(const std::string& why);
    void _finish(bool success);
    void _scanStep();
    void _sendNextChunk();
    size_t _readPayload(uint32_t offset, uint8_t* buf, size_t len);
    static uint32_t _clampChunkSize(uint32_t v) {
        if (v < 64) return 64;
        if (v > 16384) return 16384;
        return v;
    }
};

} // namespace xw
