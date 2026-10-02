#!/usr/bin/env python3
"""A2b 验收脚本 —— 按键配置真正生效（keys[] 成为唯一真值来源）。

A2b 之前：按键映射是 buttons.c 里的编译期 btn_configs[]，CONFIG SET keys 只「存着不生效」。
A2b 之后：buttons.c 每帧从 cfg_get()->keys[] 实时读，CONFIG SET keys 下一帧即生效；
          PTT 脚（keys[0].pin）也可配，busy 灯 / 板载 LED 的本地逻辑改读 buttons_ptt_pin()。

本脚本在 CDC 侧能验证的部分：
  A. 默认 keys 与旧 btn_configs 映射逐字段一致（"真值来源搬了家"但行为没变）
  B. CONFIG SET keys 合法 → OK，且 GET 回显新值（含禁用 / 改 pin / 改 mod / 改 behavior / 改 click_ms）
  C. PTT 脚实时改配冒烟：把 keys[0].pin 改到另一个安全脚，确认固件仍响应 IDN?（pin-sync 重配 GPIO 没崩），
     且 GET 回显新脚；再改回并 DEFAULT 复位
  D. CONFIG DEFAULT 恢复全部默认

注意：真正的「按下按钮 → 电脑收到新键码」需要人手按按钮验证（HID 上报无法在 CDC 脚本里断言），
      本脚本只保证配置层 + 运行时重配的健壮性与默认值正确。

用法：python tools/cdc_a2b_selftest.py   （依赖 pyserial）
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
        first = self.q("CONFIG GET", timeout=1.5)
        if first != "CONFIG BEGIN":
            FAILS.append(f"CONFIG GET 首行应为 CONFIG BEGIN，实得 {first!r}")
            return None
        lines = []
        while True:
            ln = self.s.readline().decode("ascii", errors="ignore").rstrip("\r\n")
            if ln == "OK":
                return "\n".join(lines)
            if not ln:
                FAILS.append("CONFIG GET 未收到结束标记 OK")
                return "\n".join(lines)
            lines.append(ln)

    def cfg_json(self):
        txt = self.cfg_get()
        if txt is None:
            return None
        try:
            return json.loads(txt)
        except Exception as e:
            FAILS.append(f"CONFIG GET 不是合法 JSON：{e}\n--- 原文 ---\n{txt}")
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
        d.q("CONFIG DEFAULT")   # 从干净默认开始

        print("\n=== A. 默认 keys 与旧 btn_configs 映射一致（真值来源搬了家，行为不变）===")
        c = d.cfg_json()
        if c:
            k = c["keys"]
            check(k["ptt"]["pin"] == 2 and k["ptt"]["mod"] == "LGUI" and k["ptt"]["key"] == 0x35
                  and k["ptt"]["behavior"] == "NORMAL", "ptt = GP2, LGUI+`(0x35), NORMAL")
            check(k["macro1"]["pin"] == 0 and k["macro1"]["key"] == 0x28 and k["macro1"]["mod"] == "NONE",
                  "macro1 = GP0 Enter(0x28)，无修饰键")
            check(k["macro3"]["pin"] == 3 and k["macro3"]["key"] == 0x29, "macro3 = GP3 Esc(0x29)")
            check(k["macro4"]["pin"] == 4 and k["macro4"]["mod"] == "LCTRL" and k["macro4"]["key"] == 0x04,
                  "macro4 = GP4 Ctrl+A")
            check(k["macro6"]["pin"] == 20 and k["macro6"]["mod"] == "LCTRL" and k["macro6"]["key"] == 0x19,
                  "macro6 = GP20 Ctrl+V")
            check(k["macro8"]["pin"] == 13 and k["macro8"]["mod"] == "LGUI" and k["macro8"]["key"] == 0x31
                  and k["macro8"]["behavior"] == "SINGLE" and k["macro8"]["click_ms"] == 1000,
                  "macro8 = GP13 Win+\\\\ SINGLE click_ms=1000")
            check(all(k[n]["enabled"] for n in k), "9 个键默认全部 enabled")

        print("\n=== B. CONFIG SET keys 合法 → OK，且 GET 回显新值 ===")
        patch = {"version": 1, "keys": {"macro1": {"enabled": False, "pin": 3, "mod": "LALT",
                                                    "key": 0x2C, "behavior": "SINGLE", "click_ms": 500}}}
        r = d.q("CONFIG SET " + json.dumps(patch, separators=(",", ":")))
        check(r == "OK", f"合法 keys 应回 OK（实得 {r!r}）")
        c = d.cfg_json()
        if c:
            m1 = c["keys"]["macro1"]
            check(m1["enabled"] is False, "macro1 已禁用（按下不再上报）")
            check(m1["pin"] == 3 and m1["mod"] == "LALT" and m1["key"] == 0x2C, "macro1 脚/修饰键/键码已更新")
            check(m1["behavior"] == "SINGLE" and m1["click_ms"] == 500, "macro1 行为/click_ms 已更新")
            check(c["keys"]["ptt"]["pin"] == 2, "未提及的 ptt 保持 GP2（部分更新不误伤）")

        print("\n=== C. PTT 脚实时改配冒烟（pin-sync 重配 GPIO 不能崩）===")
        before = d.cfg_json()
        ptt_before = before["keys"]["ptt"]["pin"] if before else 2
        # 把 PTT 脚改到一个安全的其它脚（这里用 GP3，避免踩音频/LED/蜂鸣器）
        r = d.q('CONFIG SET {"version":1,"keys":{"ptt":{"pin":3}}}')
        check(r == "OK", f"改 PTT 脚应回 OK（实得 {r!r}）")
        ridn = d.q("IDN?")   # 关键：pin 变更触发 GPIO 重配，固件必须还活着
        check(ridn == "vibecoding-mate mic v2.0", f"改 PTT 脚后固件仍响应 IDN?（实得 {ridn!r}）")
        c = d.cfg_json()
        if c:
            check(c["keys"]["ptt"]["pin"] == 3, "GET 回显 ptt.pin==3")
        # 改回并确认仍活着
        d.q('CONFIG SET {"version":1,"keys":{"ptt":{"pin":2}}}')
        ridn2 = d.q("IDN?")
        check(ridn2 == "vibecoding-mate mic v2.0", f"PTT 脚改回后固件仍响应 IDN?（实得 {ridn2!r}）")
        _ = ptt_before  # 仅记录，复位见 D

        print("\n=== D. CONFIG DEFAULT 恢复全部默认 ===")
        r = d.q("CONFIG DEFAULT")
        check(r == "OK", f"CONFIG DEFAULT 应回 OK（实得 {r!r}）")
        c = d.cfg_json()
        if c:
            check(c["keys"]["ptt"]["pin"] == 2, "ptt 脚恢复 GP2")
            m1 = c["keys"]["macro1"]
            # 注意：macro1 = Enter（GP0），默认无修饰键（mod=NONE），与旧 btn_configs[1]={0,0,0x28,...} 一致
            check(m1["enabled"] is True and m1["pin"] == 0 and m1["mod"] == "NONE"
                  and m1["key"] == 0x28 and m1["behavior"] == "NORMAL" and m1["click_ms"] == 1000,
                  "macro1 完全恢复默认（Enter，无修饰键）")
            check(all(c["keys"][n]["enabled"] for n in c["keys"]), "9 个键恢复全部 enabled")

    print("\n" + "=" * 62)
    if FAILS:
        print(f"❌ {len(FAILS)} 项不符：")
        for f in FAILS:
            print("   - " + f)
    else:
        print("✅ A2b（按键配置真正生效）验收通过。")
        print("   （真正『按按钮→电脑收新键』需人手按按钮验证；此处已确认配置层 + 运行时重配健壮）")
    print("=" * 62)
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())
