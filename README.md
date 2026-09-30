# vibecoding — RP2040 USB 麦克风（UAC2）固件

基于 **Raspberry Pi Pico（RP2040）** + **INMP441（I²S MEMS 麦克风）** 的 USB 音频采集固件，通过 **TinyUSB UAC2（USB Audio Class 2.0）** 把 I²S 采集到的音频送给主机，免驱识别为麦克风设备（设备名 `vibecoding-mate mic`）。

- 采样率 48 kHz，位深 24-bit，2 声道
- 裸机 superloop（无 RTOS）：PIO 实现 I²S + DMA 双缓冲 → 环形缓冲 → UAC2 IN 端点
- Pico SDK 2.1.1 工具链构建，构建过程**可复现**（SHA256 稳定）

---

## 1. 硬件

| 器件 | 说明 |
|---|---|
| 主控 | Raspberry Pi Pico / RP2040 |
| 麦克风 | INMP441（I²S 从机，24-bit MSB-first，标准 I²S） |
| 供电 | INMP441 **只能 3.3V**；由 Pico 的 3V3（**Pin 36**）供电 |

> ⚠️ INMP441 绝对不能接 5V。

---

## 2. 目录结构

```
vibecoding/
├── src/                 源码主程序（main.c / i2s.* / rec_buffer.* / led.* / usb_descriptors.c / tusb_config.h / *.pio）
├── firmware/            编译产物固件，按版本号存放（见 firmware/manifest.md）
├── doc/                 文档：需求 / 架构 / 注意事项 / handoff
├── assets/
│   ├── hardware/        硬件资料：Pico & RP2040 datasheet、INMP441 资料
│   └── reference/       借鉴参考的原始/移植工程（stereo_mic-master_mod、pico_mono_mic）
├── tools/               工具链依赖包与安装/构建脚本
├── CMakeLists.txt       根构建脚本（源码在 src/）
├── pico_sdk_import.cmake
├── LICENSE
└── README.md
```

---

## 3. 当前主线状态（重要）

`master` 主线目前是**原始立体声工程**的代码（`src/` = 原 `stereo_mic-master_mod`）：

- I²S：`SD=GP7`、`SCK(BCK)=GP8`、`WS(LRCLK)=GP9`
- `rec_take()` 原样透传立体声（未做单声道处理）
- 用于作为**基线对照**验证 I²S + USB 音频链路

**单声道适配版**（单个 INMP441、引脚 `SD=GP18 / SCK=GP19 / WS=GP20`、只取左声道复制到 L/R）目前放在
`assets/reference/pico_mono_mic/`，**尚未合入主干**。基线验证通过后，可将其并入 `src/` 作为主线。

---

## 4. 接线

### 4.1 主线（原始立体声，GP7/8/9）

| INMP441 | Pico | 物理脚 |
|---|---|---|
| SCK | GP8 | Pin 11 |
| WS  | GP9 | Pin 12 |
| SD  | GP7 | Pin 10 |
| L/R | GND | 选左声道 |
| VDD | 3V3 | **Pin 36** |
| GND | GND | Pin 18 |
| CHIPEN | 模块内部上拉，可不接 | — |

### 4.2 单声道适配版（GP18/19/20）

| INMP441 | Pico | 物理脚 |
|---|---|---|
| SD  | GP18 | Pin 24 |
| SCK | GP19 | Pin 25 |
| WS  | GP20 | Pin 26 |
| L/R | GND | 选左声道 |
| VDD | 3V3 | **Pin 36** |
| GND | GND | Pin 18 |

> ⚠️ **引脚硬约束（容易接反）**：PIO 的 `i2s_in_slave` 按 `din / din+1 / din+2` 读引脚，`i2s_out_master` 在 `base / base+1` 产生时钟，故必须满足 **`clock_pin_base == din_pin + 1`** —— 即 **SD 必须是这组连续 GPIO 里编号最小的**，顺序固定为 `SD → SCK → WS`（三个连续）。接反会完全采不到声音。
>
> ⚠️ VDD/退耦：在 INMP441 的 VDD 与 GND 之间就近并联 **0.1 µF** 陶瓷电容。

---

## 5. 构建环境

自包含的工具链装在 `%USERPROFILE%\.pico-sdk\`（由 `tools/install_deps.ps1` 安装）：

| 组件 | 路径 / 版本 |
|---|---|
| Pico SDK | `.pico-sdk\sdk\2.1.1` |
| ARM GCC | `.pico-sdk\toolchain\14_2_Rel1`（14.2.Rel1） |
| CMake | `.pico-sdk\cmake\v3.31.5` |
| Ninja | `.pico-sdk\ninja\v1.12.1` |
| pioasm / elf2uf2 | `.pico-sdk\tools-2.1.1\pico-sdk-tools`（2.1.1） |
| picotool | `.pico-sdk\tools-2.1.1\picotool`（2.1.1） |

> **pioasm 与 picotool 必须是 2.1.1 版**：SDK 2.1.1 调用 `pioasm -v <版本>`（1.x 不认该参数），且生成 `.uf2` 要求 picotool ≥ 2.1.1。

---

## 6. 编译

```powershell
# 方式 A：一键脚本
powershell -ExecutionPolicy Bypass -File tools\build.ps1

# 方式 B：命令行
$env:PICO_SDK_PATH = "$env:USERPROFILE\.pico-sdk\sdk\2.1.1"
$env:Path = "$env:USERPROFILE\.pico-sdk\toolchain\14_2_Rel1\bin;$env:USERPROFILE\.pico-sdk\cmake\v3.31.5\bin;$env:USERPROFILE\.pico-sdk\ninja\v1.12.1;$env:Path"
mkdir build; cd build
cmake -G Ninja `
  -Dpioasm_DIR="$env:USERPROFILE\.pico-sdk\tools-2.1.1\pico-sdk-tools\pioasm" `
  -Dpicotool_DIR="$env:USERPROFILE\.pico-sdk\tools-2.1.1\picotool\picotool" `
  ..
ninja
```

产物：`build/i2s_mic.uf2`。出固件后请**改版本号**并以 `i2s_mic_v<版本>.uf2` 放入 `firmware/`，同时更新 `firmware/manifest.md`。

---

## 7. 烧录与验证

1. 按住 Pico 的 **BOOTSEL** 插 USB → 出现 `RPI-RP2` 盘 → 把 `.uf2` 拖进去（自动重启进入运行模式）。
2. 正常插入（不按 BOOTSEL）→ 主机识别为 `vibecoding-mate mic`。
3. 录音软件选它 → 应有声音。主线是立体声透传，接单颗麦克风时通常**只有一个声道有声**（另一声道浮空），属预期。
4. Windows 若仍显示旧设备名：设备管理器卸载该设备后重新拔插。

> Pico 的 USB 口身兼两职：**按住 BOOTSEL 插 = 烧录盘；直接插 = USB 麦克风**。

---

## 8. 已知问题

- **软件音量未生效**：`rec_take()` 里的增益计算被注释，目前只有「静音」真正生效。
- **首帧可能错位**：I²S 从机可能在帧中间启动（数据手册已注明），影响极小。
- 原工程的 WS2812 状态灯占用 **GP16**；若后续把 GP16/17 给 OLED（I²C0），需移除 WS2812 相关代码。

---

## 9. 许可证

见 `LICENSE`（继承原工程）。
