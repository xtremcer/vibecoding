# voice_clear 版本说明（v0.4 · 左右声道清晰 + 音质提升）

`voice_clear` 分支的固件 = **master（v0.3 音频版）源码 + 一处关键修复**：`rec_take()` 改为「自动选槽、左右都响」。
它是一颗**纯音频（UAC2）麦克风固件，不含 HID / 不含 PTT**（与 ptt 分支的区别）。

- 来源：用户提供压缩包 `mic_firmware_src_LR_clear.zip`（`vibecoding-mic/` 子目录）。
- 分支：从 `master`（v0.3 基线，commit `8bf8cfa`）切出 `voice_clear`。
- 程序版本：`pico_set_program_version` = `0.4`。
- 身份：`USB_VID 0xCafe` / `USB_PID 0x4A10`，UAC2，48 kHz / 24-bit / 2 声道，音频独占（无 HID）。
- 已用静态 ELF 描述符解码核对：DEVICE `PID=0x4a10`、CONFIG `total=145`（音频 145，无 HID 段）、EP `0x81` `maxpkt=294`，与 v0.3 音频描述符**逐字节一致**。

---

## 一、这次到底改了什么（唯一改动）

只动了 **1 个文件的 1 个函数**：`src/rec_buffer.c` 的 `rec_take()`。其余文件（`usb_descriptors.c`、`main.c`、`i2s.c`、`*.pio` 等）原样保留。

I²S DMA 缓冲布局是 `L0,R0,L1,R1,...`（左右 24-bit 样本交替）。修复版把左右两槽都抽出来比绝对值，只取「有声音的那一侧」，再复制成双声道：

```c
for (size_t i = 0; i < USB_AUDIO_BUFFER_LEN; i++) {
    int32_t lv = buf[i & ~1u] >> 8;   // 偶数索引槽（左）抽出的 24-bit
    int32_t rv = buf[i |  1u] >> 8;   // 奇数索引槽（右）抽出的 24-bit
    int32_t lmag = (lv < 0) ? -lv : lv;
    int32_t rmag = (rv < 0) ? -rv : rv;
    int32_t v = (rmag > lmag) ? rv : lv;   // 选有声音的那一侧
    usb_buffer[i * 3 + 0] = (uint8_t)(v & 0xFF);
    usb_buffer[i * 3 + 1] = (uint8_t)((v >> 8) & 0xFF);
    usb_buffer[i * 3 + 2] = (uint8_t)((v >> 16) & 0xFF);
}
```

---

## 二、为什么这能提升音质（根因）

**单颗 INMP441（L/R 接地 = 左声道模式）只有一个 I²S 槽有真实音频，另一个槽是三态浮空的无效值。**

原代码逐元素 `buf[i] >> 8`，等于把「左槽有效数据」和「右槽浮空垃圾」交替当成左右声道送出，导致：
1. 一边无声 / 单边发声；
2. 浮空噪声混入信号 → 音质发飘、有底噪（这就是 mic 比 mate 音质差很多的根因）。

本次修复三件事一起解决：
- **隔离浮空槽**：左右都抽出来比绝对值，只取「有声的一侧」，浮空噪声被彻底排除 → 音质提升；
- **左右都出声**：选出的有效样本复制成双声道，录音软件看到的是干净立体声；
- **不依赖手动指定左右**：自动选槽，硬件槽序接反也不会静音（早期手动固定取左槽曾导致完全不拾音）。

> 附：原始 `>>8` 移位本来就是对的。INMP441 左对齐输出，PIO 已吞掉 I²S 1-bit 延迟位，FIFO 字 `bit31:8` 已是 24-bit 数据，直接算术右移 8 即符号正确；曾误加 `<<1` 反而把已对齐数据推到 bit31 溢出成 0（完全不拾音），已确认撤回。所以仅在「槽选择」层面修，不动采集/移位。

---

## 三、接线（接错一定没声）

- GP7 = SD（麦克风数据）
- GP8 = SCK / BCK
- GP9 = WS / LRCLK
- INMP441 的 L/R 接 GND（左声道模式），VCC = 3V3，GND = GND

PIO 硬约束：`clock_pin_base == din_pin+1` 且三者连续，即 SCK=GP8、WS=GP9 必须由 GP7 推导，顺序固定 SD→SCK→WS，接反完全采不到声音。

---

## 四、重新编译（本机已验证可跑通）

本构建环境**没有宿主 C++ 编译器**，且 SDK 的 `Findpioasm.cmake` 默认会尝试从源码编译 pioasm。为此本分支做了两处适配，使「开箱即编」：

1. `src/i2s.pio.h` 已用预编译 pioasm 预先生成并提交；`CMakeLists.txt` 移除了 `pico_generate_pio_header(...)`，避免触发 pioasm 源码编译（`ws2812.pio.h` 为手写版，已在 `src/`）。
2. `CMakeLists.txt` 增加守卫：当本机存在 `.pico-sdk` 预编译 picotool 时自动设置 `picotool_DIR`，避免 `pico_add_extra_outputs` 去联网拉取 picotool（其他机器上该守卫自动跳过，无害）。

开箱构建命令（PowerShell，使用 `.pico-sdk` 工具链）：

```powershell
$env:PICO_SDK_PATH = "C:/Users/Administrator/.pico-sdk/sdk/2.1.1"
$env:Path = "C:/Users/Administrator/.pico-sdk/cmake/v3.31.5/bin" +
            ";C:/Users/Administrator/.pico-sdk/ninja/v1.12.1" +
            ";C:/Users/Administrator/.pico-sdk/toolchain/14_2_Rel1/bin" +
            ";" + $env:Path
cmake -G Ninja -B build_voice_clear -S .
cmake --build build_voice_clear
# 产物：build_voice_clear/i2s_mic.uf2  （即 deliverable）
```

> 若换到完整工具链环境，可改回 `pico_generate_pio_header` 并删除已提交的 `src/i2s.pio.h`，构建流程不变。

---

## 五、固件清单（本分支）

| 文件 | 大小 | SHA256 | 说明 |
|------|------|--------|------|
| `firmware/i2s_mic_v0.4.uf2` | 80384 B | `0c6f28d3f5b02761090a83d273a9de297878776b8c2005a039760fd64453a01f` | ✅ 本分支重新编译的好固件（PID `0x4A10`，可烧录） |
| `firmware/i2s_mic_v0.4.elf` | 777316 B | — | 配套 ELF（调试/反汇编用） |
| `firmware/i2s_mic_v0.4_zip_prebuilt.uf2` | 80384 B | `3b4b83c637d2e6d54b7a2452a0ef9aa3d75d7b3c8a6152db87dd106912432624` | 压缩包作者原厂预编译固件（仅供参考/对照） |

> 两个 `.uf2` 的 SHA 不同是**正常的**：作者原厂固件在另一台机器编译，ELF 内嵌的构建时间戳/路径不同；二者源码与 USB 描述符逐字节一致，功能完全等价。

---

## 六、烧录

1. 按住 Pico 上的 **BOOTSEL** 键再插 USB，电脑出现 `RPI-RP2` 盘；
2. 把 `firmware/i2s_mic_v0.4.uf2` 拖入该盘，Pico 自动重启进入麦克风固件；
3. 系统识别为 USB 音频设备（UAC2，48 kHz / 24-bit / 2ch）。录音软件选该设备即可，左右声道均有干净声音。

---

## 七、与其他分支的关系

- `master`：v0.3 音频版基线（透明透传，左槽有效、右槽浮空），即本分支的「修复前」对照。
- `ptt`：复合固件（UAC2 麦克风 + HID 键盘），曾因新 PID `0x4B10` 触发 Windows 音频驱动缓存问题，已回退 PID 到 `0x4A10`（待真机确认）。
- `voice_clear`（本分支）：纯音频固件 + `rec_take()` 选槽修复，专注「左右都响 + 音质提升」，不含 HID/PTT。

> 原始作者改动说明见同目录 `08_voice_clear_改动说明.md`（含更口语化的根因叙述）。
