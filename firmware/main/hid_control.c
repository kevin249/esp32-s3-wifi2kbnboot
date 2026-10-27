#include "hid_control.h"
#include "power_control.h"
#include "sdkconfig.h"
#include "tusb.h"
#include "cJSON.h"
#include "esp_http_server.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    uint8_t instance, modifiers, keys[6], buttons;
    int8_t x, y, wheel;
    int64_t created_us;
} hid_command_t;
static QueueHandle_t commands;
static bool waiting;
static hid_command_t next;
static int64_t last_keyboard, last_mouse;
static bool keyboard_held, mouse_held;

static bool mouse_report(uint8_t buttons, int8_t x, int8_t y, int8_t wheel) {
    if (tud_hid_n_get_protocol(1) == HID_PROTOCOL_BOOT) {
        /* BIOS 的 Boot Mouse 协议只有按钮、X、Y 三字节。 */
        uint8_t report[3] = {buttons & 7, (uint8_t)x, (uint8_t)y};
        return tud_hid_n_report(1, 0, report, sizeof(report));
    }
    return tud_hid_n_mouse_report(1, 0, buttons, x, y, wheel, 0);
}

static bool integer(cJSON *root, const char *name, int min, int max, int *out) {
    cJSON *item = cJSON_GetObjectItemCaseSensitive(root, name);
    if (!cJSON_IsNumber(item) || !isfinite(item->valuedouble) ||
        item->valuedouble < min || item->valuedouble > max || floor(item->valuedouble) != item->valuedouble) return false;
    *out = item->valueint;
    return true;
}
static esp_err_t handle_hid(httpd_req_t *request) {
    char auth[256], expected[256], text[384];
    snprintf(expected, sizeof(expected), "Bearer %s", CONFIG_W2B_TOKEN);
    if (httpd_req_get_hdr_value_str(request, "Authorization", auth, sizeof(auth)) != ESP_OK || strcmp(auth, expected))
        return httpd_resp_send_err(request, HTTPD_401_UNAUTHORIZED, "访问令牌无效");
    if (request->content_len <= 0 || request->content_len >= sizeof(text))
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "请求长度无效");
    size_t used = 0;
    while (used < request->content_len) {
        int n = httpd_req_recv(request, text + used, request->content_len - used);
        if (n <= 0) return ESP_FAIL;
        used += n;
    }
    text[used] = 0;
    cJSON *root = cJSON_Parse(text);
    hid_command_t command = {.created_us = esp_timer_get_time()};
    bool ok = root != NULL;
    int value = 0;
    if (!strcmp(request->uri, "/control/power")) {
        cJSON *button = cJSON_GetObjectItemCaseSensitive(root, "button");
        int duration = 200;
        bool valid = root && cJSON_IsString(button) &&
            (!cJSON_HasObjectItem(root, "duration_ms") || integer(root, "duration_ms", 50, 8000, &duration));
        esp_err_t result = valid ? power_control_submit(button->valuestring, duration) : ESP_ERR_INVALID_ARG;
        cJSON_Delete(root);
        httpd_resp_set_type(request, "application/json");
        if (result != ESP_OK) {
            httpd_resp_set_status(request, result == ESP_ERR_INVALID_ARG ? "400 Bad Request" : "409 Conflict");
            return httpd_resp_sendstr(request, "{\"error\":\"GPIO未配置、脉冲执行中或参数无效\"}");
        }
        httpd_resp_set_status(request, "202 Accepted");
        return httpd_resp_sendstr(request, "{\"queued\":true}");
    } else if (!strcmp(request->uri, "/hid/keyboard")) {
        ok = ok && integer(root, "modifiers", 0, 255, &value);
        command.modifiers = value;
        cJSON *keys = cJSON_GetObjectItemCaseSensitive(root, "keys");
        ok = ok && cJSON_IsArray(keys) && cJSON_GetArraySize(keys) <= 6;
        if (ok) for (int i = 0; i < cJSON_GetArraySize(keys); ++i) {
            cJSON *key = cJSON_GetArrayItem(keys, i);
            if (!cJSON_IsNumber(key) || !isfinite(key->valuedouble) || key->valuedouble < 0 ||
                key->valuedouble > 255 || floor(key->valuedouble) != key->valuedouble) { ok = false; break; }
            command.keys[i] = key->valueint;
        }
    } else {
        command.instance = 1;
        ok = ok && integer(root, "buttons", 0, 31, &value); command.buttons = value;
        ok = ok && integer(root, "x", -127, 127, &value); command.x = value;
        ok = ok && integer(root, "y", -127, 127, &value); command.y = value;
        ok = ok && integer(root, "wheel", -127, 127, &value); command.wheel = value;
    }
    cJSON_Delete(root);
    if (!ok) return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "键鼠参数无效");
    if (xQueueSend(commands, &command, 0) != pdTRUE) {
        httpd_resp_set_status(request, "503 Service Unavailable");
        return httpd_resp_sendstr(request, "指令队列已满");
    }
    httpd_resp_set_status(request, "202 Accepted");
    httpd_resp_set_type(request, "application/json");
    return httpd_resp_sendstr(request, "{\"queued\":true}");
}
void hid_control_init(void) {
    commands = xQueueCreate(16, sizeof(hid_command_t));
    configASSERT(commands);
    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 6144;
    ESP_ERROR_CHECK(httpd_start(&server, &config));
    httpd_uri_t keyboard = {.uri = "/hid/keyboard", .method = HTTP_POST, .handler = handle_hid};
    httpd_uri_t mouse = {.uri = "/hid/mouse", .method = HTTP_POST, .handler = handle_hid};
    httpd_uri_t power = {.uri = "/control/power", .method = HTTP_POST, .handler = handle_hid};
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &keyboard));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &mouse));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &power));
}
void hid_control_tick(void) {
    int64_t now = esp_timer_get_time();
    int64_t expiry = (int64_t)CONFIG_W2B_HID_RELEASE_MS * 1000;
    if (!tud_mounted()) {
        waiting = keyboard_held = mouse_held = false;
        xQueueReset(commands);
        return;
    }
    uint8_t empty[6] = {0};
    if (keyboard_held && now - last_keyboard > expiry && tud_hid_n_ready(0))
        if (tud_hid_n_keyboard_report(0, 0, 0, empty)) keyboard_held = false;
    if (mouse_held && now - last_mouse > expiry && tud_hid_n_ready(1))
        if (mouse_report(0, 0, 0, 0)) mouse_held = false;
    if (!waiting) waiting = xQueueReceive(commands, &next, 0) == pdTRUE;
    if (!waiting) return;
    if (now - next.created_us > expiry) { waiting = false; return; }
    if (!tud_hid_n_ready(next.instance)) return;
    if (next.instance == 0) {
        if (tud_hid_n_keyboard_report(0, 0, next.modifiers, next.keys)) {
            last_keyboard = now;
            keyboard_held = next.modifiers || memcmp(next.keys, empty, 6);
            waiting = false;
        }
    } else if (mouse_report(next.buttons, next.x, next.y, next.wheel)) {
        last_mouse = now;
        mouse_held = next.buttons != 0;
        waiting = false;
    }
}
