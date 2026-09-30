#include "led.h"
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
void hid_task(void);

// PTT / 自定义按键：全部内部上拉，接地(低电平)触发
#define GPIO_PTT 2   // PTT 键（同时用于板载 LED 闪烁指示）

// 自定义按键表：每项 = { 引脚, HID 修饰键位, HID 键码 }；低电平(接地)=按下。
//   modifier: 左Ctrl=0x01 左Shift=0x02 左Alt=0x04 左GUI(Win)=0x08（可 OR 多个）
//   keycode : Enter=0x28 Backspace=0x2A Esc=0x29 a=0x04 c=0x06 v=0x19 l=0x0F `=0x35 \=0x31
// 想改映射/加按键，改这张表即可（支持多键同时按下）。
typedef struct {
    uint8_t pin;
    uint8_t modifier;
    uint8_t keycode;
} hid_button_t;

static const hid_button_t hid_buttons[] = {
    {  2, 0x08, 0x35 }, // GP2  PTT：Win + `
    {  0, 0x00, 0x28 }, // GP0  Enter
    {  1, 0x00, 0x2A }, // GP1  Backspace
    {  3, 0x00, 0x29 }, // GP3  Esc
    {  4, 0x01, 0x04 }, // GP4  Ctrl + A
    {  5, 0x01, 0x06 }, // GP5  Ctrl + C
    { 20, 0x01, 0x19 }, // GP20 Ctrl + V（改到空闲脚，避开与 I²S dout_pin=6 的冲突）
    { 12, 0x01, 0x0F }, // GP12 Ctrl + L
    { 13, 0x08, 0x31 }, // GP13 Win + '\'（一键脉冲：按下触发一次，最多保持 1s 自动释放）
};
#define HID_BUTTON_COUNT ((int)(sizeof(hid_buttons) / sizeof(hid_buttons[0])))

// GP13(Win+\) 一键脉冲：按下触发一次，最多保持 PULSE_HOLD_MS 后自动释放；
// 脉冲期间忽略再次按下（阻塞防抖）；长按/短按效果一致。
#define GPIO_ONESHOT 13
#define PULSE_HOLD_MS 1000

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

    // 初始化所有按键引脚：输入 + 内部上拉（外部按钮另一端接 GND，接地=低电平=按下）
    for (int i = 0; i < HID_BUTTON_COUNT; i++) {
        gpio_init(hid_buttons[i].pin);
        gpio_set_dir(hid_buttons[i].pin, GPIO_IN);
        gpio_pull_up(hid_buttons[i].pin);
    }

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
        hid_task();
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

    bool ptt = !gpio_get(GPIO_PTT);   // GP2 接地 = 低电平 = 触发
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
// HID 按键任务：把各按键脚的按下状态汇总成一个键盘报告发送（低电平=按下，内部上拉）。
// 支持多键同时按下。采用"真去抖"：原始电平需稳定 HID_DEBOUNCE_MS 才改变有效状态；
// 避免长按时触点抖动被误判为"松开"——尤其 Win 组合键被误松开会弹出 Windows 开始菜单。
//--------------------------------------------------------------------+
#define HID_DEBOUNCE_MS 20                            // 去抖时间(ms)：原始电平需稳定这么久才生效

static bool btn_raw[HID_BUTTON_COUNT];                // 原始采样状态
static bool btn_pressed[HID_BUTTON_COUNT];            // 去抖后的有效状态
static uint32_t btn_raw_change_ms[HID_BUTTON_COUNT];  // 原始状态上次变化时刻
static uint8_t last_modifier = 0;                     // 上次上报的修饰键
static uint8_t last_keycode[6] = {0};                 // 上次上报的键码

// GP13(Win+\) 一键脉冲状态
static bool oneshot_active = false;                   // 脉冲进行中（按住阶段）
static uint32_t oneshot_start_ms = 0;                 // 脉冲开始时刻
static bool oneshot_prev = false;                     // 上次去抖状态（用于检测按下沿）

void hid_task(void)
{
    uint32_t now = board_millis();

    // 1) 采样 + 真去抖：原始电平稳定 HID_DEBOUNCE_MS 后才更新有效状态
    for (int i = 0; i < HID_BUTTON_COUNT; i++) {
        bool raw = !gpio_get(hid_buttons[i].pin); // 接地=低电平=按下
        if (raw != btn_raw[i]) {
            btn_raw[i] = raw;
            btn_raw_change_ms[i] = now;           // 记录原始状态变化时刻
        } else if (btn_pressed[i] != raw && (now - btn_raw_change_ms[i]) >= HID_DEBOUNCE_MS) {
            btn_pressed[i] = raw;                 // 稳定足够久 → 生效
        }
    }

    // 2) GP13(Win+\) 一键脉冲：按下沿启动（发一次 Win+\ 按下）；到 PULSE_HOLD_MS 或
    //    按键松开（以先到者为准）即自动释放；脉冲期间忽略再次按下（阻塞防抖）。
    bool os_pressed = false;
    for (int i = 0; i < HID_BUTTON_COUNT; i++) {
        if (hid_buttons[i].pin == GPIO_ONESHOT) {
            os_pressed = btn_pressed[i];
            break;
        }
    }

    if (!oneshot_active) {
        if (os_pressed && !oneshot_prev) {        // 新的按下沿 → 启动脉冲
            oneshot_active = true;
            oneshot_start_ms = now;
        }
    } else {
        // 到时 或 按键已松开 → 结束脉冲（释放 Win+\），保证不会一直按住
        if ((now - oneshot_start_ms) >= PULSE_HOLD_MS || !os_pressed)
            oneshot_active = false;
    }
    oneshot_prev = os_pressed;
    bool oneshot_held = oneshot_active;

    // 3) 汇总成一个 HID 报告：修饰键 OR，键码最多 6 个（GP13 仅在其脉冲按住阶段计入）
    uint8_t modifier = 0;
    uint8_t keycode[6] = {0};
    int n = 0;
    for (int i = 0; i < HID_BUTTON_COUNT; i++) {
        bool on = (hid_buttons[i].pin == GPIO_ONESHOT) ? oneshot_held : btn_pressed[i];
        if (!on)
            continue;
        modifier |= hid_buttons[i].modifier;
        if (hid_buttons[i].keycode && n < 6)
            keycode[n++] = hid_buttons[i].keycode;
    }

    // 4) 与上次上报比较，有变化才发送（HID 忙时下个循环重试）
    if (modifier == last_modifier && memcmp(keycode, last_keycode, sizeof(keycode)) == 0)
        return;
    if (!tud_hid_ready())
        return;

    if (tud_hid_keyboard_report(0, modifier, keycode)) {
        last_modifier = modifier;
        memcpy(last_keycode, keycode, sizeof(last_keycode));
    }
}

// 报告发送完成回调（保留，避免未定义弱符号告警）
void tud_hid_report_sent_cb(uint8_t instance, uint8_t const* report, uint8_t len)
{
    (void)instance;
    (void)report;
    (void)len;
}