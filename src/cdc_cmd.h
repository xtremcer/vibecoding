#ifndef CDC_CMD_H
#define CDC_CMD_H

#include <stdbool.h>
#include <stdint.h>

//--------------------------------------------------------------------+
// CDC 虚拟串口指令模块（上位机 → 设备）
//   行文本协议：\r 或 \n 任一结束，大小写不敏感，统一回 "OK" / "ERR n"。
//   纯非阻塞：靠主循环调用 cdc_cmd_task()，绝不做 printf / sleep_ms，
//   以免卡住音频路径（每毫秒要送 288 字节 iso 数据）。
//--------------------------------------------------------------------+

// 单行最大长度（含结束符前的全部字符），超出即丢弃并回 ERR 2
#define CDC_CMD_LINE_MAX 64

// 半行超时：收到首字符后这么久没等到结束符就丢弃（防上位机发残缺行卡死缓冲）
#define CDC_CMD_LINE_TIMEOUT_MS 200

void cdc_cmd_init(void);   // 复位行缓冲
void cdc_cmd_task(void);   // 主循环调用：收字节 → 成行 → 分发 → 回显

#endif // CDC_CMD_H
