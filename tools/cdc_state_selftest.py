#!/usr/bin/env python3
"""State Executor（状态执行器）验收脚本 —— 配合固件 v1.9+ 使用。

逐条下发 SET STATE / STATE?，并对**响应 + 三灯物理电平**做断言，只有声音需要你用耳朵听。

为什么要断言灯：v1.9 首版就是靠 `LED?` 回读才发现两个 bug 的——
指令层全过（每个 SET 都回 OK），但 BUSY 态三灯全灭、上电"三灯全亮"实际只有两盏。
所以本脚本一律用 `LED?` 回读真实电平，不靠眼睛。

验收要点（对应 doc/12 §5.5 + firmware/manifest.md v1.9）：
  0. 上电 5s 内：三灯全亮（boot_led=ALL_ON），STATE? = NONE
     超过 5s 仍没收到 SET STATE → 交还本地控制（idle 亮，PTT 可用）
  1. SET STATE BUSY  → BUSY 灯亮（其余灭），不出声
  2. SET STATE IDLE  → IDLE 灯常亮；响 BUSY 的 END 完成提示音 ×3
  3. SET STATE AUTH  → PLAN(GP27) 灯 300/300 闪；响 AUTH 的 START 催授权 ×3
  4. AUTH→BUSY       → BUSY 灯亮，安静（授权已解决）
  5. 错误码：SET STATE FOO→ERR 4；SET STATE(缺参)→ERR 5；SET→ERR 5；FOOBAR→ERR 1
  6. RESET           → 立刻交还本地（idle 亮，STATE?=NONE）

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


class Dev:
    def __init__(self, s):
        self.s = s

    def q(self, line):
        self.s.write((line + "\r\n").encode("ascii", errors="ignore"))
        return self.s.readline().decode("ascii", errors="ignore").strip()

    def send(self, line, quiet=False):
        r = self.q(line)
        if not quiet:
            print(f"> {line}\n< {r if r else '(超时无响应)'}")
        return r

    def expect(self, line, want, note=""):
        got = self.send(line)
        ok = (got == want)
        if not ok:
            FAILS.append(f"{line!r} 期望 {want!r} 实得 {got!r}")
        print(f"   [{'PASS' if ok else 'FAIL'}] {note or want}")
        return got

    def led(self):
        """回读三灯真实物理电平 → (busy, plan, idle)。解析失败返回 None。"""
        r = self.q("LED?")
        try:
            d = dict(kv.split("=") for kv in r.split())
            return (d["BUSY"], d["PLAN"], d["IDLE"])
        except Exception:
            FAILS.append(f"LED? 解析失败：{r!r}")
            return None

    def expect_led(self, want, note=""):
        got = self.led()
        ok = (got == want)
        if not ok:
            FAILS.append(f"LED 期望 BUSY={want[0]} PLAN={want[1]} IDLE={want[2]} "
                         f"实得 {got}")
        print(f"   [{'PASS' if ok else 'FAIL'}] 灯 {note}")
        return got

    def expect_blink(self, idx, name, samples=10, gap=0.15):
        """断言第 idx 盏在闪（采样里 0/1 都出现过），其余两盏恒灭。"""
        seen = set()
        others = set()
        for _ in range(samples):
            v = self.led()
            if v is None:
                break
            seen.add(v[idx])
            others.add(v[1 - idx if idx != 1 else 0])
            others.add(v[2] if idx != 2 else 0)
            time.sleep(gap)
        ok_blink = (seen == {0, 1} or seen == {"0", "1"})
        ok_dark = others <= {0, "0"}
        if not ok_blink:
            FAILS.append(f"{name} 灯应在闪烁，采样只看到 {sorted(seen)}")
        if not ok_dark:
            FAILS.append(f"{name} 亮起时其余灯应恒灭，采样看到 {sorted(others)}")
        print(f"   [{'PASS' if (ok_blink and ok_dark) else 'FAIL'}] "
              f"{name} 灯 300/300 闪烁（采样 {samples}×{gap}s）")

    def hold(self, sec, label=""):
        """保持连接并心跳（规避设备 5s 看门狗），期间你可以听音。"""
        if label:
            print(f"  保持 {sec:.0f}s：{label}")
        t0 = time.time()
        while time.time() - t0 < sec:
            time.sleep(2.0)
            self.send("IDN?", quiet=True)

    def beep_seen(self, sec, label=""):
        """在 sec 秒内轮询 BEEP?，统计出现过几次 PLAYING（验证 count 重复）。"""
        bursts, was = 0, False
        t0 = time.time()
        while time.time() - t0 < sec:
            r = self.q("BEEP?")
            playing = "PLAYING" in r
            if playing and not was:
                bursts += 1
            was = playing
            self.q("IDN?")          # 心跳
            time.sleep(0.08)
        print(f"   {label}：检测到 {bursts} 段发声")
        return bursts


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

    d = Dev(s)
    with s:
        time.sleep(0.05)
        print("\n=== 0. 身份 / 初始态 ===")
        d.expect("IDN?", "vibecoding-mate mic v1.9", "固件版本应为 v1.9")
        d.expect("STATE?", "STATE=NONE", "未收到 SET STATE 前为 NONE")

        print("\n=== 1. BUSY：BUSY 灯亮，不出声 ===")
        d.expect("SET STATE BUSY", "OK", "切到 BUSY")
        d.expect("STATE?", "STATE=BUSY", "回读 BUSY")
        d.expect_led(("1", "0", "0"), "BUSY 亮 / 其余灭")
        d.hold(3, "确认没声音（BUSY 期间绝不骚扰）")

        print("\n=== 2. BUSY→IDLE：响 BUSY 的 END 完成提示音 ×3 ===")
        d.expect("SET STATE IDLE", "OK", "切到 IDLE")
        d.expect_led(("0", "0", "1"), "IDLE 亮 / 其余灭")
        n = d.beep_seen(6, "完成提示音 523/659/784")
        if n < 2:
            FAILS.append(f"完成提示音应重复约 3 遍，只检测到 {n} 段")

        print("\n=== 3. IDLE→AUTH：响 AUTH 的 START 催授权 ×3，PLAN 灯闪 ===")
        d.expect("SET STATE AUTH", "OK", "切到 AUTH")
        d.expect_blink(1, "PLAN(GP27)")
        n = d.beep_seen(6, "催授权音 880/660")
        if n < 2:
            FAILS.append(f"催授权音应重复约 3 遍，只检测到 {n} 段")

        print("\n=== 4. AUTH→BUSY：应安静（授权已解决）===")
        d.expect("SET STATE BUSY", "OK", "切回 BUSY")
        d.expect_led(("1", "0", "0"), "BUSY 亮 / 其余灭")
        n = d.beep_seen(3, "此段应无声音")
        if n != 0:
            FAILS.append(f"AUTH→BUSY 应完全安静，却检测到 {n} 段发声")

        print("\n=== 5. 错误码 ===")
        d.expect("SET STATE FOO", "ERR 4", "枚举值非法 → ERR 4")
        d.expect("SET STATE", "ERR 5", "缺参数 → ERR 5")
        d.expect("SET", "ERR 5", "缺参数 → ERR 5")
        d.expect("SET BEEP ON", "ERR 4", "SET 后不是 STATE → ERR 4")
        d.expect("FOOBAR", "ERR 1", "未知指令 → ERR 1")

        print("\n=== 6. RESET：立刻交还本地控制 ===")
        d.expect("RESET", "OK", "清接管 + 清状态")
        d.expect("STATE?", "STATE=NONE", "状态回到 NONE")
        d.expect_led(("0", "0", "1"), "回 v1.8 手感：idle 亮，busy 交由 PTT")

    print("\n" + "=" * 62)
    if FAILS:
        print(f"❌ 有 {len(FAILS)} 项不符合预期：")
        for f in FAILS:
            print("   - " + f)
    else:
        print("✅ 指令、错误码、三灯物理电平全部符合预期。")
        print("   剩下只有「声音好不好听 / 次数是否合适」需要你用耳朵判断。")
    print("=" * 62)
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())
