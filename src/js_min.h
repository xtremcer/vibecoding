#ifndef JS_MIN_H
#define JS_MIN_H

#include <stdbool.h>
#include <stdint.h>

//--------------------------------------------------------------------+
// 极轻量 JSON 只读扫描器（Phase A2）
//   为什么不用 cJSON：配置 schema 是**我们自己的、已知的、扁平的**，
//   引一整套通用解析器（动态分配 + 链表）既占 Flash 又要在 RP2040 上操心堆。
//   这里只做一件事：给一段 JSON，按「对象 → 键」取标量或子对象。
//
//   能力边界（够用即可，不做通用 JSON）：
//   - 只支持 object / string / number(整数) / bool / null；**不支持 array**（配置里没有）
//   - 字符串支持 \" \\ \/ \n \t 转义；不支持 \uXXXX（配置里没有非 ASCII）
//   - 不校验 JSON 语法完整性：遇到坏结构就「取不到」，由上层判 ERR 6
//--------------------------------------------------------------------+

typedef enum {
    JS_NONE = 0,   // 没找到 / 坏结构
    JS_STR,
    JS_NUM,
    JS_BOOL,
    JS_OBJ,
    JS_NULL,
} js_type_t;

typedef struct {
    js_type_t   type;
    const char* str;    // JS_STR：值起点（不含引号）
    uint16_t    slen;   // JS_STR：长度
    int32_t     num;    // JS_NUM：整数值
    bool        bol;    // JS_BOOL
    const char* obj;    // JS_OBJ：指向 '{'
} js_val_t;

// 在 obj（指向 '{'）里查找 key；找到返回 true 并填充 v。
// 只在这一层找，不递归进子对象；值本身可以是子对象（v->obj 指过去，可继续 js_get）。
bool js_get(const char* obj, const char* key, js_val_t* v);

// 便捷读取：取不到就用默认值兜底（前向兼容：老配置缺键 → 用默认，不报错）
bool     js_get_bool(const char* obj, const char* key, bool def);
int32_t  js_get_int (const char* obj, const char* key, int32_t def);
// 取字符串到 buf（会自动截断并保证 '\0' 结尾）；取不到返回 false 且 buf 置空串
bool     js_get_str (const char* obj, const char* key, char* buf, uint16_t buflen);

#endif // JS_MIN_H
