#!/usr/bin/env python3
"""State Executor（状态执行器）验收脚本 —— 配合固件 v1.9+ 使用。

逐条下发 SET STATE / STATE?，并对错误码做断言，由你用眼睛看灯、用耳朵听提示音。

验收要点（对应 doc/12 §5.5 + firmware/manifest.md v1.9）：
  1. 上电：三灯全亮（boot_led=ALL_ON），STATE? = NONE
  2. SET STATE BUSY  → busy 灯亮（其余灭），不出声
  3. SET STATE IDLE  → idle 灯常亮；响 **BUSY 的 END 提示音 ×3**（= 任务完成提示）
  4. SET STATE AUTH  → plan(GP27) 灯 300/300 闪；响 AUTH 的 START 提示音 ×3（催授权）
  5. 错误码：SET STATE FOO → ERR 4；SET STATE（缺参）→ ERR 5；SET（缺参）→ ERR 5

用法：
    python tools/cdc_state_selftest.py
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

FAILS = []


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


def expect(s, line, want, note=""):
    """发一条指令并断言响应。"""
    got = send(s, line)
    ok = (got == want)
    if not ok:
        FAILS.append(f"{line!r} 期望 {want!r} 实得 {got!r}")
    print(f"   [{'PASS' if ok else 'FAIL'}] {note or want}")
    return got


def hold(s, sec, label=""):
    """保持连接并心跳（规避设备 5s 看门狗），期间你可以看灯/听音。"""
    if label:
        print(f"  保持 {sec:.0f}s：{label}")
    t0 = time.time()
    while time.time() - t0 < sec:
        time.sleep(2.0)
        send(s, "IDN?", quiet=True)


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
        print("\n=== 0. 身份 / 初始态 ===")
        expect(s, "IDN?", "vibecoding-mate mic v1.9", "固件版本应为 v1.9")
        expect(s, "STATE?", "STATE=NONE", "未收到 SET STATE 前为 NONE；此时应三灯全亮(ALL_ON)")

        print("\n=== 1. BUSY：灯亮，不出声 ===")
        print("  期望：busy 灯亮(其余灭)，**安静**")
        expect(s, "SET STATE BUSY", "OK", "切到 BUSY")
        expect(s, "STATE?", "STATE=BUSY", "回读 BUSY")
        hold(s, 3, "看 busy 灯、确认没声音")

        print("\n=== 2. BUSY→IDLE：响 BUSY 的 END 完成提示音 ×3 ===")
        print("  期望：idle 灯常亮；响 523/659/784 三音，共 3 遍，每遍间隔 1s")
        expect(s, "SET STATE IDLE", "OK", "切到 IDLE")
        expect(s, "STATE?", "STATE=IDLE", "回读 IDLE")
        hold(s, 6, "听完成提示音（约 3 遍）")

        print("\n=== 3. IDLE→AUTH：响 AUTH 的 START 催授权 ×3 ===")
        print("  期望：plan(GP27) 灯 300/300 闪烁；响 880/660 两音，共 3 遍")
        expect(s, "SET STATE AUTH", "OK", "切到 AUTH")
        hold(s, 6, "看 plan 灯闪烁 + 听催授权音")

        print("\n=== 4. AUTH→BUSY：应安静（AUTH 是 START，BUSY 是 END 但 END 被 START 顶掉）===")
        print("  期望：busy 灯亮，**安静**（授权已解决，不需要声音）")
        expect(s, "SET STATE BUSY", "OK", "切回 BUSY")
        hold(s, 3, "确认安静")

        print("\n=== 5. 错误码 ===")
        expect(s, "SET STATE FOO", "ERR 4", "枚举值非法 → ERR 4")
        expect(s, "SET STATE", "ERR 5", "缺参数 → ERR 5")
        expect(s, "SET", "ERR 5", "缺参数 → ERR 5")
        expect(s, "SET BEEP ON", "ERR 4", "SET 后不是 STATE → ERR 4")
        expect(s, "FOOBAR", "ERR 1", "未知指令 → ERR 1")

        print("\n=== 6. 交还本地控制 ===")
        expect(s, "RESET", "OK", "清接管标志，PTT 本地行为恢复")

    print("\n" + "=" * 60)
    if FAILS:
        print(f"有 {len(FAILS)} 项不符合预期：")
        for f in FAILS:
            print("  - " + f)
        print("（若只是听/看的环节存疑，请人工复核后再判定）")
    else:
        print("全部指令响应符合预期。请确认上面每一轮的灯与声音是否也符合描述。")
    print("=" * 60)


if __name__ == "__main__":
    main()
