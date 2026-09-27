idf_component_register(SRCS "main.c" "amp.c" "speaker.c" "speaker_gap.c" "speaker_avrcp.c"
    "speaker_nvs.c" INCLUDE_DIRS "."
    REQUIRES eaf nvs_flash esp_driver_gpio esp_driver_i2c)
