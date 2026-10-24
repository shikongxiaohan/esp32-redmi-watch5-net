/*
 * 串口推包协议 + 表盘缓存 TTL 逻辑 主机单元测试
 *
 * 编译运行: cd tests && make
 */
#include "serialpush_proto.h"
#include "facecache.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace xw;

static int g_checks = 0;
static int g_failed = 0;

#define CHECK(cond) do { \
    g_checks++; \
    if (!(cond)) { \
        g_failed++; \
        std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
    } \
} while (0)

#define CHECK_EQ(a, b) do { \
    g_checks++; \
    if (!((a) == (b))) { \
        g_failed++; \
        std::printf("FAIL %s:%d  %s(%lld) != %s(%lld)\n", __FILE__, __LINE__, \
                    #a, (long long)(a), #b, (long long)(b)); \
    } \
} while (0)

static void testParseBasic() {
    PushLine p = PushParser::parse("XWFP BEGIN 1048576 face.bin");
    CHECK(p.cmd == PushCmd::BEGIN);
    CHECK_EQ(p.num, 1048576u);
    CHECK(p.text == "face.bin");

    p = PushParser::parse("XWFP END");
    CHECK(p.cmd == PushCmd::END);

    p = PushParser::parse("XWFP ABORT");
    CHECK(p.cmd == PushCmd::ABORT);

    p = PushParser::parse("XWFP STATUS");
    CHECK(p.cmd == PushCmd::STATUS);

    p = PushParser::parse("XWFP BAUD 921600");
    CHECK(p.cmd == PushCmd::BAUD);
    CHECK_EQ(p.num, 921600u);
}

static void testParseEdge() {
    // 非命令
    CHECK(!PushParser::isCommand("[xw] hello"));
    CHECK(PushParser::parse("[xw] hello").cmd == PushCmd::NONE);
    CHECK(PushParser::parse("").cmd == PushCmd::NONE);

    // 未知动词
    CHECK(PushParser::parse("XWFP FOO").cmd == PushCmd::UNKNOWN);

    // 非法数字
    CHECK(PushParser::parse("XWFP BEGIN abc x.bin").cmd == PushCmd::UNKNOWN);
    CHECK(PushParser::parse("XWFP BEGIN 12x x.bin").cmd == PushCmd::UNKNOWN);
    CHECK(PushParser::parse("XWFP BAUD abc").cmd == PushCmd::UNKNOWN);

    // 超大数字溢出
    CHECK(PushParser::parse("XWFP BEGIN 99999999999 x.bin").cmd == PushCmd::UNKNOWN);

    // 无文件名（只有大小）
    PushLine p = PushParser::parse("XWFP BEGIN 100");
    CHECK(p.cmd == PushCmd::BEGIN);
    CHECK_EQ(p.num, 100u);
    CHECK(p.text.empty());

    // 行尾 \r 被裁掉
    p = PushParser::parse("XWFP END\r");
    CHECK(p.cmd == PushCmd::END);

    // DATA 保留原始内容
    p = PushParser::parse("XWFP DATA SGVsbG8=");
    CHECK(p.cmd == PushCmd::DATA);
    CHECK(p.text == "SGVsbG8=");
}

static void testBase64() {
    // 已知向量
    struct { const char* in; const char* out; } vec[] = {
        {"", ""},
        {"Zg==", "f"},
        {"Zm8=", "fo"},
        {"Zm9v", "foo"},
        {"Zm9vYg==", "foob"},
        {"Zm9vYmE=", "fooba"},
        {"Zm9vYmFy", "foobar"},
    };
    for (auto& v : vec) {
        std::vector<uint8_t> buf(64);
        size_t len = 0;
        CHECK(base64Decode(v.in, buf.data(), buf.size(), len));
        CHECK_EQ(len, std::strlen(v.out));
        if (len) CHECK(std::memcmp(buf.data(), v.out, len) == 0);
    }

    // 非法字符
    uint8_t buf[16];
    size_t len = 0;
    CHECK(!base64Decode("Zm9v*", buf, sizeof(buf), len));
    CHECK(!base64Decode("Zm9v!Y2Fy", buf, sizeof(buf), len));

    // 容量不足
    CHECK(!base64Decode("Zm9vYmFy", buf, 2, len));

    // 编码往返
    const uint8_t data[] = {0x5A, 0xA5, 0x00, 0xFF, 0x10, 0x20, 0x30, 0x40, 0x50};
    std::string enc = base64Encode(data, sizeof(data));
    std::vector<uint8_t> back(64);
    size_t blen = 0;
    CHECK(base64Decode(enc, back.data(), back.size(), blen));
    CHECK_EQ(blen, sizeof(data));
    CHECK(std::memcmp(back.data(), data, sizeof(data)) == 0);

    // 各种长度往返
    for (size_t n = 0; n <= 17; n++) {
        std::vector<uint8_t> src(n);
        for (size_t i = 0; i < n; i++) src[i] = uint8_t(i * 31 + 7);
        std::string e = base64Encode(src.data(), src.size());
        std::vector<uint8_t> d(n + 8);
        size_t dl = 0;
        CHECK(base64Decode(e, d.data(), d.size(), dl));
        CHECK_EQ(dl, n);
        if (n) CHECK(std::memcmp(d.data(), src.data(), n) == 0);
    }
}

static void testCacheExpiry() {
    FaceCacheMeta m;
    CHECK(!m.present);
    CHECK(!faceCacheExpired(m, 1000, 100));
    CHECK_EQ(faceCacheRemaining(m, 1000, 100), 0u);

    m.present = true;
    m.name = "a.bin";
    m.size = 1024;
    m.uploadedAt = 1000;

    CHECK(!faceCacheExpired(m, 1000, 100));
    CHECK(!faceCacheExpired(m, 1050, 100));
    CHECK(faceCacheExpired(m, 1101, 100));
    // 边界：恰好等于 TTL 即视为过期。
    // 与 faceCacheRemaining 保持一致——该函数在 age >= ttl 时返回 0（已过期），
    // 若此处判定为"未过期"，两者语义会矛盾（界面显示已过期但实际不清理）。
    CHECK(faceCacheExpired(m, 1100, 100));
    CHECK_EQ(faceCacheRemaining(m, 1000, 100), 100u);
    CHECK_EQ(faceCacheRemaining(m, 1050, 100), 50u);
    CHECK_EQ(faceCacheRemaining(m, 1100, 100), 0u);

    // 时钟回拨：不误判过期
    CHECK(!faceCacheExpired(m, 500, 100));
    CHECK_EQ(faceCacheRemaining(m, 500, 100), 100u);

    // clear
    m.clear();
    CHECK(!m.present);
    CHECK(m.name.empty());
    CHECK_EQ(m.size, 0u);
}

static void testCacheText() {
    CHECK(faceCacheRemainingText(0) == "已过期");
    CHECK(faceCacheRemainingText(59 * 60) == "59 分钟");
    CHECK(faceCacheRemainingText(3600) == "1 小时");
    CHECK(faceCacheRemainingText(3600 + 600) == "1 小时 10 分钟");
    CHECK(faceCacheRemainingText(48 * 3600) == "48 小时");
}

int main() {
    testParseBasic();
    testParseEdge();
    testBase64();
    testCacheExpiry();
    testCacheText();

    std::printf("test_serialpush: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
