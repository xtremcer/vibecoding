#include "status_led.h"
#include "pico/stdlib.h"
#include "hardware/gpio.h"

// 引脚映射：busy=GP26(ADC0) / plan=GP27(ADC1) / idle=GP28(ADC2)
static const uint8_t status_led_pins[LED_COUNT] = { 26, 27, 28 };

// 点亮电平：1=高电平点亮（LED 阳极接 GPIO，阴极接 GND）；0=低电平点亮（阳极接 3V3）
#define STATUS_LED_ACTIVE_HIGH 1

void status_led_init(void)
{
    for (int i = 0; i < LED_COUNT; i++) {
        gpio_init(status_led_pins[i]);
        gpio_set_dir(status_led_pins[i], GPIO_OUT);
        gpio_put(status_led_pins[i], STATUS_LED_ACTIVE_HIGH ? 0 : 1); // 初始熄灭
    }
}

void status_led_set(status_led_t led, bool on)
{
    if (led >= LED_COUNT)
        return;
    gpio_put(status_led_pins[led], STATUS_LED_ACTIVE_HIGH ? (on ? 1 : 0) : (on ? 0 : 1));
}

void status_led_all(bool on)
{
    for (int i = 0; i < LED_COUNT; i++)
        status_led_set((status_led_t)i, on);
}
