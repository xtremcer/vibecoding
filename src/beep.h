#ifndef BEEP_H
#define BEEP_H

#include <stdbool.h>
#include <stdint.h>

//--------------------------------------------------------------------+
// 蜂鸣器模块（预留接口）—— GP14 输出，需三极管/MOS 驱动
//   适合「有源蜂鸣器」（给电平就响）。若用无源蜂鸣器需 PWM 输出方波，可后续扩展。
//--------------------------------------------------------------------+

#define BEEP_PIN 14

void beep_init(void);          // 初始化 GP14 为输出，默认不响
void beep_on(void);            // 持续响
void beep_off(void);           // 停止
void beep_ms(uint32_t ms);     // 响 ms 毫秒后自动停（非阻塞，靠 beep_task 计时）
void beep_task(void);          // 在主循环中调用，处理 beep_ms 的计时

#endif // BEEP_H
