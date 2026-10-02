#include "config.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "js_min.h"
#include "flash_store.h"      // A3：配置落盘（LittleFS）

//--------------------------------------------------------------------+
// 编译期默认 —— 与 doc/12 §5.3 / §5.6 逐项对齐
//--------------------------------------------------------------------+
static const app_config_t DEF = {
    .version = CFG_VERSION,
    .device  = { .active_high = true, .boot_all_on = true, .boot_timeout_ms = 5000 },
    .states  = {
        // BUSY：工作中。灯常亮；提示音在「结束时」×3（任务完成，执行中绝不响）
        [0] = { .led   = { .standby_on = true, .blink_on_ms = 0, .blink_off_ms = 0 },
                .sound = { .enabled = true, .timing = CFG_TIMING_END, .count = 3,
                           .loop_interval_ms = 1000, .score = "523,200;659,200;784,350" } },
        // IDLE：闲置。灯常亮；提示音在「结束时」×1（新任务开工的轻提示）
        [1] = { .led   = { .standby_on = true, .blink_on_ms = 0, .blink_off_ms = 0 },
                .sound = { .enabled = true, .timing = CFG_TIMING_END, .count = 1,
                           .loop_interval_ms = 1000, .score = "659,300;784,300" } },
        // AUTH：待授权。灯常亮 + 300/300 闪；提示音在「开始时」×3（催人工介入）
        [2] = { .led   = { .standby_on = true, .blink_on_ms = 300, .blink_off_ms = 300 },
                .sound = { .enabled = true, .timing = CFG_TIMING_START, .count = 3,
                           .loop_interval_ms = 1000, .score = "880,250;660,450" } },
    },
    // 键位默认与 A2b 之前的 buttons.c 编译期映射表一致（doc/12 §5.6）；
    // buttons.c 现在完全从这里（keys[]）实时读取，故这是键位的唯一真值来源。
    .keys = {
        [0] = { true,  2, CFG_MOD_LGUI,  0x35, CFG_BEH_NORMAL, 1000 },  // PTT   Win+`
        [1] = { true,  0, CFG_MOD_NONE,  0x28, CFG_BEH_NORMAL, 1000 },  // m1    Enter
        [2] = { true,  1, CFG_MOD_NONE,  0x2A, CFG_BEH_NORMAL, 1000 },  // m2    Backspace
        [3] = { true,  3, CFG_MOD_NONE,  0x29, CFG_BEH_NORMAL, 1000 },  // m3    Esc
        [4] = { true,  4, CFG_MOD_LCTRL, 0x04, CFG_BEH_NORMAL, 1000 },  // m4    Ctrl+A
        [5] = { true,  5, CFG_MOD_LCTRL, 0x06, CFG_BEH_NORMAL, 1000 },  // m5    Ctrl+C
        [6] = { true, 20, CFG_MOD_LCTRL, 0x19, CFG_BEH_NORMAL, 1000 },  // m6    Ctrl+V
        [7] = { true, 12, CFG_MOD_LCTRL, 0x0F, CFG_BEH_NORMAL, 1000 },  // m7    Ctrl+L
        [8] = { true, 13, CFG_MOD_LGUI,  0x31, CFG_BEH_SINGLE, 1000 },  // m8    Win+\ (单击)
    },
    .oled = { .enabled = false },
};

static const char* STATE_KEYS[3]      = { "busy", "idle", "auth" };
static const char* KEY_KEYS[CFG_KEY_SLOTS] = {
    "ptt", "macro1", "macro2", "macro3", "macro4",
    "macro5", "macro6", "macro7", "macro8"
};

// ---- 枚举 ↔ 线上传文本 ----
static const char* MOD_NAMES[CFG_MOD_COUNT] = { "NONE", "LGUI", "LCTRL", "LALT", "LSHIFT" };

static app_config_t cur;

#define CFG_PERSIST_BUF 4096          // 序列化缓冲（dump≈2400B，留足余量）
static bool     cfg_loaded = false;   // 开机是否从闪存成功载入了配置

//--------------------------------------------------------------------+
// 序列化：复用 cfg_dump 的逐行逻辑，把整份配置拼成一个紧凑 JSON 字符串
//--------------------------------------------------------------------+
static char*     ser_buf;
static uint16_t  ser_cap;
static uint16_t  ser_len;

static void ser_emit(const char* s)
{
    uint16_t n = (uint16_t)strlen(s);
    if (ser_len + n < ser_cap) {
        memcpy(ser_buf + ser_len, s, n);
        ser_len += n;
    }
}

void cfg_serialize(char* out, uint16_t cap)
{
    ser_buf = out; ser_cap = cap; ser_len = 0;
    cfg_dump(ser_emit);
    if (ser_len < cap) out[ser_len] = 0;
    else out[cap - 1] = 0;
}

//--------------------------------------------------------------------+
// 对外：基础
//--------------------------------------------------------------------+
void cfg_init(void)
{
    cur = DEF;                 // 先填编译期默认
    cfg_loaded = false;

    flash_store_init();        // 挂载 LFS（必要时格式化）

    static char buf[CFG_PERSIST_BUF];
    int n = flash_store_read_config(buf, sizeof(buf));
    if (n > 0) {
        char err[96];
        // 解析失败（文件损坏）→ 保留默认；成功则标记已从闪存载入
        if (cfg_apply_json(buf, err, sizeof(err)))
            cfg_loaded = true;
    }
}

void cfg_reset_default(void)  { cur = DEF; }

const app_config_t* cfg_get(void) { return &cur; }

bool cfg_loaded_from_flash(void) { return cfg_loaded; }

// 原子落盘：把当前 cur 序列化成 JSON 写闪存（临时文件 + rename）
bool cfg_persist(void)
{
    static char buf[CFG_PERSIST_BUF];
    cfg_serialize(buf, sizeof(buf));
    return flash_store_write_config(buf, (int)strlen(buf));
}

// 重新从闪存读 config.json 并应用（CONFIG LOAD / 恢复用）
bool cfg_load_from_flash(void)
{
    static char buf[CFG_PERSIST_BUF];
    int n = flash_store_read_config(buf, sizeof(buf));
    if (n <= 0) return false;
    char err[96];
    if (!cfg_apply_json(buf, err, sizeof(err))) return false;
    cfg_loaded = true;
    return true;
}

bool cfg_flash_mounted(void) { return flash_store_mounted(); }

//--------------------------------------------------------------------+
// 校验辅助
//--------------------------------------------------------------------+
static bool fail(char* errbuf, uint16_t errlen, const char* fmt, ...)
{
    if (errbuf && errlen) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(errbuf, errlen, fmt, ap);
        va_end(ap);
    }
    return false;
}

static bool in_range(int32_t v, int32_t lo, int32_t hi) { return v >= lo && v <= hi; }

//--------------------------------------------------------------------+
// 解析：states.<name>
//--------------------------------------------------------------------+
static bool parse_state(const char* obj, cfg_state_t* out, const char* who,
                        char* errbuf, uint16_t errlen)
{
    js_val_t v;

    // ---- led ----
    if (js_get(obj, "led", &v) && v.type == JS_OBJ) {
        char s[8];
        if (js_get_str(v.obj, "standby", s, sizeof(s))) {
            if      (strcmp(s, "ON")  == 0) out->led.standby_on = true;
            else if (strcmp(s, "OFF") == 0) out->led.standby_on = false;
            else return fail(errbuf, errlen, "%s.led.standby=%s (want ON|OFF)", who, s);
        }
        int32_t on  = js_get_int(v.obj, "blink_on_ms",  (int32_t)out->led.blink_on_ms);
        int32_t off = js_get_int(v.obj, "blink_off_ms", (int32_t)out->led.blink_off_ms);
        if (!in_range(on, 0, 60000))  return fail(errbuf, errlen, "%s.led.blink_on_ms=%ld", who, (long)on);
        if (!in_range(off, 0, 60000)) return fail(errbuf, errlen, "%s.led.blink_off_ms=%ld", who, (long)off);
        out->led.blink_on_ms  = (uint16_t)on;
        out->led.blink_off_ms = (uint16_t)off;
    }

    // ---- sound ----
    if (js_get(obj, "sound", &v) && v.type == JS_OBJ) {
        out->sound.enabled = js_get_bool(v.obj, "enabled", out->sound.enabled);

        char s[8];
        if (js_get_str(v.obj, "timing", s, sizeof(s))) {
            if      (strcmp(s, "START") == 0) out->sound.timing = CFG_TIMING_START;
            else if (strcmp(s, "END")   == 0) out->sound.timing = CFG_TIMING_END;
            else return fail(errbuf, errlen, "%s.sound.timing=%s (want START|END)", who, s);
        }

        int32_t cnt = js_get_int(v.obj, "count", out->sound.count);
        if (!in_range(cnt, 0, 10)) return fail(errbuf, errlen, "%s.sound.count=%ld (0..10)", who, (long)cnt);
        out->sound.count = (uint8_t)cnt;

        int32_t li = js_get_int(v.obj, "loop_interval_ms", out->sound.loop_interval_ms);
        if (!in_range(li, 0, 60000)) return fail(errbuf, errlen, "%s.sound.loop_interval_ms=%ld", who, (long)li);
        out->sound.loop_interval_ms = (uint16_t)li;

        // 乐谱：取到就覆盖，取不到保持原样（空串 = 静音，是合法值）
        char sc[CFG_SCORE_MAX];
        if (js_get_str(v.obj, "score", sc, sizeof(sc))) {
            if (strlen(sc) >= CFG_SCORE_MAX)
                return fail(errbuf, errlen, "%s.sound.score too long (max %d)", who, CFG_SCORE_MAX - 1);
            strncpy(out->sound.score, sc, CFG_SCORE_MAX - 1);
            out->sound.score[CFG_SCORE_MAX - 1] = 0;
        }
    }
    return true;
}

//--------------------------------------------------------------------+
// 解析：keys.<name>
//--------------------------------------------------------------------+
static bool parse_key(const char* obj, cfg_key_t* out, const char* who,
                      char* errbuf, uint16_t errlen)
{
    out->enabled = js_get_bool(obj, "enabled", out->enabled);

    int32_t pin = js_get_int(obj, "pin", out->pin);
    if (!in_range(pin, 0, 29)) return fail(errbuf, errlen, "%s.pin=%ld (0..29)", who, (long)pin);
    out->pin = (uint8_t)pin;

    char s[16];
    if (js_get_str(obj, "mod", s, sizeof(s))) {
        int i;
        for (i = 0; i < CFG_MOD_COUNT; i++)
            if (strcmp(s, MOD_NAMES[i]) == 0) { out->mod = (uint8_t)i; break; }
        if (i == CFG_MOD_COUNT)
            return fail(errbuf, errlen, "%s.mod=%s (want NONE|LGUI|LCTRL|LALT|LSHIFT)", who, s);
    }

    // key 列表里没有"无"选项 —— 必须有具体键，禁用靠 enabled
    int32_t kc = js_get_int(obj, "key", out->key);
    if (!in_range(kc, 1, 255)) return fail(errbuf, errlen, "%s.key=%ld (1..255, no NONE)", who, (long)kc);
    out->key = (uint8_t)kc;

    if (js_get_str(obj, "behavior", s, sizeof(s))) {
        if      (strcmp(s, "NORMAL") == 0) out->behavior = CFG_BEH_NORMAL;
        else if (strcmp(s, "SINGLE") == 0) out->behavior = CFG_BEH_SINGLE;
        else return fail(errbuf, errlen, "%s.behavior=%s (want NORMAL|SINGLE)", who, s);
    }

    int32_t cm = js_get_int(obj, "click_ms", out->click_ms);
    if (!in_range(cm, 1, 60000)) return fail(errbuf, errlen, "%s.click_ms=%ld (1..60000)", who, (long)cm);
    out->click_ms = (uint16_t)cm;

    return true;
}

//--------------------------------------------------------------------+
// 对外：解析 + 校验 + 替换（失败绝不动当前配置）
//--------------------------------------------------------------------+
bool cfg_apply_json(const char* json, char* errbuf, uint16_t errlen)
{
    if (!json || !*json) return fail(errbuf, errlen, "empty");

    js_val_t v;
    if (!js_get(json, "version", &v) || v.type != JS_NUM)
        return fail(errbuf, errlen, "missing \"version\"");
    if (v.num != CFG_VERSION)
        return fail(errbuf, errlen, "unsupported version %ld (firmware wants %d)",
                    (long)v.num, CFG_VERSION);

    // ★ 先解析到临时对象：任何一步失败就直接 return，cur 保持原样（要么全换要么不换）
    app_config_t t = cur;
    t.version = CFG_VERSION;

    // ---- device ----
    if (js_get(json, "device", &v) && v.type == JS_OBJ) {
        char s[16];
        if (js_get_str(v.obj, "led_polarity", s, sizeof(s))) {
            if      (strcmp(s, "ACTIVE_HIGH") == 0) t.device.active_high = true;
            else if (strcmp(s, "ACTIVE_LOW")  == 0) t.device.active_high = false;
            else return fail(errbuf, errlen, "device.led_polarity=%s", s);
        }
        if (js_get_str(v.obj, "boot_led", s, sizeof(s))) {
            if      (strcmp(s, "ALL_ON")  == 0) t.device.boot_all_on = true;
            else if (strcmp(s, "ALL_OFF") == 0) t.device.boot_all_on = false;
            else return fail(errbuf, errlen, "device.boot_led=%s (want ALL_ON|ALL_OFF)", s);
        }
        int32_t bt = js_get_int(v.obj, "boot_timeout_ms", t.device.boot_timeout_ms);
        if (!in_range(bt, 0, 60000)) return fail(errbuf, errlen, "device.boot_timeout_ms=%ld", (long)bt);
        t.device.boot_timeout_ms = (uint16_t)bt;
    }

    // ---- states ----
    if (js_get(json, "states", &v) && v.type == JS_OBJ) {
        const char* states = v.obj;
        for (int i = 0; i < 3; i++) {
            js_val_t sv;
            if (js_get(states, STATE_KEYS[i], &sv) && sv.type == JS_OBJ) {
                if (!parse_state(sv.obj, &t.states[i], STATE_KEYS[i], errbuf, errlen))
                    return false;
            }
        }
    }

    // ---- keys ----
    if (js_get(json, "keys", &v) && v.type == JS_OBJ) {
        const char* keys = v.obj;
        for (int i = 0; i < CFG_KEY_SLOTS; i++) {
            js_val_t kv;
            if (js_get(keys, KEY_KEYS[i], &kv) && kv.type == JS_OBJ) {
                if (!parse_key(kv.obj, &t.keys[i], KEY_KEYS[i], errbuf, errlen))
                    return false;
            }
        }
    }

    // ---- oled（占位，Phase D 才真用）----
    if (js_get(json, "oled", &v) && v.type == JS_OBJ)
        t.oled.enabled = js_get_bool(v.obj, "enabled", t.oled.enabled);

    cur = t;                     // 全部通过才落地
    return true;
}

//--------------------------------------------------------------------+
// 对外：序列化（逐行 emit，每行短于 200 字节以避开 TX FIFO 256 的限制）
//--------------------------------------------------------------------+
// 一行最长的情况是 sound 那行：18 空格前缀 + 键名 + score(最长 159) ≈ 220 字节。
// 给 256 留足余量——**绝不能让 JSON 被 snprintf 截断**，否则吐出来的是坏 JSON；
// 同时 220 < 254，单行塞得进 TX FIFO，不会被 CDC 静默截尾。
#define DUMPF(...) do { char _l[256]; snprintf(_l, sizeof(_l), __VA_ARGS__); emit(_l); } while (0)

// 乐谱里万一混进引号/反斜杠，会让整份 JSON 失效 —— 转义后再输出
static void esc_score(const char* in, char* out, uint16_t n)
{
    uint16_t j = 0;
    for (uint16_t i = 0; in[i] && j + 2 < n; i++) {
        if (in[i] == '"' || in[i] == '\\') out[j++] = '\\';
        out[j++] = in[i];
    }
    out[j] = 0;
}

void cfg_dump(void (*emit)(const char* line))
{
    const app_config_t* c = &cur;

    emit("{");
    DUMPF("  \"version\": %u,", (unsigned)c->version);

    emit("  \"device\": {");
    DUMPF("    \"led_polarity\": \"%s\",",  c->device.active_high ? "ACTIVE_HIGH" : "ACTIVE_LOW");
    DUMPF("    \"boot_led\": \"%s\",",      c->device.boot_all_on ? "ALL_ON" : "ALL_OFF");
    DUMPF("    \"boot_timeout_ms\": %u",    (unsigned)c->device.boot_timeout_ms);
    emit("  },");

    emit("  \"states\": {");
    for (int i = 0; i < 3; i++) {
        const cfg_state_t* s = &c->states[i];
        DUMPF("    \"%s\": {", STATE_KEYS[i]);
        DUMPF("      \"led\": { \"standby\": \"%s\", \"blink_on_ms\": %u, \"blink_off_ms\": %u },",
              s->led.standby_on ? "ON" : "OFF", (unsigned)s->led.blink_on_ms, (unsigned)s->led.blink_off_ms);
        DUMPF("      \"sound\": { \"enabled\": %s, \"timing\": \"%s\", \"count\": %u,",
              s->sound.enabled ? "true" : "false",
              s->sound.timing == CFG_TIMING_START ? "START" : "END",
              (unsigned)s->sound.count);
        char esc[CFG_SCORE_MAX * 2];
        esc_score(s->sound.score, esc, sizeof(esc));
        DUMPF("                  \"loop_interval_ms\": %u, \"score\": \"%s\" }",
              (unsigned)s->sound.loop_interval_ms, esc);
        DUMPF("    }%s", (i == 2) ? "" : ",");
    }
    emit("  },");

    emit("  \"keys\": {");
    for (int i = 0; i < CFG_KEY_SLOTS; i++) {
        const cfg_key_t* k = &c->keys[i];
        DUMPF("    \"%s\": { \"enabled\": %s, \"pin\": %u, \"mod\": \"%s\", \"key\": %u,",
              KEY_KEYS[i], k->enabled ? "true" : "false", (unsigned)k->pin,
              MOD_NAMES[(k->mod < CFG_MOD_COUNT) ? k->mod : 0], k->key);
        DUMPF("              \"behavior\": \"%s\", \"click_ms\": %u }%s",
              k->behavior == CFG_BEH_SINGLE ? "SINGLE" : "NORMAL",
              (unsigned)k->click_ms, (i == CFG_KEY_SLOTS - 1) ? "" : ",");
    }
    emit("  },");

    emit("  \"oled\": {");
    DUMPF("    \"enabled\": %s,", c->oled.enabled ? "true" : "false");
    emit("    \"layout\": \"PLACEHOLDER\"");
    emit("  }");
    emit("}");
}
