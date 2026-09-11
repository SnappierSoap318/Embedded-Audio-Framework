cmake_minimum_required(VERSION 3.20.0)
list(APPEND ZEPHYR_EXTRA_MODULES ${CMAKE_CURRENT_LIST_DIR}/../..)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(eaf_smoke)
target_sources(app PRIVATE src/main.c src/mock_i2s.c)
target_sources_ifdef(CONFIG_EAF_BT_SBC app PRIVATE src/sbc_smoke.c)
target_sources_ifndef(CONFIG_EAF_BT_SBC app PRIVATE src/sbc_smoke_null.c)

# Test the A2DP callback boundary without enabling a host/controller. Never mix
# the registration mocks with the real Bluetooth stack.
if(CONFIG_BT OR CONFIG_EAF_BT_A2DP)
  message(FATAL_ERROR "Use a board application for Bluetooth; this sample mocks A2DP registration")
endif()
target_sources(app PRIVATE src/bt_smoke.c ../../hal/zephyr/bt_a2dp_zephyr.c)
set_source_files_properties(src/bt_smoke.c ../../hal/zephyr/bt_a2dp_zephyr.c
  PROPERTIES COMPILE_DEFINITIONS "CONFIG_BT_A2DP_SINK=1;CONFIG_BT_BUF_EVT_RX_COUNT=4;CONFIG_BT_BUF_ACL_TX_COUNT=3")
