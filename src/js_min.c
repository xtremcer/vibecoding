#include "js_min.h"

#include <stdlib.h>
#include <string.h>

//--------------------------------------------------------------------+
// 内部：扫描原语
//--------------------------------------------------------------------+
static const char* skip_ws(const char* p)
{
    if (!p) return NULL;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    return p;
}

// 跳过一个「字符串」，返回右引号之后的位置
static const char* skip_string(const char* p)
{
    if (*p != '"') return NULL;
    p++;
    while (*p) {
        if (*p == '\\') { p += 2; continue; }
        if (*p == '"') return p + 1;
        p++;
    }
    return NULL;
}

// 跳过一个完整的值（对象/字符串/数字/字面量），返回其后位置；坏结构返回 NULL
static const char* skip_value(const char* p)
{
    p = skip_ws(p);
    if (!p || !*p) return NULL;

    switch (*p) {
        case '{': {
            int depth = 0;
            while (*p) {
                if (*p == '"') { p = skip_string(p); if (!p) return NULL; continue; }
                if (*p == '{') depth++;
                else if (*p == '}') { depth--; p++; if (depth == 0) return p; continue; }
                p++;
            }
            return NULL;
        }
        case '"':
            return skip_string(p);
        case '[': {   // 配置里没有数组，但遇到要能安全跳过而不是把解析带偏
            int depth = 0;
            while (*p) {
                if (*p == '"') { p = skip_string(p); if (!p) return NULL; continue; }
                if (*p == '[') depth++;
                else if (*p == ']') { depth--; p++; if (depth == 0) return p; continue; }
                p++;
            }
            return NULL;
        }
        default:
            // 数字 / true / false / null —— 吃到分隔符为止
            while (*p && *p != ',' && *p != '}' && *p != ']' && *p != ' ' && *p != '\t'
                   && *p != '\r' && *p != '\n') p++;
            return p;
    }
}

// 比较两个 JSON 键名（src 是原文里的 "xxx" 形式，key 是裸名）
static bool key_eq(const char* src, const char* key)
{
    if (*src != '"') return false;
    src++;
    uint16_t n = (uint16_t)strlen(key);
    if (strncmp(src, key, n) != 0) return false;
    return src[n] == '"';
}

static const char* key_end(const char* src)
{
    return skip_string(src);   // src 指向 '"'
}

//--------------------------------------------------------------------+
// 对外：按 key 取值
//--------------------------------------------------------------------+
bool js_get(const char* obj, const char* key, js_val_t* v)
{
    if (v) { v->type = JS_NONE; v->str = NULL; v->slen = 0; v->num = 0; v->bol = false; v->obj = NULL; }
    if (!obj) return false;

    const char* p = skip_ws(obj);
    if (!p || *p != '{') return false;
    p++;

    while (p && *p) {
        p = skip_ws(p);
        if (!p || *p == '}') return false;          // 到末尾还没找到
        if (*p == ',') { p++; continue; }
        if (*p != '"') return false;                // 期待键名

        bool hit = key_eq(p, key);
        const char* kend = key_end(p);
        if (!kend) return false;

        const char* c = skip_ws(kend);
        if (!c || *c != ':') return false;

        const char* val = skip_ws(c + 1);
        if (!val) return false;

        const char* vend = skip_value(val);
        if (!vend) return false;

        if (!hit) { p = vend; continue; }           // 不是要找的键，整段跳过

        if (!v) return true;                        // 只问存在性

        // ---- 命中：解析值 ----
        if (*val == '"') {
            const char* e = skip_string(val);
            if (!e) return false;
            v->type = JS_STR;
            v->str  = val + 1;
            v->slen = (uint16_t)(e - val - 2);      // 不含两端引号
        } else if (*val == '{') {
            v->type = JS_OBJ;
            v->obj  = val;
        } else if (strncmp(val, "true", 4) == 0) {
            v->type = JS_BOOL; v->bol = true;
        } else if (strncmp(val, "false", 5) == 0) {
            v->type = JS_BOOL; v->bol = false;
        } else if (strncmp(val, "null", 4) == 0) {
            v->type = JS_NULL;
        } else {
            // number：只取整数部分（配置里全是整数）
            char* endp = NULL;
            long n = strtol(val, &endp, 10);
            if (endp == val) { v->type = JS_NONE; return false; }
            v->type = JS_NUM;
            v->num  = (int32_t)n;
        }
        return true;
    }
    return false;
}

//--------------------------------------------------------------------+
// 对外：带默认值的便捷读取（前向兼容的关键）
//--------------------------------------------------------------------+
bool js_get_bool(const char* obj, const char* key, bool def)
{
    js_val_t v;
    if (!js_get(obj, key, &v)) return def;
    if (v.type == JS_BOOL) return v.bol;
    if (v.type == JS_NUM)  return v.num != 0;   // 容忍 0/1 写法
    return def;
}

int32_t js_get_int(const char* obj, const char* key, int32_t def)
{
    js_val_t v;
    if (!js_get(obj, key, &v)) return def;
    if (v.type == JS_NUM) return v.num;
    return def;
}

bool js_get_str(const char* obj, const char* key, char* buf, uint16_t buflen)
{
    js_val_t v;
    if (buflen) buf[0] = 0;
    if (!js_get(obj, key, &v)) return false;
    if (v.type != JS_STR || !buf || buflen == 0) return false;

    uint16_t n = v.slen;
    if (n >= buflen) n = (uint16_t)(buflen - 1);
    memcpy(buf, v.str, n);
    buf[n] = 0;
    return true;
}
