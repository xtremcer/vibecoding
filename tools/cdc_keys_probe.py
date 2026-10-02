#!/usr/bin/env python3
"""按键手动验收探针 —— 通过 CDC 的 KEYMON 回声确认物理按键「被扫到、落到哪个槽、对应哪只脚」。

固件需 v2.0+ 且已实现 KEYMON ON/OFF（默认关，不影响正常 HID 上报）。
助手（看不到 HID 键码）靠这个脚本在 CDC 串口的回声里确认用户的实体按键。

用法：
    python tools/cdc_keys_probe.py [--port COMx] [--timeout 20] [--expect "PTT:2 MACRO2:1 MACRO4:4"]

流程：
  1. 按 VID/PID(0xCAFE:0x4A10) 自动找 CDC 串口（或 --port 指定）；
  2. 发 IDN? 探活、KEYMON ON 开回声；
  3. 在 timeout 内轮询 "KEY <NAME> P<pin> DOWN/UP"；
  4. 结束列出实际扫到的 (槽,脚)，并对照 --expect 给 PASS/FAIL。

--expect 格式：空格分隔的 "槽名:脚号"，例如 "PTT:2 MACRO2:1 MACRO4:4"。
只列你打算按的键即可；没按到的会被报为 MISSING，按到的但不在 expect 里会报为 UNEXPECTED。
"""
import argparse
import re
import sys
import time

try:
    import serial
    import serial.tools.list_ports
except ImportError:
    sys.exit("缺少 pyserial，请先执行：  pip install pyserial")

VID, PID, BAUD = 0xCAFE, 0x4A10, 115200
HWID_KEY = f"{VID:04X}:{PID:04X}"

LINE_RE = re.compile(r"^KEY\s+(\S+)\s+P(\d+)\s+(DOWN|UP)$", re.IGNORECASE)


def find_ports():
    return [p for p in serial.tools.list_ports.comports()
            if HWID_KEY in f"{p.hwid} {p.manufacturer or ''} {p.product or ''} "
                           f"{p.description or ''}".upper()]


def parse_expect(s):
    out = {}
    for tok in s.split():
        if ":" not in tok:
            continue
        name, pin = tok.rsplit(":", 1)
        try:
            out[name.upper()] = int(pin)
        except ValueError:
            pass
    return out


def main():
    ap = argparse.ArgumentParser(description="KEYMON 按键验收探针")
    ap.add_argument("--port", help="显式指定串口，如 COM3 / /dev/ttyACM0")
    ap.add_argument("--timeout", type=float, default=20.0, help="轮询时长（秒）")
    ap.add_argument("--expect", default="PTT:2 MACRO2:1 MACRO4:4",
                    help='期望按到的 "槽:脚" 列表，空格分隔')
    args = ap.parse_args()

    expect = parse_expect(args.expect)

    if args.port:
        port = args.port
    else:
        ports = find_ports()
        if not ports:
            sys.exit(f"没找到设备 (VID:PID={HWID_KEY})。请确认已刷入固件并插好 USB。")
        port = ports[0].device

    print(f"[probe] 打开 {port} @ {BAUD} ...")
    s = serial.Serial(port, BAUD, timeout=0.3)
    time.sleep(1.0)              # 等 USB 枚举稳定
    s.reset_input_buffer()

    # ---- 探活 ----
    s.write(b"IDN?\r\n")
    t0 = time.time()
    idn = ""
    while time.time() - t0 < 1.0:
        line = s.readline().decode("ascii", errors="ignore").strip()
        if line:
            idn = line
            break
    if "v2.0" not in idn:
        print(f"[probe] 警告：IDN? 回 {idn!r}（非预期 v2.0），继续但可能版本不对。")
    else:
        print(f"[probe] 设备在线：{idn}")

    # ---- 开 KEYMON 回声 ----
    s.write(b"KEYMON ON\r\n")
    t0 = time.time()
    while time.time() - t0 < 1.0:
        line = s.readline().decode("ascii", errors="ignore").strip()
        if line in ("OK", "ERR 1"):
            print(f"[probe] KEYMON ON -> {line}")
            break

    print(f"[probe] 请在 {args.timeout:.0f}s 内依次按下/松开接线好的按键……")

    # (槽名) -> 脚号（以最后一次 DOWN 的脚为准）；只记录有 DOWN 的
    seen = {}
    start = time.time()
    last = start
    try:
        while time.time() - start < args.timeout:
            line = s.readline().decode("ascii", errors="ignore").strip()
            if not line:
                continue
            m = LINE_RE.match(line)
            if not m:
                continue
            name, pin, dirn = m.group(1).upper(), int(m.group(2)), m.group(3).upper()
            if dirn == "DOWN":
                seen[name] = pin
                print(f"  扫到  {name} @ P{pin}  DOWN")
                last = time.time()
            else:
                print(f"  扫到  {name} @ P{pin}  UP")
    except KeyboardInterrupt:
        print("\n[probe] 用户中断，提前结束。")

    # ---- 关回声（恢复默认关状态，免得串口里一直冒 KEY 行）----
    s.write(b"KEYMON OFF\r\n")
    s.close()

    # ---- 对照 expect ----
    print("\n==== 验收结果 ====")
    ok = True
    for name, pin in expect.items():
        if name not in seen:
            print(f"  MISSING   {name} @ P{pin}   （没扫到 —— 检查接线/是否被禁用）")
            ok = False
        elif seen[name] != pin:
            print(f"  PIN MISMATCH  {name} 期望 P{pin} 实得 P{seen[name]}  （接线脚与配置不一致）")
            ok = False
        else:
            print(f"  OK        {name} @ P{pin}   按下被扫到且脚一致")
    for name, pin in seen.items():
        if name not in expect:
            print(f"  UNEXPECTED {name} @ P{pin}   （按到了但不在 expect 列表）")
            # 不在 expect 里的不判失败，仅提示

    print("\n[probe] KEYMON 已 OFF，结果：", "PASS" if ok else "FAIL")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
