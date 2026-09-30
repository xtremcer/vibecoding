# USB 单声道麦克风（RP2040 Pico + 单颗 INMP441）

本工程是从 `stereo_mic-master_mod`（双 INMP441 立体声）**移植到单颗 INMP441 单声道** 的版本，目标硬件为 **Raspberry Pi Pico（RP2040）**。

> 源码逻辑、PIO/I2S/DMA、UAC2 协议栈与原工程完全一致；**唯一功能性改动在 `rec_buffer.c` 的 `rec_take()`**（见下文「软件改动」）。其余文件原样保留。

---

## 1. 与原工程的差异（为什么能直接移植）

| 项目 | 原工程（stereo） | 本工程（mono） |
|---|---|---|
| 麦克风 | 2× INMP441（L/R 各一） | 1× INMP441（仅左声道） |
| I2S 引脚 | SCK=GP8, WS=GP9, SD=GP7 | **完全相同**（接线不变） |
| `PICO_BOARD` | `pico` | `pico`（**无需改动**，RP2040 Pico 即此板型） |
| 系统时钟 | 132 MHz | 132 MHz（不变） |
| USB 音频 | 2 声道 | 2 声道（L==R==单声道复制，**描述符不变**） |
| 代码改动 | — | 仅 `rec_buffer.c` 中 `rec_take()` 内循环 |

**关键结论**：因为 RP2040 Pico 的板型名就是 `pico`，且 I2S 引脚与原点相同，本移植**不需要改板型、不需要改 CMake、不需要改引脚配置**，只改数据打包逻辑。

---

## 2. 硬件接线（单颗 INMP441 → Raspberry Pi Pico）

INMP441 为 **I2S 从机**，需要 RP2040 提供 BCK（SCK）和 LRCLK（WS）；麦克风输出 SD。

### 2.1 引脚对照（引脚：P10/P11/P12 = GP7/8/9 + Pin36 3V3 + GND）

| INMP441 引脚 | 名称 | 连接 RP2040 Pico | Pico 物理引脚 | 说明 |
|---|---|---|---|---|
| 1 | **SCK** | GP8 | Pin 11 | I2S 位时钟（由 Pico 主模式输出） |
| 2 | **SD** | GP7 | Pin 10 | 麦克风数据输出 → Pico 输入 |
| 3 | **WS** | GP9 | Pin 12 | 字选择 / LRCLK（由 Pico 输出） |
| 4 | **L/R** | **GND** | Pin 18（或任意 GND） | **接地 = 左声道**（单 mic 用左声道） |
| 5 | GND | GND | Pin 18 | 地 |
| 6 | GND | GND | Pin 18 | 地（VDD 退耦电容接此脚） |
| 7 | **VDD** | **3V3** | **Pin 36** | 电源 1.8–3.3V（Pico 3V3 输出直接供） |
| 8 | **CHIPEN** | 模块内部上拉（不接 Pico） | — | 使能：高=工作；模块已拉高则无需连接 |
| 9 | GND | GND | Pin 18 | 地 |

> ⚠️ **3.3V 电源是 Pin 36（`3V3` 输出脚）**，你原先说的没错。相邻的 **Pin 37 才是 `3V3_EN`**（稳压器使能输入，已被内部 100kΩ 上拉），它是控制脚、**不是电源输出**，不要用来给麦克风供电。两者容易看混，务必接 Pin 36。
>
> **引脚硬约束（必读，容易接反）**：`rec_buffer.c` 的 I2S 中，SD 必须是这组连续 GPIO 里**编号最小**的那个。`i2s_in_slave` 按 `din_pin`、`din_pin+1`、`din_pin+2` 顺序读引脚 → **SD=din_pin、BCK=din_pin+1、LRCLK=din_pin+2**；`i2s_out_master` 在 `clock_pin_base`、`clock_pin_base+1` 生成 → **BCK=clock_pin_base、LRCLK=clock_pin_base+1**。两者共用 BCK/LRCLK，故 `clock_pin_base = din_pin + 1`，即 **SD 必须是最低位、BCK 中间、LRCLK 最高**。
> 因此 GP7/8/9 的映射是唯一固定的：**SD→GP7(Pin10)、SCK→GP8(Pin11)、WS→GP9(Pin12)**。⚠️ 若把 SCK 接到 GP7、SD 接到 GP9（常见笔误），信号会全部错位、采不到声音——请严格按本表连接。

> ⚠️ **GP16 冲突**：原工程的 WS2812 状态灯代码（`led.c`/`led.h`）默认驱动 **GP16(Pin21)**。但你的规划把 **GP16/GP17 用于 OLED 的 I²C0**，两者会抢同一根脚。**解决**：删掉/禁用原 WS2812 灯代码（你的规划已有 GP11/12/13 扩展 LED 与 GP25 板载 LED 做状态指示，不再需要 WS2812），把 GP16/17 留给 OLED。详见第 6 节。

### 2.2 接线要点（来自 INMP441 数据手册）

1. **VDD 退耦**：在 INMP441 的 **VDD(7) 与 GND(6) 之间并联一个 0.1µF 陶瓷电容**，尽量靠近麦克风。
2. **CHIPEN 必须拉高**（接 3V3）才能工作；悬空会掉电。
3. **L/R 接地**选择左声道；本工程只读取左声道数据。
4. **电源**：INMP441 工作电流约 1.4 mA，Pico 的 3V3 输出完全够用，无需外接 LDO。
5. **走线**：SCK/WS/SD 尽量短，远离射频与电源噪声。

### 2.3 接线示意

```
        INMP441 (底视 9-pin)               Raspberry Pi Pico
     ┌──────────────────┐
  1  ┤ SCK    ●         ├── GP8  (Pin 11)  I2S BCK
  2  ┤ SD     ●         ├── GP7  (Pin 10)  麦克风数据
  3  ┤ WS     ●         ├── GP9  (Pin 12)  I2S LRCLK
  4  ┤ L/R    ●         ├── GND  (Pin 18)  选择左声道
  5  ┤ GND    ●         ├── GND  (Pin 18)
  6  ┤ GND    ●         ├── GND  (Pin 18) ──┐
  7  ┤ VDD    ●         ├── 3V3  (Pin 36) ──┤
  8  ┤ CHIPEN ●         ├── (模块已拉高)    │
  9  ┤ GND    ●         ├── GND  (Pin 18)   │
     └──────────────────┘                  │
                             0.1µF ────────┘  (VDD↔GND 退耦，靠近 mic)
```

---

## 3. 软件改动（仅 `rec_buffer.c`）

原工程 `rec_take()` 直接把 I2S 交错缓冲（L0,R0,L1,R1…）透传给 USB。单 mic 时右声道槽是浮空无效值，必须丢弃并把左声道复制到 L、R 两个位置，使输出仍为合法 2 声道（L==R==单声道）：

```c
for (size_t f = 0; f < AUDIO_BUFFER_FRAMES; f++) {
    int32_t v = (buf[f * 2] >> 8);     // 左声道样本（偶数字）
    size_t base = f * 6;               // 每帧 L+R = 6 字节
    usb_buffer[base + 0] = (uint8_t)(v & 0xFF);
    usb_buffer[base + 1] = (uint8_t)((v >> 8) & 0xFF);
    usb_buffer[base + 2] = (uint8_t)((v >> 16) & 0xFF);
    usb_buffer[base + 3] = usb_buffer[base + 0];  // 右 = 左
    usb_buffer[base + 4] = usb_buffer[base + 1];
    usb_buffer[base + 5] = usb_buffer[base + 2];
}
```

USB 端点、采样率（48 kHz）、包大小（288 字节/毫秒）**全部保持原值**，因此 `main.c`、`tusb_config.h`、`usb_descriptors.c` 均无需改动。

> 此方案的“单声道”在主机侧表现为一个 **L/R 相同的立体声麦克风**，绝大多数录音软件可直接使用。若需要主机识别为“真正的 1 声道设备”，见附录。

---

## 4. 构建与烧录

### 4.0 关于 UAC2 补丁（重要更正）

> ⚠️ **旧说明"必须先打 TinyUSB PR #2937 补丁"已作废**。我已直接核对本机 SDK
> `2.1.1`：`lib/tinyusb/src/class/audio/audio_device.c|h` 与官方示例
> `tinyusb/examples/device/audio_4_channel_mic` **均已内置完整 UAC2 支持**，
> 不需要、也不应该再打旧补丁（打旧补丁反而会冲突）。工程用到的宏
> `TUD_AUDIO_EP_SIZE`（定义于 `usbd.h`）、`CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_TX`、
> `CFG_TUD_AUDIO_FUNC_1_N_BYTES_PER_SAMPLE_TX`、`CFG_TUD_AUDIO_FUNC_1_CHANNEL_PER_FIFO_TX`
> 在 2.1.1 下全部可用，**代码无需改动即可编译**。

### 4.1 准备工具链（只需做一次）

本工程已针对 **Raspberry Pi Pico SDK 2.1.1** 验证。两种方式二选一：

- **方式 A（推荐，最省事）**：安装 VS Code 扩展 **Raspberry Pi Pico**。首次打开本工程时，扩展会
  自动下载并管理工具链（`arm-none-eabi-gcc` + `CMake` + `Ninja` + SDK），无需手动设环境变量。
  本工程的 `.vscode/` 已配好（`settings.json` / `cmake-kits.json`）。
- **方式 B（命令行）**：手动安装 `arm-none-eabi-gcc`（14_2_Rel1 或相近）、`CMake ≥3.13`、`Ninja`，
  并设置 `PICO_SDK_PATH` 指向 SDK 目录（例如
  `C:\Users\Administrator\.pico-sdk\sdk\2.1.1`）。

### 4.2 构建（生成 .uf2）

- **VS Code 方式 A**：底部状态栏选 `Pico SDK (2.1.1)` kit → `Build`；或 `Ctrl+Shift+P` →
  *Pico: Build Project*。产物在 `build/i2s_mic.uf2`。
- **命令行方式 B**：

```bash
cd example/pico_mono_mic
mkdir -p build && cd build
cmake -G Ninja -DPICO_SDK_PATH="C:/Users/Administrator/.pico-sdk/sdk/2.1.1" ..
ninja
# 生成 build/i2s_mic.uf2
```

### 4.3 烧录到 Pico

> Pico 的 USB 口**身兼两职**：按住 BOOTSEL 接电脑 = 出现 `RPI-RP2` 烧录盘；
> 正常接入（不按 BOOTSEL）= 以 UAC2 麦克风设备运行。两者靠"怎么插"切换。

1. **按住 Pico 上的 BOOTSEL 键不放**，用 USB 线接电脑 → 出现 `RPI-RP2` 盘。
2. 把 `build/i2s_mic.uf2` 复制进去；复制完 Pico 自动重启并进入麦克风模式。
3. （命令行党）也可：`picotool load i2s_mic.uf2 -f`（需先装 picotool，本机 `.pico-sdk/picotool` 已有）。

### 4.4 ⚠️ 烧录前再确认接线（最容易翻车）

严格按 **SD→GP7(Pin10)、SCK→GP8(Pin11)、WS→GP9(Pin12)、VDD→Pin36(3V3)、GND→任意GND、L/R→GND** 连接
（见第 2.1 节）。**SD 必须是 GP7**——PIO 硬约束要求 SD 是连续 GPIO 里编号最小的，
接反（如把 SCK 接到 GP7、SD 接到 GP9）会采不到声音。

---

### 4.5 一键构建（本机已就绪）

本机已在 `C:\Users\Administrator\.pico-sdk\` 下装好**自包含的 2.1.1 全链路工具链**：

| 组件 | 路径 |
|---|---|
| Pico SDK 2.1.1 | `.pico-sdk\sdk\2.1.1` |
| ARM GCC 14.2.Rel1 | `.pico-sdk\toolchain\14_2_Rel1` |
| CMake 3.31.5 | `.pico-sdk\cmake\v3.31.5` |
| Ninja 1.12.1 | `.pico-sdk\ninja\v1.12.1` |
| pioasm / elf2uf2 2.1.1 | `.pico-sdk\tools-2.1.1\pico-sdk-tools` |
| picotool 2.1.1 | `.pico-sdk\tools-2.1.1\picotool` |

一键编译：

```powershell
powershell -ExecutionPolicy Bypass -File tools\build.ps1
```

产物：`build211\i2s_mic.uf2`（已用 picotool 校验，family ID `rp2040`，可直接拖入 BOOTSEL 盘烧录）。

> ⚠️ **关键点**：SDK 2.1.1 调用 `pioasm -v <版本>` 生成 `.pio.h`，**1.x 版 pioasm 不认 `-v` 会报错**；且生成 `.uf2` 要求 **picotool ≥ 2.1.1**。这两个预编译工具从 `raspberrypi/pico-sdk-tools` 的 `v2.1.1-3` release 获取（体积很小），脚本已含。
> 若某环境确实没有 picotool，可加 `-DPICO_SKIP_EXTRA_OUTPUTS=1` 跳过它，再用 `tools/elf2uf2.py` 把 `.elf` 手动转成 `.uf2`。

## 5. 验证出声（核对清单）

按以下顺序逐项确认，任一步卡住都能快速定位：

1. **枚举**：正常接入 Pico（不按 BOOTSEL），主机应识别出一个 USB 音频设备，
   产品名 `MicNode_2Ch`（可在 `usb_descriptors.c` 改）。Windows 设备管理器里看得到
   "USB 音频设备 / Microphone"；macOS 在「音频 MIDI 设置」、Linux 在 `arecord -l` 可见。
   - ❌ 看不到设备 → 多半是 SDK 的 UAC2 没编进去（确认 SDK 2.1.1、没乱打旧补丁），
     或 USB 线只充电不传数据。
2. **选为输入源**：在系统声音设置 / 录音软件（Audacity、OBS、系统录音机）里把
   `MicNode_2Ch` 选为**默认输入设备**。
3. **录音测试**：录 5~10 秒，应该有声音且**左右两轨完全相同**（单 mic 复制成伪立体声，属预期）。
   - ❌ 完全静音 → 优先查接线：**SD 必须是 GP7**，SCK=GP8、WS=GP9；L/R 是否接地；VDD 是否 Pin36。
   - ❌ 录到爆音/杂音 → 检查 VDD 退耦 0.1µF 是否靠近 mic；SCK/WS/SD 走线是否过长受干扰。
4. **调试串口（可选）**：GP0 为 TX（115200 8N1），需另接 USB 转串到电脑看打印。
   上电应有 `RP2040 Starting`；切换静音/音量时打印 `Mute:` / `Volume:`。
   无串口也不影响使用，仅用于排错。
5. **状态灯**：当前固件 WS2812 灯驱动 GP16，但你的板型把 GP16/17 留给 OLED，
   故本阶段 GP16 上没接 LED 属正常（灯不亮）。枚举/静音状态可改用 GP25 板载 LED
   或串口判断。等接 OLED 时再按第 6.1 节删掉 WS2812 代码即可。

> 提示：Pico 的 USB 既当烧录用又当音频用。**要重新烧录就按住 BOOTSEL 再插**；
> **要当麦克风就直接插**。两者靠插法切换，不要同时期望一个口既在 RPI-RP2 又输出声音。

---

## 附：改造成「真正 1 声道」USB 设备（可选）

若希望主机把它识别为 1 声道（而非 L/R 复制的立体声），需改动 UAC2 描述符与缓冲；SDK 2.1.1 的 `usbd.h` 已内置对应的 1 声道宏（`TUD_AUDIO_MIC_ONE_CH_DESCRIPTOR` / `TUD_AUDIO_DESC_FEATURE_UNIT_ONE_CHANNEL`），可直接套用。要点：

1. `tusb_config.h`：`CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_TX 1`（原 2）；`CFG_TUD_AUDIO_FUNC_1_CHANNEL_PER_FIFO_TX 1`（原 2，否则整除为 0）。
2. `usb_descriptors.c`：把 `TUD_AUDIO_MIC_TWO_CH_DESCRIPTOR(...)` 换成 **ONE_CH** 变体，`nchannelslogical` 设为 `0x01`。
3. `rec_buffer.c`：`USB_AUDIO_BUFFER_LEN` 改为 `1 * SAMPLE_RATE / 1000 = 48`；`rec_take()` 只写单声道（每样本 3 字节，不再复制 R）。
4. `main.c`：`tud_audio_write_support_ff(0, buf, AUDIO_SAMPLE_RATE / 1000 * 3 * 1)`（144 字节）。

> 该路径依赖 SDK 描述符宏的具体命名，**请在你的 SDK 源码中核对后再改**，否则易编译失败或枚举异常。默认（双轨复制）方案已可直接使用，优先推荐。

---

## 已知问题（继承自原工程）

- **软件音量未生效**：`rec_take()` 的音量增益被注释，目前仅「静音」真正生效；如需软件增益，在此处实现后再写入 24-bit。
- **首帧可能损坏**：I2S 从机可能在帧中间启动，第一笔样本可能错位（数据手册已注明），对单 mic 影响极小。

---

## 6. 对接你的完整板型规划（GP16 冲突 + 后续外设）

你给出的完整 GPIO 规划（PTT、8 个功能键、扩展 LED×3、蜂鸣器、OLED I²C0、板载 LED）是**超出纯麦克风端口**的更大设备。当前 `pico_mono_mic` 固件**只实现麦克风 + 原 WS2812 灯**，其余外设需后续接入。对接时注意：

### 6.1 必须解决：GP16 冲突（WS2812 灯 vs OLED I²C）
- 原固件 `led.c`/`led.h` + `main.c` 里的 `led_init()` / `led_blinking_task()` 把 **GP16** 当 WS2812 驱动；你的规划把 **GP16/GP17** 用于 OLED 的 I²C0，二者抢脚。
- **推荐做法**：删掉 WS2812 相关代码（`led.c`、`led.h`、`main.c` 中的调用、CMakeLists 里 `led.c` 与 `ws2812.pio` 的生成），用你的 GP11/12/13 + GP25 做状态指示。
- 若想保留 WS2812，则把 OLED 挪到其它 I²C 脚（但 GP16/17 是 I²C0 默认对，不推荐）。

### 6.2 其余 GPIO 已核对、无冲突（当前固件占用：GP0/1 调试串口、GP7/8/9 I²S）
| 你的功能 | GPIO | 冲突？ |
|---|---|---|
| PTT / 功能键 1–8 / 蜂鸣器 / 备用 | GP2–GP15 | 无（固件未占用） |
| 扩展状态 LED 1/2/3 | GP11/12/13 | 无 |
| OLED SDA/SCL（I²C0） | GP16/17 | 与 WS2812 冲突 → 见 6.1 |
| 状态 LED（板载） | GP25 | 无（固件未占用） |
| I²S SCK/WS/SD | GP8/9/7 | 无（麦克风专用） |

> 按键、OLED、蜂鸣器属于后续要写进 `main.c` 的功能，加入时不会与麦克风（GP7/8/9）冲突。建议：先用本固件把麦克风 + USB 音频跑通，再逐步加入按键扫描、I²C OLED 驱动、蜂鸣器 PWM。

### 6.3 注意 stdio 串口占用 GP0/GP1
原固件 `main.c` 用 `stdio_uart_init_full(uart0, 115200, 0, -1)` 把 **GP0 当 TX** 做调试打印。你的规划没分配 GP0/GP1，留作调试即可；若日后要用 GP0/GP1 做别的，记得在 CMakeLists 里把 `pico_enable_stdio_uart` 关掉。
