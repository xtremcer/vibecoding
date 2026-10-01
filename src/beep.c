#include "beep.h"

#include "bsp/board_api.h"   // board_millis()
#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include "hardware/pwm.h"
#include "pico/stdlib.h"

//--------------------------------------------------------------------+
// 无源蜂鸣器 PWM 驱动
//   GP14 → PWM slice 7 / channel A（SDK 映射规则：slice=(gpio>>1)&7, chan=gpio&1）
//   clk_sys = 132 MHz。为让 wrap 不溢出 65535，分频取 ceil(132e6 / (65535 * hz))。
//--------------------------------------------------------------------+

static uint8_t     beep_slice = 0;
static beep_state_t state      = BEEP_OFF;
static uint16_t    cur_hz     = 0;

// ---- 旋律表（每项 {频率Hz, 时长ms}）；BEEP PLAY 1~3 分别对应 ----
typedef struct { uint16_t hz; uint32_t ms; } note_t;

static const note_t MEL1[] = { {262, 200}, {330, 200}, {392, 200}, {523, 350} }; // C 大调上行 C-E-G-C5
static const note_t MEL2[] = { {880, 250}, {660, 450} };                        // 叮——咚
static const note_t MEL3[] = { {523, 150}, {523, 150}, {659, 150}, {784, 350} }; // 开机小号
static const note_t* const mel_tbl[3] = { MEL1, MEL2, MEL3 };
static const uint8_t      mel_len[3]  = { 4, 2, 4 };

// 当前播放序列（指向旋律表或下面两个单音缓冲之一）
static const note_t* play_notes = NULL;
static uint8_t       play_len   = 0;
static uint8_t       play_idx   = 0;
static uint32_t      note_start_ms = 0;

// 单音缓冲（beep_ms / beep_note 复用，避免动态分配）
static note_t single_note;

//--------------------------------------------------------------------+
// 内部：设置 PWM 输出某频率（50% 占空）。hz==0 视为静音。
//--------------------------------------------------------------------+
static void beep_set_freq(uint16_t hz)
{
    if (hz == 0) {
        beep_off();
        return;
    }

    uint32_t sys = clock_get_hz(clk_sys);          // 132000000
    // div = ceil(sys / (65535 * hz))，保证 counter/hz <= 65535，wrap 不溢出
    uint32_t div = (sys + (65535UL * hz) - 1) / (65535UL * hz);
    if (div < 1)   div = 1;
    if (div > 255) div = 255;                      // 8.4 定点整数分频上限

    uint32_t counter = sys / div;
    uint32_t wrap    = (counter + hz / 2) / hz;      // round(counter / hz)
    if (wrap < 2)      wrap = 2;
    if (wrap > 65535)  wrap = 65535;
    uint16_t level = (uint16_t)(wrap / 2);          // 50% 占空

    pwm_config cfg = pwm_get_default_config();
    pwm_config_set_clkdiv(&cfg, (float)div);         // 8.4 定点分频
    pwm_config_set_wrap(&cfg, (uint16_t)(wrap - 1));
    pwm_init(beep_slice, &cfg, false);               // 配置但不立即启动
    gpio_set_function(BEEP_PIN, GPIO_FUNC_PWM);
    pwm_set_gpio_level(BEEP_PIN, level);             // 先设电平再使能，避免毛刺
    pwm_set_enabled(beep_slice, true);
    cur_hz = hz;
}

//--------------------------------------------------------------------+
// 彻底静音（最关键）：关电平 → 关 slice → 把 GPIO 切回 SIO 并强制低，
// 杜绝 PWM 停在 HIGH 带来的直流偏置与微噪声。
//--------------------------------------------------------------------+
void beep_off(void)
{
    pwm_set_gpio_level(BEEP_PIN, 0);
    pwm_set_enabled(beep_slice, false);
    gpio_set_function(BEEP_PIN, GPIO_FUNC_SIO);
    gpio_put(BEEP_PIN, 0);
    state       = BEEP_OFF;
    cur_hz      = 0;
    play_notes  = NULL;
    play_len    = 0;
    play_idx    = 0;
}

void beep_init(void)
{
    beep_slice = pwm_gpio_to_slice_num(BEEP_PIN);

    // 上电默认静音：GPIO 当普通输出拉低，绝不自鸣
    gpio_init(BEEP_PIN);
    gpio_set_dir(BEEP_PIN, GPIO_OUT);
    gpio_put(BEEP_PIN, 0);

    state      = BEEP_OFF;
    cur_hz     = 0;
    play_notes = NULL;
    play_len   = 0;
    play_idx   = 0;
}

void beep_on(void)
{
    state      = BEEP_ON;
    play_notes = NULL;          // 持续响模式不依赖序列
    beep_set_freq(BEEP_DEFAULT_HZ);
}

void beep_ms(uint32_t ms)
{
    if (ms == 0) { beep_off(); return; }
    single_note.hz = BEEP_DEFAULT_HZ;
    single_note.ms = ms;
    play_notes     = &single_note;
    play_len       = 1;
    play_idx       = 0;
    note_start_ms  = board_millis();
    state          = BEEP_PLAYING;
    beep_set_freq(single_note.hz);
}

void beep_note(uint16_t hz, uint32_t ms)
{
    if (hz == 0 || ms == 0) { beep_off(); return; }
    single_note.hz = hz;
    single_note.ms = ms;
    play_notes     = &single_note;
    play_len       = 1;
    play_idx       = 0;
    note_start_ms  = board_millis();
    state          = BEEP_PLAYING;
    beep_set_freq(hz);
}

void beep_play(uint8_t n)
{
    if (n < 1 || n > 3) return;     // 调用方负责范围校验与报错
    n -= 1;
    play_notes    = mel_tbl[n];
    play_len      = mel_len[n];
    play_idx      = 0;
    note_start_ms = board_millis();
    state         = BEEP_PLAYING;
    beep_set_freq(play_notes[0].hz);
}

void beep_stop(void)
{
    beep_off();
}

beep_state_t beep_get_state(void)
{
    return state;
}

uint16_t beep_get_hz(void)
{
    return (state == BEEP_OFF) ? 0 : cur_hz;
}

//--------------------------------------------------------------------+
// 非阻塞推进：PLAYING 时按每音时长切换/结束；ON 不自动停。
//--------------------------------------------------------------------+
void beep_task(void)
{
    if (state != BEEP_PLAYING || !play_notes) return;

    if ((int32_t)(board_millis() - note_start_ms) >= (int32_t)play_notes[play_idx].ms) {
        play_idx++;
        if (play_idx >= play_len) {
            beep_off();             // 序列结束，彻底静音
        } else {
            note_start_ms = board_millis();
            beep_set_freq(play_notes[play_idx].hz);
        }
    }
}
