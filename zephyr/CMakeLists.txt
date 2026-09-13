if(CONFIG_EAF)
  zephyr_library_named(eaf)
  # Keep strict diagnostics on EAF code without applying them to upstream inline headers.
  target_include_directories(eaf SYSTEM PRIVATE ${ZEPHYR_BASE}/include)
  if(CONFIG_EAF_BT_SBC)
    include(${CMAKE_CURRENT_LIST_DIR}/../cmake/sbc_fixups.cmake)
    # libsbc is registered later in Zephyr's module traversal.
    cmake_language(EVAL CODE "cmake_language(DEFER DIRECTORY \"${CMAKE_SOURCE_DIR}\" CALL eaf_sbc_fixups libsbc \"${ZEPHYR_LIBSBC_MODULE_DIR}\")")
    target_include_directories(eaf SYSTEM PUBLIC
      ${ZEPHYR_LIBSBC_MODULE_DIR}/decoder/include ${ZEPHYR_LIBSBC_MODULE_DIR}/decoder/srce)
  endif()
  zephyr_library_include_directories(${CMAKE_CURRENT_LIST_DIR}/../include)
  zephyr_include_directories(${CMAKE_CURRENT_LIST_DIR}/../include)
  zephyr_library_sources(
    ../core/eaf_types.c ../core/eaf_reservoir.c ../core/eaf_graph.c
    ../core/eaf_control.c ../core/eaf_dsp.c
    ../hal/common/hal_atomic_c11.c ../hal/common/sink_null.c
    ../hal/zephyr/hal_os_zephyr.c ../apps/player/eaf_player.c
    ../apps/decoders/dec_wav.c)
  zephyr_library_sources_ifdef(CONFIG_EAF_LMS ../apps/lms/lms_protocol.c)
  zephyr_library_sources_ifdef(CONFIG_EAF_LMS_CLIENT ../apps/lms/lms_client.c ../hal/zephyr/hal_tcp_zephyr.c)
  if(CONFIG_EAF_LMS_CLIENT)
    zephyr_compile_definitions(EAF_LMS_INGRESS_BYTES=${CONFIG_EAF_LMS_INGRESS_BYTES})
  endif()
  zephyr_library_sources_ifdef(CONFIG_EAF_BT_A2DP ../hal/zephyr/bt_a2dp_zephyr.c)
  zephyr_library_sources_ifdef(CONFIG_EAF_BT_SBC ../apps/decoders/sbc_oi.c)
  zephyr_library_sources_ifdef(CONFIG_EAF_I2S ../hal/zephyr/sink_i2s_zephyr.c)
  zephyr_library_sources_ifdef(CONFIG_EAF_BT_INGRESS ../apps/bt/bt_ingress.c ../apps/bt/bt_decoder.c)
  zephyr_library_compile_options(-Wall -Wextra -Werror -Wconversion -Wstrict-prototypes -Wshadow -Wdouble-promotion)
endif()
