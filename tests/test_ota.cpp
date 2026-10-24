/*
 * OTA 镜像头校验测试
 */
#include "ota_image.h"

#include <cstdio>
#include <cstring>

using namespace xw;

static int g_failures = 0;
static int g_checks = 0;

static void check(bool cond, const char* what) {
    g_checks++;
    if (!cond) {
        g_failures++;
        std::printf("  FAIL: %s\n", what);
    }
}

int main() {
    std::printf("[ota: image header]\n");

    uint8_t app[0x40] = {0};
    app[0] = 0xE9;
    app[0x20] = 0x32;
    app[0x21] = 0x54;
    app[0x22] = 0xCD;
    app[0x23] = 0xAB;
    check(isValidAppImageHeader(app, sizeof(app)), "accepts ESP32 app image");

    uint8_t merged[0x40];
    std::memset(merged, 0xFF, sizeof(merged));
    check(!isValidAppImageHeader(merged, sizeof(merged)), "rejects merged image padding");

    uint8_t wrongMagic[0x40] = {0};
    wrongMagic[0] = 0xE9;
    check(!isValidAppImageHeader(wrongMagic, sizeof(wrongMagic)), "rejects missing app descriptor");

    uint8_t wrongFirst[0x40] = {0};
    wrongFirst[0] = 0x5A;
    wrongFirst[0x20] = 0x32;
    wrongFirst[0x21] = 0x54;
    wrongFirst[0x22] = 0xCD;
    wrongFirst[0x23] = 0xAB;
    check(!isValidAppImageHeader(wrongFirst, sizeof(wrongFirst)), "rejects watchface magic");

    check(!isValidAppImageHeader(app, 0x23), "rejects truncated header");
    check(!isValidAppImageHeader(nullptr, 0), "rejects null buffer");

    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
