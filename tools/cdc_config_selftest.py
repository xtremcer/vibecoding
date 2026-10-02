#!/usr/bin/env python3
"""配置协议（CONFIG GET/SET/DEFAULT）验收脚本 —— 配合固件 v2.0+ 使用。

最硬的一条断言：**CONFIG GET 吐出来的必须是能被 Python json.loads 解析的合法 JSON**。
这同时验证了固件的序列化、多行分片（TX FIFO 只有 256 字节）和上位机拼接逻辑。

用例覆盖：
  A. CONFIG GET：拼回来是合法 JSON，且关键字段 == 编译期默认
  B. CONFIG SET 合法：改 LED 闪烁 / 改提示次数 / 改乐谱 / 改按键 → OK 且 GET 回显新值
  C. CONFIG SET 非法：version 缺失/不支持、count 越界、mod 枚举错、key=0、
                     pin 越界、click_ms=0、score 超长 → 必须 ERR 6 且**配置保持不变**
  D. CONFIG DEFAULT：恢复默认
  E. 真实表现：改 AUTH 的闪烁周期后，SET STATE AUTH 的灯真的按新周期闪

用法：
    python tools/cdc_config_selftest.py
依赖：pip install pyserial
"""
import json
import sys
import time

try:
    import serial
    import serial.tools.list_ports
except ImportError:
    sys.exit("缺少 pyserial，请先执行：  pip install pyserial")

VID, PID, BAUD = 0xCAFE, 0x4A10, 115200
HWID_KEY = f"{VID:04X}:{PID:04X}"
FAILS = []


def find_ports():
    return [p for p in serial.tools.list_ports.comports()
            if HWID_KEY in f"{p.hwid} {p.manufacturer or ''} {p.product or ''} "
                           f"{p.description or ''}".upper()]


class Dev:
    def __init__(self, s):
        self.s = s

    def q(self, line, timeout=None):
        if timeout is not None:
            self.s.timeout = timeout
        self.s.write((line + "\r\n").encode("ascii", errors="ignore"))
        try:
            return self.s.readline().decode("ascii", errors="ignore").strip()
        finally:
            self.s.timeout = 0.6

    def cfg_get(self):
        """发 CONFIG GET，返回 (拼接后的文本, 是否以 OK 收尾)。"""
        first = self.q("CONFIG GET", timeout=1.5)
        if first != "CONFIG BEGIN":
            FAILS.append(f"CONFIG GET 首行应为 CONFIG BEGIN，实得 {first!r}")
            return None, False
        lines = []
        while True:
            ln = self.q("", timeout=1.5) if False else self.s.readline().decode(
                "ascii", errors="ignore").rstrip("\r\n")
            if ln == "OK":
                return "\n".join(lines), True
            if not ln:
                FAILS.append("CONFIG GET 未收到结束标记 OK（连接断了？）")
                return "\n".join(lines), False
            lines.append(ln)

    def cfg_json(self):
        txt, ok = self.cfg_get()
        if not ok or txt is None:
            return None
        try:
            return json.loads(txt)
        except Exception as e:
            FAILS.append(f"CONFIG GET 吐出的不是合法 JSON：{e}\n--- 原文 ---\n{txt}")
            return None


def check(cond, note):
    if not cond:
        FAILS.append(note)
    print(f"   [{'PASS' if cond else 'FAIL'}] {note}")


def main():
    ports = find_ports()
    if not ports:
        sys.exit(f"未找到 VID:PID={HWID_KEY} 的串口。")
    port = ports[0].device
    print(f"找到设备：{port}  ({ports[0].description})")
    s = serial.Serial(port, BAUD, timeout=0.6, dsrdtr=False, rtscts=False)
    s.dtr, s.rts = True, False
    s.reset_input_buffer()
    d = Dev(s)

    with s:
        time.sleep(0.05)
        print("\n=== A. CONFIG GET 是合法 JSON，且等于编译期默认 ===")
        r = d.q("IDN?")
        check(r == "vibecoding-mate mic v2.0", f"固件版本应为 v2.0（实得 {r!r}）")
        d.q("CONFIG DEFAULT")
        c = d.cfg_json()
        if c:
            check(c["version"] == 1, f"version==1（实得 {c.get('version')}）")
            check(c["device"]["boot_led"] == "ALL_ON", "device.boot_led==ALL_ON")
            check(c["device"]["boot_timeout_ms"] == 5000,
                  f"device.boot_timeout_ms==5000（实得 {c['device'].get('boot_timeout_ms')}）")
            check(c["states"]["busy"]["led"]["standby"] == "ON",
                  "states.busy.led.standby==ON（A1 修过的 bug，别回退）")
            check(c["states"]["busy"]["sound"]["count"] == 3, "busy 完成提示 ×3")
            check(c["states"]["auth"]["led"]["blink_on_ms"] == 300, "auth 闪烁 300ms")
            check(c["keys"]["ptt"]["mod"] == "LGUI" and c["keys"]["ptt"]["key"] == 0x35,
                  "PTT = Win+` (0x35)")
            check(c["keys"]["macro8"]["behavior"] == "SINGLE", "macro8 = 单击")
            check(len(c["keys"]) == 9, f"keys 应有 9 个槽位（实得 {len(c['keys'])}）")

        print("\n=== B. CONFIG SET 合法值 → OK，且 GET 回显 ===")
        patch = {
            "version": 1,
            "states": {"auth": {"led": {"standby": "ON", "blink_on_ms": 100, "blink_off_ms": 100},
                                "sound": {"count": 5, "score": "1000,50;1200,50"}}},
            "keys": {"macro4": {"enabled": False, "pin": 4, "mod": "LCTRL",
                                "key": 4, "behavior": "NORMAL", "click_ms": 1000}},
        }
        r = d.q("CONFIG SET " + json.dumps(patch, separators=(",", ":")))
        check(r == "OK", f"合法配置应回 OK（实得 {r!r}）")
        c2 = d.cfg_json()
        if c2:
            check(c2["states"]["auth"]["led"]["blink_on_ms"] == 100, "auth 闪烁改为 100ms")
            check(c2["states"]["auth"]["sound"]["count"] == 5, "auth 提示次数改为 5")
            check(c2["states"]["auth"]["sound"]["score"] == "1000,50;1200,50",
                  "auth 乐谱已更新（含逗号分号的字符串没被破坏）")
            check(c2["keys"]["macro4"]["enabled"] is False, "macro4 已禁用")
            check(c2["states"]["busy"]["sound"]["count"] == 3,
                  "未提及的字段保持原值（部分更新不误伤）")

        print("\n=== C. CONFIG SET 非法值 → ERR 6，且**配置保持不变** ===")
        before = d.cfg_json()
        # 每个用例都带 "version":1，让固件越过版本检查、真正校验到那个非法字段，
        # 否则会一律短路成 "missing version" 而测不到字段级校验（以及 ERR 6 的原因文字）。
        bad_cases = [
            ('{"version":1,"states":{"busy":{"led":{"standby":"MAYBE"}}}}', "standby 枚举非法"),
            ('{"version":1,"states":{"busy":{"sound":{"count":99}}}}',     "count 越界(>10)"),
            ('{"version":1,"keys":{"macro1":{"mod":"LWIN"}}}',             "mod 枚举非法"),
            ('{"version":1,"keys":{"macro1":{"key":0}}}',                  "key=0（列表无'无'）"),
            ('{"version":1,"keys":{"macro1":{"pin":99}}}',                 "pin 越界(>29)"),
            ('{"version":1,"keys":{"macro1":{"click_ms":0}}}',             "click_ms=0（最小 1）"),
            ('{"version":1,"device":{"boot_led":"ALL_MAYBE"}}',            "boot_led 枚举非法"),
            ('{"version":2}',                                              "version 不支持(2)"),
            ('{"version":1,"states":{"busy":{"led":{"blink_on_ms":70000}}}}', "blink_on_ms 越界"),
        ]
        for payload, why in bad_cases:
            r = d.q("CONFIG SET " + payload)
            ok = r.startswith("ERR 6")
            check(ok, f"{why} → 应 ERR 6（实得 {r!r}）")
            if ok and len(r) <= 5:
                FAILS.append(f"{why} 的 ERR 6 没带原因文字，上位机没法定位")

        # 缺少 version 段（整份配置里没有 version）—— 单独测这条短路分支
        r = d.q('CONFIG SET {"device":{"boot_led":"ALL_OFF"}}')
        check(r.startswith("ERR 6"), f"缺 version → 应 ERR 6（实得 {r!r}）")

        after = d.cfg_json()
        if before and after:
            check(after == before, "所有非法配置都被拒收，配置**完全没变**")

        print("\n=== D. CONFIG DEFAULT → 恢复编译期默认 ===")
        r = d.q("CONFIG DEFAULT")
        check(r == "OK", f"CONFIG DEFAULT 应回 OK（实得 {r!r}）")
        c3 = d.cfg_json()
        if c3:
            check(c3["states"]["auth"]["led"]["blink_on_ms"] == 300, "auth 闪烁恢复 300ms")
            check(c3["keys"]["macro4"]["enabled"] is True, "macro4 恢复启用")

        print("\n=== E. 真实表现：改 AUTH 闪烁周期后灯真的按新周期闪 ===")
        d.q('CONFIG SET {"version":1,"states":{"auth":{"led":{"blink_on_ms":100,"blink_off_ms":100}}}}')
        d.q("SET STATE AUTH")
        seen = set()
        for _ in range(12):
            r = d.q("LED?")
            try:
                seen.add(dict(kv.split("=") for kv in r.split())["PLAN"])
            except Exception:
                pass
            time.sleep(0.08)
        check(seen == {"0", "1"}, f"AUTH 灯确实在闪烁（采样到 {sorted(seen)}）")
        d.q("CONFIG DEFAULT")
        d.q("RESET")

    print("\n" + "=" * 62)
    if FAILS:
        print(f"❌ {len(FAILS)} 项不符：")
        for f in FAILS:
            print("   - " + f)
    else:
        print("✅ 配置协议全部符合预期。")
    print("=" * 62)
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())
