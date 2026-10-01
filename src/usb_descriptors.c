/*
 * The MIT License (MIT)
 *
 * Copyright (c) 2019 Ha Thach (tinyusb.org)
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 *
 */

#define CFG_TUD_AUDIO_FUNC_1_N_BYTES_PER_SAMPLE_TX 3

#include "bsp/board_api.h"
#include "tusb.h"
#include "class/hid/hid_device.h"   // HID device 专用：TUD_HID_DESCRIPTOR / TUD_HID_REPORT_DESC_KEYBOARD / 回调声明

/* A combination of interfaces must have a unique product id, since PC will save device driver after the first plug.
 * Same VID/PID with different interface e.g MSC (first), then CDC (later) will possibly cause system error on PC.
 *
 * Auto ProductID layout's Bitmap:
 *   [MSB]     AUDIO | MIDI | HID | MSC | CDC          [LSB]
 */
#define _PID_MAP(itf, n) ((CFG_TUD_##itf) << (n))

/* Windows 会按 VID/PID 缓存 USB 设备名：只改产品字符串而不改 PID，PC 端显示名不会刷新。
 * 改 PID 可让系统把它当作"新设备"，从而读取新的产品名 "vibecoding-mate mic"。
 * 原 PID 为 0x4010（AUDIO 位），现改为 0x4A10；若日后需再次强制刷新，继续调整末位即可。 */
#define USB_VID 0xCafe
#define USB_PID 0x4A10

//--------------------------------------------------------------------+
// Device Descriptors
//--------------------------------------------------------------------+
tusb_desc_device_t const desc_device = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,

    // Use Interface Association Descriptor (IAD) for Audio
    // As required by USB Specs IAD's subclass must be common class (2) and protocol must be IAD (1)
    .bDeviceClass = TUSB_CLASS_MISC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,

    .idVendor = USB_VID,
    .idProduct = USB_PID,
    .bcdDevice = 0x0100,

    .iManufacturer = 0x01,
    .iProduct = 0x02,
    .iSerialNumber = 0x03,

    .bNumConfigurations = 0x01
};

// Invoked when received GET DEVICE DESCRIPTOR
// Application return pointer to descriptor
uint8_t const* tud_descriptor_device_cb(void)
{
    return (uint8_t const*)&desc_device;
}

//--------------------------------------------------------------------+
// Configuration Descriptor
//--------------------------------------------------------------------+
enum {
    ITF_NUM_AUDIO_CONTROL = 0,
    ITF_NUM_AUDIO_STREAMING,
    ITF_NUM_HID,
    ITF_NUM_CDC,      // CDC ACM 控制接口（必须追加在 HID 之后，保证已有接口号不变）
    ITF_NUM_CDC_DATA, // CDC 数据接口（紧跟控制接口，二者由 IAD 绑定为一组）
    ITF_NUM_TOTAL
};

#define CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + CFG_TUD_AUDIO * TUD_AUDIO_MIC_TWO_CH_DESC_LEN + TUD_HID_DESC_LEN + TUD_CDC_DESC_LEN)

#if TU_CHECK_MCU(OPT_MCU_LPC175X_6X, OPT_MCU_LPC177X_8X, OPT_MCU_LPC40XX)
// LPC 17xx and 40xx endpoint type (bulk/interrupt/iso) are fixed by its number
// 0 control, 1 In, 2 Bulk, 3 Iso, 4 In etc ...
#define EPNUM_AUDIO 0x03

#elif TU_CHECK_MCU(OPT_MCU_NRF5X)
// nRF5x ISO can only be endpoint 8
#define EPNUM_AUDIO 0x08

#else
#define EPNUM_AUDIO 0x01
#endif

// HID 键盘用独立中断端点（Audio 已占用 EP1 IN = 0x81，HID 用 EP2 IN = 0x82）
#define EPNUM_HID 0x02

// CDC 用 3 个端点：通知(中断 IN) / 数据 OUT / 数据 IN
// 注意：通知与数据 IN 同为 IN 方向，必须用不同的端点号，否则撞车
#define EPNUM_CDC_NOTIF 0x83
#define EPNUM_CDC_OUT   0x03
#define EPNUM_CDC_IN    0x84

//--------------------------------------------------------------------+
// HID 自定义键盘（PTT：GP2 接地触发）
//--------------------------------------------------------------------+

// 标准 6KRO 键盘报告描述符；系统识别为普通键盘，可绑定任意按键做 Push-To-Talk
uint8_t const hid_report_descriptor[] = {
    TUD_HID_REPORT_DESC_KEYBOARD()
};

uint8_t const desc_configuration[] = {
    // Config number, interface count, string index, total length, attribute, power in mA
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN, 0x00, 100),

    // Interface number, string index, EP Out & EP In address, EP size
    TUD_AUDIO_MIC_TWO_CH_DESCRIPTOR(/*_itfnum*/ ITF_NUM_AUDIO_CONTROL, /*_stridx*/ 0, /*_nBytesPerSample*/ 3, /*_nBitsUsedPerSample*/ 3 * 8, /*_epin*/ 0x80 | EPNUM_AUDIO, /*_epsize*/ CFG_TUD_AUDIO_EP_SZ_IN),

    // HID 自定义键盘接口（PTT：GP2 接地触发），独立中断端点 0x82，识别为 boot 键盘
    // TUD_HID_DESCRIPTOR 第 7 参 = 轮询间隔（FS 下单位 ms），10 = 10ms
    TUD_HID_DESCRIPTOR(ITF_NUM_HID, 0, HID_ITF_PROTOCOL_KEYBOARD, sizeof(hid_report_descriptor), 0x80 | EPNUM_HID, 16, 10),

    // CDC ACM 虚拟串口：上位机用它下发 LED/BEEP 指令（Win10+ 自带 usbser.sys，免驱）
    TUD_CDC_DESCRIPTOR(ITF_NUM_CDC, 5, EPNUM_CDC_NOTIF, 8, EPNUM_CDC_OUT, EPNUM_CDC_IN, 64)
};

// Invoked when received GET CONFIGURATION DESCRIPTOR
// Application return pointer to descriptor
// Descriptor contents must exist long enough for transfer to complete
uint8_t const* tud_descriptor_configuration_cb(uint8_t index)
{
    (void)index; // for multiple configurations
    return desc_configuration;
}

//--------------------------------------------------------------------+
// String Descriptors
//--------------------------------------------------------------------+

// String Descriptor Index
enum {
    STRID_LANGID = 0,
    STRID_MANUFACTURER,
    STRID_PRODUCT,
    STRID_SERIAL,
};

// array of pointer to string descriptors
char const* string_desc_arr[] = {
    (const char[]) { 0x09, 0x04 }, // 0: is supported language is English (0x0409)
    "PaniDCorp", // 1: Manufacturer
    "vibecoding-mate mic", // 2: Product
    NULL, // 3: Serials will use unique ID if possible
    "UAC2", // 4: Audio Interface
    "CDC Control", // 5: CDC 虚拟串口接口
};

static uint16_t _desc_str[32 + 1];

// Invoked when received GET STRING DESCRIPTOR request
// Application return pointer to descriptor, whose contents must exist long enough for transfer to complete
uint16_t const* tud_descriptor_string_cb(uint8_t index, uint16_t langid)
{
    (void)langid;
    size_t chr_count;

    switch (index) {
    case STRID_LANGID:
        memcpy(&_desc_str[1], string_desc_arr[0], 2);
        chr_count = 1;
        break;

    case STRID_SERIAL:
        chr_count = board_usb_get_serial(_desc_str + 1, 32);
        break;

    default:
        // Note: the 0xEE index string is a Microsoft OS 1.0 Descriptors.
        // https://docs.microsoft.com/en-us/windows-hardware/drivers/usbcon/microsoft-defined-usb-descriptors

        if (!(index < sizeof(string_desc_arr) / sizeof(string_desc_arr[0])))
            return NULL;

        const char* str = string_desc_arr[index];

        // Cap at max char
        chr_count = strlen(str);
        size_t const max_count = sizeof(_desc_str) / sizeof(_desc_str[0]) - 1; // -1 for string type
        if (chr_count > max_count)
            chr_count = max_count;

        // Convert ASCII string into UTF-16
        for (size_t i = 0; i < chr_count; i++) {
            _desc_str[1 + i] = str[i];
        }
        break;
    }

    // first byte is length (including header), second byte is string type
    _desc_str[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * chr_count + 2));

    return _desc_str;
}

//--------------------------------------------------------------------+
// HID 回调（PTT：GP2 接地触发）
//--------------------------------------------------------------------+

// 返回 HID 报告描述符
uint8_t const* tud_hid_descriptor_report_cb(uint8_t instance)
{
    (void)instance;
    return hid_report_descriptor;
}

// 主机 GET_REPORT：本设备无输入报告缓存，返回长度 0（栈会 STALL）
uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type, uint8_t* buffer, uint16_t reqlen)
{
    (void)instance;
    (void)report_id;
    (void)report_type;
    (void)buffer;
    (void)reqlen;
    return 0;
}

// 主机 SET_REPORT（如键盘 LED 输出）：本设备无需处理
void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type, uint8_t const* buffer, uint16_t bufsize)
{
    (void)instance;
    (void)report_id;
    (void)report_type;
    (void)buffer;
    (void)bufsize;
}
