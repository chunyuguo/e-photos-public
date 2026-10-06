cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/components")

file(READ "${CMAKE_CURRENT_LIST_DIR}/VERSION" PROJECT_VER_RAW)
string(STRIP "${PROJECT_VER_RAW}" PROJECT_VER)

set(EPHOTO_COPROCESSOR "C5" CACHE STRING "Hosted co-processor target (C5 or C6)")
set_property(CACHE EPHOTO_COPROCESSOR PROPERTY STRINGS C5 C6)
string(TOUPPER "${EPHOTO_COPROCESSOR}" EPHOTO_COPROCESSOR)

set(EPHOTO_HW_REVISION "v1.3" CACHE STRING "ESP32-P4 hardware revision family (v1.3 or v3.2)")
set_property(CACHE EPHOTO_HW_REVISION PROPERTY STRINGS v1.3 v3.2)

# libjpeg-turbo 3.1.x refers to this legacy target alias while configuring
# its external project.  Define the compatibility interface before ESP-IDF
# discovers managed components; newer component revisions may provide it.
if(NOT TARGET idf::libjpeg-turbo)
    add_library(idf::libjpeg-turbo INTERFACE IMPORTED GLOBAL)
endif()

if(NOT EPHOTO_HW_REVISION STREQUAL "v1.3" AND NOT EPHOTO_HW_REVISION STREQUAL "v3.2")
    message(FATAL_ERROR "Unsupported EPHOTO_HW_REVISION='${EPHOTO_HW_REVISION}', expected v1.3 or v3.2")
endif()

if(EPHOTO_COPROCESSOR STREQUAL "C5")
    set(EPHOTO_OTA_DEVICE_TYPE_KEY_DEFAULT "ESP32P4C5_${EPHOTO_HW_REVISION}")
    set(EPHOTO_HW_MODEL "esp32p4c5_${EPHOTO_HW_REVISION}")
    set(EPHOTO_DISCOVERY_MODEL "ESP32P4C5_${EPHOTO_HW_REVISION}")
elseif(EPHOTO_COPROCESSOR STREQUAL "C6")
    set(EPHOTO_OTA_DEVICE_TYPE_KEY_DEFAULT "ESP32P4C6_${EPHOTO_HW_REVISION}")
    set(EPHOTO_HW_MODEL "esp32p4c6_${EPHOTO_HW_REVISION}")
    set(EPHOTO_DISCOVERY_MODEL "ESP32P4C6_${EPHOTO_HW_REVISION}")
else()
    message(FATAL_ERROR "Unsupported EPHOTO_COPROCESSOR='${EPHOTO_COPROCESSOR}', expected C5 or C6")
endif()

set(EPHOTO_OTA_SERVER_URL "" CACHE STRING "OTA server base URL (set this in a private build)" )
set(EPHOTO_OTA_PRODUCT_KEY "e-photo" CACHE STRING "OTA product key" FORCE)
set(EPHOTO_OTA_DEVICE_TYPE_KEY "${EPHOTO_OTA_DEVICE_TYPE_KEY_DEFAULT}" CACHE STRING "OTA device type key" FORCE)
set(EPHOTO_OTA_CHANNEL_KEY "stable" CACHE STRING "OTA default channel key" FORCE)
set(EPHOTO_HW_MODEL "${EPHOTO_HW_MODEL}" CACHE INTERNAL "BLE onboarding hardware model")
set(EPHOTO_DISCOVERY_MODEL "${EPHOTO_DISCOVERY_MODEL}" CACHE INTERNAL "LAN discovery model")
set(EPHOTO_COPROCESSOR "${EPHOTO_COPROCESSOR}" CACHE INTERNAL "Hosted co-processor target")
set(EPHOTO_HW_REVISION "${EPHOTO_HW_REVISION}" CACHE INTERNAL "ESP32-P4 hardware revision family")

# The public source distribution does not include a vendor coprocessor image.
# A private build may point this at a reviewed, separately supplied image.
option(EPHOTO_ENABLE_EMBEDDED_HOSTED_FW "Embed a separately supplied coprocessor firmware image" OFF)
option(EPHOTO_ENABLE_OTA "Enable network OTA and rollback handling" OFF)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(e_photo)

idf_build_set_property(COMPILE_OPTIONS "-g0" APPEND)
idf_build_set_property(C_COMPILE_OPTIONS "-g0" APPEND)

idf_component_get_property(lwip_component lwip COMPONENT_LIB)
if(lwip_component)
    target_compile_options(${lwip_component} PRIVATE "-O0")
endif()

idf_component_get_property(parlio_component esp_driver_parlio COMPONENT_LIB)
if(parlio_component)
    target_compile_options(${parlio_component} PRIVATE "-O0")
endif()
