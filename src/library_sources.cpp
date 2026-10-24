/*
 * PlatformIO 默认只编译 src/ 目录下的源文件。
 * 本项目的实现文件放在 include/ 下（与 tests/ 主机端单测共用同一份源码），
 * 这里统一汇总，保证 ESP32 构建链接到全部实现。
 *
 * 注意：本文件仅参与 ESP32（PlatformIO）构建；主机端 make 测试直接
 * 在 Makefile 中列举 ../include/*.cpp，不会包含本文件。
 */
#include "../include/crc16.cpp"
#include "../include/crc32.cpp"
#include "../include/md5.cpp"
#include "../include/sha256.cpp"
#include "../include/proto.cpp"
#include "../include/iputil.cpp"
#include "../include/dhcp.cpp"
#include "../include/miwear.cpp"
#include "../include/netgw.cpp"
#include "../include/btlink.cpp"
#include "../include/weather.cpp"
#include "../include/webui.cpp"
#include "../include/watchface.cpp"
#include "../include/staging.cpp"
#include "../include/ota.cpp"
#include "../include/facecache.cpp"
#include "../include/serialpush_proto.cpp"
#include "../include/serialpush.cpp"
#include "../include/ledlogic.cpp"
#include "../include/led.cpp"
#include "../include/perfdata.cpp"
#include "../include/perfmon.cpp"
