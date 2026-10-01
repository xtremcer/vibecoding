#!/usr/bin/env python3
"""CDC 虚拟串口命令行调试器（Phase 1 验证用）

用法：
    python tools/cdc_host_cli.py            # 自动找口，进入交互模式
    python tools/cdc_host_cli.py IDN?       # 只发一条指令后退出
    python tools/cdc_host_cli.py --list     # 只列出匹配的串口

依赖：pip install pyserial
"""
import sys
import time

try:
    import serial
    import serial.tools.list_ports
except ImportError:
    sys.exit("缺少 pyserial，请先执行：  pip install pyserial")

# 设备身份：与 src/usb_descriptors.c 中的 USB_VID / USB_PID 保持一致
VID = 0xCAFE
PID = 0x4A10
HWID_KEY = f"{VID:04X}:{PID:04X}"   # pyserial 的 hwid 形如 "USB VID:PID=CAFE:4A10 ..."
BAUD = 115200                        # CDC ACM 忽略波特率，填什么都行


def find_ports():
    """按 VID/PID 扫描，绝不硬编码 COM3（端口号会变）。"""
    hits = []
    for p in serial.tools.list_ports.comports():
        blob = f"{p.hwid} {p.manufacturer or ''} {p.product or ''} {p.description or ''}".upper()
        if HWID_KEY in blob:
            hits.append(p)
    return hits


def open_port(port, timeout=0.6):
    """CDC ACM 是虚拟串口，硬件流控无意义；打开时保持 DTR 有效，设备端靠 DTR 判断主机在线。"""
    s = serial.Serial(port, BAUD, timeout=timeout, dsrdtr=False, rtscts=False)
    s.dtr = True
    s.rts = False
    s.reset_input_buffer()
    return s


def send(s, line, quiet=False):
    s.write((line + "\r\n").encode("ascii", errors="ignore"))
    resp = s.readline().decode("ascii", errors="ignore").strip()
    if not quiet:
        print(f"> {line}\n< {resp if resp else '(超时无响应)'}")
    return resp


def hold(s, line, sec):
    """发一条指令后保持连接并心跳 SEC 秒，便于肉眼观察（规避设备端 5s 看门狗）。"""
    send(s, line)          # send() 自己会打印 "> 指令 / < 响应"
    print(f"保持 {sec:.0f} 秒（心跳保活），请观察 …")
    t0 = time.time()
    while time.time() - t0 < sec:
        time.sleep(2.0)
        send(s, "IDN?", quiet=True)
    print("保持结束。")


def main():
    args = sys.argv[1:]

    if "--list" in args:
        for p in find_ports():
            print(f"{p.device}  {p.description}\n    {p.hwid}")
        if not find_ports():
            print("未找到设备。")
        return

    ports = find_ports()
    if not ports:
        sys.exit(f"未找到 VID:PID={HWID_KEY} 的串口。\n"
                 f"检查：1) 设备已插入且枚举成功  2) 设备管理器里出现了 COM 口\n"
                 f"      3) 没有其他程序占用该串口")

    port = ports[0].device
    print(f"找到设备：{port}  ({ports[0].description})")

    try:
        s = open_port(port)
    except Exception as e:
        sys.exit(f"打开 {port} 失败：{e}\n（串口是独占的，确认没有别的程序占用）")

    with s:
        time.sleep(0.05)

        # --hold SEC <指令>：发完保持连接 SEC 秒，方便肉眼观察灯/蜂鸣器
        if args and args[0] == "--hold":
            if len(args) < 3:
                sys.exit("用法：--hold <秒> <指令>")
            hold(s, " ".join(args[2:]), float(args[1]))
            return

        if args:                                   # 单次指令模式
            send(s, " ".join(args))
            return

        print("交互模式：直接敲指令回车，Ctrl+C 退出。已内置 IDN?\n")
        try:
            send(s, "IDN?")
            while True:
                line = input("> ").strip()
                if not line:
                    continue
                if line.lower() in ("q", "quit", "exit"):
                    break
                send(s, line)
        except (KeyboardInterrupt, EOFError):
            print("\n退出。")


if __name__ == "__main__":
    main()
