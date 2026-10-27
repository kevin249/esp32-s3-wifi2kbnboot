#pragma once
#include "esp_err.h"
#include <stdint.h>
void power_control_init(void);
esp_err_t power_control_submit(const char *button, uint32_t duration_ms);
