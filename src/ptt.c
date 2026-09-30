#include "ptt.h"

#include "hardware/gpio.h"

// 消抖：需要连续读到 PTT_DEBOUNCE_N 次相同电平，才认定状态发生改变。
// audio_task 大约每毫秒调用一次，因此 5 约为 5ms 消抖。
#define PTT_DEBOUNCE_N 5

static bool ptt_state = false;      // 消抖后的稳定状态（true = 按下）
static bool ptt_raw_prev = false;   // 上一次读到的原始电平状态
static uint8_t ptt_count = 0;       // 连续一致计数

void ptt_init(void)
{
    gpio_init(PTT_GPIO);
    gpio_set_dir(PTT_GPIO, GPIO_IN);
    gpio_pull_up(PTT_GPIO); // 上拉：未按下为高，按下接地为低

    ptt_state = false;
    ptt_count = 0;
    ptt_raw_prev = !gpio_get(PTT_GPIO); // 低电平 = 按下
}

bool ptt_is_pressed(void)
{
    bool raw = !gpio_get(PTT_GPIO); // 低电平有效

    if (raw != ptt_raw_prev) {
        // 电平变化，重新开始计数
        ptt_raw_prev = raw;
        ptt_count = 0;
    } else if (ptt_count < PTT_DEBOUNCE_N) {
        ptt_count++;
        if (ptt_count >= PTT_DEBOUNCE_N) {
            ptt_state = raw; // 稳定够久，更新状态
        }
    }

    return ptt_state;
}
