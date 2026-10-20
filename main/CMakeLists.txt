idf_component_register(SRCS "bridge.c" INCLUDE_DIRS "."
                    PRIV_REQUIRES esp_wifi esp_http_server nvs_flash esp_netif esp_event esp_timer driver)
