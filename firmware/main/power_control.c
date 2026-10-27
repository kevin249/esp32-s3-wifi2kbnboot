#include "power_control.h"
#include "sdkconfig.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include <stdbool.h>
#include <string.h>

typedef struct { int pin; uint32_t duration; } pulse_t;
static QueueHandle_t pulses;
static portMUX_TYPE guard = portMUX_INITIALIZER_UNLOCKED;
static bool busy;
#ifdef CONFIG_W2B_POWER_ACTIVE_HIGH
#define ACTIVE_LEVEL 1
#else
#define ACTIVE_LEVEL 0
#endif

static bool valid_pin(int pin) {
    /* 保守排除 USB、默认UART、启动配置及常见模块Flash/PSRAM引脚。 */
    return GPIO_IS_VALID_OUTPUT_GPIO(pin) && pin != 0 && pin != 3 && pin != 19 && pin != 20 &&
        !(pin >= 26 && pin <= 37) && pin != 43 && pin != 44 && pin != 45 && pin != 46;
}
static void pulse_task(void *argument) {
    (void)argument;
    pulse_t pulse;
    for (;;) {
        xQueueReceive(pulses, &pulse, portMAX_DELAY);
        ESP_ERROR_CHECK(gpio_set_level(pulse.pin, ACTIVE_LEVEL));
        vTaskDelay(pdMS_TO_TICKS(pulse.duration));
        ESP_ERROR_CHECK(gpio_set_level(pulse.pin, !ACTIVE_LEVEL));
        portENTER_CRITICAL(&guard); busy = false; portEXIT_CRITICAL(&guard);
    }
}
void power_control_init(void) {
    int pins[2] = {CONFIG_W2B_PWR_GPIO, CONFIG_W2B_RESET_GPIO};
    if (pins[0] >= 0 && pins[0] == pins[1]) {
        ESP_LOGE("主板按键", "PWR与RESET不能使用相同GPIO");
        ESP_ERROR_CHECK(ESP_ERR_INVALID_ARG);
    }
    for (unsigned i = 0; i < 2; ++i) {
        if (pins[i] < 0) continue;
        ESP_ERROR_CHECK(valid_pin(pins[i]) ? ESP_OK : ESP_ERR_INVALID_ARG);
        ESP_ERROR_CHECK(gpio_set_level(pins[i], !ACTIVE_LEVEL));
        gpio_config_t config = {.pin_bit_mask = UINT64_C(1) << pins[i],
#ifdef CONFIG_W2B_POWER_OPEN_DRAIN
            .mode = GPIO_MODE_OUTPUT_OD,
#else
            .mode = GPIO_MODE_OUTPUT,
#endif
            .pull_up_en = GPIO_PULLUP_DISABLE, .pull_down_en = GPIO_PULLDOWN_DISABLE, .intr_type = GPIO_INTR_DISABLE};
        ESP_ERROR_CHECK(gpio_config(&config));
    }
    pulses = xQueueCreate(1, sizeof(pulse_t));
    configASSERT(pulses);
    ESP_ERROR_CHECK(xTaskCreate(pulse_task, "主板按键", 2048, NULL, 3, NULL) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
}
esp_err_t power_control_submit(const char *button, uint32_t duration_ms) {
    int pin;
    if (!strcmp(button, "pwr")) pin = CONFIG_W2B_PWR_GPIO;
    else if (!strcmp(button, "reset")) pin = CONFIG_W2B_RESET_GPIO;
    else return ESP_ERR_INVALID_ARG;
    if (duration_ms < 50 || duration_ms > 8000) return ESP_ERR_INVALID_ARG;
    if (pin < 0) return ESP_ERR_INVALID_STATE;
    portENTER_CRITICAL(&guard);
    bool available = !busy;
    if (available) busy = true;
    portEXIT_CRITICAL(&guard);
    if (!available) return ESP_ERR_TIMEOUT;
    pulse_t pulse = {.pin = pin, .duration = duration_ms};
    if (xQueueSend(pulses, &pulse, 0) != pdTRUE) {
        portENTER_CRITICAL(&guard); busy = false; portEXIT_CRITICAL(&guard);
        return ESP_FAIL;
    }
    return ESP_OK;
}
