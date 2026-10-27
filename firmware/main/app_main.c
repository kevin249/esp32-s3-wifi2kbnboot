#include "storage.h"
#include "hid_control.h"
#include "sdkconfig.h"
#include "tusb.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_private/usb_phy.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include <string.h>

static EventGroupHandle_t wifi_events;
static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg;
    if (base == WIFI_EVENT && (id == WIFI_EVENT_STA_START || id == WIFI_EVENT_STA_DISCONNECTED)) {
        xEventGroupClearBits(wifi_events, BIT0);
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = data;
        ESP_LOGI("网络", "设备IP=" IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(wifi_events, BIT0);
    }
}
static void usb_task(void *argument) {
    (void)argument;
    /* 首次枚举就必须有正确容量，否则部分 BIOS 不再重试空介质。 */
    while (!storage_ready()) {
        storage_tick();
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    usb_phy_handle_t phy;
    usb_phy_config_t config = {
        .controller = USB_PHY_CTRL_OTG, .target = USB_PHY_TARGET_INT,
        .otg_mode = USB_OTG_MODE_DEVICE, .otg_speed = USB_PHY_SPEED_UNDEFINED,
    };
    ESP_ERROR_CHECK(usb_new_phy(&config, &phy));
    const tusb_rhport_init_t init = {.role = TUSB_ROLE_DEVICE, .speed = TUSB_SPEED_FULL};
    ESP_ERROR_CHECK(tusb_init(0, &init) ? ESP_OK : ESP_FAIL);
    /* 初始化和所有 TinyUSB API 均在此任务调用。 */
    for (;;) {
        tud_task_ext(1, false);
        storage_tick();
        hid_control_tick();
    }
}
void app_main(void) {
    configASSERT(strlen(CONFIG_W2B_TOKEN) >= 16 && strlen(CONFIG_W2B_TOKEN) <= 240);
    for (const unsigned char *p = (const unsigned char *)CONFIG_W2B_TOKEN; *p; ++p)
        configASSERT(*p > 32 && *p < 127);
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    wifi_events = xEventGroupCreate();
    configASSERT(wifi_events);
    wifi_init_config_t wifi_init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wifi_init));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event, NULL));
    wifi_config_t wifi = {0};
    configASSERT(strlen(CONFIG_W2B_WIFI_SSID) < sizeof(wifi.sta.ssid));
    configASSERT(strlen(CONFIG_W2B_WIFI_PASSWORD) < sizeof(wifi.sta.password));
    memcpy(wifi.sta.ssid, CONFIG_W2B_WIFI_SSID, strlen(CONFIG_W2B_WIFI_SSID));
    memcpy(wifi.sta.password, CONFIG_W2B_WIFI_PASSWORD, strlen(CONFIG_W2B_WIFI_PASSWORD));
    wifi.sta.scan_method = WIFI_FAST_SCAN;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    xEventGroupWaitBits(wifi_events, BIT0, pdFALSE, pdTRUE, portMAX_DELAY);
    storage_init();
    hid_control_init();
    ESP_ERROR_CHECK(xTaskCreate(usb_task, "USB设备", 6144, NULL, 5, NULL) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
}
