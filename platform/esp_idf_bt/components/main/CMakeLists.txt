idf_component_register(SRCS "main.c" "amp.c" "speaker.c" "speaker_gap.c" "speaker_avrcp.c"
    "speaker_nvs.c" "${CMAKE_CURRENT_LIST_DIR}/../../../../platform/esp32_common/tas5805m.c"
    INCLUDE_DIRS "." "${CMAKE_CURRENT_LIST_DIR}/../../../../platform/esp32_common"
    REQUIRES eaf nvs_flash esp_driver_gpio esp_driver_i2c)
