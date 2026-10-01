# 11 · CDC 虚拟串口控制方案分析（可行性 / 技术路线 / 踩坑指南 / 上位机）

> 目标：让上位机软件通过 USB 下发指令，控制 3 颗扩展状态灯（GP26/27/28）和蜂鸣器（GP14）。
> 分支：`mic_ptt_led_key-m_BEEP-LED_CDCs`（自 `mic_ptt_led_key-m_BEEP-LED` @ `95e4e48` 拉出）
> 状态：**方案分析完成，代码未改动，等待拍板**

---

## 一、结论速览

**可行，推荐采用。** 但你的方案描述里有 **1 处致命误解、2 处需要修正、1 处数字偏保守**，动手前必须先对齐：

| # | 你的判断 | 核验结论 |
|---|---|---|
| 1 | "TinyUSB 原生支持 CDC，描述符复制粘贴即可" | ✅ 基本成立，但 `ITF_NUM_TOTAL`、`CONFIG_TOTAL_LEN` 两处必须同步改，漏一处**全设备完蛋** |
| 2 | "只开一个宏 + 主循环十几行，**完全不触碰已有代码**" | ⚠️ 不成立。要改 3 个文件：`tusb_config.h`、`usb_descriptors.c`、`main.c`（+新建 `cdc_cmd.c`）。都是**追加式**修改，但确实是动已有文件 |
| 3 | "CDC 部分写崩了，键盘和麦克风依然正常工作" | ❌ **最危险的误解**。配置描述符是**整体下发**的，长度/接口数/端点号错一个字节 → 主机拒绝整个配置 → **复合设备整体枚举失败，键盘和麦克风一起死** |
| 4 | "RP2040 有 8 个双向端点，资源充足" | 数字偏保守。SDK 实测 `hardware/structs/usb_dpram.h: USB_NUM_ENDPOINTS = 16`，即 **16 个端点号 × IN/OUT 各一**。当前只用 2 个，加 CDC 后 5 个，占 31% |

**最大风险不在固件，在 Windows 驱动缓存**——本项目有明确前科（见第四节）。

---

## 二、当前设备占用盘点（实测，取自 ELF 静态解码 `doc/07`）

### 接口

| 接口号 | 功能 | 类/子/协议 | 端点 |
|---|---|---|---|
| ITF0 | AudioControl | 01/01/20 | 无 |
| ITF1 | AudioStreaming | 01/02/20 | 0x81 iso IN, 294B @1ms |
| ITF2 | HID 键盘（Boot） | 03/01/01 | 0x82 int IN, 16B @10ms |

- IAD 覆盖 ITF0~ITF1；`ITF_NUM_TOTAL = 3`；配置描述符总长 **170 字节**（9 + 136 音频 + 25 HID）

### 端点（RP2040 上限 16 号 × IN/OUT）

| 端点 | 方向/类型 | 用途 | 大小 |
|---|---|---|---|
| 0x00/0x80 | 控制 | EP0 | 64B |
| 0x81 | IN / Isochronous | 音频 | 294B |
| 0x82 | IN / Interrupt | HID 键盘 | 16B |

---

## 三、新增 CDC 后的资源预算

### 接口（**必须在现有接口之后追加，不能插在中间**）

| 接口号 | 功能 | 端点 |
|---|---|---|
| ITF3 | CDC ACM Control | 0x83 int IN, 8B @16ms（notification） |
| ITF4 | CDC Data | 0x03 bulk OUT 64B + 0x84 bulk IN 64B |

- 新 IAD 覆盖 ITF3~ITF4；`ITF_NUM_TOTAL = 3 → 5`
- `CONFIG_TOTAL_LEN = 170 + TUD_CDC_DESC_LEN(66) = **236**`
  - 拆账：`TUD_CONFIG_DESC_LEN(9) + 音频(136) + TUD_HID_DESC_LEN(25) + TUD_CDC_DESC_LEN(66) = 236`
  - `TUD_CDC_DESC_LEN = 8+9+5+5+4+5+7+9+7+7 = 66`（SDK `src/device/usbd.h:229` 实测）

### 端点总量

5 个端点号（0x81 / 0x82 / 0x83 / 0x84 / 0x03），**占 16 个中的 31%**。✅ 完全够。

### 带宽（全速 12Mbps，每帧 ~1500B）

| 传输 | 每帧占用 |
|---|---|
| 音频 iso | 294 B/ms |
| HID int | 16 B / 10ms ≈ 1.6 B/ms |
| CDC notif | 8 B / 16ms ≈ 0.5 B/ms |
| **周期性合计** | **≈ 296 B/ms，占约 20%** |

Bulk（CDC 数据）用剩余带宽，不影响音频。✅

### RAM

CDC RX/TX FIFO 各 256B（我们将显式配置），相对 264KB SRAM 可忽略。✅

---

## 四、⚠️ 最大风险：Windows 驱动缓存（本项目有前科）

### 前科记录（`doc/07_根因分析_OptionC.md`）

- 复合固件第一次把 PID 从 `0x4A10` 改成 `0x4B10` 时，实测现象是：**HID 键盘能正常打字，但音频完全没声音**。
- 结论：固件描述符逐字节正确，问题在 Windows 对新 PID 建立了全新设备实例，音频子功能的 `usbaudio2.sys` 未绑定/未装好。

### 本次改动对 Windows 意味着什么

Windows 复合设备父驱动（`usbccgp`）为每个接口生成子节点，硬件 ID 形如：

```
USB\VID_0CAFE&PID_4A10&MI_00   ← 音频（现有）
USB\VID_0CAFE&PID_4A10&MI_01   ← 音频流（现有）
USB\VID_0CAFE&PID_4A10&MI_02   ← HID 键盘（现有）
USB\VID_0CAFE&PID_4A10&MI_03   ← CDC Control（新增）
USB\VID_0CAFE&PID_4A10&MI_04   ← CDC Data（新增）
```

### 因此我的建议（与 TinyUSB 官方惯例相反）

- **保持 `USB_PID = 0x4A10` 不变**。
  这样 MI_00/MI_01/MI_02 三个已有子节点的硬件 ID 一字不改，Windows 里已装好的 `usbaudio2.sys` / `hidusb.sys` **继续沿用缓存绑定**；只有新增的 MI_03/MI_04 走一次新驱动安装（`usbser.sys`，Win10 起系统自带，无需 INF）。
- **不要**按 TinyUSB 的 `_PID_MAP` 惯例去改 PID。TinyUSB 之所以那么建议，是因为它假设你的设备没有"已装好的音频驱动身份"这一包袱——**本项目恰恰有**。
- **CDC 必须追加在 HID 之后（ITF3/ITF4）**，不能插到 ITF2 位置。否则 HID 从 MI_02 变成 MI_04，Windows 会把它当新设备重装键盘，可能出现"键盘短暂失灵"。

### 万一音频又没了

```powershell
# 管理员 PowerShell：卸载本设备全部节点并删除驱动
Get-PnpDevice -PresentOnly | Where-Object { $_.InstanceId -match 'VID_0CAFE&PID_4A10' } |
  ForEach-Object { pnputil /remove-device $_.InstanceId }
# 然后拔插一次
```

---

## 五、技术路线（实施步骤）

### Step 0 — 建分支 ✅ 已完成
```
mic_ptt_led_key-m_BEEP-LED_CDCs  ← 自 95e4e48
```

### Step 1 — `src/tusb_config.h`（追加，不改已有值）
```c
#define CFG_TUD_CDC 1                    // 0 → 1（唯一必须改的已有宏）
#define CFG_TUD_CDC_EP_BUFSIZE 64        // 全速 bulk 最大包
#define CFG_TUD_CDC_RX_BUFSIZE 256       // 行缓冲足够放下最长指令
#define CFG_TUD_CDC_TX_BUFSIZE 256       // 回显缓冲
```

### Step 2 — `src/usb_descriptors.c`（追加式修改，4 处）
```c
enum {
    ITF_NUM_AUDIO_CONTROL = 0,
    ITF_NUM_AUDIO_STREAMING,
    ITF_NUM_HID,
    ITF_NUM_CDC,          // = 3（新增）
    ITF_NUM_CDC_DATA,     // = 4（新增，紧跟 Control）
    ITF_NUM_TOTAL         // 自动 = 5
};

#define EPNUM_CDC_NOTIF 0x83   // int IN
#define EPNUM_CDC_OUT   0x03   // bulk OUT
#define EPNUM_CDC_IN    0x84   // bulk IN

#define CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + CFG_TUD_AUDIO * TUD_AUDIO_MIC_TWO_CH_DESC_LEN \
                          + TUD_HID_DESC_LEN + TUD_CDC_DESC_LEN)   // = 236

// desc_configuration[] 末尾追加（HID 之后）：
TUD_CDC_DESCRIPTOR(ITF_NUM_CDC, 5, EPNUM_CDC_NOTIF, 8, EPNUM_CDC_OUT, EPNUM_CDC_IN, 64)

// string_desc_arr[] 追加：
"CDC Control",   // 5: CDC 接口字符串
```

### Step 3 — 新建 `src/cdc_cmd.h` / `src/cdc_cmd.c`（新模块，风格对齐 `buttons.c`）
```c
void cdc_cmd_init(void);   // 清空行缓冲
void cdc_cmd_task(void);   // 主循环调用，非阻塞
```
内部：行缓冲（64B，`\r` 或 `\n` 任一结束）→ 分词 → 指令表分发 → `tud_cdc_write("OK\r\n")` + `tud_cdc_write_flush()`。

### Step 4 — `src/main.c`（追加 3 行）
```c
#include "cdc_cmd.h"
...
cdc_cmd_init();                 // 与 buttons_init() 并列
while (1) {
    tud_task();
    cdc_cmd_task();             // 新增，放在 beep_task() 旁边
    ...
}
```

### Step 5 — `CMakeLists.txt`
- `add_executable` 加 `src/cdc_cmd.c`
- `pico_set_program_version(i2s_mic "1.4")` → `"1.5"`

### Step 6 — **刷机前强制静态校验**（关键！）
用仓库里已有的 `tools/decode_uf2_descriptors.py` 直接解码 ELF：
```
CONFIG total=236 #if=5
  IAD first=0 count=2 ...(音频)
  INTERFACE if=2 ...(HID)  ENDPOINT 0x82
  IAD first=3 count=2 class=0x2 ...(CDC)
  INTERFACE if=3 ...  ENDPOINT 0x83 int 8
  INTERFACE if=4 ...  ENDPOINT 0x03 bulk 64 / 0x84 bulk 64
```
**`total=236`、`#if=5`、5 个端点齐全，三者都对才允许刷机。** 这一步能在不接设备的情况下拦掉 90% 的枚举事故。

### Step 7 — 上位机联调（见第七节）

---

## 六、指令协议设计（文本行协议）

波特率无意义（CDC ACM 忽略），上位机填 115200 即可。

| 指令 | 含义 | 返回 |
|---|---|---|
| `IDN?` | 查询身份 | `vibecoding-mate mic v1.5` + `OK` |
| `LED BUSY ON\|OFF\|TOGGLE` | 单灯控制（BUSY/PLAN/IDLE） | `OK` |
| `LED ALL ON\|OFF` | 全控 | `OK` |
| `BEEP <ms>` | 响 N 毫秒（1~5000） | `OK` |
| `BEEP ON\|OFF` | 持续响 / 停 | `OK` |
| `LED?` | 回读三灯状态 | `BUSY=1 PLAN=0 IDLE=1` + `OK` |
| `PTT?` | 回读 PTT 电平（可选） | `0`/`1` + `OK` |
| `RESET` | 交还本地控制（LED 恢复 PTT 驱动） | `OK` |
| 其他 | — | `ERR 1` |

规则：
- 结束符 `\r`、`\n` 或 `\r\n` 都认；大小写不敏感
- 行缓冲 64B，溢出即丢弃并回 `ERR 2`；200ms 未收到结束符也丢弃
- 所有响应以 `\r\n` 结尾，便于 `readline()`
- **看门狗**：连续 5 秒无任何指令 → 自动 `RESET`，恢复本地控制。防止上位机崩溃后 LED/蜂鸣器卡死状态（这条很重要，实际用起来一定会遇到）

---

## 七、上位机程序设计与实现

### 7.1 端口发现（必须按 VID/PID 扫描，不能硬编码 COM3）

```python
import serial, serial.tools.list_ports

VID_PID = "CAFE:4A10"          # 注意大写十六进制

def find_port():
    for p in serial.tools.list_ports.comports():
        if VID_PID in (p.hwid or "").upper():
            return p.device
    return None
```
Windows 下 `p.hwid` 形如 `USB VID:PID=CAFE:4A10 SER=E660... LOCATION=1-3`。

### 7.2 打开与收发

```python
ser = serial.Serial(port, 115200, timeout=0.5, dsrdtr=False, rtscts=False)

def cmd(line: str) -> str:
    ser.write((line + "\r\n").encode())
    return ser.readline().decode(errors="ignore").strip()

print(cmd("IDN?"))          # vibecoding-mate mic v1.5
print(cmd("LED BUSY ON"))   # OK
print(cmd("BEEP 100"))      # OK
```

### 7.3 交付物（建议先做这两个脚本验证闭环）

| 文件 | 用途 |
|---|---|
| `tools/cdc_host_cli.py` | 命令行：自动找口、`IDN?`、`LED`/`BEEP` 手动敲，肉眼验证 |
| `tools/cdc_host_panel.py` | tkinter 小面板：3 个 LED 勾选框 + "Beep 100ms" 按钮 + 状态回读 + 2s 自动重连扫描 |

### 7.4 C# 集成片段（你的正式软件）

```csharp
using System.IO.Ports;
using System.Management; // 需引用 System.Management

static string? FindPort()
{
    var q = new ManagementObjectSearcher(
        "SELECT * FROM Win32_PnPEntity WHERE DeviceID LIKE '%VID_0CAFE&PID_4A10%'");
    foreach (ManagementObject o in q.Get())
    {
        var name = o["Name"] as string ?? "";
        int i = name.IndexOf("(COM"), j = name.IndexOf(')', i + 1);
        if (i >= 0 && j > i) return name.Substring(i + 1, j - i - 1);
    }
    return null;
}

using var sp = new SerialPort(port, 115200) { ReadTimeout = 500, DtrEnable = false, RtsEnable = false };
sp.Open();
sp.Write("LED BUSY ON\r\n");
var resp = sp.ReadLine();   // "OK"
```

### 7.5 上位机设计约定

- **串口独占**：同一时刻只能一个进程打开。你的软件要处理 `UnauthorizedAccessException`（被占用）并给出提示
- **事件驱动下发**：只在状态变化时发指令，不要 100Hz 轮询刷屏
- **断线重连**：每 2s 扫一次端口，重连后重新下发一次全量状态（设备侧状态可能因拔插丢失）
- **看门狗配合**：正常运行时至少每 3s 发一条（哪怕是 `IDN?`），否则设备 5s 后恢复本地控制

---

## 八、踩坑清单（Top 10）

1. **`CONFIG_TOTAL_LEN` 漏加 `TUD_CDC_DESC_LEN`** → 主机拒绝配置 → 设备管理器报"代码 43"，键盘麦克风**一起死**
2. **`ITF_NUM_TOTAL` 漏改** → Windows 只认前 3 个接口，CDC 不存在
3. **端点号撞车**（如 notif 和 bulk IN 都用 0x83）→ 未定义行为，现象随机
4. **CDC 插在 HID 前面** → HID 从 MI_02 变 MI_04，Windows 重装键盘，可能短暂失灵
5. **改 PID** → 本项目历史踩过的坑，音频可能再次失声。**保持 0x4A10**
6. **主机没开串口时无脑 `tud_cdc_write`** → TX FIFO 塞满后静默丢弃；回显前先判 `tud_cdc_connected()`
7. **CDC 解析里做重活**（`printf` / 长循环 / `sleep_ms`）→ 音频 iso 每 ms 要写 288 字节，主循环被卡住就爆音断流。解析必须 ≤ 几十 µs
8. **`pico_enable_stdio_usb` 绝不能打开**（当前已为 0，别改），否则 SDK 的 stdio_usb 会和我们的 CDC 抢接口
9. **字符串表索引重复/越界** → 现有 `string_desc_arr` 里 index 4 `"UAC2"` 是历史遗留未被引用，新增用 index 5，别复用 4
10. **上位机硬编码 COM 口 / 双开** → 端口号会变且串口独占，必须按 VID/PID 扫描 + 捕获占用异常

---

## 九、工作量估算

| 项 | 规模 | 风险 |
|---|---|---|
| `tusb_config.h` | 改 1 行 + 加 3 行 | 极低 |
| `usb_descriptors.c` | 加 ~10 行，改 2 行 | **中（描述符长度/接口数）** |
| `src/cdc_cmd.c/.h`（新） | ~120 行 | 低 |
| `main.c` / `CMakeLists.txt` | ~6 行 | 极低 |
| 上位机脚本 ×2 | ~150 行 | 低 |
| **合计** | **~290 行** | 风险集中在描述符一处，可用 Step 6 静态解码拦截 |

---

## 十、备选方案对比（供决策）

| 方案 | 新增接口 | 端点 | Windows 驱动 | 上位机库 | 评价 |
|---|---|---|---|---|---|
| **CDC ACM**（本方案） | +2（Control+Data） | +3 | Win10+ 自带 `usbser.sys` | pyserial / SerialPort | ✅ 推荐。免驱、调试直观、扩展性好 |
| **RawHID**（厂商自定义 HID） | +1 | +2（中断 IN/OUT，或仅 Feature Report 走 EP0） | 系统自带 `hidusb.sys` | hidapi / HidLibrary | 免驱且更"轻"，但无标准终端工具，调试要自己写；Feature Report 走 EP0 会占用控制传输 |
| **Vendor / WinUSB** | +1 | +2 | **需 Zadig 装驱动** | libusb | ❌ 不推荐，用户端部署麻烦 |
| **复用现有 HID 加 Report ID** | 0 | 0 | 无需 | hidapi | 描述符改动最小，但污染键盘报告描述符，且 Windows 有 HID 描述符缓存，改完常要删注册表缓存 |

---

## 十一、决策建议

1. **采用 CDC 方案**，但按本文第五节的顺序做，并且**先做最小闭环**验证：
   只加 CDC + `IDN?` + `LED?` + `BEEP 100`，确认枚举正常、音频仍在、键盘仍在，再补全指令表。
2. **PID 保持 0x4A10 不变**，CDC 追加在 HID 之后。
3. **刷机前必过 Step 6 静态解码**（`total=236` / `#if=5`）。
4. 手上**保留一份 v1.4 的 `.uf2`**（`firmware/i2s_mic_mic_hid_v1.4.uf2`），一旦枚举翻车立刻回滚。
