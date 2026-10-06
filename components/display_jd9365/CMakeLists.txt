idf_component_register(SRCS "display_service.c" "lvgl_backend.c" "ephoto_ui_font_18.c" "ephoto_ui_font_27.c" "ephoto_ui_font_36.c" "ephoto_clock_num_81.c"
                       INCLUDE_DIRS "include"
                       REQUIRES ephoto_common
                                bsp_ephoto_p4c6
                                services
                                esp_lcd
                                esp_lcd_jd9365
                                esp_driver_jpeg
                                esp_driver_gpio
                                esp_hw_support
                                esp_timer
                                spi_flash
                                lvgl__lvgl
                                espressif__esp_lvgl_port
                                espressif__esp_jpeg
                                espressif__esp_new_jpeg
                                espressif__libjpeg-turbo
                                espressif__libpng
                       EMBED_FILES "assets/miniapp_qr_112.rgb565")

idf_component_get_property(esp_jpeg_dir espressif__esp_jpeg COMPONENT_DIR)
target_include_directories(${COMPONENT_LIB} PRIVATE "${esp_jpeg_dir}/tjpgd")
target_compile_definitions(${COMPONENT_LIB} PRIVATE EPHOTO_PROJECT_VER="${PROJECT_VER}")
