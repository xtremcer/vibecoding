#include "buttons.h"
#include "config.h"          // cfg_get()：键位配置的唯一来源（A2b）
#include "cdc_cmd.h"         // cdc_cmd_send_line()：KEYMON 调试回声出口
#include "bsp/board_api.h"   // board_millis()
#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "tusb.h"
#include "class/hid/hid_device.h"
#include <stdio.h>
#include <string.h>

//--------------------------------------------------------------------+
// 槽位名（与 config.c 的 KEY_KEYS 对应），仅供 KEYMON 调试回声用
//--------------------------------------------------------------------+
static const char* KEY_NAMES[CFG_KEY_SLOTS] = {
    "ptt", "macro1", "macro2", "macro3", "macro4",
    "macro5", "macro6", "macro7", "macro8"
};

// KEYMON 调试回声开关（默认关；开启后每次按键边沿经 CDC 回 "KEY <槽位> DOWN/UP"）
static bool key_echo = false;

//--------------------------------------------------------------------+
// 按键完全由 CONFIG 的 keys[] 驱动（doc/12 §5.3）。
//   keys[0]=ptt，keys[1..8]=macro1..8，与 doc/12 §5.6 槽位一一对应。
//   每帧从 cfg_get() 实时读，所以 CONFIG SET 改完下一帧即生效，无需额外 reapply。
//--------------------------------------------------------------------+
#define BTN_DEBOUNCE_MS 20   // 真去抖：原始电平需稳定这么久才更新有效状态
#define BTN_COUNT       CFG_KEY_SLOTS

// 每个按键的运行时状态（数组长度 = 配置槽位数）
static bool     btn_raw[BTN_COUNT];            // 原始采样
static bool     btn_pressed[BTN_COUNT];        // 去抖后状态
static uint32_t btn_raw_change_ms[BTN_COUNT];  // 原始状态变化时刻
static bool     os_active[BTN_COUNT];          // ONESHOT：脉冲进行中
static uint32_t os_start_ms[BTN_COUNT];        // ONESHOT：脉冲开始时刻
static bool     os_prev[BTN_COUNT];            // ONESHOT：上次去抖状态
static uint8_t  cur_pin[BTN_COUNT];            // 当前已初始化的 pin（检测 pin 变更后重配）

static uint8_t last_modifier = 0;      // 上次上报的修饰键
static uint8_t last_keycode[6] = {0};  // 上次上报的键码

// cfg_mod_t 单枚举 → HID 修饰键位（仅左系：NONE/LGUI/LCTRL/LALT/LSHIFT）
static uint8_t mod_to_bit(uint8_t m)
{
    switch (m) {
        case CFG_MOD_LGUI:  return MOD_LGUI;
        case CFG_MOD_LCTRL: return MOD_LCTRL;
        case CFG_MOD_LALT:  return MOD_LALT;
        case CFG_MOD_LSHIFT:return MOD_LSHIFT;
        default:            return 0;   // CFG_MOD_NONE / 未知
    }
}

// 把某脚复位成安全输入（仍上拉，避免悬空），用于 pin 变更后释放旧脚
static void gpio_safe_input(uint8_t pin)
{
    gpio_set_dir(pin, GPIO_IN);
    gpio_pull_up(pin);
}

uint8_t buttons_ptt_pin(void)
{
    return cfg_get()->keys[0].pin;   // keys[0] = ptt
}

void buttons_set_echo(bool on)  { key_echo = on; }
bool buttons_get_echo(void)     { return key_echo; }

void buttons_init(void)
{
    for (int i = 0; i < BTN_COUNT; i++) {
        cur_pin[i] = cfg_get()->keys[i].pin;
        gpio_init(cur_pin[i]);
        gpio_set_dir(cur_pin[i], GPIO_IN);
        gpio_pull_up(cur_pin[i]);     // 内部上拉：按钮另一端接 GND，接地=低电平=按下
        btn_raw[i] = btn_pressed[i] = false;
        btn_raw_change_ms[i] = 0;
        os_active[i] = os_prev[i] = false;
        os_start_ms[i] = 0;
    }
}

// 若某槽位的 pin 被 CONFIG 改了，重新配置 GPIO：释放旧脚、初始化新脚、清空去抖状态。
static void buttons_sync_pins(void)
{
    for (int i = 0; i < BTN_COUNT; i++) {
        uint8_t p = cfg_get()->keys[i].pin;
        if (p == cur_pin[i]) continue;
        gpio_safe_input(cur_pin[i]);  // 旧脚交还安全输入
        cur_pin[i] = p;
        gpio_init(p);
        gpio_set_dir(p, GPIO_IN);
        gpio_pull_up(p);
        // 切换瞬间重置去抖/脉冲状态，避免误触发
        btn_raw[i] = btn_pressed[i] = false;
        btn_raw_change_ms[i] = 0;
        os_active[i] = os_prev[i] = false;
        os_start_ms[i] = 0;
    }
}

void buttons_task(void)
{
    uint32_t now = board_millis();
    buttons_sync_pins();

    // 1) 采样 + 真去抖（原始电平稳定 BTN_DEBOUNCE_MS 才更新有效状态）
    for (int i = 0; i < BTN_COUNT; i++) {
        const cfg_key_t* k = &cfg_get()->keys[i];
        if (!k->enabled) {            // 禁用的键：强制松开，不参与上报
            btn_pressed[i] = false;
            os_active[i] = false;
            continue;
        }
        bool raw = !gpio_get(cur_pin[i]);   // 接地=低电平=按下
        if (raw != btn_raw[i]) {
            btn_raw[i] = raw;
            btn_raw_change_ms[i] = now;
        } else if (btn_pressed[i] != raw &&
                   (now - btn_raw_change_ms[i]) >= BTN_DEBOUNCE_MS) {
            btn_pressed[i] = raw;
            // KEYMON 回声：仅在开启时，于物理去抖边沿经 CDC 回一行，
            // 让上位机在不接 HID 监听时也能确认「扫到哪个键、落到哪个槽、对应哪只脚」。
            if (key_echo) {
                char buf[32];
                const char* nm = KEY_NAMES[i];
                char up[12];
                int j = 0;
                for (; nm[j] && j < (int)sizeof(up) - 1; j++)
                    up[j] = (char)(nm[j] >= 'a' && nm[j] <= 'z' ? nm[j] - 32 : nm[j]);
                up[j] = 0;
                snprintf(buf, sizeof(buf), "KEY %s P%u %s",
                         up, (unsigned)cur_pin[i], raw ? "DOWN" : "UP");
                cdc_cmd_send_line(buf);
            }
        }
    }

    // 2) 汇总成一个 HID 报告：修饰键按位 OR，键码最多 6 个
    uint8_t modifier = 0;
    uint8_t keycode[6] = {0};
    int n = 0;
    for (int i = 0; i < BTN_COUNT; i++) {
        const cfg_key_t* k = &cfg_get()->keys[i];
        if (!k->enabled) continue;

        bool on;
        if (k->behavior == CFG_BEH_SINGLE) {
            // 一键脉冲：按下沿启动；到时(click_ms)或松开（先到者）即释放
            bool p = btn_pressed[i];
            if (!os_active[i]) {
                if (p && !os_prev[i]) {
                    os_active[i] = true;
                    os_start_ms[i] = now;
                }
            } else {
                if ((now - os_start_ms[i]) >= k->click_ms || !p)
                    os_active[i] = false;
            }
            os_prev[i] = p;
            on = os_active[i];
        } else {
            on = btn_pressed[i];       // CFG_BEH_NORMAL → 按住保持
        }
        if (!on) continue;

        modifier |= mod_to_bit(k->mod);
        if (k->key && n < 6)
            keycode[n++] = k->key;
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
