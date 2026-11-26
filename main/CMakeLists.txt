idf_component_register(SRCS "prueba_mp3.c" "oled.c" "temas.c" "control_ble.c"
    EMBED_TXTFILES "control.html"
    PRIV_REQUIRES esp_driver_uart esp_driver_gpio esp_driver_i2c
                  bt esp_event esp_http_server esp_netif esp_timer esp_wifi nvs_flash)
