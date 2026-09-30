# 固件版本清单

每次编译的固件按版本号放入本目录（`i2s_mic_v<版本>.uf2`），并记录哈希与来源，便于回溯与对比。

## v0.1（当前主线）

| 项目 | 值 |
|---|---|
| 文件 | `i2s_mic_v0.1.uf2` / `i2s_mic_v0.1.elf` |
| 版本 | 0.1（`pico_set_program_version`） |
| 编译日期 | 2026-09-30 |
| SHA256 | `f30973ebe147163735813727feecc6a78c59b16ad579d6f0bb97adb9b32fc3fc` |
| 设备名 | `vibecoding-mate mic` |
| 平台 | RP2040（Pico），family ID `rp2040` |
| 源码 | 根目录 `src/`（= 原始立体声工程 `stereo_mic-master_mod`，I²S 立体声原样透传） |
| I²S 引脚 | SD=GP7、SCK(BCK)=GP8、WS(LRCLK)=GP9 |
| 音频 | UAC2，48 kHz / 24-bit / 2 声道 |
| 工具链 | Pico SDK 2.1.1 + ARM GCC 14.2.Rel1 + CMake 3.31.5 + Ninja 1.12.1 + pioasm/picotool 2.1.1 |

### 可复现性

全新构建目录重建后，`.uf2` 的 SHA256 与上述**逐位一致**，构建是确定性的。

```powershell
# 复现验证
powershell -ExecutionPolicy Bypass -File tools\build.ps1
certutil -hashfile build\i2s_mic.uf2 SHA256
# 应输出 f30973ebe147163735813727feecc6a78c59b16ad579d6f0bb97adb9b32fc3fc
```

---

## 版本号约定

- 版本号跟随 `CMakeLists.txt` 里的 `pico_set_program_version(i2s_mic "<版本>")`；
- 每次出固件时同步改版本号，并把 `.uf2`/`.elf` 以 `i2s_mic_v<版本>.uf2` 命名放入本目录；
- 在本文件追加一条记录（版本 / 日期 / SHA256 / 源码 commit / 变更说明）。
