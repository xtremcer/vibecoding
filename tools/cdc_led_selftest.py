#!/usr/bin/env python3
"""状态灯自检：逐个点亮 → 闪烁 → 熄灭，最后全亮 / 全灭。

用来核对「哪一盏灯对应哪个 GPIO」以及「极性是否正确」。
全程保持串口连接并心跳，不会被设备端 5 秒看门狗收回控制权。

用法：
    python tools/cdc_led_selftest.py            # 完整序列（约 1 分钟）
    python tools/cdc_led_selftest.py --fast     # 每步时间减半
依赖： pip install pyserial
"""
import sys
import time

try:
    import serial
    import serial.tools.list_ports
except ImportError:
    sys.exit("缺少 pyserial，请先执行：  pip install pyserial")

VID, PID = 0xCAFE, 0x4A10
HWID_KEY = f"{VID:04X}:{PID:04X}"
BAUD = 115200

# (指令, 保持秒数, 提示语)
SEQ = [
    ("LED ALL OFF",          4, "基线：三盏灯全部熄灭"),
    ("LED BUSY ON",          5, "【BUSY / GP26 / Pin31】单独点亮"),
    ("LED BUSY BLINK 200 200", 6, "【BUSY】单独闪烁 200ms 亮 / 200ms 灭"),
    ("LED BUSY OFF",         2, "【BUSY】熄灭"),
    ("LED PLAN ON",          5, "【PLAN / GP27 / Pin32】单独点亮"),
    ("LED PLAN BLINK 200 200", 6, "【PLAN】单独闪烁"),
    ("LED PLAN OFF",         2, "【PLAN】熄灭"),
    ("LED IDLE ON",          5, "【IDLE / GP28 / Pin34】单独点亮"),
    ("LED IDLE BLINK 200 200", 6, "【IDLE】单独闪烁"),
    ("LED IDLE OFF",         2, "【IDLE】熄灭"),
    ("LED ALL ON",           6, "三盏灯【全部点亮】"),
    ("LED ALL OFF",          6, "三盏灯【全部熄灭】"),
    ("RESET",                2, "交还本地控制，恢复上电默认"),
]


def find_port():
    for p in serial.tools.list_ports.comports():
        blob = f"{p.hwid} {p.manufacturer or ''} {p.product or ''}".upper()
        if HWID_KEY in blob:
            return p.device
    return None


def main():
    fast = "--fast" in sys.argv
    scale = 0.5 if fast else 1.0

    port = find_port()
    if not port:
        sys.exit(f"未找到 VID:PID={HWID_KEY} 的设备")
    print(f"设备：{port}\n")

    s = serial.Serial(port, BAUD, timeout=0.5, dsrdtr=False, rtscts=False)
    s.dtr = True
    s.rts = False
    s.reset_input_buffer()

    def send(line):
        s.reset_input_buffer()
        s.write((line + "\r\n").encode())
        time.sleep(0.05)
        return s.readline().decode("ascii", errors="ignore").strip() or "(无响应)"

    with s:
        print(f"身份：{send('IDN?')}\n")
        for i, (cmd, sec, tip) in enumerate(SEQ, 1):
            sec *= scale
            print(f"[{i:2d}/{len(SEQ)}] {tip}")
            print(f"        指令 {cmd}  →  {send(cmd)}")
            t0 = time.time()
            while time.time() - t0 < sec:
                time.sleep(min(2.0, max(0.1, sec - (time.time() - t0))))
                send("IDN?")                  # 心跳保活
            print(f"        当前状态 {send('LED?')}\n")
        print("自检结束。")


if __name__ == "__main__":
    main()
