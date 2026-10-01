#include "cdc_cmd.h"

#include <ctype.h>
#include <string.h>

#include "bsp/board_api.h"
#include "tusb.h"

//--------------------------------------------------------------------+
// 内部状态
//--------------------------------------------------------------------+
static char     line[CDC_CMD_LINE_MAX + 1];
static uint8_t  line_len = 0;
static uint32_t last_rx_ms = 0;

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

// 取下一个以空格分隔的 token（原地截断），没有则返回 NULL
static char* cdc_next_token(char* s)
{
    if (!s) return NULL;
    while (*s == ' ') s++;
    if (*s == 0) return NULL;
    char* start = s;
    while (*s && *s != ' ') s++;
    if (*s) *s++ = 0;
    return start;
}

static void handle_line(char* raw)
{
    char* cmd = cdc_normalize(raw);
    if (*cmd == 0) return;                 // 空行

    // ---- Phase 1：只做身份查询，用来确认链路通 ----
    if (strncmp(cmd, "IDN?", 4) == 0) {
        cdc_reply_line("vibecoding-mate mic v1.5");
        return;
    }

    // 后续阶段在此追加：LED / BEEP / ...
    cdc_reply_err(1);   // ERR 1 = 未知指令
}

//--------------------------------------------------------------------+
// 对外 API
//--------------------------------------------------------------------+
void cdc_cmd_init(void)
{
    line_len = 0;
    last_rx_ms = 0;
}

void cdc_cmd_task(void)
{
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
