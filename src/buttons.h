#ifndef BUTTONS_H
#define BUTTONS_H

#include <stdbool.h>
#include <stdint.h>

//--------------------------------------------------------------------+
// 自定义按键模块
//   把「按键引脚 → HID 键（修饰键 + 键码）」的映射、去抖、触发行为封装在一起。
//   未来要让用户自由组合设定，只需修改 buttons.c 里的 btn_configs[] 表即可。
//--------------------------------------------------------------------+

// ---- HID 修饰键位（可按位 OR 自由组合）----
#define MOD_LCTRL  0x01
#define MOD_LSHIFT 0x02
#define MOD_LALT   0x04
#define MOD_LGUI   0x08   // 左 Windows 键
#define MOD_RCTRL  0x10
#define MOD_RSHIFT 0x20
#define MOD_RALT   0x40
#define MOD_RGUI   0x80

// ---- 触发行为 ----
typedef enum {
    BTN_HOLD    = 0,  // 按住保持：按下→发按下；松开→发释放（普通组合键 / PTT）
    BTN_ONESHOT = 1,  // 一键脉冲：按下→触发一次；到时(hold_ms)或松开→自动释放
} btn_behavior_t;

// ---- 单个按键配置（用户可自由组合 modifier + keycode）----
typedef struct {
    uint8_t  pin;       // GPIO 引脚（输入 + 内部上拉；低电平=按下）
    uint8_t  modifier;  // 修饰键位（MOD_xxx 的 OR 组合，0=无）
    uint8_t  keycode;   // HID 键码（0=仅修饰键）
    uint8_t  behavior;  // BTN_HOLD / BTN_ONESHOT
    uint16_t hold_ms;   // BTN_ONESHOT 的最长保持时间(ms)
} btn_config_t;

// PTT 键引脚（板载 LED 闪烁指示也用它）
#define PIN_PTT 2

// 用户按键表（定义在 buttons.c，按需修改/增删）
extern const btn_config_t btn_configs[];
extern const int          btn_config_count;

// 初始化所有按键引脚（输入 + 内部上拉）
void buttons_init(void);

// 按键任务：去抖 + 触发状态机 + 汇总成一个 HID 键盘报告并上报（有变化才发）
void buttons_task(void);

#endif // BUTTONS_H
