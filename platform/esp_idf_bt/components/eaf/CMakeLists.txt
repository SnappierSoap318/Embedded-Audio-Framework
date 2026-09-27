# EAF portable core plus the ESP-IDF HAL adapters needed by the Bluetooth A2DP
# sink: the Bluedroid A2DP binding, the portable SBC ingress/decoder and the
# vendored OI/libsbc decoder. Sources stay in the repository tree.
set(EAF_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../..")

idf_component_register(
  SRCS
    "${EAF_ROOT}/core/eaf_types.c"
    "${EAF_ROOT}/core/eaf_reservoir.c"
    "${EAF_ROOT}/apps/bt/bt_ingress.c"
    "${EAF_ROOT}/apps/bt/bt_decoder.c"
    "${EAF_ROOT}/apps/bt/bt_volume.c"
    "${EAF_ROOT}/apps/decoders/sbc_oi.c"
    "${EAF_ROOT}/hal/common/hal_atomic_c11.c"
    "${EAF_ROOT}/hal/esp_idf/hal_os_esp_idf.c"
    "${EAF_ROOT}/hal/esp_idf/sink_i2s_esp_idf.c"
    "${EAF_ROOT}/hal/esp_idf/bt_a2dp_esp_idf.c"
  INCLUDE_DIRS
    "${EAF_ROOT}/include"
    "${EAF_ROOT}/hal/esp_idf"
  REQUIRES
    esp_driver_i2s
    esp_driver_gpio
    esp_timer
    bt)

# Vendored OI/libsbc decoder (Apache-2.0). Compiled as a separate static library
# with vendor warnings disabled; cmake/sbc_fixups.cmake generates the patched
# synthesis copies in the component build directory, leaving the sources alone.
set(EAF_SBC_ROOT "${EAF_ROOT}/third_party/libsbc")
set(EAF_SBC_FILES
    alloc bitalloc bitalloc-sbc bitstream-decode decoder-oina decoder-private
    decoder-sbc dequant framing framing-sbc oi_codec_version synthesis-8-generated
    synthesis-dct8 synthesis-sbc)
add_library(eaf_sbc_vendor STATIC)
foreach(file IN LISTS EAF_SBC_FILES)
  target_sources(eaf_sbc_vendor PRIVATE "${EAF_SBC_ROOT}/decoder/srce/${file}.c")
endforeach()
include("${EAF_ROOT}/cmake/sbc_fixups.cmake")
eaf_sbc_fixups(eaf_sbc_vendor "${EAF_SBC_ROOT}")
target_compile_options(eaf_sbc_vendor PRIVATE -w)
target_include_directories(eaf_sbc_vendor SYSTEM PUBLIC
  "${EAF_SBC_ROOT}/decoder/include" "${EAF_SBC_ROOT}/decoder/srce")
target_link_libraries(${COMPONENT_LIB} PUBLIC eaf_sbc_vendor)
