#pragma once
#include <stdint.h>
#include "esp_err.h"
void power_control_init(void);
esp_err_t power_control_submit(const char *button, uint32_t duration_ms);
