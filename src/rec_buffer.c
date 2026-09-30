#include "rec_buffer.h"
#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/i2c.h"
#include "hardware/irq.h"
#include "hardware/pio.h"
#include "i2s.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

static __attribute__((aligned(8))) pio_i2s i2s;

#define SAMPLE_RATE 48000
#define USB_AUDIO_BUFFER_LEN (2 * SAMPLE_RATE / 1000)
#define USB_AUDIO_BUFFERS 4

static int32_t audio_buffer_items = 0;
static int32_t audio_buffer_write_index = 0;

int audio_data_ready()
{
    return audio_buffer_items > 0;
}

static __attribute__((aligned(8))) uint8_t zero_buffer[USB_AUDIO_BUFFER_LEN * 3];
static __attribute__((aligned(8))) int32_t audio_buffers[USB_AUDIO_BUFFERS][USB_AUDIO_BUFFER_LEN];

// 实际硬件接线（RP2040 Pico + 单颗 INMP441，L/R 接地 => 左声道模式）：
//   Pin10 = GP7  -> SD   麦克风数据
//   Pin11 = GP8  -> SCK/BCK
//   Pin12 = GP9  -> WS/LRCLK
//   Pin36 = 3V3 (VCC)，Pin18 = GND（INMP441 的 L/R 也接 GND）
//
// 硬约束：clock_pin_base 必须等于 din_pin+1，且三者连续 ——
//   i2s_in_slave   按 din / din+1 / din+2 读引脚（SD / BCK / LRCLK）
//   i2s_out_master 在 base / base+1 产生时钟（BCK / LRCLK）
// 两者共用 BCK/LRCLK，所以 SD 必须是这组 GPIO 里编号最小的那个。
// 顺序固定为 SD(最小) -> SCK -> WS，接反会完全采不到声音。
//
// sck_pin=10、dout_pin=6 在 sck_enable=false（仅接收）下未使用，保持原值即可。
// I²S 引脚固定为实际硬件接线（GP7/8/9），不再提供备用板型（GP18/19/20 已舍弃）。
// PIO 硬约束：clock_pin_base == din_pin+1，且三者连续 —— SCK=GP8、WS=GP9 由 GP7 推导。
static i2s_config my_i2s_config = { 48000, 256, 32, 10, 6, 7, 8, false };

static void process_audio(const int32_t* input, int32_t* output, size_t num_frames)
{
    if (audio_buffer_items < USB_AUDIO_BUFFERS) {
        memcpy(audio_buffers[audio_buffer_write_index], input, num_frames * 2 * sizeof(int32_t));
        audio_buffer_items++;
        audio_buffer_write_index = (audio_buffer_write_index + 1) % USB_AUDIO_BUFFERS;
    }
}

static void dma_i2s_in_handler(void)
{
    /* We're double buffering using chained TCBs. By checking which buffer the
     * DMA is currently reading from, we can identify which buffer it has just
     * finished reading (the completion of which has triggered this interrupt).
     */
    if (*(int32_t**)dma_hw->ch[i2s.dma_ch_in_ctrl].read_addr == i2s.input_buffer) {
        // It is inputting to the second buffer so we can overwrite the first
        process_audio(i2s.input_buffer, i2s.output_buffer, AUDIO_BUFFER_FRAMES);
    } else {
        // It is currently inputting the first buffer, so we write to the second
        process_audio(&i2s.input_buffer[STEREO_BUFFER_SIZE], &i2s.output_buffer[STEREO_BUFFER_SIZE], AUDIO_BUFFER_FRAMES);
    }
    dma_hw->ints0 = 1u << i2s.dma_ch_in_data; // clear the IRQ
}

void rec_init()
{
    memset(zero_buffer, 0, sizeof(zero_buffer));
    i2s_program_start_synched(pio1, &my_i2s_config, dma_i2s_in_handler, &i2s);
}

__attribute__((aligned(8))) uint8_t usb_buffer[USB_AUDIO_BUFFER_LEN * 3];

// 单颗 INMP441（L/R 接地=左声道模式）：I2S 缓冲布局为 L0,R0,L1,R1,... 共 USB_AUDIO_BUFFER_LEN 个 24-bit 样本。
// 只接一颗麦时，未被驱动的那个槽（实测多为奇数字/右槽）是三态浮空的无效值；若直接当有效音频送出，
//   会一边无声/单边发声，且浮空噪声混入 → 原 mic 音质差的根因。
// 修复（见下方 rec_take）：同时抽出左右两槽、各自 >>8 取 24-bit，比较绝对值取"有声的那一侧"，
//   再复制成双声道输出。这样既隔离浮空底噪（音质提升），又让左右都出声，且不依赖手动指定左右槽。
// 关键：原始 mic 用 raw>>8 能跑通（有声），说明 FIFO 字实际布局是 bit31:8 已是 24-bit 数据
// （I2S 1-bit 延迟位已被 PIO 吞掉 / INMP441 左对齐），直接算术右移 8 即正确抽取、符号也正确。
// 曾错误地加 <<1 想“跳过延迟位”，结果把已对齐的数据推到 bit31 溢出 → 输出全 0 → 完全不拾音，已撤回。
// 说明：volume（软件增益）当前未生效，仅 mute 真正生效（与原始工程一致）；采集/时钟/USB 全未改动。
uint8_t* rec_take(uint8_t mute, uint8_t volume)
{
    (void)volume; // 软件增益未实现，保持与原始工程一致
    if (audio_buffer_items == 0 || mute) {
        return zero_buffer;
    }

    int buffer_pos = (audio_buffer_write_index + USB_AUDIO_BUFFERS - audio_buffer_items) % USB_AUDIO_BUFFERS;
    int32_t* buf = audio_buffers[buffer_pos];

    for (size_t i = 0; i < USB_AUDIO_BUFFER_LEN; i++) {
        // 单麦只一个槽有有效音频：同时抽出左右两槽的 24-bit，取绝对值更大者（有声侧），复制成双声道。
        // 这样无论硬件实际是左槽还是右槽在响，都能自动选对，避免手动选错槽导致静音。
        int32_t lv = buf[i & ~1u] >> 8;     // 偶数索引槽抽出的 24-bit
        int32_t rv = buf[i |  1u] >> 8;     // 奇数索引槽抽出的 24-bit
        int32_t lmag = (lv < 0) ? -lv : lv; // 绝对值
        int32_t rmag = (rv < 0) ? -rv : rv;
        int32_t v = (rmag > lmag) ? rv : lv; // 选有声音的那一侧
        usb_buffer[i * 3 + 0] = (uint8_t)(v & 0xFF);
        usb_buffer[i * 3 + 1] = (uint8_t)((v >> 8) & 0xFF);
        usb_buffer[i * 3 + 2] = (uint8_t)((v >> 16) & 0xFF);
    }

    audio_buffer_items--;

    return usb_buffer;
}
