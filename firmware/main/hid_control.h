#pragma once
#include "esp_http_server.h"
void hid_control_register(httpd_handle_t server);
void hid_control_tick(void);
