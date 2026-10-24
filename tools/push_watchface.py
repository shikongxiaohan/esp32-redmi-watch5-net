#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
表盘安装包 → ESP32 网关（USB 串口推送）

用法：
    python3 push_watchface.py <表盘.bin> [--port COM3] [--baud 115200]

功能：
    1. 打开串口，发送 "XWFP BEGIN <size> <name>"
    2. 分块 base64 编码推送文件内容
    3. 发送 "XWFP END"，板子随即通过蓝牙把表盘装进手表
    4. 打印板子返回的状态（XWFS ...）

依赖：
    pip install pyserial
    （Windows 若没装 Python，可用打包好的 exe：tools/dist/）
"""

import argparse
import base64
import os
import sys
import time

try:
    import serial
except ImportError:
    print("缺少 pyserial，请先执行: pip install pyserial")
    sys.exit(1)

PREFIX = "XWFP "


def send_line(ser, text):
    # 协议行是 ASCII。文件名可能含中文/其它非 ASCII 字符，
    # 直接 encode("ascii") 会抛 UnicodeEncodeError 让整个脚本崩掉。
    # 这里把非 ASCII 字符转义成 \xNN（板子侧原样存为文件名的一部分，
    # 只作显示用，不影响安装），保证协议行始终可发送。
    raw = text.encode("ascii", "backslashreplace").decode("ascii")
    ser.write((raw + "\n").encode("ascii"))
    ser.flush()


def read_status(ser, timeout=2.0):
    """读取板子返回的 XWFS 状态行（忽略日志行）。"""
    end = time.time() + timeout
    while time.time() < end:
        line = ser.readline()
        if not line:
            continue
        s = line.decode("utf-8", "replace").strip()
        if s.startswith("XWFS "):
            return s[5:].strip()
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("file", help="表盘 .bin 文件")
    ap.add_argument("--port", default=None, help="串口号（Windows 如 COM3；Linux 如 /dev/ttyUSB0）")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--chunk", type=int, default=3072, help="每次 DATA 的原始字节数")
    args = ap.parse_args()

    path = args.file
    if not os.path.isfile(path):
        print("文件不存在:", path)
        return 1
    size = os.path.getsize(path)
    name = os.path.basename(path)

    port = args.port
    if not port:
        # 自动挑选第一个可用串口
        try:
            from serial.tools import list_ports
            ports = [p.device for p in list_ports.comports()]
        except Exception:
            ports = []
        if not ports:
            print("未找到串口，请用 --port 指定")
            return 1
        port = ports[0]
        print("自动选择串口:", port)

    print("打开 %s @ %d" % (port, args.baud))
    ser = serial.Serial(port, args.baud, timeout=0.2)
    try:
        send_line(ser, PREFIX + "BEGIN %d %s" % (size, name))
        st = read_status(ser, 5.0)
        if not st or not st.startswith("READY"):
            print("板子未就绪:", st)
            return 2
        print("板子已就绪，开始推送 %d 字节 ..." % size)

        sent = 0
        with open(path, "rb") as f:
            while True:
                chunk = f.read(args.chunk)
                if not chunk:
                    break
                b64 = base64.b64encode(chunk).decode("ascii")
                send_line(ser, PREFIX + "DATA " + b64)
                sent += len(chunk)
                st = read_status(ser, 3.0)
                pct = sent * 100 // max(size, 1)
                if st and st.startswith("PROGRESS"):
                    print("  %3d%%  %d/%d" % (pct, sent, size))
                elif st and st.startswith("ERR"):
                    print("错误:", st)
                    send_line(ser, PREFIX + "ABORT")
                    return 3
                else:
                    print("  %3d%%  %d/%d (无回执)" % (pct, sent, size))

        send_line(ser, PREFIX + "END")
        st = read_status(ser, 10.0)
        if st and st.startswith("OK"):
            print("推送完成，板子已开始安装到手表。")
            print("现在可以在浏览器打开网关页面查看安装进度。")
            return 0
        print("结束握手异常:", st)
        return 4
    finally:
        ser.close()


if __name__ == "__main__":
    sys.exit(main())
