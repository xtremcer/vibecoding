#include "status_led.h"

#include "bsp/board_api.h"   // board_millis()
#include "hardware/gpio.h"
#include "pico/stdlib.h"

// 引脚映射：busy=GP26(ADC0) / plan=GP27(ADC1) / idle=GP28(ADC2)
static const uint8_t status_led_pins[LED_COUNT] = { 26, 27, 28 };

// 点亮电平：1=高电平点亮（LED 阳极接 GPIO，阴极接 GND）；0=低电平点亮（阳极接 3V3）
#define STATUS_LED_ACTIVE_HIGH 1

// 闪烁周期边界，防止上位机下发 0 或超大值把相位卡死
#define LED_BLINK_MIN_MS 10
#define LED_BLINK_MAX_MS 60000

typedef struct {
    status_led_mode_t mode;
    uint16_t          on_ms;
    uint16_t          off_ms;
    uint32_t          next_ms;   // 下一次翻转的时刻
    bool              lit;       // 当前物理状态
} led_state_t;

static led_state_t leds[LED_COUNT];
static bool        host_control = false;   // true = 上位机接管，本地逻辑让位

static inline void led_write_hw(int i, bool on)
{
    gpio_put(status_led_pins[i], STATUS_LED_ACTIVE_HIGH ? (on ? 1 : 0) : (on ? 0 : 1));
}

void status_led_init(void)
{
    for (int i = 0; i < LED_COUNT; i++) {
        gpio_init(status_led_pins[i]);
        gpio_set_dir(status_led_pins[i], GPIO_OUT);
        leds[i].mode    = LED_MODE_OFF;
        leds[i].on_ms   = 500;
        leds[i].off_ms  = 500;
        leds[i].next_ms = 0;
        leds[i].lit     = false;
        led_write_hw(i, false);      // 初始熄灭
    }
    host_control = false;
}

void status_led_set(status_led_t led, bool on)
{
    if (led >= LED_COUNT)
        return;
    int i = (int)led;
    leds[i].mode = on ? LED_MODE_ON : LED_MODE_OFF;
    leds[i].lit  = on;
    led_write_hw(i, on);
}

void status_led_all(bool on)
{
    for (int i = 0; i < LED_COUNT; i++)
        status_led_set((status_led_t)i, on);
}

void status_led_set_mode(status_led_t led, status_led_mode_t mode)
{
    if (led >= LED_COUNT)
        return;
    int i = (int)led;

    leds[i].mode = mode;
    if (mode == LED_MODE_ON) {
        leds[i].lit = true;
        led_write_hw(i, true);
    } else if (mode == LED_MODE_OFF) {
        leds[i].lit = false;
        led_write_hw(i, false);
    } else {
        // 进入闪烁：从"亮"开始，立即起拍
        leds[i].lit     = true;
        leds[i].next_ms = board_millis() + leds[i].on_ms;
        led_write_hw(i, true);
    }
}

void status_led_blink(status_led_t led, uint16_t on_ms, uint16_t off_ms)
{
    if (led >= LED_COUNT)
        return;

    if (on_ms < LED_BLINK_MIN_MS)  on_ms  = LED_BLINK_MIN_MS;
    if (off_ms < LED_BLINK_MIN_MS) off_ms = LED_BLINK_MIN_MS;
    if (on_ms > LED_BLINK_MAX_MS)  on_ms  = LED_BLINK_MAX_MS;
    if (off_ms > LED_BLINK_MAX_MS) off_ms = LED_BLINK_MAX_MS;

    leds[(int)led].on_ms  = on_ms;
    leds[(int)led].off_ms = off_ms;
    status_led_set_mode(led, LED_MODE_BLINK);
}

status_led_mode_t status_led_get_mode(status_led_t led)
{
    return (led < LED_COUNT) ? leds[(int)led].mode : LED_MODE_OFF;
}

bool status_led_is_lit(status_led_t led)
{
    return (led < LED_COUNT) ? leds[(int)led].lit : false;
}

// 非阻塞：每圈只做几次比较，绝不延时
void status_led_task(void)
{
    uint32_t now = board_millis();
    for (int i = 0; i < LED_COUNT; i++) {
        if (leds[i].mode != LED_MODE_BLINK)
            continue;
        if ((int32_t)(now - leds[i].next_ms) < 0)
            continue;
        leds[i].lit = !leds[i].lit;
        led_write_hw(i, leds[i].lit);
        leds[i].next_ms = now + (leds[i].lit ? leds[i].on_ms : leds[i].off_ms);
    }
}

void status_led_host_control(bool on)
{
    host_control = on;
}

bool status_led_is_host_control(void)
{
    return host_control;
}
