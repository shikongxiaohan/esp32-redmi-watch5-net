/*
 * 表盘安装主机单元测试
 *   - vela .bin 文件头解析（id/name/本地化表）
 *   - 完整安装流程: 扫描 -> installStart -> installStatus -> dataUploadRequest
 *     -> dataUploadAck -> Mass 通道分块 -> set 激活
 *   - 分块内容与 Python(zlib/hashlib) 参考值逐字节比对
 *   - 失败路径: 坏文件 / installStatus=2 / ack 异常 / 超时 / 取消
 *
 * 编译运行: cd tests && make
 */
#include "watchface.h"
#include "proto.h"

#include <Arduino.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace xw;

// ---- 测试可控制的假时钟 ----
static uint32_t g_fakeNow = 1000;
uint32_t xw_test_millis() { return g_fakeNow; }

// ---- 内存文件源 ----
class MemSource : public WatchfaceSource {
public:
    std::vector<uint8_t> data;
    uint32_t size() const override { return uint32_t(data.size()); }
    size_t readAt(uint32_t offset, uint8_t* buf, size_t len) override {
        if (offset >= data.size()) return 0;
        size_t k = std::min(len, data.size() - offset);
        std::memcpy(buf, data.data() + offset, k);
        return k;
    }
};

// ---- 合成 vela 表盘文件（1024 字节）----
// 参考值（Python zlib/hashlib 计算，与实现输出交叉验证）:
//   md5 = 0d21ee366db59de98abd397c9aa7ba28
//   payload = 00 10 + md5 + size(1024) + file + crc32(155bc08e) 共 1046 字节
//   chunkSize=64 -> partSize=60 -> 18 块
static void fillWatchfaceFile(MemSource& src) {
    src.data.assign(1024, 0);
    for (size_t i = 0; i < src.data.size(); i++) src.data[i] = uint8_t((i * 7 + 13) & 0xFF);
    src.data[0] = 0x5A;
    src.data[1] = 0xA5;
    auto putU32 = [&](size_t off, uint32_t v) {
        src.data[off] = uint8_t(v);
        src.data[off + 1] = uint8_t(v >> 8);
        src.data[off + 2] = uint8_t(v >> 16);
        src.data[off + 3] = uint8_t(v >> 24);
    };
    putU32(0x20, 0x100); // previewOffset
    const char* id = "1001";
    std::memcpy(&src.data[0x28], id, 5); // "1001\0"
    const char* name = "\xe7\xbb\x8f\xe5\x85\xb8\xe8\xa1\xa8\xe7\x9b\x98"; // 经典表盘
    std::memcpy(&src.data[0x68], name, 13); // 12 字节 + NUL
}

static int g_failures = 0;
static int g_checks = 0;

static void check(bool cond, const char* what) {
    g_checks++;
    if (!cond) {
        g_failures++;
        std::printf("  FAIL: %s\n", what);
    }
}

// ---- 构造手表 -> 手机的命令（供 installer.onCommand）----
static std::vector<uint8_t> installStatusCmd(uint32_t status) {
    std::vector<uint8_t> wf;
    pbPutVarintField(wf, 5, status);
    std::vector<uint8_t> out;
    pbPutVarintField(out, 1, 4);
    pbPutVarintField(out, 2, 4);
    pbPutMessageField(out, 6, wf);
    return out;
}

static std::vector<uint8_t> uploadAckCmd(uint32_t unknown2, uint32_t resume,
                                         bool hasChunk, uint32_t chunk) {
    std::vector<uint8_t> ack;
    if (unknown2) pbPutVarintField(ack, 2, unknown2);
    if (resume) pbPutVarintField(ack, 4, resume);
    if (hasChunk) pbPutVarintField(ack, 5, chunk);
    std::vector<uint8_t> du;
    pbPutMessageField(du, 2, ack);
    std::vector<uint8_t> out;
    pbPutVarintField(out, 1, 22);
    pbPutVarintField(out, 2, 0);
    pbPutMessageField(out, 24, du);
    return out;
}

// ---- 记录发送的命令/数据块 ----
struct Recorder {
    std::vector<std::vector<uint8_t>> cmds;
    std::vector<std::vector<uint8_t>> chunks;
};

// ===================== 测试 =====================

static void testHeaderParse() {
    std::printf("[watchface: header]\n");
    MemSource src;
    fillWatchfaceFile(src);
    WatchfaceInfo info = parseWatchfaceHeader(src);
    check(info.valid, "valid watchface file");
    check(info.id == "1001", "id = 1001");
    check(info.name == "\xe7\xbb\x8f\xe5\x85\xb8\xe8\xa1\xa8\xe7\x9b\x98", "name = 经典表盘");
    check(info.size == 1024, "size = 1024");
    check(info.previewOffset == 0x100, "previewOffset = 0x100");

    // 坏 magic
    MemSource bad;
    fillWatchfaceFile(bad);
    bad.data[1] = 0x5B;
    check(!parseWatchfaceHeader(bad).valid, "bad magic rejected");

    // 非数字 id
    MemSource badId;
    fillWatchfaceFile(badId);
    badId.data[0x28] = 'a';
    check(!parseWatchfaceHeader(badId).valid, "non-numeric id rejected");

    // 过小文件
    MemSource tiny;
    tiny.data.assign(64, 0);
    tiny.data[0] = 0x5A; tiny.data[1] = 0xA5;
    check(!parseWatchfaceHeader(tiny).valid, "tiny file rejected");
}

// 构造带 i18n 表的表盘文件（1024 字节，表位于 0x200）
static void fillLocalizedFile(MemSource& src, uint64_t bitmap,
                              const std::vector<std::string>& names) {
    src.data.assign(1024, 0);
    for (size_t i = 0; i < src.data.size(); i++) src.data[i] = uint8_t((i * 11 + 5) & 0xFF);
    src.data[0] = 0x5A;
    src.data[1] = 0xA5;
    const char* id = "2002";
    std::memcpy(&src.data[0x28], id, 5);
    // name = FFFFFFFF -> 本地化
    src.data[0x68] = src.data[0x69] = src.data[0x6A] = src.data[0x6B] = 0xFF;
    auto putU32 = [&](size_t off, uint32_t v) {
        src.data[off] = uint8_t(v);
        src.data[off + 1] = uint8_t(v >> 8);
        src.data[off + 2] = uint8_t(v >> 16);
        src.data[off + 3] = uint8_t(v >> 24);
    };
    uint32_t tableOff = 0x200;
    // 表: [bitmap:8][sizes:4*n][data...]
    std::vector<uint8_t> table;
    for (int i = 0; i < 8; i++) table.push_back(uint8_t(bitmap >> (i * 8)));
    for (const auto& n : names) {
        uint32_t v = uint32_t(n.size());
        table.push_back(uint8_t(v));
        table.push_back(uint8_t(v >> 8));
        table.push_back(uint8_t(v >> 16));
        table.push_back(uint8_t(v >> 24));
    }
    for (const auto& n : names) table.insert(table.end(), n.begin(), n.end());
    putU32(0x74, tableOff);
    putU32(0x78, uint32_t(table.size()));
    std::memcpy(&src.data[tableOff], table.data(), table.size());
}

static void testLocalizedName() {
    std::printf("[watchface: i18n]\n");
    // zh_CN + en_US -> 选 zh_CN
    MemSource a;
    fillLocalizedFile(a, (1ull << 0) | (1ull << 1), {"Classic", "\xe7\xbb\x8f\xe5\x85\xb8"});
    WatchfaceInfo ia = parseWatchfaceHeader(a);
    check(ia.valid && ia.name == "\xe7\xbb\x8f\xe5\x85\xb8", "i18n picks zh_CN");

    // 仅 en_US + zh_TW -> 选 zh_TW（语言匹配优先于 fallback）
    MemSource b;
    fillLocalizedFile(b, (1ull << 0) | (1ull << 2), {"Classic", "\xe5\x82\xb3\xe7\xb5\xa1"});
    WatchfaceInfo ib = parseWatchfaceHeader(b);
    check(ib.valid && ib.name == "\xe5\x82\xb3\xe7\xb5\xa1", "i18n picks zh_TW over en_US");

    // 仅 de_DE -> 退化到第一个可用
    MemSource c;
    fillLocalizedFile(c, 1ull << 6, {"Klassisch"});
    WatchfaceInfo ic = parseWatchfaceHeader(c);
    check(ic.valid && ic.name == "Klassisch", "i18n falls back to first available");
}

// 完整安装流程；chunkSize 由参数指定（0 = ack 不带 chunkSize 字段）
static void runInstallFlow(uint32_t ackChunkSize, bool expectSuccess) {
    MemSource src;
    fillWatchfaceFile(src);
    Recorder rec;

    WatchfaceInstaller inst;
    int lastProgress = -1;
    std::string lastStage;
    bool doneCalled = false;
    bool doneSuccess = false;
    std::string doneError;
    inst.begin(
        [&](const uint8_t* d, size_t n) {
            rec.cmds.push_back(std::vector<uint8_t>(d, d + n));
            return true;
        },
        [&](const uint8_t* d, size_t n) {
            rec.chunks.push_back(std::vector<uint8_t>(d, d + n));
            return true;
        });
    inst.onProgress = [&](int pct, const char* stage) {
        lastProgress = pct;
        lastStage = stage ? stage : "";
    };
    inst.onDone = [&](bool ok, const std::string& err) {
        doneCalled = true;
        doneSuccess = ok;
        doneError = err;
    };

    check(inst.start(src), "installer start ok");
    check(inst.state() == WfState::SCANNING, "state SCANNING");

    // 扫描阶段：每次 loop 扫一片（4KB），1024 字节文件 -> md5 一片 + crc 一片
    for (int i = 0; i < 8 && inst.state() == WfState::SCANNING; i++) inst.loop();
    check(inst.state() == WfState::INSTALL_START, "state INSTALL_START after scan");
    check(rec.cmds.size() == 1, "installStart cmd sent");
    if (rec.cmds.size() == 1) {
        MiCommand c = parseCommand(rec.cmds[0].data(), rec.cmds[0].size());
        check(c.ok && c.type == 4 && c.subtype == 4, "installStart header");
        PbField start;
        PbReader(c.watchface.data, c.watchface.len).subReader(6, start);
        if (start.wire == 2) {
            std::string id;
            PbReader sr(start.data, start.len);
            check(sr.stringOf(1, id) && id == "1001", "installStart id");
            check(sr.varintOf(2, 0) == 1024, "installStart size");
        }
    }

    // 手表确认可安装
    auto isc = installStatusCmd(0);
    MiCommand iscCmd = parseCommand(isc.data(), isc.size());
    inst.onCommand(iscCmd);
    check(inst.state() == WfState::UPLOAD_REQUEST, "state UPLOAD_REQUEST");
    check(rec.cmds.size() == 2, "dataUploadRequest cmd sent");
    if (rec.cmds.size() == 2) {
        MiCommand c = parseCommand(rec.cmds[1].data(), rec.cmds[1].size());
        check(c.ok && c.type == 22 && c.subtype == 0, "upload request header");
        PbField req;
        PbReader(c.dataUpload.data, c.dataUpload.len).subReader(1, req);
        if (req.wire == 2) {
            std::vector<uint8_t> md5;
            PbReader rr(req.data, req.len);
            check(rr.varintOf(1, 0) == 16, "upload type = 16");
            check(rr.bytesOf(2, md5) && md5.size() == 16, "upload md5 present");
            // Python 参考: md5(file) = 0d21ee366db59de98abd397c9aa7ba28
            static const uint8_t expectMd5[16] = {
                0x0d, 0x21, 0xee, 0x36, 0x6d, 0xb5, 0x9d, 0xe9,
                0x8a, 0xbd, 0x39, 0x7c, 0x9a, 0xa7, 0xba, 0x28};
            check(md5.size() == 16 && std::memcmp(md5.data(), expectMd5, 16) == 0,
                  "upload md5 matches python");
            check(rr.varintOf(3, 0) == 1024, "upload size = 1024");
        }
    }

    // 手表上传应答
    auto ack = uploadAckCmd(0, 0, ackChunkSize != 0, ackChunkSize);
    MiCommand ackCmd = parseCommand(ack.data(), ack.size());
    inst.onCommand(ackCmd);
    check(inst.state() == WfState::UPLOADING, "state UPLOADING");

    uint32_t chunkSize = ackChunkSize ? ackChunkSize : 2048;
    uint32_t partSize = chunkSize - 4;
    uint32_t payloadLen = 22 + 1024 + 4;
    uint32_t totalParts = (payloadLen + partSize - 1) / partSize;

    // 逐块发送
    int guard = 0;
    while (inst.state() == WfState::UPLOADING && guard++ < 100000) inst.loop();

    check(inst.state() == WfState::DONE, "state DONE");
    check(doneCalled && doneSuccess, "onDone(true) called");
    check(inst.progress() == 100, "progress 100");
    check(rec.chunks.size() == totalParts, "chunk count = totalParts");
    check(rec.cmds.size() == 3, "set cmd sent after last chunk");
    if (rec.cmds.size() == 3) {
        MiCommand c = parseCommand(rec.cmds[2].data(), rec.cmds[2].size());
        check(c.ok && c.type == 4 && c.subtype == 1, "set cmd header");
        std::string id;
        PbReader wr(c.watchface.data, c.watchface.len);
        check(wr.stringOf(2, id) && id == "1001", "set watchfaceId");
    }

    // 重组 payload 并与参考值逐字节比对
    if (rec.chunks.size() == totalParts) {
        std::vector<uint8_t> payload;
        payload.reserve(payloadLen);
        for (size_t i = 0; i < rec.chunks.size(); i++) {
            const auto& ch = rec.chunks[i];
            check(ch.size() >= 4, "chunk has header");
            uint16_t tp = uint16_t(ch[0]) | (uint16_t(ch[1]) << 8);
            uint16_t cp = uint16_t(ch[2]) | (uint16_t(ch[3]) << 8);
            check(tp == totalParts, "chunk totalParts");
            check(cp == i + 1, "chunk currentPart 1-based");
            size_t dataLen = ch.size() - 4;
            check(dataLen <= partSize, "chunk data <= partSize");
            if (i + 1 == totalParts) {
                check(dataLen == payloadLen - i * partSize, "last chunk size");
            } else {
                check(dataLen == partSize, "full chunk size");
            }
            payload.insert(payload.end(), ch.begin() + 4, ch.end());
        }
        check(payload.size() == payloadLen, "payload length");
        // 前缀 [00][10][md5][size]
        check(payload[0] == 0x00 && payload[1] == 0x10, "payload prefix magic/type");
        static const uint8_t expectMd5[16] = {
            0x0d, 0x21, 0xee, 0x36, 0x6d, 0xb5, 0x9d, 0xe9,
            0x8a, 0xbd, 0x39, 0x7c, 0x9a, 0xa7, 0xba, 0x28};
        check(std::memcmp(payload.data() + 2, expectMd5, 16) == 0, "payload md5");
        uint32_t sz = uint32_t(payload[18]) | (uint32_t(payload[19]) << 8) |
                      (uint32_t(payload[20]) << 16) | (uint32_t(payload[21]) << 24);
        check(sz == 1024, "payload size field");
        // 文件数据逐字节
        check(std::memcmp(payload.data() + 22, src.data.data(), 1024) == 0,
              "payload file data verbatim");
        // crc32 尾部（Python zlib: 155bc08e -> LE 8e c0 5b 15）
        static const uint8_t expectCrc[4] = {0x8e, 0xc0, 0x5b, 0x15};
        check(std::memcmp(payload.data() + 22 + 1024, expectCrc, 4) == 0,
              "payload crc32 tail");
    }

    check(lastProgress >= 0, "progress callback fired");
    (void)lastStage;
}

static void testInstallFlowSmallChunks() {
    std::printf("[watchface: install flow, chunkSize=64]\n");
    runInstallFlow(64, true);
}

static void testInstallFlowDefaultChunks() {
    std::printf("[watchface: install flow, chunkSize absent]\n");
    runInstallFlow(0, true);
}

static void testFailurePaths() {
    std::printf("[watchface: failures]\n");

    // 1) 坏文件直接失败
    {
        MemSource src;
        fillWatchfaceFile(src);
        src.data[0] = 0x00;
        WatchfaceInstaller inst;
        bool done = false;
        inst.onDone = [&](bool ok, const std::string&) { done = ok; };
        check(!inst.start(src), "start rejected for bad file");
        check(inst.state() == WfState::FAILED && !done, "bad file -> FAILED");
    }

    // 2) installStatus=2（已安装）-> 失败
    {
        MemSource src;
        fillWatchfaceFile(src);
        WatchfaceInstaller inst;
        inst.begin([](const uint8_t*, size_t) { return true; },
                   [](const uint8_t*, size_t) { return true; });
        check(inst.start(src), "start ok");
        for (int i = 0; i < 8 && inst.state() == WfState::SCANNING; i++) inst.loop();
        auto isc = installStatusCmd(2);
        MiCommand c = parseCommand(isc.data(), isc.size());
        inst.onCommand(c);
        check(inst.state() == WfState::FAILED, "installStatus=2 -> FAILED");
    }

    // 3) ack unknown2 != 0 -> 失败
    {
        MemSource src;
        fillWatchfaceFile(src);
        WatchfaceInstaller inst;
        inst.begin([](const uint8_t*, size_t) { return true; },
                   [](const uint8_t*, size_t) { return true; });
        check(inst.start(src), "start ok");
        for (int i = 0; i < 8 && inst.state() == WfState::SCANNING; i++) inst.loop();
        auto isc = installStatusCmd(0);
        inst.onCommand(parseCommand(isc.data(), isc.size()));
        auto ack = uploadAckCmd(1, 0, true, 2048);
        inst.onCommand(parseCommand(ack.data(), ack.size()));
        check(inst.state() == WfState::FAILED, "ack unknown2!=0 -> FAILED");
    }

    // 4) 超时: INSTALL_START 等 16s -> 失败
    {
        MemSource src;
        fillWatchfaceFile(src);
        WatchfaceInstaller inst;
        inst.begin([](const uint8_t*, size_t) { return true; },
                   [](const uint8_t*, size_t) { return true; });
        check(inst.start(src), "start ok");
        for (int i = 0; i < 8 && inst.state() == WfState::SCANNING; i++) inst.loop();
        check(inst.state() == WfState::INSTALL_START, "INSTALL_START");
        g_fakeNow += 16000;
        inst.loop();
        check(inst.state() == WfState::FAILED, "install timeout -> FAILED");
        g_fakeNow -= 16000;
    }

    // 5) 取消
    {
        MemSource src;
        fillWatchfaceFile(src);
        WatchfaceInstaller inst;
        inst.begin([](const uint8_t*, size_t) { return true; },
                   [](const uint8_t*, size_t) { return true; });
        check(inst.start(src), "start ok");
        inst.cancel();
        check(inst.state() == WfState::FAILED, "cancel -> FAILED");
    }

    // 6) 安装中拒绝新的 start
    {
        MemSource src;
        fillWatchfaceFile(src);
        WatchfaceInstaller inst;
        inst.begin([](const uint8_t*, size_t) { return true; },
                   [](const uint8_t*, size_t) { return true; });
        check(inst.start(src), "start ok");
        check(!inst.start(src), "second start refused while busy");
    }

    // 7) 临时暂存容量上限
    {
        MemSource src;
        fillWatchfaceFile(src);
        WatchfaceInstaller inst;
        inst.begin([](const uint8_t*, size_t) { return true; },
                   [](const uint8_t*, size_t) { return true; });
        inst.setMaxBytes(900);
        check(!inst.start(src), "oversize watchface refused");
        check(inst.state() == WfState::FAILED, "oversize -> FAILED");
    }
}

int main() {
    testHeaderParse();
    testLocalizedName();
    testInstallFlowSmallChunks();
    testInstallFlowDefaultChunks();
    testFailurePaths();
    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
