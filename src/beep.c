#include "beep.h"
#include "bsp/board_api.h"   // board_millis()
#include "pico/stdlib.h"
#include "hardware/gpio.h"

static bool     beeping = false;
static uint32_t beep_until_ms = 0;

void beep_init(void)
{
    gpio_init(BEEP_PIN);
    gpio_set_dir(BEEP_PIN, GPIO_OUT);
    gpio_put(BEEP_PIN, 0);   // 默认不响
}

void beep_on(void)
{
    beeping = false;
    gpio_put(BEEP_PIN, 1);
}

void beep_off(void)
{
    beeping = false;
    gpio_put(BEEP_PIN, 0);
}

void beep_ms(uint32_t ms)
{
    gpio_put(BEEP_PIN, 1);
    beep_until_ms = board_millis() + ms;
    beeping = true;
}

void beep_task(void)
{
    if (beeping && (int32_t)(board_millis() - beep_until_ms) >= 0) {
        gpio_put(BEEP_PIN, 0);
        beeping = false;
    }
}
