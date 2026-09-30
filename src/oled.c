#include "oled.h"
#include "pico/stdlib.h"
#include "hardware/i2c.h"
#include "hardware/gpio.h"

void oled_init(void)
{
    i2c_init(OLED_I2C, OLED_I2C_BAUD);
    gpio_set_function(OLED_I2C_SDA, GPIO_FUNC_I2C);
    gpio_set_function(OLED_I2C_SCL, GPIO_FUNC_I2C);
    gpio_pull_up(OLED_I2C_SDA);
    gpio_pull_up(OLED_I2C_SCL);
}

bool oled_present(void)
{
    uint8_t dummy = 0x00;
    return i2c_write_timeout_us(OLED_I2C, OLED_I2C_ADDR, &dummy, 1, false, 5000) >= 0;
}

bool oled_write_cmd(uint8_t cmd)
{
    uint8_t buf[2] = { 0x00, cmd }; // 控制字节 0x00 = 后续为命令
    return i2c_write_timeout_us(OLED_I2C, OLED_I2C_ADDR, buf, 2, false, 5000) >= 0;
}

bool oled_write_data(const uint8_t* data, uint32_t len)
{
    uint8_t buf[17]; // 1 个控制字节(0x40) + 最多 16 字节数据
    while (len) {
        uint32_t n = len > 16 ? 16 : len;
        buf[0] = 0x40; // 控制字节 0x40 = 后续为数据
        for (uint32_t i = 0; i < n; i++)
            buf[1 + i] = data[i];
        if (i2c_write_timeout_us(OLED_I2C, OLED_I2C_ADDR, buf, n + 1, false, 5000) < 0)
            return false;
        data += n;
        len -= n;
    }
    return true;
}

void oled_task(void)
{
    // 预留：后续在此做周期性刷新（例如按系统状态重绘）。
}
