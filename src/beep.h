#ifndef BEEP_H
#define BEEP_H

#include <stdbool.h>
#include <stdint.h>

//--------------------------------------------------------------------+
// 蜂鸣器模块（无源蜂鸣器，PWM 方波驱动）—— GP14 → PWM slice 7 / channel A
//   频率 = 音调；占空比固定 50%（对称方波，无源蜂鸣器最干净）。
//   所有"发声"都是非阻塞的：旋律/单音靠 beep_task() 在主循环里推进计时。
//--------------------------------------------------------------------+

#define BEEP_PIN 14
#define BEEP_DEFAULT_HZ 2000   // 默认音（2kHz，常见压电蜂鸣器谐振区）

typedef enum {
    BEEP_OFF = 0,    // 静音
    BEEP_ON,         // 持续响（无自动停，需 BEEP OFF 收尾）
    BEEP_PLAYING,    // 单音或旋律播放中（按计时推进，结束自停）
} beep_state_t;

void beep_init(void);          // 初始化 PWM slice，上电静音
void beep_on(void);            // 默认音持续响
void beep_off(void);           // 彻底静音（无直流偏置、无微噪声）
void beep_ms(uint32_t ms);     // 默认音单音 ms 后自停（非阻塞）
void beep_note(uint16_t hz, uint32_t ms); // 指定音高单音 ms（非阻塞）
void beep_play(uint8_t n);     // 播放预设旋律 1~3
void beep_play_score(const char* score); // 播放乐谱 "频率,时长ms;..."（如 "523,200;659,200"），播完自停
void beep_stop(void);          // 立即停止
beep_state_t beep_get_state(void);
uint16_t beep_get_hz(void);    // 当前发声频率（静音时返回 0）
void beep_task(void);          // 主循环调用：推进旋律/单音计时

#endif // BEEP_H
