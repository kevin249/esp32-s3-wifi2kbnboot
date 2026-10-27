#include "tusb.h"
#include "esp_mac.h"
#include <stdio.h>
#include <string.h>

/* 示例 VID/PID 仅供开发，产品请申请自己的标识。 */
static const tusb_desc_device_t device = {
    .bLength = sizeof(tusb_desc_device_t), .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200, .bMaxPacketSize0 = 64,
    .idVendor = 0xCAFE, .idProduct = 0x4021, .bcdDevice = 0x0100,
    .iManufacturer = 1, .iProduct = 2, .iSerialNumber = 3, .bNumConfigurations = 1,
};
static const uint8_t keyboard_report[] = { TUD_HID_REPORT_DESC_KEYBOARD() };
static const uint8_t mouse_report[] = { TUD_HID_REPORT_DESC_MOUSE() };
#define CONFIG_LENGTH (TUD_CONFIG_DESC_LEN + 2 * TUD_HID_DESC_LEN + TUD_MSC_DESC_LEN)
static const uint8_t configuration[] = {
    TUD_CONFIG_DESCRIPTOR(1, 3, 0, CONFIG_LENGTH, 0, 250),
    TUD_HID_DESCRIPTOR(0, 0, HID_ITF_PROTOCOL_KEYBOARD, sizeof(keyboard_report), 0x81, 16, 5),
    TUD_HID_DESCRIPTOR(1, 0, HID_ITF_PROTOCOL_MOUSE, sizeof(mouse_report), 0x82, 16, 5),
    TUD_MSC_DESCRIPTOR(2, 0, 0x03, 0x83, 64),
};
uint8_t const *tud_descriptor_device_cb(void) { return (const uint8_t *)&device; }
uint8_t const *tud_descriptor_configuration_cb(uint8_t index) { (void)index; return configuration; }
uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance) {
    return instance == 0 ? keyboard_report : mouse_report;
}
uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t language) {
    (void)language;
    static uint16_t output[40];
    static char serial[13];
    const char *text = NULL;
    if (index == 0) { output[0] = (TUSB_DESC_STRING << 8) | 4; output[1] = 0x0409; return output; }
    if (index == 1) text = "W2B";
    else if (index == 2) text = "WiFi Keyboard Mouse Boot";
    else if (index == 3) {
        uint8_t mac[6];
        if (esp_read_mac(mac, ESP_MAC_WIFI_STA) != ESP_OK) return NULL;
        snprintf(serial, sizeof(serial), "%02X%02X%02X%02X%02X%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        text = serial;
    } else return NULL;
    size_t length = strlen(text);
    if (length > 39) length = 39;
    for (size_t i = 0; i < length; ++i) output[i + 1] = (uint8_t)text[i];
    output[0] = (TUSB_DESC_STRING << 8) | (2 * length + 2);
    return output;
}
uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t id, hid_report_type_t type, uint8_t *buffer, uint16_t length) {
    (void)instance; (void)id; (void)type; (void)buffer; (void)length;
    return 0;
}
void tud_hid_set_report_cb(uint8_t instance, uint8_t id, hid_report_type_t type, uint8_t const *buffer, uint16_t length) {
    /* 本示例不回传 CapsLock 等指示灯状态。 */
    (void)instance; (void)id; (void)type; (void)buffer; (void)length;
}
