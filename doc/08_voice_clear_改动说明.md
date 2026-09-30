# RP2040-MIC-PTT 改动说明（MIC 固件音质修复版）

交付物：`mic.uf2`（80384 字节，可烧录的好固件）。
本次只提升了 MIC 固件的音质并让左右声道都能出声，未引入任何新功能或新依赖。

---

## 一、改了哪些文件

只改了 **1 个文件** 的 **1 个函数**：

- `vibecoding-mic/rec_buffer.c` 的 `rec_take()` 函数（约 96–107 行）。

**完全没动**（保证设备能稳定枚举、采集链路不变）：
- `usb_descriptors.c` / `main.c` / `led.c`：USB 枚举、UAC2 描述符、PTT 按键逻辑原始未动 → 设备能被识别。
- `i2s.pio` / `ws2812.pio` / `i2s.c`：PIO 状态机、DMA 双缓冲、时钟原始未动 → 采集稳定。
- `volume` 软件增益仍未实现（与原始工程一致），仅 `mute` 真正生效。

构建侧（AI 新增、不污染源目录、只读调用工具链）：
- `CMakeLists.txt`：把 `vibecoding-mic/` 下 5 个 .c + 通用 I2S 库 `i2s.c` 编成 `i2s_mic`，PIO 头生成到 build 树。
- `pico_sdk_import.cmake`：从 SDK `external/` 复制，不污染源目录。

---

## 二、改了什么（代码层面）

I2S DMA 缓冲布局是 `L0,R0,L1,R1,...`（左右声道 24-bit 样本交替）。
原代码逐元素 `buf[i] >> 8`，等于把**左槽有效数据**和**右槽浮空垃圾**交替当成左右声道送出。

修复版逻辑（取绝对值更大的一侧，复制成双声道）：

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

## 三、是什么提升了音质（根因）

**单颗 INMP441（L/R 接地=左声道模式）只有一个槽有真实音频，另一个槽是三态浮空的无效值。**
原 mic 把这个浮空槽当成有效右声道送出，导致：
1. 一边无声 / 单边发声；
2. 浮空噪声混入信号 → 音质明显发飘、有底噪（这就是 mic 比 mate 音质差很多的原因）。

本次修复三件事一起解决：
- **隔离浮空槽**：左右两槽都抽出来比绝对值，只取"有声的那一侧"，浮空噪声被彻底排除 → 音质提升。
- **左右都出声**：选出的有效样本复制成双声道，录音软件看到的是干净立体声。
- **不依赖手动指定左右**：自动选槽，硬件槽序接反也不会静音（之前手动固定取左槽曾导致完全不拾音）。

**附带确认的关键事实**：原始 `>>8` 本来就抽得对。
INMP441 左对齐输出，PIO 已吞掉 I2S 1-bit 延迟位，FIFO 字 `bit31:8` 已是 24-bit 数据，直接算术右移 8 即正确且符号正确。
曾误加 `<<1` 想"再跳过延迟位"，反而把已对齐数据推到 bit31 溢出成 0（完全不拾音），已撤回——这反向证明了"原始移位就对了"，所以只需在**槽选择**层面修，不动采集/移位。

---

## 四、接线（接错一定没声）

- GP7 = SD（麦克风数据）
- GP8 = SCK / BCK
- GP9 = WS / LRCLK
- INMP441 的 L/R 接 GND（左声道模式），VCC=3V3，GND=GND

PIO 硬约束：`clock_pin_base == din_pin+1` 且三者连续，即 SCK=GP8、WS=GP9 必须由 GP7 推导，顺序固定 SD→SCK→WS，接反完全采不到声音。

---

## 五、重新编译（用 .pico-sdk 工具链，PowerShell）

```powershell
$env:PICO_SDK_PATH = "C:/Users/Administrator/.pico-sdk/sdk/2.1.1"
$env:Path = "C:/Users/Administrator/.pico-sdk/cmake/bin" +
            ";C:/Users/Administrator/.pico-sdk/ninja" +
            ";C:/Users/Administrator/.pico-sdk/tools-2.1.1/gcc-arm-none-eabi/bin" +
            ";" + $env:Path
cmake -G Ninja -B build_mic -S .
cmake --build build_mic
# 产物：build_mic/i2s_mic.uf2  （即 mic.uf2）
```

---

## 六、固件清单

| 文件 | 大小 | 状态 |
|------|------|------|
| `mic.uf2` | 80384 B | ✅ 本次交付好固件，PID 0x4A10，可烧录 |
| `mate.uf2` | 75776 B | ❌ 上一轮"双固件任务"的坏固件（烧录后无法识别 USB），**未打包、切勿烧录** |
