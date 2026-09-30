#ifndef LED_H
#define LED_H

#include <stdbool.h>
#include <stdint.h>

void led_init();
void led_set_color(uint8_t r, uint8_t g, uint8_t b);

// 板载 LED（标准 Pico = GP25 / PICO_DEFAULT_LED_PIN；Pico W = CYW43 的 WL_GPIO0）
// 参考官方示例 pico-examples/blink/blink.c
void led_onboard_init(void);
void led_onboard_set(bool on);

#endif
