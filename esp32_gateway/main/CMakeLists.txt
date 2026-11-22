idf_component_register(SRCS "main.c"
                    PRIV_REQUIRES esp_wifi nvs_flash protocol_examples_common esp_driver_uart esp_driver_gpio mqtt
                    INCLUDE_DIRS ".")
