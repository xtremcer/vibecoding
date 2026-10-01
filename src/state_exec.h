#ifndef STATE_EXEC_H
#define STATE_EXEC_H

#include <stdbool.h>
#include <stdint.h>

//--------------------------------------------------------------------+
// 状态执行器（Phase A1）——「设备 = 哑执行器」的落地
//   设备只认「当前状态」+ 配置，不关心状态从哪来（上位机 / hook / 手动都行）。
//   状态由上位机经 CDC `SET STATE <BUSY|IDLE|AUTH>` 下发，本模块把它翻译成
//   LED 表现 + 提示音表现。
//
//   配置目前是**编译期内置默认**（与 doc/12 §5.3 示例、§5.6 键位默认同思路）。
//   A2 起支持 JSON 下发覆盖；A3 起持久化到 LittleFS；缺失/损坏就回退到这里。
//--------------------------------------------------------------------+

typedef enum {
    ST_BUSY = 0,   // 正在生成对话 / 执行任务
    ST_IDLE,       // 完成 / 闲置
    ST_AUTH,       // 需要用户对授权操作
    ST_COUNT
} app_state_t;

// ---- 某态的 LED 表现 ----
typedef struct {
    bool     standby_on;    // 无闪烁时的稳态：true=亮 false=灭
    uint16_t blink_on_ms;   // 点亮时长；与 off 同为 0 = 按 standby 常亮/常灭
    uint16_t blink_off_ms;  // 熄灭时长
} led_cfg_t;

// ---- 某态的提示音 ----
typedef struct {
    bool        enabled;          // 关 = 该态静音（**无全局总开关**）
    bool        at_start;         // true=进入该态时播(START)；false=该态结束切走时播(END)
    uint8_t     count;            // 提示次数
    uint16_t    loop_interval_ms; // 多次之间的循环间隔
    const char* score;            // 乐谱 "频率,时长ms;..."（空串=静音）
} snd_cfg_t;

typedef struct {
    led_cfg_t led;
    snd_cfg_t sound;
} state_cfg_t;

//--------------------------------------------------------------------+
// 物理 LED 映射（硬件三颗灯：BUSY=GP26 / PLAN=GP27 / IDLE=GP28）
//   应用三态各占一颗；AUTH 复用 PLAN 灯（GP27）。
//--------------------------------------------------------------------+
typedef enum {
    PHYS_BUSY = 0,  // GP26
    PHYS_AUTH,      // GP27（原 PLAN 灯，这里作 AUTH）
    PHYS_IDLE,      // GP28
} phys_led_t;

void           state_exec_init(void);                  // 上电：应用初始化展示（boot_led），期间由执行器接管 LED
void           state_exec_release(void);               // 交还本地控制（RESET / 看门狗）：清接管 + 清状态，回 v1.8 手感
void           state_exec_task(void);                  // 主循环：推进提示音重复/间隔、初始化展示超时
bool           state_exec_set(app_state_t s);          // 切换状态（触发 LED + 提示音）
app_state_t    state_exec_get(void);                   // 当前态（ST_COUNT = 还没收到过状态）
const char*    state_exec_name(app_state_t s);         // "BUSY"/"IDLE"/"AUTH"/"NONE"
const state_cfg_t* state_exec_cfg(app_state_t s);      // 只读配置（A2 的 CONFIG GET 会用）

#endif // STATE_EXEC_H
