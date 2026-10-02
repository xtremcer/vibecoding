#ifndef BUTTONS_H
#define BUTTONS_H

#include <stdbool.h>
#include <stdint.h>

//--------------------------------------------------------------------+
// 自定义按键模块（A2b：**完全由 CONFIG 的 keys[] 驱动**，不再有编译期映射表）
//   CONFIG SET 改了 keys 后，下一帧 buttons_task 自动生效：
//     - 修饰键 / 键码 / 行为 / click_ms 实时读配置；
//     - pin 变更由 buttons_sync_pins() 自动重配 GPIO（释放旧脚、初始化新脚、清空去抖）。
//   键位 / 修饰键 / 行为 / 引脚全部可经 CDC 下发（doc/12 §5.3）。
//
//   注意：不要用 I²S(GP6/7/8/9)、WS2812(GP16)、板载LED(GP25)、OLED(GP10/11)、蜂鸣器(GP14)
//        做按键脚，会被这些外设的初始化覆盖 / 互相干扰。
//--------------------------------------------------------------------+

// ---- HID 修饰键位（按位 OR 自由组合，与 HID 规范一致）----
#define MOD_LCTRL  0x01
#define MOD_LSHIFT 0x02
#define MOD_LALT   0x04
#define MOD_LGUI   0x08   // 左 Windows 键
#define MOD_RCTRL  0x10
#define MOD_RSHIFT 0x20
#define MOD_RALT   0x40
#define MOD_RGUI   0x80

// 初始化所有按键引脚（输入 + 内部上拉）
void buttons_init(void);

// 按键任务：去抖 + 触发状态机 + 汇总成一个 HID 键盘报告并上报（有变化才发）
void buttons_task(void);

// 返回当前 PTT 引脚（= keys[0].pin，可经 CONFIG 改）。
// 供 main.c 里「PTT→busy 灯」与「板载 LED 闪烁」的本地逻辑使用，使 PTT 脚也可配。
uint8_t buttons_ptt_pin(void);

// 调试回声开关（KEYMON ON/OFF）：开启后每次按键按下/松开经 CDC 回 "KEY <槽位> DOWN/UP"，
// 便于上位机在不接 HID 监听的情况下确认「物理按键被扫描到、对应哪个槽」。默认关。
void buttons_set_echo(bool on);
bool buttons_get_echo(void);

#endif // BUTTONS_H
