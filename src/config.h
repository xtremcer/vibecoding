#ifndef CONFIG_H
#define CONFIG_H

#include <stdbool.h>
#include <stdint.h>

//--------------------------------------------------------------------+
// 应用配置（Phase A2）—— doc/12 §5.3 的 C 侧落地
//   A2：配置常驻 RAM，覆盖编译期默认；非法 JSON → 拒收（ERR 6），原配置不动。
//   A3：再加 LittleFS 持久化（解析/校验/序列化这套不用改）。
//
//   设计要点：
//   - 结构体里**不放指针**：乐谱用定长数组，整个配置是可 memcpy 的纯值对象，
//     后面要做双份缓冲（正在用的 / 正在解析的）时不会踩坑。
//   - 枚举一律用 uint8_t 存、字符串在边界转换：线上传文本，机内跑整数。
//--------------------------------------------------------------------+

#define CFG_VERSION     1
#define CFG_SCORE_MAX   160         // 乐谱文本最大长度（含 '\0'）
#define CFG_KEY_SLOTS   9           // ptt + macro1..8

// ---- 修饰键（对应网页「按键1」下拉）----
typedef enum {
    CFG_MOD_NONE = 0,
    CFG_MOD_LGUI,      // Win
    CFG_MOD_LCTRL,     // Ctrl
    CFG_MOD_LALT,      // Alt
    CFG_MOD_LSHIFT,    // Shift
    CFG_MOD_COUNT
} cfg_mod_t;

// ---- 提示时机 ----
typedef enum { CFG_TIMING_END = 0, CFG_TIMING_START } cfg_timing_t;

// ---- 触发行为 ----
typedef enum { CFG_BEH_NORMAL = 0, CFG_BEH_SINGLE } cfg_beh_t;

// ---- 某态的 LED ----
typedef struct {
    bool     standby_on;
    uint16_t blink_on_ms;
    uint16_t blink_off_ms;
} cfg_led_t;

// ---- 某态的提示音 ----
typedef struct {
    bool     enabled;
    uint8_t  timing;             // cfg_timing_t
    uint8_t  count;
    uint16_t loop_interval_ms;
    char     score[CFG_SCORE_MAX];
} cfg_snd_t;

typedef struct {
    cfg_led_t led;
    cfg_snd_t sound;
} cfg_state_t;

// ---- 单个按键（PTT 与 macro1..8 结构完全一致）----
typedef struct {
    bool     enabled;
    uint8_t  pin;
    uint8_t  mod;                // cfg_mod_t
    uint8_t  key;                // HID keycode（无"无"选项，禁用靠 enabled）
    uint8_t  behavior;           // cfg_beh_t
    uint16_t click_ms;           // 仅 SINGLE 生效
} cfg_key_t;

typedef struct {
    bool     active_high;
    bool     boot_all_on;        // true=ALL_ON 三灯全亮；false=ALL_OFF
    uint16_t boot_timeout_ms;
} cfg_dev_t;

typedef struct {
    bool enabled;                // layout 仍为占位（Phase D 才真做）
} cfg_oled_t;

typedef struct {
    uint16_t   version;
    cfg_dev_t  device;
    cfg_state_t states[3];       // 下标 = app_state_t（BUSY/IDLE/AUTH）
    cfg_key_t  keys[CFG_KEY_SLOTS];  // 0=ptt，1..8=macro1..8
    cfg_oled_t oled;
} app_config_t;

//--------------------------------------------------------------------+
// API
//--------------------------------------------------------------------+
void                cfg_init(void);                 // 载入编译期默认
const app_config_t* cfg_get(void);                  // 当前生效配置（只读）

// 解析 + 校验 + 替换。成功返回 true；失败返回 false 并把原因写进 errbuf（可空）。
// 关键：**失败时绝不动当前配置**（要么全换，要么不换）。
bool cfg_apply_json(const char* json, char* errbuf, uint16_t errlen);

// 恢复编译期默认
void cfg_reset_default(void);

// 序列化：逐行 emit（每行 ≤ 200 字节，避开 CDC TX FIFO 只有 256 的坑）
void cfg_dump(void (*emit)(const char* line));

#endif // CONFIG_H
