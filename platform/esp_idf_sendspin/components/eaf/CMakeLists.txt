# EAF portable core plus the ESP-IDF HAL adapters (PCM and FLAC decoders only).
# Sources stay in the repository tree; this component only selects them.
set(EAF_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../..")

idf_component_register(
  SRCS
    "${EAF_ROOT}/core/eaf_types.c"
    "${EAF_ROOT}/core/eaf_dec_input.c"
    "${EAF_ROOT}/core/eaf_decoder.c"
    "${EAF_ROOT}/hal/common/hal_atomic_c11.c"
    "${EAF_ROOT}/hal/esp_idf/hal_os_esp_idf.c"
    "${EAF_ROOT}/hal/esp_idf/hal_tcp_esp_idf.c"
    "${EAF_ROOT}/hal/esp_idf/sink_i2s_esp_idf.c"
    "${EAF_ROOT}/apps/decoders/dec_pcm.c"
    "${EAF_ROOT}/apps/decoders/dec_flac.c"
    "${EAF_ROOT}/third_party/impl/dr_flac_impl.c"
  INCLUDE_DIRS
    "${EAF_ROOT}/include"
    "${EAF_ROOT}/hal/esp_idf"
  PRIV_INCLUDE_DIRS
    "${EAF_ROOT}/third_party/dr_libs"
  REQUIRES
    esp_driver_i2s
    esp_driver_gpio
    esp_timer
    lwip)

# Xtensa's int32_t is long while dr_flac declares its PCM type as int; the
# pointer types differ although both are 32-bit. The adapter is owned by another
# workstream, so downgrade that one diagnostic for this translation unit.
set_source_files_properties("${EAF_ROOT}/apps/decoders/dec_flac.c"
                           PROPERTIES COMPILE_OPTIONS "-Wno-incompatible-pointer-types")
