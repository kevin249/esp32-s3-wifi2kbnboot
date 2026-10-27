idf_component_register(
    SRCS "app_main.c" "block_cache.c" "media_rules.c" "sd_media.c" "storage.c"
         "usb_descriptors.c" "hid_control.c" "power_control.c" "web_server.c"
    INCLUDE_DIRS "."
    REQUIRES tinyusb_port esp_wifi esp_event esp_netif nvs_flash fatfs sdmmc esp_driver_sdmmc
             esp_http_server json esp_timer esp_hw_support usb esp_driver_gpio
    EMBED_FILES "index.html"
    WHOLE_ARCHIVE)
# 描述符和 TinyUSB 回调没有应用侧直接引用，必须保留在最终链接中。
