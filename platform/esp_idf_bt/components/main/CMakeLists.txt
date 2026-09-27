idf_component_register(SRCS "main.c" "amp.c" "speaker.c" INCLUDE_DIRS "."
    REQUIRES eaf nvs_flash esp_driver_gpio esp_driver_i2c)
