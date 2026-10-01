#include "cdc_cmd.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "bsp/board_api.h"
#include "beep.h"
#include "state_exec.h"
#include "status_led.h"
#include "tusb.h"

//--------------------------------------------------------------------+
// 内部状态
//--------------------------------------------------------------------+
static char     line[CDC_CMD_LINE_MAX + 1];
static uint8_t  line_len = 0;
static uint32_t last_rx_ms = 0;
static uint32_t last_cmd_ms = 0;   // 最近一次收到完整指令的时刻，用于主机看门狗

//--------------------------------------------------------------------+
// 回显（只在主机真的打开了串口时发，否则丢弃并清空 TX FIFO）
//--------------------------------------------------------------------+
static void cdc_reply(const char* s)
{
    if (!tud_cdc_connected()) {
        tud_cdc_write_clear();
        return;
    }
    uint32_t n = tud_cdc_write(s, (uint32_t)strlen(s));
    (void)n;
    tud_cdc_write_flush();
}

// 回显一行（自动补 \r\n，上位机可直接 readline）
static void cdc_reply_line(const char* s)
{
    if (!tud_cdc_connected()) {
        tud_cdc_write_clear();
        return;
    }
    tud_cdc_write(s, (uint32_t)strlen(s));
    tud_cdc_write("\r\n", 2);
    tud_cdc_write_flush();
}

static void cdc_reply_ok(void)      { cdc_reply_line("OK"); }
static void cdc_reply_err(uint8_t c)
{
    if (!tud_cdc_connected()) {
        tud_cdc_write_clear();
        return;
    }
    tud_cdc_write("ERR ", 4);
    char d = (char)('0' + (c % 10));
    tud_cdc_write(&d, 1);
    tud_cdc_write("\r\n", 2);
    tud_cdc_write_flush();
}

//--------------------------------------------------------------------+
// 指令分发
//--------------------------------------------------------------------+

// 去掉首尾空白并原地转大写，返回起始指针
static char* cdc_normalize(char* s)
{
    while (*s == ' ' || *s == '\t') s++;
    char* start = s;
    for (char* p = s; *p; p++) {
        if (*p == '\r' || *p == '\n') { *p = 0; break; }
        *p = (char)toupper((unsigned char)*p);
    }
    size_t n = strlen(start);
    while (n > 0 && (start[n - 1] == ' ' || start[n - 1] == '\t')) start[--n] = 0;
    return start;
}

// strtok 风格分词：从 *cur 取下一个以空格分隔的 token（原地截断），并推进 *cur
static char* cdc_tok(char** cur)
{
    char* s = *cur;
    if (!s) return NULL;
    while (*s == ' ') s++;
    if (*s == 0) { *cur = NULL; return NULL; }
    char* start = s;
    while (*s && *s != ' ') s++;
    if (*s) { *s = 0; *cur = s + 1; } else { *cur = NULL; }
    return start;
}

// 把 "BUSY"/"PLAN"/"IDLE"/"ALL" 解析成索引；ALL 返回 LED_COUNT；无法识别返回 -1
static int cdc_led_id(const char* s)
{
    if (!s) return -1;
    if (strcmp(s, "BUSY") == 0) return (int)LED_BUSY;
    if (strcmp(s, "PLAN") == 0) return (int)LED_PLAN;
    if (strcmp(s, "IDLE") == 0) return (int)LED_IDLE;
    if (strcmp(s, "ALL")  == 0) return (int)LED_COUNT;
    return -1;
}

static void cdc_apply_led(int id, status_led_mode_t mode)
{
    // 上位机一旦下发 LED 指令即接管：本地逻辑（PTT→busy 灯）让位，避免互相打架
    status_led_host_control(true);
    if (id == (int)LED_COUNT) {
        for (int i = 0; i < LED_COUNT; i++)
            status_led_set_mode((status_led_t)i, mode);
    } else {
        status_led_set_mode((status_led_t)id, mode);
    }
}

// 解析十进制无符号整数（只接受纯数字，避免 atoi 把 "abc" 当 0）
static bool cdc_parse_u16(const char* s, uint16_t* out)
{
    if (!s || !*s) return false;
    uint32_t v = 0;
    for (const char* p = s; *p; p++) {
        if (*p < '0' || *p > '9') return false;
        v = v * 10u + (uint32_t)(*p - '0');
        if (v > 0xFFFFu) return false;
    }
    *out = (uint16_t)v;
    return true;
}

// ---- Phase 3：三灯全部开放 + 闪烁 ----
//   LED <BUSY|PLAN|IDLE|ALL> ON
//   LED <BUSY|PLAN|IDLE|ALL> OFF
//   LED <BUSY|PLAN|IDLE|ALL> BLINK [on_ms [off_ms]]   （省略默认 500/500；只给一个是on=off）
static void cdc_handle_led(char** cur)
{
    char* name = cdc_tok(cur);
    char* act  = cdc_tok(cur);
    int   id   = cdc_led_id(name);

    if (id < 0) { cdc_reply_err(3); return; }   // ERR 3 = 灯名不认识
    if (!act)   { cdc_reply_err(5); return; }   // ERR 5 = 缺参数

    if (strcmp(act, "ON") == 0)  { cdc_apply_led(id, LED_MODE_ON);  cdc_reply_ok(); return; }
    if (strcmp(act, "OFF") == 0) { cdc_apply_led(id, LED_MODE_OFF); cdc_reply_ok(); return; }

    if (strcmp(act, "BLINK") == 0) {
        uint16_t on_ms = 500, off_ms = 500;
        char* a = cdc_tok(cur);
        if (a) {
            if (!cdc_parse_u16(a, &on_ms)) { cdc_reply_err(5); return; }
            char* b = cdc_tok(cur);
            if (b && !cdc_parse_u16(b, &off_ms)) { cdc_reply_err(5); return; }
            if (!b) off_ms = on_ms;              // 只给一个参数 → 对称闪烁
        }
        status_led_host_control(true);
        if (id == (int)LED_COUNT) {
            for (int i = 0; i < LED_COUNT; i++)
                status_led_blink((status_led_t)i, on_ms, off_ms);
        } else {
            status_led_blink((status_led_t)id, on_ms, off_ms);
        }
        cdc_reply_ok();
        return;
    }

    cdc_reply_err(5);
}

static void cdc_handle_led_query(void)
{
    char buf[48];
    snprintf(buf, sizeof(buf), "BUSY=%d PLAN=%d IDLE=%d",
             status_led_is_lit(LED_BUSY) ? 1 : 0,
             status_led_is_lit(LED_PLAN) ? 1 : 0,
             status_led_is_lit(LED_IDLE) ? 1 : 0);
    cdc_reply_line(buf);
}

// ---- Phase 4：蜂鸣器（PWM 旋律引擎）----
//   BEEP ON                  → 默认音持续响
//   BEEP OFF                 → 彻底静音
//   BEEP STOP                → 立即停止
//   BEEP <ms>                → 默认音单音 ms 毫秒后自停
//   BEEP NOTE <hz> <ms>      → 指定音高单音 ms
//   BEEP PLAY <1|2|3>        → 播放预设旋律
static void cdc_handle_beep(char** cur)
{
    char* sub = cdc_tok(cur);
    if (!sub) { cdc_reply_err(5); return; }   // ERR 5 = 缺参数

    if (strcmp(sub, "ON") == 0)   { beep_on();   cdc_reply_ok(); return; }
    if (strcmp(sub, "OFF") == 0)  { beep_off();  cdc_reply_ok(); return; }
    if (strcmp(sub, "STOP") == 0) { beep_stop(); cdc_reply_ok(); return; }

    if (strcmp(sub, "NOTE") == 0) {
        char* a = cdc_tok(cur);
        char* b = cdc_tok(cur);
        uint16_t hz, ms;
        if (!a || !b || !cdc_parse_u16(a, &hz) || !cdc_parse_u16(b, &ms)) {
            cdc_reply_err(5); return;
        }
        beep_note(hz, (uint32_t)ms);
        cdc_reply_ok();
        return;
    }

    if (strcmp(sub, "PLAY") == 0) {
        char* a = cdc_tok(cur);
        uint16_t n;
        if (!a || !cdc_parse_u16(a, &n) || n < 1 || n > 3) {
            cdc_reply_err(5); return;
        }
        beep_play((uint8_t)n);
        cdc_reply_ok();
        return;
    }

    // 纯数字 → 默认音定时
    uint16_t ms;
    if (cdc_parse_u16(sub, &ms)) { beep_ms((uint32_t)ms); cdc_reply_ok(); return; }

    cdc_reply_err(5);
}

static void cdc_handle_beep_query(void)
{
    char buf[32];
    beep_state_t st = beep_get_state();
    const char* name = (st == BEEP_ON) ? "ON" : (st == BEEP_PLAYING) ? "PLAYING" : "OFF";
    snprintf(buf, sizeof(buf), "STATE=%s NOTE=%u", name, beep_get_hz());
    cdc_reply_line(buf);
}

// 交还本地控制：清接管标志，并复位状态执行器。
//   复位后 STATE? 回到 NONE，灯回到「上电初始化展示」（device.boot_led），
//   此后由本地逻辑（PTT→busy 灯等）驱动，直到下一个 SET STATE。
//   看门狗超时自动走这条路，所以"上位机崩了"会表现为三灯全亮的未接管态，很好认。
static void cdc_release_local(void)
{
    state_exec_release();
}

// ---- Phase A1：状态执行器 ----
//   SET STATE <BUSY|IDLE|AUTH>  → 切换状态（按配置驱动 LED + 提示音）
//   STATE?                      → 回当前态
static int cdc_state_id(const char* s)
{
    if (!s) return -1;
    if (strcmp(s, "BUSY") == 0) return (int)ST_BUSY;
    if (strcmp(s, "IDLE") == 0) return (int)ST_IDLE;
    if (strcmp(s, "AUTH") == 0) return (int)ST_AUTH;
    return -1;
}

static void cdc_handle_set(char** cur)
{
    char* w = cdc_tok(cur);
    if (!w) { cdc_reply_err(5); return; }          // ERR 5 = 缺参数

    if (strcmp(w, "STATE") == 0) {
        char* v = cdc_tok(cur);
        if (!v)  { cdc_reply_err(5); return; }   // ERR 5 = 缺参数
        int s = cdc_state_id(v);
        if (s < 0) { cdc_reply_err(4); return; } // ERR 4 = 枚举值非法
        state_exec_set((app_state_t)s);
        cdc_reply_ok();
        return;
    }

    cdc_reply_err(4);   // ERR 4 = SET 的第二个词不认识
}

static void cdc_handle_state_query(void)
{
    char buf[24];
    snprintf(buf, sizeof(buf), "STATE=%s", state_exec_name(state_exec_get()));
    cdc_reply_line(buf);
}

static void handle_line(char* raw)
{
    char* cmd = cdc_normalize(raw);
    if (*cmd == 0) return;                 // 空行

    // 任何一条完整指令都算"主机还活着"，用于刷新看门狗
    last_cmd_ms = board_millis();

    char* cur = cmd;
    char* w0  = cdc_tok(&cur);
    if (!w0) return;

    // ---- 身份查询（也可当心跳用）----
    if (strcmp(w0, "IDN?") == 0) {
        cdc_reply_line("vibecoding-mate mic v1.9");
        return;
    }

    // ---- LED 控制（Phase 3：三灯独立 亮/灭/闪）----
    if (strcmp(w0, "LED") == 0)  { cdc_handle_led(&cur); return; }
    if (strcmp(w0, "LED?") == 0) { cdc_handle_led_query(); return; }

    // ---- 交还本地控制 ----
    if (strcmp(w0, "RESET") == 0) { cdc_release_local(); cdc_reply_ok(); return; }

    // ---- 蜂鸣器控制（Phase 4：PWM 旋律引擎）----
    if (strcmp(w0, "BEEP") == 0)  { cdc_handle_beep(&cur); return; }
    if (strcmp(w0, "BEEP?") == 0) { cdc_handle_beep_query(); return; }

    // ---- 状态执行器（Phase A1）：设备只认状态，不认业务 ----
    if (strcmp(w0, "SET") == 0)    { cdc_handle_set(&cur); return; }
    if (strcmp(w0, "STATE?") == 0) { cdc_handle_state_query(); return; }

    // 后续阶段在此追加：...
    cdc_reply_err(1);   // ERR 1 = 未知指令
}

//--------------------------------------------------------------------+
// 对外 API
//--------------------------------------------------------------------+
void cdc_cmd_init(void)
{
    line_len = 0;
    last_rx_ms = 0;
    last_cmd_ms = 0;
}

void cdc_cmd_task(void)
{
    // 主机看门狗：接管状态下超过 CDC_HOST_TIMEOUT_MS 没收到任何指令，
    // 认为上位机崩了/断线，自动交还本地控制，避免灯卡在最后的状态。
    if (status_led_is_host_control() && last_cmd_ms &&
        (int32_t)(board_millis() - last_cmd_ms) >= CDC_HOST_TIMEOUT_MS) {
        cdc_release_local();
    }

    if (!tud_cdc_connected()) {
        line_len = 0;                      // 主机断开，丢弃半行
        tud_cdc_write_clear();
        return;
    }

    // 半行超时保护
    if (line_len && (int32_t)(board_millis() - last_rx_ms) >= CDC_CMD_LINE_TIMEOUT_MS) {
        line_len = 0;
        cdc_reply_err(2);                  // ERR 2 = 行超时/超长
    }

    uint32_t avail = tud_cdc_available();
    while (avail && tud_cdc_connected()) {
        char c;
        if (tud_cdc_read(&c, 1) == 0) break;
        avail--;
        last_rx_ms = board_millis();

        if (c == '\r' || c == '\n') {
            if (line_len) {
                line[line_len] = 0;
                handle_line(line);
                line_len = 0;
            }
            continue;                      // 空行直接忽略
        }

        if (line_len < CDC_CMD_LINE_MAX) {
            line[line_len++] = c;
        } else {
            line_len = 0;                  // 超长：丢弃并报错
            cdc_reply_err(2);
        }
    }
}
