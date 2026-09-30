#ifndef STATUS_LED_H
#define STATUS_LED_H

#include <stdbool.h>
#include <stdint.h>

//--------------------------------------------------------------------+
// 额外状态灯模块（3 个独立 GPIO LED，预留接口）
//   busy = GP26(ADC0) / plan = GP27(ADC1) / idle = GP28(ADC2)
//   普通 GPIO 输出直驱；点亮电平由 STATUS_LED_ACTIVE_HIGH 决定。
//--------------------------------------------------------------------+

typedef enum {
    LED_BUSY = 0,   // GP26
    LED_PLAN,       // GP27
    LED_IDLE,       // GP28
    LED_COUNT
} status_led_t;

void status_led_init(void);                  // 初始化 3 个引脚为输出，初始全灭
void status_led_set(status_led_t led, bool on);
void status_led_all(bool on);

#endif // STATUS_LED_H
