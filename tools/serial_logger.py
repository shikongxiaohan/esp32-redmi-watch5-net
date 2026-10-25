#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
ESP32 网关 · 串口日志自动接收器（PC 侧）

用途：
    开发板通过 USB 数据线连到电脑（数据线同时负责供电）后，板子的串口
    日志（115200）自动记录到电脑的日志文件，无需手动打开串口监视器。
    拔线、复位、掉线后自动重连，恢复即继续记录。

特性：
    - 自动识别板子串口（CH340 / CH341 / CP210x / FT232 / PL2303 等
      USB 转串口桥，覆盖常见 ESP32 开发板）
    - 每行日志带本地时间戳；按"每次连接"分文件，可设单文件大小上限滚动
    - 固件无需任何改动：日志本来就走在供电那根 USB 线的 UART0 上

用法：
    python3 serial_logger.py                      # 自动识别，日志存 ./logs
    python3 serial_logger.py --list               # 只列出识别到的串口
    python3 serial_logger.py --port COM3          # 指定串口（Windows）
    python3 serial_logger.py --port /dev/ttyUSB0  # 指定串口（Linux）
    python3 serial_logger.py --dir D:\\logs       # 指定输出目录
    python3 serial_logger.py --only-xw            # 只收 [xw]/XWFS 行
    python3 serial_logger.py --max-mb 10          # 单文件超 10MB 滚动

依赖：
    pip install pyserial

Windows 后台静默运行（无黑窗，可配计划任务开机启动）：
    pythonw serial_logger.py --dir %USERPROFILE%\\watch5-logs
"""

import argparse
import datetime
import os
import sys
import time

try:
    import serial
    from serial.tools import list_ports
except ImportError:
    print("缺少 pyserial，请先执行: pip install pyserial")
    sys.exit(1)

DEFAULT_BAUD = 115200
SCAN_INTERVAL = 2.0        # 未发现串口时的重试间隔（秒）
NOTICE_INTERVAL = 30.0     # 等待状态下重复提示的间隔（秒）

# 常见 USB 转串口桥的 VID/PID（ESP32 开发板基本都用这些）
BRIDGE_VID_PID = {
    0x1A86: {0x7523, 0x5523},  # CH340 / CH341
    0x10C4: {0xEA60, 0xEA70},  # CP2102 / CP2104
    0x0403: {0x6001},          # FT232
    0x067B: {0x2303},          # PL2303
}
# 设备描述关键词（部分平台/驱动不报 VID/PID，靠描述兜底）
BRIDGE_KEYWORDS = (
    "usb-serial", "usb serial", "ch340", "ch341", "cp210", "ft232", "pl2303",
)


def log(msg):
    """状态信息走 stderr，stdout 保持干净（整体重定向时不错位）。"""
    sys.stderr.write(msg + "\n")
    sys.stderr.flush()


def is_bridge(info):
    """判断一个串口信息是否是 USB 转串口桥（即开发板的串口）。"""
    if info.vid is not None and info.pid is not None:
        pids = BRIDGE_VID_PID.get(info.vid)
        if pids and info.pid in pids:
            return True
    text = " ".join(str(x or "") for x in (info.description, info.hwid, info.name)).lower()
    return any(k in text for k in BRIDGE_KEYWORDS)


def find_board_port():
    """返回识别到的第一个板子串口名；没有则 None。"""
    for info in list_ports.comports():
        try:
            if is_bridge(info):
                return info.device
        except Exception:
            continue
    return None


class Logger:
    """记录器：带时间戳写行 + 大小滚动。懒建文件——收到第一行才落盘，
    避免设备消失后路径仍可打开时每次重试都留下空文件。"""

    def __init__(self, directory, max_mb, only_xw):
        self.directory = directory
        self.max_mb = max_mb
        self.only_xw = only_xw
        self.f = None
        self.path = None
        self.base = None  # 本次会话的基准名（滚动时按它编号，避免 .1.1.1 叠名）
        self.roll = 0
        self.lines = 0

    def _open_new(self):
        stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
        base = os.path.join(self.directory, "watch5-net_%s" % stamp)
        path = base + ".log"
        n = 1
        while os.path.exists(path):  # 同一秒重连时避让
            path = "%s.%d.log" % (base, n)
            n += 1
        self.base = path[:-4]
        self.roll = 0
        self.path = path
        self.f = open(path, "a", encoding="utf-8", errors="replace")

    def _roll(self):
        self.f.close()
        self.roll += 1
        self.path = "%s.%d.log" % (self.base, self.roll)
        while os.path.exists(self.path):  # 同名避让
            self.roll += 1
            self.path = "%s.%d.log" % (self.base, self.roll)
        self.f = open(self.path, "a", encoding="utf-8", errors="replace")
        log("[日志滚动] %s" % self.path)

    def write(self, line):
        if self.only_xw and "[xw]" not in line and not line.startswith("XWFS"):
            return
        if self.f is None:
            self._open_new()
            log("[记录中] -> %s" % self.path)
        now = datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S.%f")[:-3]
        self.f.write("[%s] %s\n" % (now, line))
        self.f.flush()
        self.lines += 1
        if self.max_mb and self.f.tell() > self.max_mb * 1024 * 1024:
            self._roll()

    def close(self):
        """关闭当前文件并清空句柄——下次连接（重插/复位）会另起一个新文件。
        句柄必须置 None，否则重连后的第一行会写进已关闭的文件对象。"""
        try:
            if self.f:
                self.f.close()
        except Exception:
            pass
        self.f = None
        self.base = None
        self.roll = 0


def run_session(port_name, baud, logger):
    """打开串口并记录直到断开。返回 (退出原因, 是否收到过数据)。"""
    log("[连接中] %s @ %d" % (port_name, baud))
    try:
        ser = serial.Serial(port=port_name, baudrate=baud, timeout=0.5)
    except serial.SerialException as e:
        # 串口被别的程序占用（如 Arduino IDE 监视器）时给出明确提示
        msg = str(e)
        if "Permission" in msg or "Access" in msg or "could not open port" in msg:
            log("[打不开] %s：可能被其他程序占用（Arduino IDE 串口监视器？），或设备已拔出，稍后重试" % port_name)
        else:
            log("[打不开] %s: %s" % (port_name, msg))
        return "open-failed", False

    log("[已连接] %s，等待日志…" % port_name)

    # DTR/RST 拉高一次，让板子从已知状态启动（部分板子靠 DTR 时序进下载模式，
    # 这里只做一次正常电平，不触发下载）
    try:
        ser.dtr = False
        ser.rts = False
    except Exception:
        pass

    reason = "unknown"
    got_data = False
    try:
        while True:
            raw = ser.readline()
            if not raw:
                # 超时（板子静默）或 EOF（对端消失）：靠写操作探测断连
                continue
            try:
                line = raw.decode("utf-8", "replace").rstrip("\r\n")
            except Exception:
                continue
            if line:
                logger.write(line)
                got_data = True
    except KeyboardInterrupt:
        reason = "user-interrupt"
        raise
    except (serial.SerialException, OSError) as e:
        reason = "disconnected: %s" % e
    finally:
        try:
            ser.close()
        except Exception:
            pass
        logger.close()
        log("[断开] %s（%s）" % (port_name, reason))
    return reason, got_data


def main():
    ap = argparse.ArgumentParser(
        description="ESP32 网关串口日志自动接收器：插线即记录，拔线自动重连")
    ap.add_argument("--port", help="指定串口；缺省自动识别（如 COM3 / /dev/ttyUSB0）")
    ap.add_argument("--baud", type=int, default=DEFAULT_BAUD,
                    help="波特率，默认 %d" % DEFAULT_BAUD)
    ap.add_argument("--dir", default="logs", help="日志输出目录，默认 ./logs")
    ap.add_argument("--only-xw", action="store_true",
                    help="只记录含 [xw] 或以 XWFS 开头的行（过滤系统启动噪声）")
    ap.add_argument("--max-mb", type=float, default=0,
                    help="单文件大小上限（MB），超出自动滚动；0 = 不限制")
    ap.add_argument("--list", action="store_true", help="列出识别到的串口后退出")
    args = ap.parse_args()

    # Windows 控制台 GBK 码页下打印中文设备信息不崩
    try:
        sys.stderr.reconfigure(errors="replace")
    except Exception:
        pass

    if args.list:
        found = False
        for info in list_ports.comports():
            mark = "  <-- 板子" if is_bridge(info) else ""
            if mark:
                found = True
            print("%s | %s | %s%s" % (info.device, info.description,
                                      info.hwid, mark))
        if not found:
            print("未发现 USB 转串口设备（板子插好了吗？驱动装了吗？）")
        return 0

    os.makedirs(args.dir, exist_ok=True)
    logger = Logger(args.dir, args.max_mb, args.only_xw)

    log("[启动] 等待板子接入（自动识别串口，Ctrl+C 退出）")
    last_notice = 0.0
    fails = 0  # 连续无数据会话数，用于失败退避（设备消失但路径可开时防忙等）
    try:
        while True:
            port_name = args.port or find_board_port()
            if not port_name:
                now = time.time()
                if now - last_notice >= NOTICE_INTERVAL:
                    log("[等待] 未发现板子串口，插上 USB 数据线即可自动开始记录…")
                    last_notice = now
                time.sleep(SCAN_INTERVAL)
                continue
            reason, got_data = run_session(port_name, args.baud, logger)
            if got_data:
                fails = 0
            else:
                fails += 1
            # 退避：设备没了/被占用时从 2s 指数拉到 30s；板子复位（DTR 抖动）
            # 造成的短暂掉线也会被这层缓冲覆盖，不会疯狂重连
            backoff = min(30.0, 2.0 * fails)
            if not got_data:
                log("[重试] %.0f 秒后再试（%s）" % (backoff, reason))
            time.sleep(backoff)
    except KeyboardInterrupt:
        log("[退出] 共记录 %d 行，最后文件：%s" % (logger.lines, logger.path))


if __name__ == "__main__":
    sys.exit(main())
