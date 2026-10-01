#include "state_exec.h"

#include <string.h>

#include "bsp/board_api.h"
#include "beep.h"
#include "config.h"
#include "status_led.h"

//--------------------------------------------------------------------+
// A2 起：表现配置不再写死在这里，一律读 cfg_get()->states[]。
//   默认值在 config.c 的 DEF（与 doc/12 §5.3 逐项对齐），
//   CONFIG SET 下发后这里读到的就是新值 —— 本文件不需要任何改动。
//--------------------------------------------------------------------+
#define CFG         (cfg_get()->states)
#define BOOT_ALL_ON (cfg_get()->device.boot_all_on)
#define BOOT_TIMEOUT_MS ((int32_t)cfg_get()->device.boot_timeout_ms)

static const char* STATE_NAMES[ST_COUNT] = { "BUSY", "IDLE", "AUTH" };

//--------------------------------------------------------------------+
// 内部状态
//--------------------------------------------------------------------+
static app_state_t cur        = ST_COUNT;   // ST_COUNT = 还没收到过任何状态
static uint32_t    boot_at_ms = 0;
static bool        local_mode = false;      // true = 已交还本地控制（PTT 驱动灯），执行器不再摆灯

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

const cfg_state_t* state_exec_cfg(app_state_t s)
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
    const cfg_state_t* c = &CFG[s];
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
static void cue_start(const cfg_snd_t* snd)
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
    local_mode = false;

    // 上电初始化展示：三灯全亮（ALL_ON）或全灭（ALL_OFF）。
    // ★ 必须置 host_control：否则本地 PTT 逻辑每帧都会把 BUSY 灯拉回"PTT 未按=灭"，
    //   "三灯全亮"根本立不住（实测 LED? 只有 PLAN=1 IDLE=1）。
    status_led_host_control(true);
    for (int i = 0; i < LED_COUNT; i++)
        status_led_set_mode((status_led_t)i, BOOT_ALL_ON ? LED_MODE_ON : LED_MODE_OFF);
}

// 交还本地控制：RESET 指令 / CDC 看门狗超时 / 初始化展示超时 都走这里。
//   清掉主机状态，恢复 v1.8 手感（idle 亮，busy 随后由 PTT 驱动）。
void state_exec_release(void)
{
    cur        = ST_COUNT;
    rep_left   = 0;
    local_mode = true;                 // 执行器不再摆灯，等下一个 SET STATE

    status_led_host_control(false);    // 交还：本地按键逻辑重新生效
    status_led_set(LED_BUSY, false);
    status_led_set(LED_PLAN, false);
    status_led_set(LED_IDLE, true);
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
    if (CFG[s].sound.enabled && CFG[s].sound.timing == CFG_TIMING_START) {
        cue_start(&CFG[s].sound);
    } else if (cur < ST_COUNT && CFG[cur].sound.enabled &&
               CFG[cur].sound.timing == CFG_TIMING_END) {
        cue_start(&CFG[cur].sound);
    }

    cur        = s;
    local_mode = false;
    status_led_host_control(true);   // 执行器接管 LED，本地 PTT 逻辑让位
    led_apply(s);
    return true;
}

// 配置被 CONFIG SET / CONFIG DEFAULT 改了：按新配置重摆灯。
//   **不播提示音**——改配置不是状态边界，不该出声。
void state_exec_reapply(void)
{
    if (cur < ST_COUNT && !local_mode) led_apply(cur);
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

    // ---- 初始化展示超时：仍没等到 SET STATE → 交还本地控制 ----
    //   不切到 idle 态：切态会置 host_control 并锁死本地 PTT，
    //   没跑上位机时用户会以为按键坏了。交还本地才是正解。
    if (cur == ST_COUNT && !local_mode && BOOT_TIMEOUT_MS > 0 &&
        (int32_t)(board_millis() - boot_at_ms) >= BOOT_TIMEOUT_MS) {
        state_exec_release();
    }
}
