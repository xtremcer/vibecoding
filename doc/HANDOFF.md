# HANDOFF · 交接文档

> 最后更新：2026-09-30

## 1. 项目是什么

RP2040 Pico + INMP441 的 **USB 麦克风固件**，通过 **TinyUSB UAC2** 把 I²S 采集的音频送给主机，免驱识别为音频输入设备（设备名 `vibecoding-mate mic`，48 kHz / 24-bit / 2 声道）。

仓库根目录 `vibecoding/` 即为项目文件夹，结构见根 `README.md`。

## 2. 当前进度

| 事项 | 状态 |
|---|---|
| 原始工程分析 | ✅ 完成（`doc/03_架构分析_原始立体声工程.md`） |
| 工具链（SDK 2.1.1 全链路）搭建 | ✅ 完成，装在 `%USERPROFILE%\.pico-sdk\` |
| 主线（`src/`）可编译 | ✅ 成功，固件 `firmware/i2s_mic_v0.1.uf2`，SHA256 `f30973eb…b32fc3fc` |
| 构建可复现性验证 | ✅ 全新目录重建哈希一致 |
| 设备名改为 `vibecoding-mate mic` | ✅ 已改并编译验证 |
| 目录按 GitHub 规范整理（src/firmware/doc/assets/tools） | ✅ 完成 |
| Git 版本控制（master 主线） | ✅ 本地仓库已初始化并提交 |
| 推送到 GitHub 远程仓库 | ⏳ 待办（需要 GitHub 账号授权/令牌） |
| **实机验证：烧录后能否出声** | ⏳ **待办（当前最优先）** |
| 单声道适配合入主线 | ⏳ 待基线验证通过后进行 |

## 3. 主线状态（v0.3）

- **`src/`（master 主线）= v0.3**：引脚固定 `SD=GP7 / SCK=GP8 / WS=GP9`（实际硬件接线，P10/P11/P12），`rec_take()` 立体声透明透传（移植自 `assets/reference/stereo_mic-master`）。
- 设备名 `vibecoding-mate mic`，VID/PID = `0xCafe`/`0x4A10`（改 PID 强制 Windows 刷新缓存的设备名）。
- **GP18/19/20 备用板型已舍弃**：固件引脚写死 GP7/8/9，不再提供 `-DI2S_DIN_PIN` 切换。
- `assets/reference/pico_mono_mic/`、`stereo_mic-master_mod/` 仅为历史快照，勿直接照抄引脚。

也就是说：**当前主线固件要接 GP7/8/9**，不是 GP18/19/20。

## 4. 怎么编译 / 烧录

```powershell
# 编译（一键）
powershell -ExecutionPolicy Bypass -File tools\build.ps1      # 产物 build\i2s_mic.uf2

# 烧录
# 1) 按住 BOOTSEL 插 USB -> 出现 RPI-RP2 盘
# 2) 把 .uf2 拖进去，自动重启
```

环境依赖见根 `README.md` 第 5 节；若换机器，跑 `tools\install_deps.ps1` 重建工具链。

## 5. 下一步（建议顺序）

1. **基线验证（当前最优先）**
   烧 `firmware/i2s_mic_v0.1.uf2`，麦克风接 **GP7/8/9**，确认：
   - 主机是否识别出 `vibecoding-mate mic`；
   - 录音是否有声（单麦时通常只有一个声道有声，属预期）。
   - 若**不出声** → 问题在硬件/接线/USB 枚举层面；若**有声** → 链路 OK，可进入第 2 步。
2. ~~合入单声道版（GP18/19/20）~~：已验证与用户实际硬件不符，v0.3 回退为 GP7/8/9 透明透传，GP18/19/20 备用板型已舍弃。
3. **后续功能**（用户规划过的完整板型）：PTT/功能键、OLED（I²C0，注意 GP16 冲突需先移除 WS2812）、蜂鸣器 PWM、扩展 LED。
4. 可选：改为「真 1 声道」UAC2 描述符（当前是 L==R 伪立体声）。

## 6. 关键坑位（详见 `doc/04_注意事项.md`）

- I²S 三脚必须连续且 **SD 编号最小**（`SD → SCK → WS`），接反完全无声。
- Pico 供电脚是 **Pin 36（3V3）**，不是 Pin 37（3V3_EN）。
- pioasm / picotool **必须 2.1.1**（1.x 会报 `unknown option -v`）。
- 软件音量未生效；WS2812 占 GP16，与 OLED I²C0 冲突。

## 7. 哈希溯源（用于确认固件来自哪份源码）

| 固件 | SHA256 | 来源 |
|---|---|---|
| `firmware/i2s_mic_v0.1.uf2` | `f30973ebe147163735813727feecc6a78c59b16ad579d6f0bb97adb9b32fc3fc` | 根 `src/`（= 原 `stereo_mic-master_mod`） |
| `assets/reference/pico_mono_mic/i2s_mic.uf2` | `6dc80cfb104335c4ef8cb8d3f68202f526cb7f8bf1207126560acd11a1e22876` | 单声道适配版 |
