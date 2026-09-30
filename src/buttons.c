#include "buttons.h"
#include "bsp/board_api.h"   // board_millis()
#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "tusb.h"
#include "class/hid/hid_device.h"
#include <string.h>

//--------------------------------------------------------------------+
// 用户按键映射表 —— 想改键位/组合/行为，只改这张表即可。
//   每行 = { 引脚, 修饰键, 键码, 行为, 保持时长(ms) }
//   修饰键：MOD_LCTRL / MOD_LSHIFT / MOD_LALT / MOD_LGUI(Win) … 可按位 OR 组合
//   键码  ：Enter=0x28 Backspace=0x2A Esc=0x29 a=0x04 c=0x06 v=0x19 l=0x0F `=0x35 \=0x31
//   行为  ：BTN_HOLD（按住保持）/ BTN_ONESHOT（一键脉冲，到时或松开释放）
//   注意  ：不要用 I²S(GP6/7/8/9)、WS2812(GP16)、板载LED(GP25)、OLED(GP10/11)、蜂鸣器(GP14)
//--------------------------------------------------------------------+
const btn_config_t btn_configs[] = {
    // pin,      modifier,   keycode, behavior,     hold_ms
    {  PIN_PTT,  MOD_LGUI,   0x35,    BTN_HOLD,     0    }, // GP2  PTT：Win + `
    {  0,        0,          0x28,    BTN_HOLD,     0    }, // GP0  Enter
    {  1,        0,          0x2A,    BTN_HOLD,     0    }, // GP1  Backspace
    {  3,        0,          0x29,    BTN_HOLD,     0    }, // GP3  Esc
    {  4,        MOD_LCTRL,  0x04,    BTN_HOLD,     0    }, // GP4  Ctrl + A
    {  5,        MOD_LCTRL,  0x06,    BTN_HOLD,     0    }, // GP5  Ctrl + C
    { 20,        MOD_LCTRL,  0x19,    BTN_HOLD,     0    }, // GP20 Ctrl + V
    { 12,        MOD_LCTRL,  0x0F,    BTN_HOLD,     0    }, // GP12 Ctrl + L
    { 13,        MOD_LGUI,   0x31,    BTN_ONESHOT,  1000 }, // GP13 Win + '\'（一键脉冲，最多保持 1s）
};
const int btn_config_count = (int)(sizeof(btn_configs) / sizeof(btn_configs[0]));

#define BTN_DEBOUNCE_MS 20   // 真去抖：原始电平需稳定这么久才更新有效状态

// 每个按键的运行时状态（数组长度 = 配置表条目数）
static bool     btn_raw[sizeof(btn_configs) / sizeof(btn_configs[0])];            // 原始采样
static bool     btn_pressed[sizeof(btn_configs) / sizeof(btn_configs[0])];        // 去抖后状态
static uint32_t btn_raw_change_ms[sizeof(btn_configs) / sizeof(btn_configs[0])];  // 原始状态变化时刻
static bool     os_active[sizeof(btn_configs) / sizeof(btn_configs[0])];          // ONESHOT：脉冲进行中
static uint32_t os_start_ms[sizeof(btn_configs) / sizeof(btn_configs[0])];        // ONESHOT：脉冲开始时刻
static bool     os_prev[sizeof(btn_configs) / sizeof(btn_configs[0])];            // ONESHOT：上次去抖状态

static uint8_t last_modifier = 0;      // 上次上报的修饰键
static uint8_t last_keycode[6] = {0};  // 上次上报的键码

void buttons_init(void)
{
    for (int i = 0; i < btn_config_count; i++) {
        gpio_init(btn_configs[i].pin);
        gpio_set_dir(btn_configs[i].pin, GPIO_IN);
        gpio_pull_up(btn_configs[i].pin);   // 内部上拉：按钮另一端接 GND，接地=低电平=按下
    }
}

void buttons_task(void)
{
    uint32_t now = board_millis();

    // 1) 采样 + 真去抖（原始电平稳定 BTN_DEBOUNCE_MS 才更新有效状态）
    for (int i = 0; i < btn_config_count; i++) {
        bool raw = !gpio_get(btn_configs[i].pin);   // 接地=低电平=按下
        if (raw != btn_raw[i]) {
            btn_raw[i] = raw;
            btn_raw_change_ms[i] = now;
        } else if (btn_pressed[i] != raw && (now - btn_raw_change_ms[i]) >= BTN_DEBOUNCE_MS) {
            btn_pressed[i] = raw;
        }
    }

    // 2) 汇总成一个 HID 报告：修饰键按位 OR，键码最多 6 个
    uint8_t modifier = 0;
    uint8_t keycode[6] = {0};
    int n = 0;
    for (int i = 0; i < btn_config_count; i++) {
        bool on;
        if (btn_configs[i].behavior == BTN_ONESHOT) {
            // 一键脉冲：按下沿启动；到时(hold_ms)或松开（先到者）即释放
            bool p = btn_pressed[i];
            if (!os_active[i]) {
                if (p && !os_prev[i]) {
                    os_active[i] = true;
                    os_start_ms[i] = now;
                }
            } else {
                if ((now - os_start_ms[i]) >= btn_configs[i].hold_ms || !p)
                    os_active[i] = false;
            }
            os_prev[i] = p;
            on = os_active[i];
        } else {
            on = btn_pressed[i];
        }
        if (!on)
            continue;
        modifier |= btn_configs[i].modifier;
        if (btn_configs[i].keycode && n < 6)
            keycode[n++] = btn_configs[i].keycode;
    }

    // 3) 与上次上报比较，有变化才发送（HID 忙时下个循环重试）
    if (modifier == last_modifier && memcmp(keycode, last_keycode, sizeof(keycode)) == 0)
        return;
    if (!tud_hid_ready())
        return;
    if (tud_hid_keyboard_report(0, modifier, keycode)) {
        last_modifier = modifier;
        memcpy(last_keycode, keycode, sizeof(keycode));
    }
}
