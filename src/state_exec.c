#include "state_exec.h"

#include <string.h>

#include "bsp/board_api.h"
#include "beep.h"
#include "status_led.h"

//--------------------------------------------------------------------+
// 编译期内置默认配置（与 doc/12 §5.3 示例逐项一致）
//   A2 会被 JSON 配置覆盖；A3 起持久化到 LittleFS；缺失/损坏就回退到这里。
//--------------------------------------------------------------------+
static const state_cfg_t CFG[ST_COUNT] = {
    // BUSY：执行中。待机灭、不闪；提示音在「结束时」播 3 次（= 任务完成提示，执行中绝不出声）
    [ST_BUSY] = {
        .led   = { .standby_on = false, .blink_on_ms = 0,   .blink_off_ms = 0   },
        .sound = { .enabled = true, .at_start = false, .count = 3, .loop_interval_ms = 1000,
                   .score = "523,200;659,200;784,350" },
    },
    // IDLE：闲置。待机亮、不闪；提示音在「结束时」播 1 次
    [ST_IDLE] = {
        .led   = { .standby_on = true, .blink_on_ms = 0,   .blink_off_ms = 0   },
        .sound = { .enabled = true, .at_start = false, .count = 1, .loop_interval_ms = 1000,
                   .score = "659,300;784,300" },
    },
    // AUTH：需授权。待机亮 + 300/300 闪；提示音在「开始时」播 3 次（持续轻催直到解决/超时）
    [ST_AUTH] = {
        .led   = { .standby_on = true, .blink_on_ms = 300, .blink_off_ms = 300 },
        .sound = { .enabled = true, .at_start = true,  .count = 3, .loop_interval_ms = 1000,
                   .score = "880,250;660,450" },
    },
};

// ---- device 段（编译期默认）----
#define BOOT_LED_ALL_ON   true   // true = ALL_ON 三灯全亮；false = ALL_OFF 三灯全灭
#define BOOT_TIMEOUT_MS   0      // 0 = 收到首个 SET STATE 前一直保持初始化展示

static const char* STATE_NAMES[ST_COUNT] = { "BUSY", "IDLE", "AUTH" };

//--------------------------------------------------------------------+
// 内部状态
//--------------------------------------------------------------------+
static app_state_t cur        = ST_COUNT;   // ST_COUNT = 还没收到过任何状态
static uint32_t    boot_at_ms = 0;

// 提示音重复调度（第 1 次立即播，剩下 count-1 次按间隔排队）
static uint8_t       rep_left     = 0;
static uint32_t      rep_next_ms  = 0;
static const char*   rep_score    = NULL;
static uint16_t      rep_interval = 0;

//--------------------------------------------------------------------+
// 查询接口
//--------------------------------------------------------------------+
const char* state_exec_name(app_state_t s)
{
    return (s < ST_COUNT) ? STATE_NAMES[s] : "NONE";
}

const state_cfg_t* state_exec_cfg(app_state_t s)
{
    return (s < ST_COUNT) ? &CFG[s] : NULL;
}

app_state_t state_exec_get(void)
{
    return cur;
}

//--------------------------------------------------------------------+
// 内部：应用态 → 物理灯
//--------------------------------------------------------------------+
static status_led_t phys_of(app_state_t s)
{
    switch (s) {
        case ST_BUSY: return LED_BUSY;   // GP26
        case ST_AUTH: return LED_PLAN;   // GP27（复用 PLAN 灯作 AUTH）
        case ST_IDLE:
        default:      return LED_IDLE;   // GP28
    }
}

// 只应用 LED（不碰蜂鸣器）
static void led_apply(app_state_t s)
{
    const state_cfg_t* c = &CFG[s];
    status_led_t       mine = phys_of(s);

    // 三态各占一颗、互斥：先全灭再点亮当前态那一颗
    for (int i = 0; i < LED_COUNT; i++)
        status_led_set_mode((status_led_t)i, LED_MODE_OFF);

    if (c->led.blink_on_ms || c->led.blink_off_ms) {
        status_led_blink(mine, c->led.blink_on_ms, c->led.blink_off_ms);
    } else {
        status_led_set_mode(mine, c->led.standby_on ? LED_MODE_ON : LED_MODE_OFF);
    }
}

// 播一段提示音（含重复次数调度）
static void cue_start(const snd_cfg_t* snd)
{
    if (!snd || !snd->enabled || !snd->score || !*snd->score) return;

    uint8_t n = snd->count ? snd->count : 1;
    rep_score    = snd->score;
    rep_interval = snd->loop_interval_ms;
    rep_left     = n - 1;      // 第 1 次立刻播
    rep_next_ms  = 0;
    beep_play_score(snd->score);
}

//--------------------------------------------------------------------+
// 对外接口
//--------------------------------------------------------------------+
void state_exec_init(void)
{
    cur        = ST_COUNT;
    boot_at_ms = board_millis();
    rep_left   = 0;

    // 上电初始化展示：三灯全亮（ALL_ON）或全灭（ALL_OFF），
    // 一直保持到首个 SET STATE 到达（或 BOOT_TIMEOUT_MS 超时转 idle）
    for (int i = 0; i < LED_COUNT; i++)
        status_led_set_mode((status_led_t)i, BOOT_LED_ALL_ON ? LED_MODE_ON : LED_MODE_OFF);
}

bool state_exec_set(app_state_t s)
{
    if (s >= ST_COUNT) return false;

    // ---- 提示音：一次切换最多播一段，避免两段叠在一起 ----
    //   优先级：①进入态标记了「开始时播(START)」→ 播进入态的；②否则离开态标记了「结束时播(END)」→ 播离开态的。
    //   为什么进入态优先：AUTH 是"需要人工介入"的急态，必须保证它一进就响；
    //   而"离开 IDLE"这种只是新任务开始的轻微提示，被压掉无伤大雅。
    //   默认配置下二者不冲突：BUSY→IDLE 时 IDLE 是 END，于是回落到 BUSY 的 END，
    //   播的正是"任务完成"提示音——符合"只在状态结束边界出声、执行中绝不骚扰"。
    if (CFG[s].sound.enabled && CFG[s].sound.at_start) {
        cue_start(&CFG[s].sound);
    } else if (cur < ST_COUNT && CFG[cur].sound.enabled && !CFG[cur].sound.at_start) {
        cue_start(&CFG[cur].sound);
    }

    cur = s;
    status_led_host_control(true);   // 执行器接管 LED，本地 PTT 逻辑让位
    led_apply(s);
    return true;
}

void state_exec_task(void)
{
    // ---- 提示音重复调度：上次播完（BEEP_OFF）→ 等间隔 → 播下一次 ----
    if (rep_left && rep_score && beep_get_state() == BEEP_OFF) {
        if (rep_next_ms == 0) {
            rep_next_ms = board_millis() + rep_interval;
        } else if ((int32_t)(board_millis() - rep_next_ms) >= 0) {
            rep_next_ms = 0;
            rep_left--;
            beep_play_score(rep_score);
        }
    }

    // ---- 初始化展示超时（0 = 不超时，一直保持到首个 SET STATE）----
    if (cur == ST_COUNT && BOOT_TIMEOUT_MS > 0 &&
        (int32_t)(board_millis() - boot_at_ms) >= BOOT_TIMEOUT_MS) {
        state_exec_set(ST_IDLE);
    }
}
