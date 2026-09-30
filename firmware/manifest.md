# 固件版本清单

每次编译的固件按版本号放入本目录（`i2s_mic_v<版本>.uf2`），并记录哈希与来源，便于回溯与对比。

## v0.3（当前主线）

| 项目 | 值 |
|---|---|
| 文件 | `i2s_mic_v0.3.uf2` / `i2s_mic_v0.3.elf` |
| 版本 | 0.3（`pico_set_program_version`） |
| 编译日期 | 2026-09-30 |
| SHA256 | `deced72effeca81fa739f58b7f75461cee773a1fae0192a231b1f49efb0d692e` |
| 设备名 | `vibecoding-mate mic` |
| VID/PID | `0xCafe` / `0x4A10`（**新 PID**：原 `0x4010`，改 PID 强制 Windows 刷新缓存的设备名） |
| 平台 | RP2040（Pico），family ID `rp2040` |
| 源码 | 根目录 `src/`（**移植自原始 `assets/reference/stereo_mic-master`**：`rec_take()` 透明透传，立体声原样输出） |
| I²S 引脚 | **SD=GP7(Pin10)、SCK(BCK)=GP8(Pin11)、WS(LRCLK)=GP9(Pin12)**（实际硬件接线） |
| 音频 | UAC2，48 kHz / 24-bit / 2 声道（透明透传；单 INMP441 接 L/R=GND，左声道有效、右声道为麦克风三态无效值） |
| 工具链 | Pico SDK 2.1.1 + ARM GCC 14.2.Rel1 + CMake 3.31.5 + Ninja 1.12.1 + pioasm/picotool 2.1.1 |
| 变更 | 主线引脚改回实际硬件 **GP7/8/9**（与原始工程一致）；`rec_take` 恢复原始立体声透传；**改 PID `0x4010`→`0x4A10` 解决 Windows 缓存旧设备名**；版本 0.2→0.3；**彻底移除 GP18/19/20 备用板型（`-DI2S_DIN_PIN` 编译开关已删除）** |

### 可复现性

全新构建目录重建后，`.uf2` 的 SHA256 与上述**逐位一致**，构建是确定性的。
默认构建即 GP7/8/9（无需额外参数）。

```powershell
# 默认即 SD=GP7 / SCK=GP8 / WS=GP9（实际硬件接线）
powershell -ExecutionPolicy Bypass -File tools\build.ps1
certutil -hashfile build211\i2s_mic.uf2 SHA256
# 应输出 deced72effeca81fa739f58b7f75461cee773a1fae0192a231b1f49efb0d692e
```

---

## voice_clear / v0.4（voice_clear 分支 · 左右声道清晰 + 音质提升）

> 分支：`voice_clear`（从 `master` v0.3 基线切出）。纯音频固件（**无 HID / 无 PTT**），唯一改动是 `rec_take()` 改为「自动选槽、左右都响」，修复单 INMP441 浮空槽噪声、提升音质。

| 项目 | 值 |
|---|---|
| 文件 | `i2s_mic_v0.4.uf2` / `i2s_mic_v0.4.elf` |
| 版本 | 0.4（`pico_set_program_version`） |
| 编译日期 | 2026-10-01 |
| SHA256（本分支重编） | `0c6f28d3f5b02761090a83d273a9de297878776b8c2005a039760fd64453a01f` |
| SHA256（作者原厂预编译） | `3b4b83c637d2e6d54b7a2452a0ef9aa3d75d7b3c8a6152db87dd106912432624`（仅供对照，见下注） |
| 设备名 | `i2s_mic` |
| VID/PID | `0xCafe` / `0x4A10`（与 v0.3 同，音频独占身份） |
| 平台 | RP2040（Pico），family ID `rp2040` |
| 源码 | 根目录 `src/`（来自 `mic_firmware_src_LR_clear.zip` 的 `vibecoding-mic/`，仅 `rec_buffer.c` 的 `rec_take()` 有改动） |
| I²S 引脚 | SD=GP7(Pin10)、SCK(BCK)=GP8(Pin11)、WS(LRCLK)=GP9(Pin12)（实际硬件接线） |
| 音频 | UAC2，48 kHz / 24-bit / 2 声道；`rec_take()` 取左右两槽绝对值较大者复制成双声道（隔离浮空槽噪声） |
| HID/PTT | 无（`CFG_TUD_HID 0`），与 ptt 分支不同 |
| 工具链 | Pico SDK 2.1.1 + ARM GCC 14.2.Rel1 + CMake 3.31.5 + Ninja 1.12.1；pioasm 用预编译版、picotool 用预编译版 |
| 变更 | 基于 v0.3 音频版；`rec_take()` 由「左槽透传」改为「左右取绝对值较大侧并复制双声道」，解决单边无声/浮空底噪，左右均出声、音质提升；版本 0.3→0.4 |

### 可复现性 / 说明

- 两个 `.uf2` 的 SHA 不同是**正常的**：作者原厂固件在另一台机器编译，ELF 内嵌的时间戳/路径不同；二者源码与 USB 描述符逐字节一致，功能等价。
- 本机构建环境无宿主 C++ 编译器，已通过将 `src/i2s.pio.h` 预生成并提交、并在 `CMakeLists.txt` 增加 picotool 预编译守卫，使普通 `cmake + ninja` 即可出固件。
- 详细描述见 `doc/08_voice_clear说明.md`；作者原始改动说明见 `doc/08_voice_clear_改动说明.md`。

---

## mic-clear_hid-ptt / v0.4（mic-clear_hid-ptt 分支 · 复合设备：麦克风 + HID 键盘 PTT）

> 分支：`mic-clear_hid-ptt`（从 `voice_clear` 切出）。voice_clear 音频固件 + HID 自定义键盘复合；PTT = **GP2 接地时按下 `Win + \``，断开时松开**（对讲机效果）。

| 项目 | 值 |
|---|---|
| 文件 | `i2s_mic_mic_hid_v0.4.uf2` / `i2s_mic_mic_hid_v0.4.elf` |
| 版本 | 0.4（`pico_set_program_version`） |
| 编译日期 | 2026-10-01 |
| SHA256 | `99ac79f66ff22178abe72a763de8e3c756761cecaf5dfcb08f6cde4223c786fe` |
| 设备名 | `i2s_mic` |
| VID/PID | `0xCafe` / `0x4A10`（复用已验证可用的音频身份） |
| 平台 | RP2040（Pico），family ID `rp2040` |
| 复合设备 | UAC2 麦克风（EP 0x81）+ HID 键盘（EP 0x82），`CFG_TUD_HID=1`，CONFIG total=170 / #if=3 |
| 源码 | 根目录 `src/`（来自 `mic_hid_src.zip`） |
| I²S 引脚 | SD=GP7(Pin10)、SCK(BCK)=GP8(Pin11)、WS(LRCLK)=GP9(Pin12) |
| 音频 | UAC2，48 kHz / 24-bit / 2 声道；含 voice_clear 的 rec_take 选槽修复（左右都出声、隔离浮空槽噪声） |
| PTT | GP2 接按钮到 GND；接地=按下 `Win + \``（modifier `0x08` + keycode `0x35`），断开=松开；30ms 去抖 |
| 工具链 | Pico SDK 2.1.1 + ARM GCC 14.2.Rel1 + CMake 3.31.5 + Ninja 1.12.1（pioasm/picotool 用预编译版） |
| 变更 | 相对 voice_clear：`tusb_config.h` 开 HID；`usb_descriptors.c` 加 HID 接口/报告描述符/回调；`main.c` 加 GP2 PTT→Win+`。修复 `hid_report_descriptor` 前向引用编译错误 |
| 文档 | `doc/09_mic_clear_hid_ptt说明.md` |

---

## mic-clear_hid-ptt / v0.5（新增：GP2 控制板载 LED 常亮/高频闪烁）

> 分支：`mic-clear_hid-ptt`。在 v0.4 复合固件基础上新增 LED 指示功能：**上电默认常亮；GP2 接地时高频闪烁；GP2 断开后恢复常亮**。

| 项目 | 值 |
|---|---|
| 文件 | `i2s_mic_mic_hid_v0.5.uf2` / `i2s_mic_mic_hid_v0.5.elf` |
| 版本 | 0.5（`pico_set_program_version`） |
| 编译日期 | 2026-10-01 |
| SHA256 | `4fd6ffb42f498b5cfa2314d87f3bc9f5320c9076e1b80f6461aa0d3198adb9b4` |
| 设备名 | `i2s_mic` |
| VID/PID | `0xCafe` / `0x4A10` |
| 复合设备 | UAC2 麦克风（EP 0x81）+ HID 键盘（EP 0x82），`CFG_TUD_HID=1` |
| LED 引脚 | WS2812 数据线 = **GP16**（PIO0 SM0，800 kHz） |
| 触发引脚 | **GP2**（输入 + 内部上拉，接地 = 低电平） |
| 状态逻辑 | 上电默认常亮（蓝；静音时偏红）→ GP2 接地：**100ms 半周期高频闪烁** → GP2 断开：恢复常亮 |
| 变更 | 相对 v0.4：重写 `main.c` 的 `led_blinking_task()`，LED 亮灭由 GP2 电平驱动（`LED_BLINK_FAST_MS=100`）；版本 0.4→0.5 |

---

## v0.2（历史版本）

| 项目 | 值 |
|---|---|
| 文件 | `i2s_mic_v0.2.uf2` / `i2s_mic_v0.2.elf` |
| 版本 | 0.2（`pico_set_program_version`） |
| 编译日期 | 2026-09-30 |
| SHA256 | `04be3815841299aa75930ddb0bbca168e1fe3e73e2f59fbef7487669e5de4a5b` |
| 设备名 | `vibecoding-mate mic` |
| 平台 | RP2040（Pico），family ID `rp2040` |
| 源码 | 根目录 `src/`（单声道版：`rec_take()` 只取左声道并复制到 L/R） |
| I²S 引脚 | SD=GP18(Pin24)、SCK(BCK)=GP19(Pin25)、WS(LRCLK)=GP20(Pin26) |
| 音频 | UAC2，48 kHz / 24-bit / 2 声道（L == R，单声道复制） |
| 工具链 | Pico SDK 2.1.1 + ARM GCC 14.2.Rel1 + CMake 3.31.5 + Ninja 1.12.1 + pioasm/picotool 2.1.1 |
| 变更 | 合并单声道适配版进主线；引脚由 GP7/8/9 改为 GP18/19/20（**后证实与用户实际硬件不符，被 v0.3 回退**） |

---

## v0.1（历史版本 · 基线对照）

| 项目 | 值 |
|---|---|
| 文件 | `i2s_mic_v0.1.uf2` / `i2s_mic_v0.1.elf` |
| SHA256 | `f30973ebe147163735813727feecc6a78c59b16ad579d6f0bb97adb9b32fc3fc` |
| 源码 | `src/`（= 原始立体声工程，I²S 立体声原样透传，**未做单声道处理**） |
| I²S 引脚 | SD=GP7(Pin10)、SCK=GP8(Pin11)、WS=GP9(Pin12) |
| 用途 | 作为**基线对照**固件，验证 I²S + USB 音频链路是否正常（用户实际可工作的版本） |

---

## 版本号约定

- 版本号跟随 `CMakeLists.txt` 里的 `pico_set_program_version(i2s_mic "<版本>")`；
- 每次出固件时同步改版本号，并把 `.uf2`/`.elf` 以 `i2s_mic_v<版本>.uf2` 命名放入本目录；
- 在本文件追加一条记录（版本 / 日期 / SHA256 / 源码 commit / 变更说明 / 引脚）。
