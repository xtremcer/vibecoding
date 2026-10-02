#!/usr/bin/env python3
"""A3 验收脚本：配置落盘（LittleFS）掉电不丢。

验证点：
  A. FLASH?             → MOUNT=1（LFS 已挂载）
  B. 改动后 SET→LOAD→GET round-trip：SET 改 busy 提示次数→GET 见新值→LOAD→GET 仍见新值
                        （证明「写闪存 + 从闪存重载」链路通）
  C. DEFAULT→LOAD→GET  → 默认也落盘，重载后仍是默认
  D. REBOOT 真·掉电模拟：SET 一个标记值→REBOOT→等重枚举→GET 仍见标记值
                        （证明重启后配置从闪存恢复，而非 RAM 残留）
  E. 收尾 DEFAULT         → 把设备留回默认配置

用法：python tools/cdc_a3_selftest.py [--port COMx]
前置：已刷入 v2.0（含 A3）固件，设备插好 USB；pip install pyserial
"""
import argparse
import json
import re
import sys
import time
import threading

try:
    import serial
    import serial.tools.list_ports
except ImportError:
    sys.exit("缺少 pyserial，请先执行：  pip install pyserial")

import os
import traceback

# ---- 看门狗：主线程若卡死（readline 越过自身超时仍阻塞）就转储堆栈并强退，
#      避免被外部超时杀掉后留下楔死端口且不知卡在哪 ----
_WD = {"last": 0.0, "limit": 35.0, "installed": False}

def mark():
    """主线程每做一次有效推进就调一次；看门狗据此判断是真·卡死还是正常等待。"""
    _WD["last"] = time.time()

def _watchdog_loop():
    main = threading.main_thread()
    while True:
        time.sleep(1.5)
        idle = time.time() - _WD["last"]
        if idle >= _WD["limit"]:
            sys.stderr.write(
                f"\n[WATCHDOG] 主线程无推进 {idle:.0f}s（> {_WD['limit']:.0f}s），"
                f"疑似卡死，转储主线程堆栈：\n")
            frames = sys._current_frames()
            fr = frames.get(main.ident)
            if fr:
                for ln in traceback.format_stack(fr):
                    sys.stderr.write(ln)
            sys.stderr.write("[WATCHDOG] 强制退出（端口可能需重插清除）。\n")
            sys.stderr.flush()
            os._exit(2)

def start_watchdog(limit=35.0):
    if _WD["installed"]:
        return
    _WD["installed"] = True
    _WD["limit"] = limit
    _WD["last"] = time.time()
    threading.Thread(target=_watchdog_loop, daemon=True).start()

VID, PID, BAUD = 0xCAFE, 0x4A10, 115200
HWID_KEY = f"{VID:04X}:{PID:04X}"
FAILS = []
MARKER = 9          # 与默认 count=3 区分的标记值
MARKER_KEY = "states.busy.sound.count"


def find_ports():
    return [p for p in serial.tools.list_ports.comports()
            if HWID_KEY in f"{p.hwid} {p.manufacturer or ''} {p.product or ''} "
                           f"{p.description or ''}".upper()]


def jget(json_text):
    """把 CONFIG GET 的多行文本拼回 dict。"""
    try:
        return json.loads(json_text)
    except Exception:
        return None


class Dev:
    def __init__(self, s):
        self.s = s

    def q(self, line, timeout=2.0):
        mark()
        # 发命令前先清输入缓冲：避免上一条命令的残留字节错位，导致首行读不到预期响应
        self.s.reset_input_buffer()
        self.s.write((line + "\r\n").encode("ascii", errors="ignore"))
        t0 = time.time()
        while time.time() - t0 < timeout:
            mark()
            r = self.s.readline().decode("ascii", errors="ignore").strip()
            if r:
                return r
        return ""

    def cfg_get(self):
        mark()
        # 清缓冲后再发 CONFIG GET，逐行读取直到 OK（对齐已验证可用的诊断脚本逻辑）
        self.s.reset_input_buffer()
        self.s.write(b"CONFIG GET\r\n")
        t0 = time.time()
        # 容忍前导杂字节（上一条命令残留/换行）：跳过直到看见 CONFIG BEGIN 起始行
        saw_begin = False
        while time.time() - t0 < 3.0:
            mark()
            r = self.s.readline().decode("ascii", errors="ignore").strip()
            if r == "CONFIG BEGIN":
                saw_begin = True
                break
        if not saw_begin:
            FAILS.append("CONFIG GET 未收到起始行 CONFIG BEGIN（设备可能未响应）")
            return None
        lines = []
        while time.time() - t0 < 12.0:       # 有界，避免设备不回 OK 时无限挂死
            mark()
            l = self.s.readline().decode("ascii", errors="ignore").strip()
            if l == "OK":
                break
            if not l:
                continue
            lines.append(l)
        else:
            FAILS.append("CONFIG GET 12s 内未收到结束标记 OK（设备可能失去响应）")
            return None
        text = "\n".join(lines)
        return jget(text)


def open_guarded(port, baud=BAUD, tmo=2.0, timeout_s=10.0):
    """带超时的串口打开，避免 Windows 上端口被占用/驱动卡死时无限阻塞。"""
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


def reconnect(timeout=20.0):
    """REBOOT 后等设备重枚举，返回新的 Dev（或 None）。

    不强制要求旧端口先"消失"——只要能打开并能回 IDN? v2.0 即算连上，
    这样无论设备重枚举到同一 COM 还是新 COM 都能正确恢复。
    打开必须用 open_guarded（带超时），否则重启瞬间旧 COM 处于半断开态时
    serial.Serial() 会阻塞卡死整个脚本。
    """
    deadline = time.time() + timeout
    while time.time() < deadline:
        mark()
        for p in find_ports():
            s, err = open_guarded(p.device, timeout_s=8.0)
            if s is None:
                continue
            try:
                time.sleep(0.8)             # 等 USB 枚举稳定
                mark()
                s.reset_input_buffer()
                s.write(b"IDN?\r\n")
                t0 = time.time()
                while time.time() - t0 < 1.5:
                    mark()
                    r = s.readline().decode("ascii", errors="ignore").strip()
                    if "v2.0" in r:
                        return Dev(s)
            except Exception:
                pass
            try:
                s.close()
            except Exception:
                pass
        time.sleep(0.4)
    return None


def _open_dev(port):
    s, err = open_guarded(port)
    if err or s is None:
        sys.exit(f"[A3] 打开 {port} 失败：{err or 'OPEN_BLOCKED'} "
                 f"（端口被占用或驱动卡死 → 请重插 USB / 重启设备后重试）")
    time.sleep(1.0)
    mark()
    s.reset_input_buffer()
    dev = Dev(s)
    idn = dev.q("IDN?")
    if "v2.0" not in idn:
        sys.exit(f"[A3] IDN? 回 {idn!r}，非预期 v2.0，请先升级固件。")
    return dev


def _flash_check(dev):
    flash = dev.q("FLASH?")
    m = re.search(r"MOUNT=(\d)", flash)
    if not m or m.group(1) != "1":
        FAILS.append(f"FLASH? 应 MOUNT=1，实得 {flash!r}")
        print("[A3] A. FLASH? -> FAIL", flush=True)
    else:
        print(f"[A3] A. FLASH? -> {flash}  ✓", flush=True)


def _bc_roundtrip(dev):
    """B + C：SET/LOAD/GET 往返 + DEFAULT 落盘（不含掉电）。"""
    dev.q(f'CONFIG SET {{"version":1,"states":{{"busy":{{"sound":{{"count":{MARKER}}}}}}}}}')
    after_set = dev.cfg_get()
    if not after_set or after_set["states"]["busy"]["sound"]["count"] != MARKER:
        FAILS.append(f"B. SET 后 count 应={MARKER}，实得 {after_set and after_set['states']['busy']['sound']['count']}")
    else:
        print(f"[A3] B. SET -> count={MARKER} ✓", flush=True)
    dev.q("CONFIG LOAD")
    after_load = dev.cfg_get()
    if not after_load or after_load["states"]["busy"]["sound"]["count"] != MARKER:
        FAILS.append(f"B. LOAD 后 count 应={MARKER}（落盘重载），实得 {after_load and after_load['states']['busy']['sound']['count']}")
    else:
        print(f"[A3] B. LOAD -> count={MARKER}（从闪存重载）✓", flush=True)

    dev.q("CONFIG DEFAULT")
    after_def = dev.cfg_get()
    if not after_def or after_def["states"]["busy"]["sound"]["count"] != 3:
        FAILS.append(f"C. DEFAULT 后 count 应=3，实得 {after_def and after_def['states']['busy']['sound']['count']}")
    else:
        print("[A3] C. DEFAULT -> count=3 ✓", flush=True)
    dev.q("CONFIG LOAD")
    after_def_load = dev.cfg_get()
    if not after_def_load or after_def_load["states"]["busy"]["sound"]["count"] != 3:
        FAILS.append(f"C. DEFAULT 后 LOAD count 应=3（默认也落盘），实得 {after_def_load and after_def_load['states']['busy']['sound']['count']}")
    else:
        print("[A3] C. DEFAULT+LOAD -> count=3（默认已落盘）✓", flush=True)


def run_phase1(port_arg):
    """阶段1：A + B + C + 落盘标记 count=9，然后退出，等用户手动拔插（真实掉电）。"""
    port = port_arg or _first_port()
    dev = _open_dev(port)
    _flash_check(dev)
    _bc_roundtrip(dev)
    # 落盘标记：把 count 设为 9 并持久化（供阶段2验证断电存活）
    dev.q(f'CONFIG SET {{"version":1,"states":{{"busy":{{"sound":{{"count":{MARKER}}}}}}}}}')
    dev.q("CONFIG LOAD")
    mk = dev.cfg_get()
    if mk and mk["states"]["busy"]["sound"]["count"] == MARKER:
        print("[A3] 标记 count=9 已落盘 ✓", flush=True)
    else:
        FAILS.append("标记 count=9 落盘失败")
    dev.s.close()
    _report("阶段1")
    if FAILS:
        print("[A3] >>> 阶段1 有失败项，请先排查再继续。", flush=True)
        sys.exit(1)
    print("[A3] >>> 请拔掉 USB，等 2 秒，再插回（模拟真实掉电）。"
          "插好后告诉我，我跑阶段2验证持久化。", flush=True)
    sys.exit(0)


def run_phase2(port_arg):
    """阶段2：用户拔插后重连，验证 count=9 跨掉电存活 + DEFAULT 落盘。"""
    print("[A3] 等待设备重枚举（你插回后自动连）……", flush=True)
    dev = None
    deadline = time.time() + 60.0
    while time.time() < deadline:
        mark()
        for p in find_ports():
            s2, e2 = open_guarded(p.device, 6.0)
            if s2 is None:
                continue
            try:
                time.sleep(0.8); mark(); s2.reset_input_buffer(); s2.write(b"IDN?\r\n")
                t0 = time.time(); idn = ""
                while time.time() - t0 < 1.5:
                    mark()
                    r = s2.readline().decode(errors="ignore").strip()
                    if r: idn = r; break
                if "v2.0" in idn:
                    dev = Dev(s2)
                    break
            except Exception:
                pass
            try: s2.close()
            except Exception: pass
        if dev:
            break
        time.sleep(0.5)
    if dev is None:
        sys.exit("[A3] 阶段2：60s 内未检测到设备，请确认已插回。")
    print(f"[A3] 已重连 {dev.s.port}", flush=True)
    idn = dev.q("IDN?")
    if "v2.0" not in idn:
        FAILS.append(f"IDN? {idn!r}")

    # D（掉电持久化）：重连后 GET 应见标记 count=9
    after_reboot = dev.cfg_get()
    if after_reboot and after_reboot["states"]["busy"]["sound"]["count"] == MARKER:
        print(f"[A3] D. 断电后 count={MARKER}（配置从闪存恢复）✓", flush=True)
    else:
        FAILS.append(f"D. 断电后 count 应={MARKER}，实得 {after_reboot and after_reboot['states']['busy']['sound']['count']}")

    # E：收尾 DEFAULT 落盘 + 重载验证
    dev.q("CONFIG DEFAULT")
    after_def = dev.cfg_get()
    if after_def and after_def["states"]["busy"]["sound"]["count"] == 3:
        print("[A3] E. DEFAULT -> count=3 ✓", flush=True)
    else:
        FAILS.append(f"E. DEFAULT 后 count 应=3，实得 {after_def and after_def['states']['busy']['sound']['count']}")
    dev.q("CONFIG LOAD")
    after_def_load = dev.cfg_get()
    if after_def_load and after_def_load["states"]["busy"]["sound"]["count"] == 3:
        print("[A3] E. DEFAULT+LOAD -> count=3（默认已落盘）✓", flush=True)
    else:
        FAILS.append(f"E. DEFAULT 后 LOAD count 应=3，实得 {after_def_load and after_def_load['states']['busy']['sound']['count']}")
    dev.s.close()
    _report("阶段2（掉电持久化）")
    sys.exit(1 if FAILS else 0)


def run_auto(port_arg):
    """原自动流程（含软件 REBOOT，保留备用；本环境重枚举不可靠）。"""
    port = port_arg or _first_port()
    dev = _open_dev(port)
    _flash_check(dev)
    _bc_roundtrip(dev)
    # D. REBOOT 真·掉电模拟
    dev.q(f'CONFIG SET {{"version":1,"states":{{"busy":{{"sound":{{"count":{MARKER}}}}}}}}}')
    dev.q("REBOOT")
    try:
        dev.s.close()
    except Exception:
        pass
    print("[A3] D. 已发 REBOOT，等待设备重枚举（≤20s）……", flush=True)
    time.sleep(0.3)
    dev2 = reconnect()
    if dev2 is None:
        FAILS.append("D. REBOOT 后设备未重枚举，无法验证重启持久化")
    else:
        after_reboot = dev2.cfg_get()
        if not after_reboot or after_reboot["states"]["busy"]["sound"]["count"] != MARKER:
            FAILS.append(f"D. 重启后 count 应={MARKER}（持久化），实得 {after_reboot and after_reboot['states']['busy']['sound']['count']}")
        else:
            print(f"[A3] D. 重启后 count={MARKER}（配置从闪存恢复）✓", flush=True)
        dev2.q("CONFIG DEFAULT")
        dev2.q("CONFIG LOAD")
        print("[A3] E. 已 DEFAULT+LOAD，设备留回默认配置", flush=True)
        dev2.s.close()
    if not dev2:
        try:
            dev.q("CONFIG DEFAULT")
        except Exception:
            pass
    _report("A3 验收（auto）")
    sys.exit(1 if FAILS else 0)


def _first_port():
    ports = find_ports()
    if not ports:
        sys.exit(f"没找到设备 (VID:PID={HWID_KEY})。请确认已刷入 A3 固件并插好 USB。")
    return ports[0].device


def _report(tag):
    print(f"\n==== {tag} 结果 ====", flush=True)
    if FAILS:
        for f in FAILS:
            print("  FAIL  " + f, flush=True)
        print("\n[A3] 结果：FAIL", flush=True)
    else:
        print("  全部通过", flush=True)
        print("\n[A3] 结果：PASS", flush=True)


def main():
    start_watchdog(35.0)
    ap = argparse.ArgumentParser()
    ap.add_argument("--port")
    ap.add_argument("--mode", choices=["auto", "phase1", "phase2"], default="auto")
    args = ap.parse_args()
    if args.mode == "phase1":
        run_phase1(args.port)
    elif args.mode == "phase2":
        run_phase2(args.port)
    else:
        run_auto(args.port)


if __name__ == "__main__":
    main()
