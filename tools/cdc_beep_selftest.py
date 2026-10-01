#!/usr/bin/env python3
"""蜂鸣器（PWM 旋律引擎）听测脚本 —— 配合固件 v1.8+ 使用。

逐条下发 BEEP 指令，并在需要的地方保持连接（心跳保活），由你用耳朵验收。
用法：
    python tools/cdc_beep_selftest.py
依赖：pip install pyserial
"""
import sys
import time

try:
    import serial
    import serial.tools.list_ports
except ImportError:
    sys.exit("缺少 pyserial，请先执行：  pip install pyserial")

VID = 0xCAFE
PID = 0x4A10
HWID_KEY = f"{VID:04X}:{PID:04X}"
BAUD = 115200


def find_ports():
    hits = []
    for p in serial.tools.list_ports.comports():
        blob = f"{p.hwid} {p.manufacturer or ''} {p.product or ''} {p.description or ''}".upper()
        if HWID_KEY in blob:
            hits.append(p)
    return hits


def open_port(port, timeout=0.6):
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
    """发指令后保持连接并心跳 SEC 秒（规避设备 5s 看门狗，方便听测）。"""
    send(s, line)
    print(f"  保持 {sec:.0f} 秒，请听 …")
    t0 = time.time()
    while time.time() - t0 < sec:
        time.sleep(2.0)
        send(s, "IDN?", quiet=True)
    print("  保持结束。")


def main():
    ports = find_ports()
    if not ports:
        sys.exit(f"未找到 VID:PID={HWID_KEY} 的串口。\n"
                 "确认设备已插入且枚举成功（不是 BOOTSEL 大容量盘模式）。")
    port = ports[0].device
    print(f"找到设备：{port}  ({ports[0].description})")

    try:
        s = open_port(port)
    except Exception as e:
        sys.exit(f"打开 {port} 失败：{e}\n（串口独占，确认没有其他程序占用）")

    with s:
        time.sleep(0.05)
        print("\n=== 蜂鸣器听测（无源蜂鸣器 / PWM）===")
        send(s, "BEEP?")                 # 应 STATE=OFF NOTE=0
        hold(s, "BEEP ON", 3)            # 持续 2kHz 音
        send(s, "BEEP OFF")              # 立刻静音
        send(s, "BEEP 300")              # 响 300ms 自停（看表）
        time.sleep(1)
        hold(s, "BEEP NOTE 440 500", 1)   # A4 440Hz 500ms
        time.sleep(1)
        print("\n--- 旋律 1（C 大调上行），听 ~1s ---")
        hold(s, "BEEP PLAY 1", 3)
        send(s, "BEEP?")                 # 播放中应 STATE=PLAYING NOTE=xxx
        send(s, "BEEP STOP")             # 中途停
        send(s, "BEEP?")                 # 应 STATE=OFF NOTE=0
        print("\n=== 听测结束 ===")


if __name__ == "__main__":
    main()
