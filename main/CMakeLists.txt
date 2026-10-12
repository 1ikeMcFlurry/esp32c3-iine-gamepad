idf_component_register(SRCS "main.c" "gamepad_hid.c" "hid_input.c"
                           "iine_protocol.c" "iine_link.c"
                           "gamepad_ui.c" "gamepad_render.c" "gamepad_screen.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES bt nvs_flash esp_lcd esp_driver_spi esp_driver_ledc esp_adc esp_timer)
