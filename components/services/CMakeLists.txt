set(EPHOTO_HOSTED_EMBEDDED_FW)
if(EPHOTO_ENABLE_EMBEDDED_HOSTED_FW AND CONFIG_SLAVE_IDF_TARGET_ESP32C5 AND EXISTS "${CMAKE_CURRENT_LIST_DIR}/assets/ESP32C5_2.12.9_0X0.bin")
    set(EPHOTO_HOSTED_EMBEDDED_FW "assets/ESP32C5_2.12.9_0X0.bin")
elseif(EPHOTO_ENABLE_EMBEDDED_HOSTED_FW AND CONFIG_SLAVE_IDF_TARGET_ESP32C6 AND EXISTS "${CMAKE_CURRENT_LIST_DIR}/assets/ESP32C6_2.12.9_0X0.bin")
    set(EPHOTO_HOSTED_EMBEDDED_FW "assets/ESP32C6_2.12.9_0X0.bin")
endif()

set(EPHOTO_EMBEDDED_FW_ARGS)
if(EPHOTO_HOSTED_EMBEDDED_FW)
    list(APPEND EPHOTO_EMBEDDED_FW_ARGS EMBED_FILES "${EPHOTO_HOSTED_EMBEDDED_FW}")
endif()

idf_component_register(
    SRCS
        "settings_service.c"
        "boot_image_service.c"
        "factory_reset_service.c"
        "lan_discovery_service.c"
        "ambient_light_service.c"
        "storage_service.c"
        "gallery_service.c"
        "notification_service.c"
        "ota_service.c"
        "wifi_admin.c"
        "ble_provisioning.c"
        "hosted_service.c"
        "clock_service.c"
        "input_service.c"
        "web_api.c"
    INCLUDE_DIRS "include"
    REQUIRES
        ephoto_common
        bsp_ephoto_p4c6
        nvs_flash
        esp_http_server
        esp_http_client
        esp_netif
        esp_wifi
        esp_timer
        esp_event
        esp_adc
        bt
        app_update
        esp_https_ota
        espressif__mdns
        espressif__esp_hosted
        fatfs
        esp_driver_sdmmc
        json
    PRIV_REQUIRES
        display_jd9365
        mbedtls
    EMBED_TXTFILES
        "webui/index.html"
        "webui/style.css"
    ${EPHOTO_EMBEDDED_FW_ARGS})

target_compile_definitions(${COMPONENT_LIB} PRIVATE EPHOTO_PROJECT_VER="${PROJECT_VER}")
target_compile_definitions(${COMPONENT_LIB} PRIVATE EPHOTO_OTA_SERVER_URL="${EPHOTO_OTA_SERVER_URL}")
target_compile_definitions(${COMPONENT_LIB} PRIVATE EPHOTO_OTA_PRODUCT_KEY="${EPHOTO_OTA_PRODUCT_KEY}")
target_compile_definitions(${COMPONENT_LIB} PRIVATE EPHOTO_OTA_DEVICE_TYPE_KEY="${EPHOTO_OTA_DEVICE_TYPE_KEY}")
target_compile_definitions(${COMPONENT_LIB} PRIVATE EPHOTO_OTA_CHANNEL_KEY="${EPHOTO_OTA_CHANNEL_KEY}")
target_compile_definitions(${COMPONENT_LIB} PRIVATE EPHOTO_HW_MODEL="${EPHOTO_HW_MODEL}")
target_compile_definitions(${COMPONENT_LIB} PRIVATE EPHOTO_DISCOVERY_MODEL="${EPHOTO_DISCOVERY_MODEL}")
target_compile_definitions(${COMPONENT_LIB} PRIVATE EPHOTO_COPROCESSOR="${EPHOTO_COPROCESSOR}")
if(EPHOTO_ENABLE_OTA)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE EPHOTO_ENABLE_OTA=1)
else()
    target_compile_definitions(${COMPONENT_LIB} PRIVATE EPHOTO_ENABLE_OTA=0)
endif()
if(EPHOTO_HOSTED_EMBEDDED_FW)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE EPHOTO_HAS_EMBEDDED_HOSTED_FW=1)
else()
    target_compile_definitions(${COMPONENT_LIB} PRIVATE EPHOTO_HAS_EMBEDDED_HOSTED_FW=0)
endif()
