# mic-clear_hid-ptt 版本说明（v0.4 · 复合设备：麦克风 + HID 键盘 PTT）

`mic-clear_hid-ptt` 分支 = **voice_clear 音频固件（含 rec_take 选槽修复）+ HID 自定义键盘（PTT 触发）** 的复合设备版本。

- 来源：用户提供压缩包 `mic_hid_src.zip`（自包含工程，`vibecoding-mic/`）。
- 分支：从 `voice_clear`（已含 rec_take 修复与构建适配）切出 `mic-clear_hid-ptt`。
- 程序版本：`pico_set_program_version` = `0.4`。
- 身份：`USB_VID 0xCafe` / `USB_PID 0x4A10`（复用已验证可用的音频身份 → Windows `usbaudio2` 正常绑定）。
- 复合设备：**UAC2 麦克风（EP 0x81）+ HID 自定义键盘（EP 0x82）**，`CFG_TUD_HID = 1`。
- 已用静态 ELF 描述符解码核对：DEVICE `PID=0x4a10`、CONFIG `total=170`、`#if=3`（Audio Control / Audio Streaming / HID）、HID `class=0x3 sub=0x1 proto=0x1`、HID 报告描述符 65 字节。

---

## 一、相对 voice_clear 的改动（3 个文件）

| 文件 | 改动 |
|---|---|
| `src/tusb_config.h` | `CFG_TUD_HID` 由 `0` 改为 `1`（启用 HID 类驱动） |
| `src/usb_descriptors.c` | 新增 HID 接口（`ITF_NUM_HID`）、`hid_report_descriptor[]`（标准 6KRO 键盘）、三个 HID 回调；HID 独立中断端点 `0x82`（音频占 `0x81`）；`CONFIG_TOTAL_LEN` 加入 `TUD_HID_DESC_LEN` |
| `src/main.c` | 新增 `GPIO_PTT 2` 输入（内部上拉）+ `hid_task()`：GP2 接地触发键盘、断开释放 |

`rec_buffer.c`（rec_take 选槽修复）、`i2s.c`、`led.c`、`*.pio` 等与 voice_clear 一致，未改。

### PTT 行为（本次重点）

- **GP2 接地（低电平）= 同时按下 `Win + \``**；
- **GP2 断开接地（高电平）= 同时松开 `Win + \``**；
- 效果等同"对讲机"按键。

实现（`main.c`）：

```c
#define PTT_MODIFIER 0x08   // 左 GUI（Windows 键）
#define PTT_KEYCODE  0x35   // Grave Accent / 反引号 `
...
if (ptt_current) {                       // GP2 接地
    keycode[0] = PTT_KEYCODE;
    ok = tud_hid_keyboard_report(0, PTT_MODIFIER, keycode);  // 按下 Win+`
} else {                                 // GP2 断开
    ok = tud_hid_keyboard_report(0, 0, NULL);                // 松开全部
}
```

想换组合改这两个宏：
- `PTT_MODIFIER`：Win=`0x08`、左Ctrl=`0x01`、左Alt=`0x04`（可 `|` 叠加，如 Win+Alt=`0x0C`）
- `PTT_KEYCODE`：`` ` ``=`0x35`、空格=`0x2C`、Tab=`0x2B`、Esc=`0x29`、F13=`0x68`

去抖/重试：边沿去抖 30ms；`tud_hid_keyboard_report` 忙时返回 false，下个主循环重试。

---

## 二、编译中修复的源码 bug

原 `usb_descriptors.c` 在 `desc_configuration[]` 里用 `sizeof(hid_report_descriptor)`，但数组定义写在文件末尾（其后）→ C 不允许前向引用，编译报错 `'hid_report_descriptor' undeclared`。
**修复**：把 `hid_report_descriptor[]` 的定义移到 `desc_configuration[]` 之前（回调仍留在末尾）。仅此一处顺序调整，逻辑不变。

---

## 三、接线

- 音频（接错一定没声）：GP7=SD、GP8=SCK/BCK、GP9=WS/LRCLK；INMP441 的 L/R 接 GND（左声道模式），VCC=3V3。
- PTT 按键：GP2 接一个按钮，按钮另一端接 GND（内部已上拉，按下=接地=低电平=触发）。

---

## 四、构建（本机已验证）

本机无宿主 C++ 编译器，构建适配同 voice_clear：`src/i2s.pio.h` 已预生成提交、`CMakeLists.txt` 未用 `pico_generate_pio_header`、并含 picotool 预编译守卫。

```powershell
$env:PICO_SDK_PATH = "C:/Users/Administrator/.pico-sdk/sdk/2.1.1"
$env:Path = "C:/Users/Administrator/.pico-sdk/cmake/v3.31.5/bin" +
            ";C:/Users/Administrator/.pico-sdk/ninja/v1.12.1" +
            ";C:/Users/Administrator/.pico-sdk/toolchain/14_2_Rel1/bin" +
            ";" + $env:Path
cmake -G Ninja -B build_mic_hid -S .
cmake --build build_mic_hid
# 产物：build_mic_hid/i2s_mic.uf2
```

---

## 五、固件清单（本分支）

| 文件 | 大小 | SHA256 | 说明 |
|------|------|--------|------|
| `firmware/i2s_mic_mic_hid_v0.4.uf2` | 83968 B | `99ac79f66ff22178abe72a763de8e3c756761cecaf5dfcb08f6cde4223c786fe` | ✅ 复合固件（麦克风 + HID 键盘），PID `0x4A10`，可烧录 |
| `firmware/i2s_mic_mic_hid_v0.4.elf` | 812832 B | — | 配套 ELF（调试/反汇编用） |

---

## 六、烧录与验证

1. 按住 Pico 的 **BOOTSEL** 插 USB → 出现 `RPI-RP2` 盘，把 `.uf2` 拖入；
2. 系统应同时出现「USB 音频设备（麦克风）」与「键盘」；
3. 录音软件选该麦克风，左右声道均有干净声音（rec_take 选槽修复）；
4. 在记事本/网页按住 PTT（GP2 接地）→ 触发 **Win + `**；松手 → 松开。

---

## 七、与其他分支的关系

- `master`：v0.3 音频基线（修复前）。
- `voice_clear`：纯音频固件 + rec_take 选槽修复（无 HID）。
- `ptt`：早期复合固件（PID 曾为 `0x4B10` 导致 Windows 音频驱动未绑定，已回退 `0x4A10`，待真机确认）。
- `mic-clear_hid-ptt`（本分支）：voice_clear 音频 + HID 键盘复合，PTT = `Win + \``，PID `0x4A10`。

---

## 八、v0.6：GP2 控制板载 LED（常亮 / 高频闪烁）——改用真正的板载 LED

**背景**：v0.5 用的是 WS2812 数据线 GP16（原工程遗留），在标准 Raspberry Pi Pico 上**无效果**——标准 Pico 的板载 LED 是 **GP25**（普通 GPIO）。v0.6 按官方示例 `pico-examples/blink/blink.c` 重构为直接驱动板载 LED。

**引脚配置**
- 板载 LED：**GP25**（`PICO_DEFAULT_LED_PIN`，普通 GPIO 直接驱动；Pico W 为 CYW43 的 `WL_GPIO0`，代码用 `#if` 兼容）。见 `led.c` 的 `led_onboard_init()` / `led_onboard_set()`。
- 触发引脚：**GP2**（宏 `GPIO_PTT`，输入 + 内部上拉；按钮另一端接 GND）。与 PTT 复用同一输入，仅读取、不冲突。
- 兼容：仍保留 WS2812(GP16) 驱动（`led_set_color()`），板上有该灯时同样亮。

**状态切换逻辑**（`main.c` 的 `led_blinking_task()`）
- 上电默认：**常亮**（`led_onboard_init()` 里 `gpio_put(GP25, 1)`；WS2812 蓝 `(0,0,140)`）。
- GP2 接地（低电平）：**高频闪烁**，每 `LED_BLINK_FAST_MS = 100ms` 翻转一次（≈5Hz），`gpio_put(GP25, on)`。
- GP2 断开（回到高电平）：**立即恢复常亮**。
- 判定用 `gpio_get(GPIO_PTT)`；`prev_ptt` 做边沿检测（进入闪烁时复位计时）；`next_ms` 控节奏；常亮态只在需要时刷新一次。

真值表：

| GP2 电平 | 板载 LED(GP25) 表现 |
|---|---|
| 高（与 GND 断开） | 常亮 |
| 低（接到 GND） | 高频闪烁（100ms 半周期，≈5Hz） |

对应固件：`firmware/i2s_mic_mic_hid_v0.6.uf2`（版本 0.6）。

> 参考官方示例：<https://github.com/raspberrypi/pico-examples/blob/master/blink/blink.c>
