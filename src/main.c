#include "led.h"
#include "buttons.h"
#include "status_led.h"
#include "oled.h"
#include "beep.h"
#include "cdc_cmd.h"
#include "state_exec.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bsp/board_api.h"
#include "hardware/clocks.h"
#include "hardware/uart.h"
#include "pico/stdio.h"
#include "pico/stdio_uart.h"
#include "rec_buffer.h"
#include "tusb.h"
#include "tusb_config.h"
#include "class/hid/hid_device.h"   // HID device API：tud_hid_keyboard_report / tud_hid_ready / HID_KEY_*
#include "hardware/gpio.h"   // GP2 按键输入

//--------------------------------------------------------------------+
// MACRO CONSTANT TYPEDEF PROTYPES
//--------------------------------------------------------------------+
#define AUDIO_SAMPLE_RATE CFG_TUD_AUDIO_FUNC_1_SAMPLE_RATE

/* Blink pattern
 * - 250 ms  : device not mounted
 * - 1000 ms : device mounted
 * - 2500 ms : device is suspended
 */
enum {
    BLINK_NOT_MOUNTED = 250,
    BLINK_MOUNTED = 1000,
    BLINK_SUSPENDED = 2500,
};

static uint32_t blink_interval_ms = BLINK_NOT_MOUNTED;

// Audio controls
// Current states
uint8_t mute[CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_TX + 1]; // +1 for master channel 0
uint8_t volume[CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_TX + 1]; // +1 for master channel 0
uint32_t sampFreq;
uint8_t clkValid;

// Range states
audio_control_range_2_n_t(1) volumeRng[CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_TX + 1]; // Volume range state
audio_control_range_4_n_t(1) sampleFreqRng; // Sample frequency range state

void led_blinking_task(void);
void audio_task(void);

int main(void)
{
    set_sys_clock_khz(132000, true);
    stdio_uart_init_full(uart0, 115200, 0, -1);
    sleep_ms(50);
    printf("RP2040 Starting\n");

    board_init();
    led_init();
    led_onboard_init();   // 板载 LED（标准 Pico=GP25）初始化，上电默认常亮

    for (int i = 0; i < 3; i++) {
        volume[i] = 255; // max volume
        mute[i] = 0;
    }

    // init device stack on configured roothub port
    tusb_rhport_init_t dev_init = {
        .role = TUSB_ROLE_DEVICE,
        .speed = TUSB_SPEED_AUTO
    };
    tusb_init(BOARD_TUD_RHPORT, &dev_init);

    if (board_init_after_tusb) {
        board_init_after_tusb();
    }

    buttons_init();      // 初始化所有按键引脚（输入 + 内部上拉），见 buttons.c
    status_led_init();   // 额外状态灯 busy(GP26)/plan(GP27)/idle(GP28)
    oled_init();         // OLED I2C1（SDA=GP10 / SCL=GP11）
    beep_init();         // 蜂鸣器 GP14（需三极管/MOS 驱动）
    // 状态执行器：上电应用「初始化展示」（boot_led = 三灯全亮/全灭）。
    // 注意：这里**不**置位 host_control，所以首个 SET STATE 到达前，
    // 本地 PTT→busy 灯的老行为仍然生效（v1.8 手感不变）；一旦收到 SET STATE 才由执行器接管。
    state_exec_init();
    cdc_cmd_init();                   // CDC 虚拟串口指令模块（上位机 LED/BEEP/STATE 控制）

    rec_init();

    sampFreq = AUDIO_SAMPLE_RATE;
    clkValid = 1;

    sampleFreqRng.wNumSubRanges = 1;
    sampleFreqRng.subrange[0].bMin = AUDIO_SAMPLE_RATE;
    sampleFreqRng.subrange[0].bMax = AUDIO_SAMPLE_RATE;
    sampleFreqRng.subrange[0].bRes = 0;

    while (1) {
        tud_task();
        led_blinking_task();
        audio_task();
        buttons_task();   // 自定义按键模块（去抖 + 组合键上报）
        cdc_cmd_task();   // CDC 虚拟串口指令解析（非阻塞，只做收/发/分发）
        status_led_task();  // 状态灯闪烁相位推进（非阻塞）
        beep_task();        // 蜂鸣器计时（旋律/单音）
        state_exec_task();  // 状态执行器：提示音重复/间隔、初始化展示超时

        // 本地示例：PTT 按下 → busy 灯亮。
        // 上位机一旦下发过 LED 指令就置位"主机接管"，此时本地逻辑让位，避免两边抢同一盏灯。
        if (!status_led_is_host_control()) {
            status_led_set(LED_BUSY, !gpio_get(PIN_PTT));
        }
    }
}

//--------------------------------------------------------------------+
// Device callbacks
//--------------------------------------------------------------------+

// Invoked when device is mounted
void tud_mount_cb(void)
{
    blink_interval_ms = BLINK_MOUNTED;
}

// Invoked when device is unmounted
void tud_umount_cb(void)
{
    blink_interval_ms = BLINK_NOT_MOUNTED;
}

// Invoked when usb bus is suspended
// remote_wakeup_en : if host allow us  to perform remote wakeup
// Within 7ms, device must draw an average of current less than 2.5 mA from bus
void tud_suspend_cb(bool remote_wakeup_en)
{
    (void)remote_wakeup_en;
    blink_interval_ms = BLINK_SUSPENDED;
}

// Invoked when usb bus is resumed
void tud_resume_cb(void)
{
    blink_interval_ms = tud_mounted() ? BLINK_MOUNTED : BLINK_NOT_MOUNTED;
}

uint8_t is_muted()
{
    return mute[0] || mute[1] || mute[2];
}

void audio_task(void)
{
    static uint32_t start_ms = 0;
    if (audio_data_ready()) {
        int32_t vol = ((int32_t)volume[0] * (int32_t)volume[1]) / 255;
        uint8_t* buf = rec_take(is_muted(), vol);
        tud_audio_write_support_ff(0, buf, AUDIO_SAMPLE_RATE / 1000 * 3 * 2);
    }
}

//--------------------------------------------------------------------+
// Application Callback API Implementations
//--------------------------------------------------------------------+

// Invoked when audio class specific set request received for an EP
bool tud_audio_set_req_ep_cb(uint8_t rhport, tusb_control_request_t const* p_request, uint8_t* pBuff)
{
    (void)rhport;
    (void)pBuff;

    // We do not support any set range requests here, only current value requests
    TU_VERIFY(p_request->bRequest == AUDIO_CS_REQ_CUR);

    // Page 91 in UAC2 specification
    uint8_t channelNum = TU_U16_LOW(p_request->wValue);
    uint8_t ctrlSel = TU_U16_HIGH(p_request->wValue);
    uint8_t ep = TU_U16_LOW(p_request->wIndex);

    (void)channelNum;
    (void)ctrlSel;
    (void)ep;

    return false; // Yet not implemented
}

// Invoked when audio class specific set request received for an interface
bool tud_audio_set_req_itf_cb(uint8_t rhport, tusb_control_request_t const* p_request, uint8_t* pBuff)
{
    (void)rhport;
    (void)pBuff;

    // We do not support any set range requests here, only current value requests
    TU_VERIFY(p_request->bRequest == AUDIO_CS_REQ_CUR);

    // Page 91 in UAC2 specification
    uint8_t channelNum = TU_U16_LOW(p_request->wValue);
    uint8_t ctrlSel = TU_U16_HIGH(p_request->wValue);
    uint8_t itf = TU_U16_LOW(p_request->wIndex);

    (void)channelNum;
    (void)ctrlSel;
    (void)itf;

    return false; // Yet not implemented
}

// Invoked when audio class specific set request received for an entity
bool tud_audio_set_req_entity_cb(uint8_t rhport, tusb_control_request_t const* p_request, uint8_t* pBuff)
{
    (void)rhport;

    // Page 91 in UAC2 specification
    uint8_t channelNum = TU_U16_LOW(p_request->wValue);
    uint8_t ctrlSel = TU_U16_HIGH(p_request->wValue);
    uint8_t itf = TU_U16_LOW(p_request->wIndex);
    uint8_t entityID = TU_U16_HIGH(p_request->wIndex);

    (void)itf;

    // We do not support any set range requests here, only current value requests
    TU_VERIFY(p_request->bRequest == AUDIO_CS_REQ_CUR);

    // If request is for our feature unit
    if (entityID == 2) {
        switch (ctrlSel) {
        case AUDIO_FU_CTRL_MUTE:
            // Request uses format layout 1
            TU_VERIFY(p_request->wLength == sizeof(audio_control_cur_1_t));

            mute[channelNum] = ((audio_control_cur_1_t*)pBuff)->bCur;

            printf("Mute: %d of channel: %u\n", mute[channelNum], channelNum);
            return true;

        case AUDIO_FU_CTRL_VOLUME:
            // Request uses format layout 2
            TU_VERIFY(p_request->wLength == sizeof(audio_control_cur_2_t));

            volume[channelNum] = (uint16_t)((audio_control_cur_2_t*)pBuff)->bCur;
            printf("Volume: %d of channel: %u\n", volume[channelNum], channelNum);
            return true;

            // Unknown/Unsupported control
        default:
            TU_BREAKPOINT();
            return false;
        }
    }
    return false; // Yet not implemented
}

// Invoked when audio class specific get request received for an EP
bool tud_audio_get_req_ep_cb(uint8_t rhport, tusb_control_request_t const* p_request)
{
    (void)rhport;

    // Page 91 in UAC2 specification
    uint8_t channelNum = TU_U16_LOW(p_request->wValue);
    uint8_t ctrlSel = TU_U16_HIGH(p_request->wValue);
    uint8_t ep = TU_U16_LOW(p_request->wIndex);

    (void)channelNum;
    (void)ctrlSel;
    (void)ep;

    //	return tud_control_xfer(rhport, p_request, &tmp, 1);

    return false; // Yet not implemented
}

// Invoked when audio class specific get request received for an interface
bool tud_audio_get_req_itf_cb(uint8_t rhport, tusb_control_request_t const* p_request)
{
    (void)rhport;

    // Page 91 in UAC2 specification
    uint8_t channelNum = TU_U16_LOW(p_request->wValue);
    uint8_t ctrlSel = TU_U16_HIGH(p_request->wValue);
    uint8_t itf = TU_U16_LOW(p_request->wIndex);

    (void)channelNum;
    (void)ctrlSel;
    (void)itf;

    return false; // Yet not implemented
}

// Invoked when audio class specific get request received for an entity
bool tud_audio_get_req_entity_cb(uint8_t rhport, tusb_control_request_t const* p_request)
{
    (void)rhport;

    // Page 91 in UAC2 specification
    uint8_t channelNum = TU_U16_LOW(p_request->wValue);
    uint8_t ctrlSel = TU_U16_HIGH(p_request->wValue);
    // uint8_t itf = TU_U16_LOW(p_request->wIndex); 			// Since we have only one audio function implemented, we do not need the itf value
    uint8_t entityID = TU_U16_HIGH(p_request->wIndex);

    // Input terminal (Microphone input)
    if (entityID == 1) {
        switch (ctrlSel) {
        case AUDIO_TE_CTRL_CONNECTOR: {
            // The terminal connector control only has a get request with only the CUR attribute.
            audio_desc_channel_cluster_t ret;

            // Those are dummy values for now
            ret.bNrChannels = 1;
            ret.bmChannelConfig = (audio_channel_config_t)0;
            ret.iChannelNames = 0;

            printf("Get terminal connector\n");

            return tud_audio_buffer_and_schedule_control_xfer(rhport, p_request, (void*)&ret, sizeof(ret));
        } break;

            // Unknown/Unsupported control selector
        default:
            TU_BREAKPOINT();
            return false;
        }
    }

    // Feature unit
    if (entityID == 2) {
        switch (ctrlSel) {
        case AUDIO_FU_CTRL_MUTE:
            // Audio control mute cur parameter block consists of only one byte - we thus can send it right away
            // There does not exist a range parameter block for mute
            printf("Get Mute of channel: %u\n", channelNum);
            return tud_control_xfer(rhport, p_request, &mute[channelNum], 1);

        case AUDIO_FU_CTRL_VOLUME:
            switch (p_request->bRequest) {
            case AUDIO_CS_REQ_CUR:
                printf("Get Volume of channel: %u\n", channelNum);
                return tud_control_xfer(rhport, p_request, &volume[channelNum], sizeof(volume[channelNum]));

            case AUDIO_CS_REQ_RANGE:
                printf("Get Volume range of channel: %u\n", channelNum);

                // Copy values - only for testing - better is version below
                audio_control_range_2_n_t(1)
                    ret;

                ret.wNumSubRanges = 1;
                ret.subrange[0].bMin = 0;
                ret.subrange[0].bMax = 255;
                ret.subrange[0].bRes = 1; // 1 step increment in 0..255 range

                return tud_audio_buffer_and_schedule_control_xfer(rhport, p_request, (void*)&ret, sizeof(ret));

                // Unknown/Unsupported control
            default:
                TU_BREAKPOINT();
                return false;
            }
            break;

            // Unknown/Unsupported control
        default:
            TU_BREAKPOINT();
            return false;
        }
    }

    // Clock Source unit
    if (entityID == 4) {
        switch (ctrlSel) {
        case AUDIO_CS_CTRL_SAM_FREQ:
            // channelNum is always zero in this case
            switch (p_request->bRequest) {
            case AUDIO_CS_REQ_CUR:
                printf("Get Sample Freq.\n");
                // Buffered control transfer is needed for IN flow control to work
                return tud_audio_buffer_and_schedule_control_xfer(rhport, p_request, &sampFreq, sizeof(sampFreq));

            case AUDIO_CS_REQ_RANGE:
                printf("Get Sample Freq. range\n");
                return tud_control_xfer(rhport, p_request, &sampleFreqRng, sizeof(sampleFreqRng));

                // Unknown/Unsupported control
            default:
                TU_BREAKPOINT();
                return false;
            }
            break;

        case AUDIO_CS_CTRL_CLK_VALID:
            // Only cur attribute exists for this request
            printf("Get Sample Freq. valid\n");
            return tud_control_xfer(rhport, p_request, &clkValid, sizeof(clkValid));

        // Unknown/Unsupported control
        default:
            TU_BREAKPOINT();
            return false;
        }
    }

    printf("Unsupported entity: %d\n", entityID);
    return false; // Yet not implemented
}

bool tud_audio_tx_done_pre_load_cb(uint8_t rhport, uint8_t itf, uint8_t ep_in, uint8_t cur_alt_setting)
{
    (void)rhport;
    (void)itf;
    (void)ep_in;
    (void)cur_alt_setting;

    // In read world application data flow is driven by I2S clock,
    // both tud_audio_tx_done_pre_load_cb() & tud_audio_tx_done_post_load_cb() are hardly used.
    // For example in your I2S receive callback:
    // void I2S_Rx_Callback(int channel, const void* data, uint16_t samples)
    // {
    //    tud_audio_write_support_ff(channel, data, samples * N_BYTES_PER_SAMPLE * N_CHANNEL_PER_FIFO);
    // }

    return true;
}

bool tud_audio_tx_done_post_load_cb(uint8_t rhport, uint16_t n_bytes_copied, uint8_t itf, uint8_t ep_in, uint8_t cur_alt_setting)
{
    (void)rhport;
    (void)n_bytes_copied;
    (void)itf;
    (void)ep_in;
    (void)cur_alt_setting;

    return true;
}

bool tud_audio_set_itf_close_EP_cb(uint8_t rhport, tusb_control_request_t const* p_request)
{
    (void)rhport;
    (void)p_request;

    return true;
}

//--------------------------------------------------------------------+
// LED 任务：默认常亮；GP2 接地(低电平)时高频闪烁；断开后恢复常亮
//--------------------------------------------------------------------+
#define LED_BLINK_FAST_MS 50    // 高频闪烁的半周期(ms)，50ms ≈ 10Hz

void led_blinking_task(void)
{
    static uint32_t next_ms = 0;    // 下一次闪烁翻转的时刻
    static bool led_on = false;     // 当前 LED 亮灭
    static bool prev_ptt = false;   // 上一次的 GP2 状态

    uint8_t r = is_muted() ? 120 : 0; // 静音时偏红，否则纯蓝
    const uint8_t b = 140;

    bool ptt = !gpio_get(PIN_PTT);    // PTT 键接地 = 低电平 = 触发
    uint32_t now = board_millis();

    if (!ptt) {
        // 默认：常亮。仅在需要点亮时刷新一次，避免每圈都重刷
        if (!led_on || prev_ptt) {
            led_on = true;
            led_onboard_set(true);   // 板载 LED（GP25）常亮
            led_set_color(r, 0, b);  // WS2812（若板上有）
        }
        next_ms = now;      // 复位闪烁计时，便于下次触发立即开始
        prev_ptt = false;
        return;
    }

    // GP2 接地：高频闪烁
    if (!prev_ptt) {        // 刚进入闪烁：立即复位计时
        next_ms = now;
        prev_ptt = true;
    }
    if ((int32_t)(now - next_ms) < 0) {
        return;             // 未到下一个闪烁时刻
    }
    next_ms = now + LED_BLINK_FAST_MS;
    led_on = !led_on;
    led_onboard_set(led_on);                          // 板载 LED（GP25）闪烁
    led_set_color(led_on ? r : 0, 0, led_on ? b : 0); // WS2812（若板上有）
}

//--------------------------------------------------------------------+
// 自定义按键（去抖 + 组合键上报）已抽到独立模块：src/buttons.c / buttons.h
//   映射表 btn_configs[] 在 buttons.c，未来用户自由组合设定只需改那张表。
//--------------------------------------------------------------------+

// 报告发送完成回调（保留，避免未定义弱符号告警）
void tud_hid_report_sent_cb(uint8_t instance, uint8_t const* report, uint8_t len)
{
    (void)instance;
    (void)report;
    (void)len;
}