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

// 引脚（单 INMP441 单声道，板型规划：SD=GP18 / SCK=GP19 / WS=GP20）：
//   din_pin        = 18 (GP18, Pin24) -> SD  麦克风数据
//   clock_pin_base = 19 (GP19, Pin25) -> SCK/BCK，且 19+1=20 -> WS/LRCLK (GP20, Pin26)
//
// 硬约束：clock_pin_base 必须等于 din_pin+1，且三者连续 ——
//   i2s_in_slave   按 din / din+1 / din+2 读引脚（SD / BCK / LRCLK）
//   i2s_out_master 在 base / base+1 产生时钟（BCK / LRCLK）
// 两者共用 BCK/LRCLK，所以 SD 必须是这组 GPIO 里编号最小的那个。
// 顺序固定为 SD(最小) -> SCK -> WS，接反会完全采不到声音。
//
// sck_pin=10、dout_pin=6 在 sck_enable=false（仅接收）下未使用，保持原值即可。
static i2s_config my_i2s_config = { 48000, 256, 32, 10, 6, 18, 19, false };

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

// 已知问题：volume（软件增益）目前未生效，仅 mute 真正生效。
// 如需软件增益，在下面写入 usb_buffer 前对 v 做增益运算即可。
uint8_t* rec_take(uint8_t mute, uint8_t volume)
{
    if (audio_buffer_items == 0 || mute) {
        return zero_buffer;
    }

    int buffer_pos = (audio_buffer_write_index + USB_AUDIO_BUFFERS - audio_buffer_items) % USB_AUDIO_BUFFERS;
    int32_t* buf = audio_buffers[buffer_pos];

    // ===== 单声道(MONO)处理 =====
    // 单颗 INMP441（L/R 接地 = 左声道）时，I2S 只有"左声道槽"有有效数据，
    // "右声道槽"是麦克风三态(浮空)读出的无效值，必须丢弃。
    // 本实现把左声道数据同时写入 L 和 R 两个位置，使 USB 仍以"立体声"格式
    // 输出 (L==R==单声道)，无需改动 UAC2 描述符。
    for (size_t f = 0; f < AUDIO_BUFFER_FRAMES; f++) {
        int32_t v = (buf[f * 2] >> 8); // I2S 缓冲为 L0,R0,L1,R1,...，左声道在偶数字处
        size_t base = f * 6;           // 每帧 2 通道 × 3 字节 = 6 字节

        usb_buffer[base + 0] = (uint8_t)(v & 0xFF);
        usb_buffer[base + 1] = (uint8_t)((v >> 8) & 0xFF);
        usb_buffer[base + 2] = (uint8_t)((v >> 16) & 0xFF);

        // 右声道 = 左声道（单声道复制）
        usb_buffer[base + 3] = usb_buffer[base + 0];
        usb_buffer[base + 4] = usb_buffer[base + 1];
        usb_buffer[base + 5] = usb_buffer[base + 2];
    }

    audio_buffer_items--;

    return usb_buffer;
}
