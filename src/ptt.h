#ifndef PTT_H
#define PTT_H

#include <stdbool.h>

// PTT（Push-To-Talk，一键通话）按键。
// 硬件：GP2 —— 按键一端接 GP2、另一端接 GND，使用芯片内部上拉。
//       未按下 = 高电平；按下 = 低电平（低电平有效）。
#define PTT_GPIO 2

// 初始化 GP2 为输入 + 上拉
void ptt_init(void);

// 返回 true 表示 PTT 当前被按住（已消抖）。
// 该函数需要在主循环里周期性调用（当前由 audio_task 每毫秒调用），消抖依赖调用频率。
bool ptt_is_pressed(void);

#endif
