#include "cdc_cmd.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "bsp/board_api.h"
#include "beep.h"
#include "buttons.h"
#include "config.h"
#include "flash_store.h"         // flash_store_last_err / flash_store_rwtest（A3 诊断）
#include "hardware/watchdog.h"   // watchdog_reboot（REBOOT 命令）
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
// CONFIG GET 大块多行回复（A2 的坑）：
//   整份配置序列化后约 2400 字节，远超 TX FIFO（CFG_TUD_CDC_TX_BUFSIZE=1024），
//   若一次性 tud_cdc_write 会被静默丢尾（tud_cdc_write 满时直接丢弃多余字节）。
//   对策：先整份序列化进 RAM 缓冲，再在 cdc_cmd_task 里按
//   tud_cdc_write_available() 逐行吞吐，非阻塞、绝不丢字节。
//--------------------------------------------------------------------+
#define CDC_GET_BUF_MAX 3200
static char     get_buf[CDC_GET_BUF_MAX];
static uint32_t get_len = 0;     // 已序列化字节数（每行以 '\n' 结尾）
static uint32_t get_pos = 0;     // 已发出的字节数
static bool     get_pending = false;

// cfg_dump 的 emit：把每一行（不含 \r\n）以 '\n' 结尾追加进 get_buf
static void get_emit(const char* s)
{
    uint32_t n = (uint32_t)strlen(s);
    if (get_len + n + 1 > CDC_GET_BUF_MAX) return;   // 兜底，绝不越界
    memcpy(get_buf + get_len, s, n);
    get_len += n;
    get_buf[get_len++] = '\n';
}

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

// 对外单行回显出口（KEYMON 调试回声经此发）：仅主机连着时才发，否则丢弃。
void cdc_cmd_send_line(const char* s)
{
    if (!tud_cdc_connected()) {
        tud_cdc_write_clear();
        return;
    }
    tud_cdc_write(s, (uint32_t)strlen(s));
    tud_cdc_write("\r\n", 2);
    tud_cdc_write_flush();
}

// 带文字说明的错误（CONFIG SET 校验失败时把原因带回上位机，不然只能猜）
static void cdc_reply_err_msg(uint8_t c, const char* msg)
{
    if (!tud_cdc_connected()) { tud_cdc_write_clear(); return; }
    char head[8];
    snprintf(head, sizeof(head), "ERR %u ", (unsigned)(c % 10));
    tud_cdc_write(head, (uint32_t)strlen(head));
    if (msg) tud_cdc_write(msg, (uint32_t)strlen(msg));
    tud_cdc_write("\r\n", 2);
    tud_cdc_write_flush();
}
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

// 前缀匹配（忽略大小写 + 前导空白）；命中返回 payload 起点，否则 NULL。
//   用于 CONFIG SET：它的 payload 是 JSON，**绝不能**走 cdc_normalize（会把键名转大写）。
static const char* match_cmd(const char* s, const char* pfx)
{
    while (*s == ' ' || *s == '\t') s++;
    for (; *pfx; s++, pfx++) {
        char a = *s, b = *pfx;
        if (a >= 'a' && a <= 'z') a = (char)(a - 32);
        if (b >= 'a' && b <= 'z') b = (char)(b - 32);
        if (a != b) return NULL;
    }
    return s;
}

// ---- Phase A2：配置 ----
//   CONFIG GET              → 多行输出当前配置（CONFIG BEGIN … OK）
//   CONFIG SET <一行 JSON>  → 校验通过才替换；失败 ERR 6 并带回原因
//   CONFIG DEFAULT          → 恢复编译期默认
static void cdc_handle_config(const char* payload)
{
    if (!payload) { cdc_reply_err(5); return; }

    const char* p;
    if ((p = match_cmd(payload, "GET")) != NULL) {
        // 整份配置先序列进 RAM 缓冲，再由 cdc_cmd_task 逐行吐出（非阻塞、
        // 按 tud_cdc_write_available() 节流），彻底绕开 TX FIFO 静默截尾。
        get_len = 0;
        get_pos = 0;
        get_pending = true;
        get_emit("CONFIG BEGIN");
        cfg_dump(get_emit);
        get_emit("OK");
        return;
    }
    if ((p = match_cmd(payload, "DEFAULT")) != NULL) {
        cfg_reset_default();
        state_exec_reapply();     // 让新默认立刻作用到当前态的灯
        // A3：默认也落盘（删掉已改配置）。落盘失败必须报错，不能沿用旧实现静默回 OK。
        if (!cfg_persist()) {
            char perr[64];
            snprintf(perr, sizeof(perr), "persist-failed lfs=%d", flash_store_last_err());
            cdc_reply_err_msg(6, perr);
            return;
        }
        cdc_reply_ok();
        return;
    }
    if ((p = match_cmd(payload, "LOAD")) != NULL) {
        // 重新从闪存读 config.json 并应用（验证落盘 / 恢复用）
        if (!cfg_load_from_flash()) { cdc_reply_err(6); return; }
        state_exec_reapply();
        cdc_reply_ok();
        return;
    }
    if ((p = match_cmd(payload, "SET")) != NULL) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) { cdc_reply_err(5); return; }

        char err[96];
        if (!cfg_apply_json(p, err, sizeof(err))) {
            cdc_reply_err_msg(6, err);   // 配置**未被采用**，保持原样
            return;
        }
        state_exec_reapply();     // 新配置立刻作用到当前态的灯
        // A3：改动立即落盘，掉电不丢。原实现忽略 cfg_persist 返回值，写失败仍回 OK，
        // 把「配置根本没进闪存」完全掩盖了（CONFIG LOAD 才诚实地报 ERR 6）。现在报错。
        if (!cfg_persist()) {
            char perr[64];
            snprintf(perr, sizeof(perr), "persist-failed lfs=%d", flash_store_last_err());
            cdc_reply_err_msg(6, perr);
            return;
        }
        cdc_reply_ok();
        return;
    }
    cdc_reply_err(5);
}

static void handle_line(char* raw)
{
    // ★ CONFIG 必须走原始行：JSON 里有小写键名和带小写的字符串值，
    //   经 cdc_normalize() 转大写后就废了。所以在这里就拦下来。
    {
        const char* p = match_cmd(raw, "CONFIG");
        if (p) { cdc_handle_config(p); return; }
    }

    char* cmd = cdc_normalize(raw);
    if (*cmd == 0) return;                 // 空行

    // 任何一条完整指令都算"主机还活着"，用于刷新看门狗
    last_cmd_ms = board_millis();

    char* cur = cmd;
    char* w0  = cdc_tok(&cur);
    if (!w0) return;

    // ---- 身份查询（也可当心跳用）----
    if (strcmp(w0, "IDN?") == 0) {
        cdc_reply_line("vibecoding-mate mic v2.0");
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

    // ---- 闪存状态（A3：配置落盘诊断）----
    //   FLASH?      →  MOUNT=<0|1> LOAD=<0|1> ERR=<lfs错误码>
    //   FLASH RWTEST →  RWTEST ERASE=<0|1> PROG=<0|1> FIRST=<hex>（原始闪存自检，
    //                   会毁掉分区并重新格式化，仅诊断用）
    if (strcmp(w0, "FLASH") == 0) {
        char* arg = cdc_tok(&cur);
        if (arg && strcmp(arg, "RWTEST") == 0) {
            char buf[64];
            flash_store_rwtest(buf, sizeof(buf));
            cdc_reply_line(buf);
            return;
        }
        cdc_reply_err(5);
        return;
    }
    if (strcmp(w0, "FLASH?") == 0) {
        char buf[48];
        snprintf(buf, sizeof(buf), "MOUNT=%d LOAD=%d ERR=%d",
                 cfg_flash_mounted() ? 1 : 0,
                 cfg_loaded_from_flash() ? 1 : 0,
                 flash_store_last_err());
        cdc_reply_line(buf);
        return;
    }

    // ---- 重启（开发用：验证掉电/重启后配置仍保留）----
    //   先回 OK 再触发看门狗复位；主机靠串口掉线 + 重枚举检测到重启完成。
    if (strcmp(w0, "REBOOT") == 0) {
        cdc_reply_ok();
        watchdog_reboot(0, 0, 0);
        return;             // watchdog_reboot 会复位芯片，不会到这里
    }

    // ---- KEYMON：按键调试回声开关 ----
    //   KEYMON ON  → 之后每次按键按下/松开经 CDC 回 "KEY <槽位> <pin> DOWN/UP"
    //   KEYMON OFF → 关闭回声（默认关）
    //   用于不接 HID 监听时确认「物理按键被扫描到、落到哪个槽、对应哪个脚」。
    if (strcmp(w0, "KEYMON") == 0) {
        char* arg = cdc_tok(&cur);
        if (!arg) { cdc_reply_err(5); return; }
        if (strcmp(arg, "ON") == 0)        { buttons_set_echo(true);  cdc_reply_ok(); return; }
        if (strcmp(arg, "OFF") == 0)       { buttons_set_echo(false); cdc_reply_ok(); return; }
        cdc_reply_err(5);
        return;
    }

    // 后续阶段在此追加：...
    cdc_reply_err(1);   // ERR 1 = 未知指令
}

//--------------------------------------------------------------------+
// 对外 API
//--------------------------------------------------------------------+

// 逐行把 get_buf 吐进 CDC TX FIFO；仅当 FIFO 有余量才写，绝不丢字节。
//   每个主循环 tick 调一次：能发几行发几行，发不完留到下个 tick，自然节流。
static void cdc_get_drain(void)
{
    if (!get_pending) return;
    if (!tud_cdc_connected()) { get_pending = false; return; }   // 主机断开，放弃

    bool wrote = false;
    while (get_pos < get_len) {
        uint32_t nl = get_pos;
        while (nl < get_len && get_buf[nl] != '\n') nl++;
        if (nl >= get_len) break;                 // 理论不会到（末行也带 '\n'）
        uint32_t line_bytes = nl - get_pos;        // 不含 '\n'
        // 一行要 line_bytes + 2（\r\n）；FIFO 装不下就等下个 tick（主机读走后腾出空间）
        if (tud_cdc_write_available() < line_bytes + 2) break;
        tud_cdc_write(get_buf + get_pos, line_bytes);
        tud_cdc_write("\r\n", 2);
        get_pos = nl + 1;                          // 跳过 '\n'
        wrote = true;
    }
    if (wrote) tud_cdc_write_flush();
    if (get_pos >= get_len) get_pending = false;   // 全部发完
}

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
        get_pending = false;               // 进行中的 CONFIG GET 也一并放弃
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

    // CONFIG GET 的多行回复在此逐行吐出（非阻塞、按 FIFO 余量节流）
    cdc_get_drain();
}
