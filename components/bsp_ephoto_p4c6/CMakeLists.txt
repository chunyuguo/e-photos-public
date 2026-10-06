idf_component_register(SRCS "board_profile.c" "bsp_ephoto.c"
                       INCLUDE_DIRS "include"
                       REQUIRES ephoto_common esp_driver_gpio esp_driver_sdmmc)
