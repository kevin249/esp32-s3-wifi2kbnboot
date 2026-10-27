idf_component_register(
    SRCS "app_main.c" "block_cache.c" "storage.c" "usb_descriptors.c" "hid_control.c"
    INCLUDE_DIRS "."
    REQUIRES tinyusb_port esp_wifi esp_event esp_netif nvs_flash esp_http_client
             esp_http_server json esp_timer esp_hw_support usb)
