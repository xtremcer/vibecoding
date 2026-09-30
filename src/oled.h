#ifndef OLED_H
#define OLED_H

#include <stdbool.h>
#include <stdint.h>

//--------------------------------------------------------------------+
// OLED 模块（预留接口）—— I2C1：SDA=GP10、SCL=GP11
//   这里只做总线初始化 + 底层读写；具体显示驱动（SSD1306/SH1106 等）后续按需补充。
//--------------------------------------------------------------------+

#define OLED_I2C       i2c1
#define OLED_I2C_SDA   10
#define OLED_I2C_SCL   11
#define OLED_I2C_BAUD  400000
#define OLED_I2C_ADDR  0x3C   // 常见 SSD1306 地址（0x3C 或 0x3D）

void oled_init(void);                                  // 初始化 I2C1 与 SDA/SCL 引脚
bool oled_present(void);                               // 探测器件是否在总线上（ACK）
bool oled_write_cmd(uint8_t cmd);                      // 底层：写一个命令字节
bool oled_write_data(const uint8_t* data, uint32_t len); // 底层：写数据（自动加 0x40 前缀分块）
void oled_task(void);                                  // 预留：周期性刷新任务

#endif // OLED_H
