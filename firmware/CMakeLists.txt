# SmartGhar Smart Switch — ESP32-C6 firmware
cmake_minimum_required(VERSION 3.16)

# Single source of truth for the version: firmware/VERSION → PROJECT_VER + the
# generated main/version_gen.h (FIRMWARE_VERSION). Bump VERSION, then reconfigure.
file(STRINGS "${CMAKE_CURRENT_LIST_DIR}/VERSION" SS_VERSION)
file(WRITE "${CMAKE_CURRENT_LIST_DIR}/main/version_gen.h"
     "// AUTO-GENERATED from firmware/VERSION — do not edit.\n#pragma once\n#define FIRMWARE_VERSION \"${SS_VERSION}\"\n")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(smartghar_smart_switch VERSION ${SS_VERSION})
