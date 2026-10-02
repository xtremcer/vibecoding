#!/usr/bin/env python3
"""A3 掉电持久化 · 开机载入路径取证诊断。

背景：两阶段验收里阶段1全绿（SET→LOAD→GET 都拿到标记值 9），但真实拔插掉电后
阶段2 读回的是默认 3。需要分清两种可能：
  (a) 闪存里存着 9，但开机没还原 → 开机载入路径（cfg_init）有问题
  (b) 闪存里根本读不到 9      → 落盘没跨掉电存活 / 开机被 lfs_format 抹掉

用法：
  --mode pre   写入标记 count=9 并确认落盘，干净退出（等你拔插）
  --mode post  重连后「不破坏现场」取证：
                 FLASH?      → MOUNT / LOAD
                 CONFIG GET  → 开机还原值
                 CONFIG LOAD → 手动重载
                 CONFIG GET  → 闪存里的真实值
               不做 DEFAULT（会覆盖证据）

注意（Windows + RP2040 CDC 的两个坑，已从 cdc_a3_selftest.py 继承）：
  1) 绝不在开端口后再赋值 s.timeout —— 那会触发 _reconfigure_port → GetCommState，
     设备半死时会无限阻塞。统一在 open 时设好 2.0s，之后只读不重配。
  2) 开端口一律用 open_guarded（子线程 + join 超时），否则驱动卡死会阻塞主线程。
"""
import argparse
import json
import sys
import time
import threading

try:
    import serial
    import serial.tools.list_ports
except ImportError:
    sys.exit("缺少 pyserial，请先执行：  pip install pyserial")

VID, PID, BAUD = 0xCAFE, 0x4A10, 115200
HWID_KEY = f"{VID:04X}:{PID:04X}"
MARKER = 9


def find_ports():
    return [p for p in serial.tools.list_ports.comports()
            if HWID_KEY in f"{p.hwid} {p.manufacturer or ''} "
                           f"{p.product or ''} {p.description or ''}".upper()]


def open_guarded(port, baud=BAUD, tmo=2.0, timeout_s=10.0):
    """带超时的串口打开。tmo 一次性设好读超时，之后不再重配。"""
    res = {}

    def worker():
        try:
            res["s"] = serial.Serial(port, baud, timeout=tmo, write_timeout=tmo)
        except Exception as e:
            res["err"] = repr(e)

    th = threading.Thread(target=worker, daemon=True)
    th.start()
    th.join(timeout_s)
    if th.is_alive():
        return None, "OPEN_BLOCKED"
    return res.get("s"), res.get("err")


def rdline(s):
    return s.readline().decode("ascii", errors="ignore").strip()


def q(s, line, timeout=2.0):
    s.reset_input_buffer()
    s.write((line + "\r\n").encode("ascii", errors="ignore"))
    t0 = time.time()
    while time.time() - t0 < timeout:
        r = rdline(s)
        if r:
            return r
    return ""


def cfg_get(s):
    s.reset_input_buffer()
    s.write(b"CONFIG GET\r\n")
    t0 = time.time()
    saw = False
    while time.time() - t0 < 3.0:
        if rdline(s) == "CONFIG BEGIN":
            saw = True
            break
    if not saw:
        return None, "no-CONFIG-BEGIN"
    lines = []
    while time.time() - t0 < 12.0:
        l = rdline(s)
        if l == "OK":
            break
        if l:
            lines.append(l)
    else:
        return None, "no-OK"
    try:
        return json.loads("\n".join(lines)), None
    except Exception as e:
        return None, f"bad-json:{e}"


def count_of(cfg):
    if not isinstance(cfg, dict):
        return None
    try:
        return cfg["states"]["busy"]["sound"]["count"]
    except Exception:
        return None


def connect(timeout=60.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        for p in find_ports():
            s, err = open_guarded(p.device, timeout_s=8.0)
            if s is None:
                continue
            try:
                time.sleep(0.8)
                s.reset_input_buffer()
                if "v2.0" in q(s, "IDN?"):
                    return s
            except Exception:
                pass
            try:
                s.close()
            except Exception:
                pass
        time.sleep(0.5)
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--mode", choices=["pre", "post"], required=True)
    ap.add_argument("--port")
    a = ap.parse_args()

    if a.port:
        s, err = open_guarded(a.port)
        if s is None:
            sys.exit(f"[diag] 打开 {a.port} 失败：{err}")
    else:
        print("[diag] 等待/连接设备……", flush=True)
        s = connect()
        if s is None:
            sys.exit("[diag] 60s 内未连上设备")
    time.sleep(0.5)
    print(f"[diag] 已连 {s.port}", flush=True)

    if a.mode == "pre":
        payload = f'CONFIG SET {{"version":1,"states":{{"busy":{{"sound":{{"count":{MARKER}}}}}}}}}'
        print("[diag] SET 标记 count=9 ->", q(s, payload), flush=True)
        cfg, err = cfg_get(s)
        print(f"[diag] SET 后 GET  count = {count_of(cfg)}  (err={err})", flush=True)
        print("[diag] CONFIG LOAD ->", q(s, "CONFIG LOAD"), flush=True)
        cfg, err = cfg_get(s)
        print(f"[diag] LOAD 后 GET count = {count_of(cfg)}  (err={err})", flush=True)
        s.close()
        print("\n[diag] >>> 请拔掉 USB，等 2 秒，插回后告诉我（我跑 --mode post 取证）", flush=True)
        return

    # ---- post：只取证，不 DEFAULT ----
    print("[diag] FLASH? ->", q(s, "FLASH?"), flush=True)
    cfg, err = cfg_get(s)
    boot = count_of(cfg)
    print(f"[diag] 开机还原值 count = {boot}  (err={err})", flush=True)
    print("[diag] CONFIG LOAD ->", q(s, "CONFIG LOAD"), flush=True)
    cfg, err = cfg_get(s)
    flashv = count_of(cfg)
    print(f"[diag] 闪存实际值 count = {flashv}  (err={err})", flush=True)
    s.close()

    print("\n==== 取证结论 ====", flush=True)
    if flashv == MARKER and boot == MARKER:
        print("  开机即从闪存还原 → 持久化正常", flush=True)
    elif flashv == MARKER and boot != MARKER:
        print(f"  闪存存着 {MARKER}，但开机没还原（boot={boot}）→ 开机载入路径(cfg_init)有问题", flush=True)
    elif flashv != MARKER:
        print(f"  闪存读不到 {MARKER}（flash={flashv}）→ 落盘没跨掉电存活 / 开机被 lfs_format 抹掉", flush=True)
    else:
        print(f"  未分类：boot={boot} flash={flashv}", flush=True)


if __name__ == "__main__":
    main()
