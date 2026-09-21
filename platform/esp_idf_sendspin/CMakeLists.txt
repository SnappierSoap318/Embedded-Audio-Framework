# Standalone ESP-IDF project for the EAF Sendspin-class board smoke target.
# The repository root CMakeLists.txt is not used here; components under
# components/ compile the portable EAF sources plus the ESP-IDF HAL adapters.
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(eaf_esp_idf_sendspin)
