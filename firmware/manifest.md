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

---

## ptt 分支固件（push-to-talk 实验版 · 仅 ptt 分支提供）

| 项目 | 值 |
|---|---|
| 文件 | `i2s_mic_ptt.uf2` / `i2s_mic_ptt.elf` |
| 基线 | 与主线 v0.3 一致：GP7/8/9 + PID `0x4A10` + 立体声透明透传 + 设备名 `vibecoding-mate mic` |
| 附加功能 | PTT：GP2 输入 + 内部上拉，低电平有效，约 5ms 消抖；`rec_take(is_muted() || !ptt_is_pressed(), vol)` —— **按住 PTT 才送音，松开静音** |
| SHA256 | `f65dab9645cd0b71abf3866a6c08d2ae40c58b4ab1398704eec6cbc703098661` |
| 同步 | 已 `git merge master`（v0.3，GP7/8/9 + PID 修复）合入 ptt |
