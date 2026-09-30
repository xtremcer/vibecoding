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

// 单声道复制（L = R）—— 针对本项目的单颗 INMP441（L/R 接地 = 左声道模式）。
//
// I²S 环形缓冲的布局是「左0, 右0, 左1, 右1, …」共 USB_AUDIO_BUFFER_LEN(=96) 个 24-bit 样本，
// 即 48 个立体声帧。由于只接了一颗麦克风且被配置为左声道：
//   - 偶数字（左声道槽）才是麦克风的有效数据；
//   - 奇数字（右声道槽）是 INMP441 在 WS=1 期间三态输出、PIO 读到的无效值，必须丢弃。
//
// 若直接透明透传，录音软件只会录到「左声道有声、右声道噪声/静音」，很多应用会只取单声道。
// 这里把左声道样本同时写入 L 与 R 两个位置（L == R == 单声道），使 USB 仍以 2 声道输出、
// 且左右两路都有声，而**无需改动 UAC2 描述符**（仍声明 2 声道），最稳妥。
//
// 说明：volume（软件增益）当前未生效，仅 mute 真正生效（保持与原始工程一致）。
uint8_t* rec_take(uint8_t mute, uint8_t volume)
{
    (void)volume; // 软件增益未实现，保持与原始工程一致（仅 mute 真正生效）
    if (audio_buffer_items == 0 || mute) {
        return zero_buffer; // 无数据或被静音：返回全 0 缓冲（静音）
    }

    int buffer_pos = (audio_buffer_write_index + USB_AUDIO_BUFFERS - audio_buffer_items) % USB_AUDIO_BUFFERS;
    int32_t* buf = audio_buffers[buffer_pos];

    // 按「立体声帧」遍历（每帧 = L,R 两路，各自 3 字节，共 6 字节）
    for (size_t f = 0; f < AUDIO_BUFFER_FRAMES; f++) {
        // 取左声道样本：I²S 缓冲中偶数字处的 32-bit 容器内，高 24 位有效
        int32_t v = (buf[f * 2] >> 8);
        size_t base = f * 6; // 本帧在 usb_buffer 中的字节起点

        // 左声道：24-bit 小端写入 3 字节
        usb_buffer[base + 0] = (uint8_t)(v & 0xFF);
        usb_buffer[base + 1] = (uint8_t)((v >> 8) & 0xFF);
        usb_buffer[base + 2] = (uint8_t)((v >> 16) & 0xFF);

        // 右声道 = 左声道（单声道复制），让左右两路都有声
        usb_buffer[base + 3] = usb_buffer[base + 0];
        usb_buffer[base + 4] = usb_buffer[base + 1];
        usb_buffer[base + 5] = usb_buffer[base + 2];
    }

    audio_buffer_items--;

    return usb_buffer;
}
