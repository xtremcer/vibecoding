#ifndef STATUS_LED_H
#define STATUS_LED_H

#include <stdbool.h>
#include <stdint.h>

//--------------------------------------------------------------------+
// 额外状态灯模块（3 个独立 GPIO LED）
//   busy = GP26(ADC0) / plan = GP27(ADC1) / idle = GP28(ADC2)
//   普通 GPIO 输出直驱；点亮电平由 STATUS_LED_ACTIVE_HIGH 决定。
//--------------------------------------------------------------------+

typedef enum {
    LED_BUSY = 0,   // GP26
    LED_PLAN,       // GP27
    LED_IDLE,       // GP28
    LED_COUNT
} status_led_t;

// 灯的三种工作模式（原 status_led_set 等价于 ON / OFF 两种静态模式）
typedef enum {
    LED_MODE_OFF = 0,
    LED_MODE_ON,
    LED_MODE_BLINK
} status_led_mode_t;

// ---- 原有 API（签名与语义不变）----
void status_led_init(void);                  // 初始化 3 个引脚为输出，初始全灭
void status_led_set(status_led_t led, bool on);
void status_led_all(bool on);

// ---- 新增 API（Phase 2 起，为上位机控制准备）----
void status_led_set_mode(status_led_t led, status_led_mode_t mode);
void status_led_blink(status_led_t led, uint16_t on_ms, uint16_t off_ms); // 进入闪烁模式
status_led_mode_t status_led_get_mode(status_led_t led);
bool status_led_is_lit(status_led_t led);    // 当前物理是否点亮（闪烁中也会随之变化）
void status_led_task(void);                  // 主循环调用：非阻塞推进闪烁相位

// 主机接管标志：置位后本地逻辑（如 PTT→busy 灯）不再改写 LED，避免和上位机抢
void status_led_host_control(bool on);
bool status_led_is_host_control(void);

#endif // STATUS_LED_H
