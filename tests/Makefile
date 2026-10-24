# 主机端单元测试（不需要 ESP32 工具链）
#   make           编译并运行全部测试
#   make verbose   显示详细输出
CXX ?= g++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -g -O1
INC := -I../include -I.

PROTO_SRC := ../include/crc16.cpp ../include/crc32.cpp ../include/md5.cpp ../include/sha256.cpp \
             ../include/proto.cpp ../include/iputil.cpp ../include/dhcp.cpp
# miwear.cpp 含 KDF/AES 纯函数部分（ESP32 专属代码被 #if 排除）
# XW_HOST_TEST: 额外编译协议状态机（帧格式/握手推进，AES 路径除外）
MIWEAR_SRC := ../include/miwear.cpp
MIWEAR_FLAGS := -DXW_HOST_TEST
NETGW_SRC := ../include/netgw.cpp
BTLINK_SRC := ../include/btlink.cpp
WATCHFACE_SRC := ../include/watchface.cpp
SERIALPUSH_SRC := ../include/serialpush_proto.cpp
FACECACHE_SRC := ../include/facecache.cpp
LEDLOGIC_SRC := ../include/ledlogic.cpp

.PHONY: all clean run

all: test_protocol test_netgw test_btlink test_miwear test_watchface test_ota test_serialpush test_led run

test_protocol: test_protocol.cpp $(PROTO_SRC) $(MIWEAR_SRC)
	$(CXX) $(CXXFLAGS) $(INC) -o $@ $^

test_netgw: test_netgw.cpp $(NETGW_SRC) $(PROTO_SRC)
	$(CXX) $(CXXFLAGS) $(INC) -o $@ $^

test_btlink: test_btlink.cpp $(BTLINK_SRC)
	$(CXX) $(CXXFLAGS) $(INC) -o $@ $^

test_miwear: test_miwear.cpp $(MIWEAR_SRC) $(PROTO_SRC)
	$(CXX) $(CXXFLAGS) $(INC) $(MIWEAR_FLAGS) -o $@ $^

test_watchface: test_watchface.cpp $(WATCHFACE_SRC) $(PROTO_SRC)
	$(CXX) $(CXXFLAGS) $(INC) -o $@ $^

test_ota: test_ota.cpp
	$(CXX) $(CXXFLAGS) $(INC) -o $@ $^

test_serialpush: test_serialpush.cpp $(SERIALPUSH_SRC) $(FACECACHE_SRC)
	$(CXX) $(CXXFLAGS) $(INC) -o $@ $^

test_led: test_led.cpp $(LEDLOGIC_SRC) $(SERIALPUSH_SRC) $(FACECACHE_SRC) $(PROTO_SRC)
	$(CXX) $(CXXFLAGS) $(INC) -o $@ $^

run: test_protocol test_netgw test_btlink test_miwear test_watchface test_ota test_serialpush test_led
	@echo "===== test_protocol ====="
	@./test_protocol
	@echo "===== test_netgw ====="
	@./test_netgw
	@echo "===== test_btlink ====="
	@./test_btlink
	@echo "===== test_miwear ====="
	@./test_miwear
	@echo "===== test_watchface ====="
	@./test_watchface
	@echo "===== test_ota ====="
	@./test_ota
	@echo "===== test_serialpush ====="
	@./test_serialpush
	@echo "===== test_led ====="
	@./test_led
	@echo "===== ALL TESTS PASSED ====="

clean:
	rm -f test_protocol test_netgw test_btlink test_miwear test_watchface test_ota test_serialpush test_led
