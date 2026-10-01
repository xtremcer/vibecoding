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

## mic-clear_hid-ptt / v0.6（修正：改用真正的板载 LED GP25）

> 分支：`mic-clear_hid-ptt`。v0.5 的 LED 用的是 WS2812(GP16)，在标准 Pico 上**无效果**；v0.6 改为驱动**真正的板载 LED（GP25 / `PICO_DEFAULT_LED_PIN`）**，参考官方示例 `pico-examples/blink/blink.c`。

| 项目 | 值 |
|---|---|
| 文件 | `i2s_mic_mic_hid_v0.6.uf2` / `i2s_mic_mic_hid_v0.6.elf` |
| 版本 | 0.6（`pico_set_program_version`） |
| 编译日期 | 2026-10-01 |
| SHA256 | `8fae2dbc0e7e7f7358216ab856937b94b452920d130448167d3e161186f1cfa2` |
| 设备名 | `i2s_mic` |
| VID/PID | `0xCafe` / `0x4A10` |
| 复合设备 | UAC2 麦克风（EP 0x81）+ HID 键盘（EP 0x82），`CFG_TUD_HID=1` |
| 板载 LED | **GP25**（`PICO_DEFAULT_LED_PIN`，普通 GPIO 直接驱动；Pico W 用 CYW43 的 WL_GPIO0，代码 `#if` 兼容） |
| 触发引脚 | **GP2**（输入 + 内部上拉，接地 = 低电平） |
| 状态逻辑 | 上电默认常亮 → GP2 接地：**100ms 半周期高频闪烁** → GP2 断开：恢复常亮 |
| 变更 | 相对 v0.5：新增 `led.c/led.h` 的 `led_onboard_init()/led_onboard_set()`（GP25 直接 GPIO，参考官方 blink）；`main.c` 在常亮/闪烁处调用；保留 WS2812(GP16) 兼容。版本 0.5→0.6 |

---

## mic-clear_hid-ptt / v0.7（LED 闪烁频率加倍）

> 分支：`mic-clear_hid-ptt`。高频闪烁半周期 100ms→50ms（≈5Hz→≈10Hz），闪烁快一倍。其余同 v0.6（板载 LED GP25 + GP2 触发）。

| 项目 | 值 |
|---|---|
| 文件 | `i2s_mic_mic_hid_v0.7.uf2` / `i2s_mic_mic_hid_v0.7.elf` |
| 版本 | 0.7（`pico_set_program_version`） |
| 编译日期 | 2026-10-01 |
| SHA256 | `9bf1489a0d1cb481feabfa0d073bfd231709d05134fa829defaeaad751fc349c` |
| 设备名 | `i2s_mic` |
| VID/PID | `0xCafe` / `0x4A10` |
| 板载 LED | GP25（`PICO_DEFAULT_LED_PIN`） |
| 触发引脚 | GP2（接地=低电平） |
| 变更 | `main.c` 的 `LED_BLINK_FAST_MS` 100→50（闪烁频率 ×2）；版本 0.6→0.7 |

---

## mic_ptt_led / v0.8（新增 8 个自定义按键）

> 分支：`mic_ptt_led`。新增 8 个自定义按键（GP0/1/3/4/5/6/12/13，内部上拉、低有效），与 PTT 一起由统一的 HID 报告任务汇总处理（支持多键同按）。

| 项目 | 值 |
|---|---|
| 文件 | `i2s_mic_mic_hid_v0.8.uf2` / `i2s_mic_mic_hid_v0.8.elf` |
| 版本 | 0.8（`pico_set_program_version`） |
| 编译日期 | 2026-10-01 |
| SHA256 | `e2cb5e8b008288f374dff56349b51638664df893c382fc49365fcc0ba89af5c2` |
| 设备名 | `i2s_mic` |
| VID/PID | `0xCafe` / `0x4A10` |
| 复合设备 | UAC2 麦克风（EP 0x81）+ HID 键盘（EP 0x82） |
| 按键映射 | GP0=Enter、GP1=Backspace、GP3=Esc、GP4=Ctrl+A、GP5=Ctrl+C、GP6=Ctrl+V、GP12=Ctrl+L、GP13=Win+\；GP2=PTT(Win+`) |
| 引脚 | 全部输入 + 内部上拉，接地(低电平)=按下；板载 LED=GP25；I²S=GP7/8/9；WS2812=GP16 |
| 变更 | `main.c` 新增 `hid_buttons[]` 映射表；`hid_task()` 改为表驱动汇总（修饰键 OR + 最多 6 键码，变化才发送）；`main()` 循环初始化所有按键脚。版本 0.7→0.8 |

---

## mic_ptt_led / v0.9（暂禁用 GP6/Ctrl+V 映射，用于测试其余按键）

> 分支：`mic_ptt_led`。**GP6 与 I²S `dout_pin(=6)` 冲突**：I²S 的 out-master 状态机把 GP6 配置为输出并拉低，导致 GP6 按键被当成常按 → Ctrl+V 无限触发。本版先注释掉 GP6 映射以测试其余按键。

| 项目 | 值 |
|---|---|
| 文件 | `i2s_mic_mic_hid_v0.9.uf2` / `i2s_mic_mic_hid_v0.9.elf` |
| 版本 | 0.9（`pico_set_program_version`） |
| 编译日期 | 2026-10-01 |
| SHA256 | `93385f161d57ce94ac03b286581a5d6874dd1a5d2892d4db1b3c7902b14af5fa` |
| 设备名 | `i2s_mic` |
| VID/PID | `0xCafe` / `0x4A10` |
| 按键映射 | 保留 GP0=Enter、GP1=Backspace、GP2=PTT(Win+`)、GP3=Esc、GP4=Ctrl+A、GP5=Ctrl+C、GP12=Ctrl+L、GP13=Win+\；**GP6=Ctrl+V 已注释禁用** |
| 变更 | `main.c` 注释掉 `hid_buttons[]` 中 GP6 行（GP6 与 I²S dout_pin=6 冲突）。版本 0.8→0.9 |
| 待办 | 彻底修复：把 `rec_buffer.c` 的 `my_i2s_config` 里 `dout_pin` 由 6 改为空闲脚（如 GP14），再恢复 GP6 映射 |

---

## mic_ptt_led / v1.0（Ctrl+V 改到 GP20；按键改真去抖）

> 分支：`mic_ptt_led`。Ctrl+V 从冲突的 GP6 改到空闲脚 **GP20**；按键改为**真去抖**（原始电平稳定 20ms 才生效），解决 GP13(Win+\) **长按时误弹 Windows 开始菜单**的问题。

| 项目 | 值 |
|---|---|
| 文件 | `i2s_mic_mic_hid_v1.0.uf2` / `i2s_mic_mic_hid_v1.0.elf` |
| 版本 | 1.0（`pico_set_program_version`） |
| 编译日期 | 2026-10-01 |
| SHA256 | `6481b35cf017c64282f14d596893e19b343dd974aab75a7fc0277e4b577cd66f` |
| 设备名 | `i2s_mic` |
| VID/PID | `0xCafe` / `0x4A10` |
| 按键映射 | GP0=Enter、GP1=Backspace、GP2=PTT(Win+`)、GP3=Esc、GP4=Ctrl+A、GP5=Ctrl+C、**GP20=Ctrl+V**、GP12=Ctrl+L、GP13=Win+\ |
| 变更 | GP6 不再做按键（留给 I²S dout）；Ctrl+V 由 GP6→GP20；`hid_task()` 改**真去抖**（`HID_DEBOUNCE_MS=20`：原始电平稳定 20ms 才更新有效状态，防长按触点抖动误判松开）。版本 0.9→1.0 |
| 备注 | **未 push 到 GitHub**（用户要求仅本地提交） |

---

## mic_ptt_led / v1.1（GP13 改一键脉冲：按下触发一次，最多保持 1s 自动释放）

> 分支：`mic_ptt_led`。GP13(Win+\) 改为**一键脉冲**：按下只触发一次 Win+\，最多保持 1 秒后自动释放；脉冲期间再次按下无效（阻塞防抖）；长按/短按效果一致。

| 项目 | 值 |
|---|---|
| 文件 | `i2s_mic_mic_hid_v1.1.uf2` / `i2s_mic_mic_hid_v1.1.elf` |
| 版本 | 1.1（`pico_set_program_version`） |
| 编译日期 | 2026-10-01 |
| SHA256 | `c99bb69cb57df728ef9eee901219ca73017adf279ae96e9db5738aa0e4c9d27e` |
| 设备名 | `i2s_mic` |
| VID/PID | `0xCafe` / `0x4A10` |
| 变更 | `main.c` 新增 GP13 一键脉冲状态机（`GPIO_ONESHOT=13`、`PULSE_HOLD_MS=1000`）：按下沿启动脉冲→发一次 Win+`\`→最多 1s 后自动释放；脉冲期间忽略新按下（阻塞防抖）；需松开后再按才触发下一次。长按/短按效果一致。版本 1.0→1.1 |
| 备注 | **未 push 到 GitHub**（用户要求仅本地提交） |

---

## mic_ptt_led / v1.2（修复 GP13：松开即释放，保证不会一直按住 Win）

> 分支：`mic_ptt_led`。修复 v1.1 "GP13 触发后 Win 一直不释放"：脉冲改为**到时(1s) 或 按键松开（以先到者为准）即释放**。

| 项目 | 值 |
|---|---|
| 文件 | `i2s_mic_mic_hid_v1.2.uf2` / `i2s_mic_mic_hid_v1.2.elf` |
| 版本 | 1.2（`pico_set_program_version`） |
| 编译日期 | 2026-10-01 |
| SHA256 | `0a6013ccab8bf7da1c19d32964a45b24e713f3fdd1a67c4eaf418a803460f95a` |
| 设备名 | `i2s_mic` |
| VID/PID | `0xCafe` / `0x4A10` |
| 变更 | GP13 脉冲结束条件由"仅 1s 超时"改为"`(now-start)>=PULSE_HOLD_MS` **或** 按键松开，先到者即释放"，并用无符号计时；避免 Win 被一直按住。版本 1.1→1.2 |
| 备注 | **未 push 到 GitHub**（用户要求仅本地提交） |

---

## mic_ptt_led / v1.3（按键抽成独立模块 buttons.c / buttons.h）

> 分支：`mic_ptt_led_key`。把自定义按键（映射表 + 去抖 + 触发行为 + HID 上报）抽成**独立模块**，便于未来"用户自由组合设定"——只需改 `buttons.c` 里的 `btn_configs[]` 表。

| 项目 | 值 |
|---|---|
| 文件 | `i2s_mic_mic_hid_v1.3.uf2` / `i2s_mic_mic_hid_v1.3.elf` |
| 版本 | 1.3（`pico_set_program_version`） |
| 编译日期 | 2026-10-01 |
| SHA256 | `a0822dfbfa89132adada894e7841502cad09144c31ee79a1047362f28214e089` |
| 新增模块 | `src/buttons.h` / `src/buttons.c`：`btn_config_t{pin,modifier,keycode,behavior,hold_ms}` 配置表 + `buttons_init()` + `buttons_task()`；修饰键宏 `MOD_LCTRL/LSHIFT/LALT/LGUI/...`；行为 `BTN_HOLD`（按住保持）/ `BTN_ONESHOT`（一键脉冲）；`PIN_PTT` 宏 |
| 变更 | `main.c` 移除按键表/去抖/`hid_task`，改为 `buttons_init()` + `buttons_task()`；LED 用 `PIN_PTT`；CMake 增加 `src/buttons.c`。版本 1.2→1.3 |
| 备注 | **未 push 到 GitHub**（用户要求仅本地提交） |

---

## mic_ptt_led_key-m_BEEP-LED / v1.4（新增预留模块：状态灯 / OLED / 蜂鸣器）

> 分支：`mic_ptt_led_key-m_BEEP-LED`。新增 3 个独立外设模块（只预留接口，便于后续实现）。

| 项目 | 值 |
|---|---|
| 文件 | `i2s_mic_mic_hid_v1.4.uf2` / `i2s_mic_mic_hid_v1.4.elf` |
| 版本 | 1.4（`pico_set_program_version`） |
| 编译日期 | 2026-10-01 |
| SHA256 | `d6589b5a5c48af97cffa08981322212a11e13af11a4f5857649e97f966117643` |
| 新增模块 | `status_led.c/.h`（busy=GP26 / plan=GP27 / idle=GP28）；`oled.c/.h`（I2C1 SDA=GP10 / SCL=GP11）；`beep.c/.h`（GP14，需三极管/MOS） |
| 变更 | `main.c` 增加三个模块的 init 与主循环调用（含 idle/busy 灯示例）；CMake 增加 3 个 `.c`；版本 1.3→1.4 |
| 文档 | `doc/10_预留模块说明.md` |
| 已知问题 | **Win 组合键触发仍有细微 bug**（GP13=Win+反斜杠 一键脉冲 / GP2 PTT=Win+反引号），待排查（疑与脉冲时序或 Windows 对长按 Win 的处理有关） |
| 备注 | 已 push 到 GitHub（分支 `mic_ptt_led_key-m_BEEP-LED`） |

---

## mic_ptt_led_key-m_BEEP-LED_CDCs / v1.5（Phase 1：新增 CDC 虚拟串口）

> 分支：`mic_ptt_led_key-m_BEEP-LED_CDCs`。**递进开发第 1 步**，只打通 CDC 链路，尚未接入 LED/BEEP 控制。

| 项目 | 值 |
|---|---|
| 文件 | `i2s_mic_mic_hid_v1.5.uf2` / `i2s_mic_mic_hid_v1.5.elf` |
| 版本 | 1.5（`pico_set_program_version`） |
| 编译日期 | 2026-10-01 |
| SHA256 | `d7943e45cf8c9ab50a0e2812f168a4fc585743024d3018d4d98de661dda2c846` |
| VID/PID | `0xCafe` / `0x4A10`（**刻意保持不变**，避免打破 Windows 已有的音频驱动绑定） |
| USB 结构 | 音频 IAD(ITF0/1) + HID(ITF2) + **CDC IAD(ITF3/4)**，共 **5 个接口** |
| 配置描述符 | **236 字节**（= 9 + 音频 136 + HID 25 + CDC 66），比 v1.4 的 170 字节**纯追加** |
| 端点 | 0x81 iso IN 294B（音频） / 0x82 int IN 16B（HID） / **0x83 int IN 8B（CDC 通知）** / **0x03 bulk OUT 64B** / **0x84 bulk IN 64B** —— 共 5 个，RP2040 上限 16 |
| 新增文件 | `src/cdc_cmd.c/.h`（行文本协议：64B 行缓冲、200ms 半行超时、`IDN?` 回显、其余回 `ERR 1`） |
| 指令 | 本阶段仅 `IDN?` → `vibecoding-mate mic v1.5` |
| 变更 | `tusb_config.h`：`CFG_TUD_CDC` 0→1 + FIFO 尺寸；`usb_descriptors.c`：接口枚举/端点宏/描述符项/字符串表；`main.c` 加 `cdc_cmd_init()` 与主循环 `cdc_cmd_task()`；CMake 加 `src/cdc_cmd.c`，版本 1.4→1.5 |
| 静态校验 | `tools/decode_uf2_descriptors.py` 解码 ELF：`total=236 / #if=5 / 5 端点` ✅；**与 v1.3 逐字节 diff 仅新增 CDC 块，音频与 HID 部分零改动** ✅ |
| 上位机 | `tools/cdc_host_cli.py`（需 `pip install pyserial`） |
| 未改动 | `rec_buffer.c` / `i2s.c` / `buttons.c` / `led.c` / `status_led.c` / `oled.c` / `beep.c` 均一字未动 |
| 回滚 | 出问题刷回 `i2s_mic_mic_hid_v1.4.uf2` |

---

## mic_ptt_led_key-m_BEEP-LED_CDCs / v1.6（Phase 2：单灯 BUSY/GP26 控制）

> 分支：`mic_ptt_led_key-m_BEEP-LED_CDCs`。**递进开发第 2 步**：只开放 BUSY 一盏灯。

| 项目 | 值 |
|---|---|
| 文件 | `i2s_mic_mic_hid_v1.6.uf2` / `i2s_mic_mic_hid_v1.6.elf` |
| 版本 | 1.6（`pico_set_program_version`） |
| 编译日期 | 2026-10-01 |
| SHA256 | `77b2354fe6f339bffb82f1809c709a0606ddbf5dfa86bd481f4f7cfbf7a9f03b` |
| USB 结构 | 与 v1.5 **完全一致**（total=236 / #if=5 / 5 端点），本阶段未动描述符 |
| `status_led` 新增 | `status_led_set_mode` / `status_led_blink` / `status_led_get_mode` / `status_led_is_lit` / `status_led_task` / `status_led_host_control` / `status_led_is_host_control`。**原有 3 个 API 签名与语义不变** |
| 闪烁实现 | 每灯独立 `on_ms/off_ms/next_ms` 相位，`status_led_task()` 在主循环非阻塞推进；边界钳位 10~60000 ms |
| 主机接管 | `status_led_host_control(true)`；`main.c` 中 PTT→busy 灯的本地逻辑加了 `if (!status_led_is_host_control())` 门控，**默认行为不变** |
| 指令 | `LED BUSY ON` / `LED BUSY OFF` → `OK`；`LED?` → `BUSY=0\|1`；`LED PLAN/IDLE ...` → `ERR 4`（本阶段未开放）；灯名错 `ERR 3`，缺参数 `ERR 5`，未知指令 `ERR 1` |
| 未改动 | 描述符、`rec_buffer.c` / `i2s.c` / `buttons.c` / `led.c` / `oled.c` / `beep.c` |
| 已知限制 | 本阶段主机一旦接管就**不会自动交还**（Phase 3 加看门狗） |
| 回滚 | 出问题刷回 `i2s_mic_mic_hid_v1.5.uf2` |

---

## mic_ptt_led_key-m_BEEP-LED_CDCs / v1.7（Phase 3：三灯独立 亮/灭/闪 + 看门狗）

> 分支：`mic_ptt_led_key-m_BEEP-LED_CDCs`。**递进开发第 3 步**：三盏灯全部开放。

| 项目 | 值 |
|---|---|
| 文件 | `i2s_mic_mic_hid_v1.7.uf2` / `i2s_mic_mic_hid_v1.7.elf` |
| 版本 | 1.7（`pico_set_program_version`） |
| 编译日期 | 2026-10-01 |
| SHA256 | `593e7d433d1455a8fa902ab88247b469c73c7dcab8b7798d538de4f01349224a` |
| USB 结构 | 与 v1.5/v1.6 一致（total=236 / #if=5 / 5 端点），本阶段未动描述符 |
| 指令 · LED | `LED <BUSY\|PLAN\|IDLE\|ALL> ON\|OFF`；`LED <...> BLINK [on_ms [off_ms]]`（省略默认 500/500，只给一个则 on=off；钳位 10~60000ms） |
| 指令 · 查询 | `LED?` → `BUSY=0 PLAN=0 IDLE=1`（回读每灯的**当前物理状态**，闪烁中会随之变化） |
| 指令 · 交还 | `RESET` → `OK`，清接管标志并恢复上电默认（idle 亮，busy/plan 灭） |
| 主机看门狗 | `CDC_HOST_TIMEOUT_MS 5000`：接管状态下 5 秒无任何指令 → 自动 `RESET`，防上位机崩溃后灯卡死 |
| 上位机 | `tools/cdc_led_panel.py`（tkinter 图形面板：三灯独立控制 + 周期输入 + 状态回读 + 自动重连 + **2s 心跳保活**） |
| 未改动 | 描述符、`rec_buffer.c` / `i2s.c` / `buttons.c` / `led.c` / `oled.c` / `beep.c` |
| 注意 | 命令行单次调用（开→发→关）时，5 秒后看门狗会把灯交还本地；图形面板靠心跳保持接管 |
| 回滚 | 出问题刷回 `i2s_mic_mic_hid_v1.6.uf2` |

---

## mic_ptt_led_key-m_BEEP-LED_CDCs / v1.8（Phase 4：蜂鸣器改 PWM 旋律引擎）

> 分支：`mic_ptt_led_key-m_BEEP-LED_CDCs`。**递进开发第 4 步**：蜂鸣器从纯 GPIO 直驱改为 **PWM 方波**，驱动无源蜂鸣器演奏真实音高；并通过 CDC 暴露完整 BEEP 指令。

| 项目 | 值 |
|---|---|
| 文件 | `i2s_mic_mic_hid_v1.8.uf2` / `i2s_mic_mic_hid_v1.8.elf` |
| 版本 | 1.8（`pico_set_program_version`） |
| 编译日期 | 2026-10-01 |
| SHA256 | `b58ee7ee1d9b110f6b17febf28eef35b0a323af08fdee6d4fdaefe4fd5ee9e09` |
| USB 结构 | 与 v1.5~v1.7 一致（total=236 / #if=5 / 5 端点），本阶段未动描述符 |
| 蜂鸣器硬件 | GP14 → **PWM slice 7 / channel A**（8.4 定点分频，clk_sys=132MHz）；无源蜂鸣器，频率=音调，50% 占空方波 |
| `beep` 重写 | PWM 频率计算 `div=ceil(132e6/(65535*hz))` 保证 wrap≤65535；**彻底静音三步法**（`set level 0` → `disable slice` → `GPIO 切回 SIO 置低`，杜绝直流偏置/微噪声）；非阻塞旋律状态机 `BEEP_OFF/ON/PLAYING` |
| 旋律 | 3 段预设：MEL1=C 大调上行(C-E-G-C5)、MEL2=叮咚、MEL3=开机小号 |
| 指令 · 蜂鸣器 | `BEEP ON`（持续）/ `BEEP OFF` / `BEEP STOP` / `BEEP <ms>`（默认音定时）/ `BEEP NOTE <hz> <ms>`（单音）/ `BEEP PLAY <1\|2\|3>`（旋律）；`BEEP?` → `STATE=ON\|PLAYING\|OFF NOTE=<hz>` |
| CMake | `target_link_libraries` 新增 `hardware_pwm` |
| 上位机 | `tools/cdc_beep_selftest.py`（逐条下发 + 心跳保持，供听测） |
| 未改动 | 描述符、`rec_buffer.c` / `i2s.c` / `buttons.c` / `led.c` / `oled.c` / `status_led.c` |
| 已知限制 | **看门狗只收回 LED，不静音蜂鸣器**：`BEEP ON` 持续响后若上位机断线不会自动停，记得 `BEEP OFF`；长时间响优先用 `BEEP <ms>` 或旋律。GP0 同时被 UART0 TX 与按键(Enter)占用（既有隐患，未处理） |
| 回滚 | 出问题刷回 `i2s_mic_mic_hid_v1.7.uf2` |

---

## mic_config_base_v2 / v1.9（Phase A1：State Executor 状态执行器）

> 分支：`mic_config_base_v2`（自 `mic_ptt_led_key-m_BEEP-LED_CDCs` 拉出）。**配置基座第一阶段**：把「业务语义」从固件里剥离，固件只做**哑执行器**——它只知道「当前状态」+「配置」，只负责把这个状态翻译成灯和声音。

| 项目 | 值 |
|---|---|
| 文件 | `i2s_mic_mic_hid_v1.9.uf2` / `i2s_mic_mic_hid_v1.9.elf` |
| 版本 | 1.9（`pico_set_program_version`） |
| 编译日期 | 2026-10-02 |
| SHA256 | `7ae84a9eaacfb4d1681aaa170cc807e26aeaae4e0994214ce3997a8be8472563` |
| USB 结构 | 与 v1.5~v1.8 **完全一致**（total=236 / #if=5 / 5 端点 / PID=0x4a10 / CDC 在 HID 之后），本阶段未动描述符（`decode_uf2_descriptors.py` 已校验） |
| 新增源码 | `src/state_exec.c` / `src/state_exec.h` |
| 三态 | `ST_BUSY`（工作中）/ `ST_IDLE`（空闲）/ `ST_AUTH`（待授权），`ST_COUNT=3` |
| 状态→灯 | BUSY→`LED_BUSY`、IDLE→`LED_IDLE`、AUTH→`LED_PLAN`(GP27)；每态独立 `{standby(ON/OFF), blink_on_ms, blink_off_ms}`，两个 ms 全 0 = 跟随 standby 常亮/常灭 |
| 状态→声 | 每态独立 `{enabled, timing(START\|END), count, loop_interval_ms, score}`；`score` 为乐谱文本 `"hz,ms;hz,ms"` |
| 提示音优先级 | **一次切换只响一段**：`START`(进入态) **优先于** `END`(离开态)。即进入态配了 START → 播进入态的；否则离开态配了 END → 播离开态的。这样 `IDLE→AUTH` 一定响"催授权"而不是被开工短音顶掉；`BUSY→IDLE` 回落响 BUSY 的完成提示音 |
| 循环提示 | `count>1` 时由 `state_exec_task()` 在 `beep_get_state()==BEEP_OFF` 后按 `loop_interval_ms` 补播（非阻塞，不占主循环） |
| 编译期默认配置 | BUSY：灯**常亮**，结束提示音×3（`523,200;659,200;784,350`，间隔 1000ms）<br>IDLE：灯常亮，结束提示音×1（`659,300;784,300`）<br>AUTH：灯常亮 + 300/300 闪烁，开始提示音×3（`880,250;660,450`） |
| 开机显示 | `device.boot_led = ALL_ON`，`boot_timeout_ms = 5000`：上电三灯全亮 **5 秒**，期间执行器**接管** LED；超时仍没等到 `SET STATE` 就交还本地（idle 亮，PTT 可用）。`0` = 永不交还 |
| 指令 · 状态 | `SET STATE <BUSY\|IDLE\|AUTH>` → `OK`；`STATE?` → `STATE=BUSY`（未设过回 `STATE=NONE`） |
| 指令 · 蜂鸣器 | 新增 `beep_play_score("<乐谱文本>")`（内部最多 16 音符）；**v1.8 的 `BEEP PLAY 1/2/3` 全部保留**，向后兼容 |
| 指令 · 交还 | `RESET` → `OK`：清接管标志 **并交还本地**（`STATE?` 回 `NONE`，灯回本地默认 idle 亮，PTT 恢复驱动 busy）。5s 看门狗超时、`boot_timeout_ms` 超时都走这条路 |
| 错误码 | 补了完整错误码表（`src/cdc_cmd.h`）：1=未知指令 / 2=行超长超时 / 3=灯名不认识 / **4=枚举值非法** / 5=缺参数 / 6=配置非法(A2) |
| 兼容性 | `state_exec_init()` **故意不置 `host_control`**，因此首次 `SET STATE` 之前，v1.8 的「按 PTT → busy 灯亮、松开灭」本地行为完整保留 |
| 验收脚本 | `tools/cdc_state_selftest.py`（自动断言 IDN/STATE/SET/错误码 **+ `LED?` 回读三灯物理电平 + `BEEP?` 轮询发声段数**，只有音色需要人耳判断） |
| 首版实测抓到并已修的 2 个 bug | ① **BUSY 态三灯全灭**：默认 `busy.led.standby` 误设为 `OFF`，切到 BUSY 后看不出是哪盏亮 → 改 `ON`。<br>② **上电"三灯全亮"只亮两盏**：`state_exec_init()` 不置 `host_control`，主循环里 `status_led_set(LED_BUSY, !gpio_get(PIN_PTT))` 每帧把 busy 拉灭 → 改为展示期间接管，5s 后交还本地。<br>两个都是"指令全回 OK、灯却是错的"，所以验收脚本从此一律用 `LED?` 回读，不靠眼睛 |
| 未改动 | 描述符、`rec_buffer.c` / `i2s.c` / `buttons.c` / `led.c` / `oled.c` / `status_led.c` / `beep.c` 的既有 API |
| 依赖文档 | `doc/12_产品需求与技术栈.md`（§5.3 schema、§5.6 默认宏映射）、`doc/13_配置网页原型.html`（Phase B 参照 UI） |
| 回滚 | 出问题刷回 `i2s_mic_mic_hid_v1.8.uf2` |

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
