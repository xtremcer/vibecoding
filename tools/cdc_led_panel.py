#!/usr/bin/env python3
"""CDC LED 控制面板（Phase 3）

三盏灯（BUSY=GP26 / PLAN=GP27 / IDLE=GP28）各自独立控制：亮 / 灭 / 闪（可调周期）。
自动发现串口、自动重连、2 秒一次心跳保活（防止设备 5 秒看门狗交还本地控制）。

用法：
    python tools/cdc_led_panel.py
依赖： pip install pyserial
"""
import threading
import time
import tkinter as tk
from tkinter import ttk

try:
    import serial
    import serial.tools.list_ports
except ImportError:
    raise SystemExit("缺少 pyserial，请先执行：  pip install pyserial")

VID, PID = 0xCAFE, 0x4A10
HWID_KEY = f"{VID:04X}:{PID:04X}"
BAUD = 115200
HEARTBEAT_S = 2.0          # 必须小于设备端 CDC_HOST_TIMEOUT_MS(5000)
RESCAN_S = 2.0

LEDS = ["BUSY", "PLAN", "IDLE"]


class Device:
    """串口封装：自动发现 + 自动重连 + 线程安全收发。"""

    def __init__(self):
        self.ser = None
        self.lock = threading.Lock()
        self.port = "-"
        self.status = "未连接"

    # ---- 连接管理 ----
    def find_port(self):
        for p in serial.tools.list_ports.comports():
            blob = f"{p.hwid} {p.manufacturer or ''} {p.product or ''}".upper()
            if HWID_KEY in blob:
                return p.device
        return None

    def ensure_open(self):
        if self.ser and self.ser.is_open:
            return True
        port = self.find_port()
        if not port:
            self.status = "未找到设备"
            self.port = "-"
            return False
        try:
            s = serial.Serial(port, BAUD, timeout=0.5, dsrdtr=False, rtscts=False)
            s.dtr = True
            s.rts = False
            s.reset_input_buffer()
            self.ser, self.port = s, port
            self.status = "已连接"
            return True
        except Exception as e:                      # 串口被占用 / 权限问题
            self.ser, self.port = None, "-"
            self.status = f"打开失败：{e.__class__.__name__}"
            return False

    def send(self, line):
        """发一条指令，返回响应字符串；失败返回 None。"""
        with self.lock:
            if not self.ensure_open():
                return None
            try:
                self.ser.reset_input_buffer()
                self.ser.write((line + "\r\n").encode())
                r = self.ser.readline().decode("ascii", errors="ignore").strip()
                return r or None
            except Exception:
                try:
                    self.ser.close()
                except Exception:
                    pass
                self.ser, self.status = None, "连接断开"
                return None


class App(tk.Tk):
    def __init__(self):
        super().__init__()
        self.title("vibecoding-mate 状态灯控制面板")
        self.resizable(False, False)
        self.dev = Device()
        self.rows = {}

        head = ttk.Frame(self, padding=8)
        head.grid(row=0, column=0, columnspan=6, sticky="w")
        ttk.Label(head, text="端口：").grid(row=0, column=0)
        self.lbl_port = ttk.Label(head, text="-", width=8)
        self.lbl_port.grid(row=0, column=1)
        ttk.Label(head, text="状态：").grid(row=0, column=2)
        self.lbl_status = ttk.Label(head, text="未连接", width=28)
        self.lbl_status.grid(row=0, column=3)

        # ---- 三盏灯 ----
        for i, name in enumerate(LEDS, start=1):
            f = ttk.LabelFrame(self, text=f"{name}", padding=6)
            f.grid(row=i, column=0, columnspan=6, sticky="ew", padx=8, pady=3)
            var = tk.StringVar(value="OFF")
            ttk.Radiobutton(f, text="灭", value="OFF", variable=var,
                            command=lambda n=name, v=var: self.apply(n, v)).grid(row=0, column=0)
            ttk.Radiobutton(f, text="亮", value="ON", variable=var,
                            command=lambda n=name, v=var: self.apply(n, v)).grid(row=0, column=1)
            ttk.Radiobutton(f, text="闪", value="BLINK", variable=var,
                            command=lambda n=name, v=var: self.apply(n, v)).grid(row=0, column=2)
            ttk.Label(f, text="亮(ms)").grid(row=0, column=3)
            e_on = ttk.Entry(f, width=6)
            e_on.insert(0, "300")
            e_on.grid(row=0, column=4)
            ttk.Label(f, text="灭(ms)").grid(row=0, column=5)
            e_off = ttk.Entry(f, width=6)
            e_off.insert(0, "300")
            e_off.grid(row=0, column=6)
            ttk.Button(f, text="应用闪烁",
                       command=lambda n=name, a=e_on, b=e_off: self.blink(n, a.get(), b.get())
                       ).grid(row=0, column=7, padx=(6, 0))
            self.rows[name] = (var, e_on, e_off)

        # ---- 全体 + 回读 ----
        bar = ttk.Frame(self, padding=8)
        bar.grid(row=4, column=0, columnspan=6, sticky="w")
        ttk.Button(bar, text="全部亮", command=lambda: self.dev.send("LED ALL ON")).grid(row=0, column=0)
        ttk.Button(bar, text="全部灭", command=lambda: self.dev.send("LED ALL OFF")).grid(row=0, column=1)
        ttk.Button(bar, text="全闪 300/300", command=lambda: self.dev.send("LED ALL BLINK 300 300")).grid(row=0, column=2)
        ttk.Button(bar, text="交还本地控制", command=lambda: self.dev.send("RESET")).grid(row=0, column=3)
        ttk.Button(bar, text="查询状态", command=self.query).grid(row=0, column=4)

        self.lbl_read = ttk.Label(self, text="BUSY=? PLAN=? IDLE=?", padding=(8, 4))
        self.lbl_read.grid(row=5, column=0, columnspan=6, sticky="w")

        self.after(200, self.tick)

    # ---- 指令 ----
    def apply(self, name, var):
        self.dev.send(f"LED {name} {var.get()}")

    def blink(self, name, on_ms, off_ms):
        try:
            on_ms, off_ms = int(on_ms), int(off_ms)
        except ValueError:
            self.lbl_status.config(text="周期必须是整数")
            return
        self.dev.send(f"LED {name} BLINK {on_ms} {off_ms}")

    def query(self):
        self.lbl_read.config(text=self.dev.send("LED?") or "无响应")

    def tick(self):
        """心跳 + 重连 + 状态刷新（每 500ms 检查一次，心跳 2s 一次）。"""
        self.lbl_port.config(text=self.dev.port)
        self.lbl_status.config(text=self.dev.status)
        now = time.time()
        if not hasattr(self, "_hb") or now - self._hb >= HEARTBEAT_S:
            self._hb = now
            self.dev.send("IDN?")            # 心跳：保持主机接管
            self.query()
        self.after(500, self.tick)


if __name__ == "__main__":
    App().mainloop()
