#include "led.h"
#include "pico/stdlib.h"

#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/pio.h"

#include "ws2812.pio.h"

#ifdef CYW43_WL_GPIO_LED_PIN
#include "pico/cyw43_arch.h"
#endif

#define WS2812_PIN 16

PIO pixelPio;
uint pixelSm;
uint8_t pixelBuffer[3];

uint32_t urgb_u32(uint8_t r, uint8_t g, uint8_t b)
{
    return ((uint32_t)(r) << 8) | ((uint32_t)(g) << 16) | (uint32_t)b;
}

void led_put_pixel(uint32_t pixel_grb)
{
    pio_sm_put_blocking(pixelPio, pixelSm, pixel_grb << 8u);
}

void led_init()
{
    pixelSm = 0;
    pixelPio = pio0;

    uint offset = pio_add_program(pixelPio, &ws2812_program);
    ws2812_program_init(pixelPio, pixelSm, offset, WS2812_PIN, 800000, false);

    led_put_pixel(0);
    sleep_ms(1);
}

void led_set_color(uint8_t r, uint8_t g, uint8_t b)
{
    led_put_pixel(urgb_u32(r, g, b));
}

//--------------------------------------------------------------------+
// 板载 LED：标准 Pico 为 GP25(PICO_DEFAULT_LED_PIN)，普通 GPIO 直接驱动；
// Pico W 为 CYW43 无线芯片上的 WL_GPIO0。参考官方示例 pico-examples/blink/blink.c。
//--------------------------------------------------------------------+
void led_onboard_init(void)
{
#if defined(PICO_DEFAULT_LED_PIN)
    // 标准 Pico：GPIO 直接驱动板载 LED
    gpio_init(PICO_DEFAULT_LED_PIN);
    gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);
    gpio_put(PICO_DEFAULT_LED_PIN, 1); // 上电默认常亮
#elif defined(CYW43_WL_GPIO_LED_PIN)
    // Pico W：通过无线驱动设置 LED
    cyw43_arch_init();
    cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 1); // 上电默认常亮
#endif
}

void led_onboard_set(bool on)
{
#if defined(PICO_DEFAULT_LED_PIN)
    gpio_put(PICO_DEFAULT_LED_PIN, on ? 1 : 0);
#elif defined(CYW43_WL_GPIO_LED_PIN)
    cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, on ? 1 : 0);
#else
    (void)on;
#endif
}
