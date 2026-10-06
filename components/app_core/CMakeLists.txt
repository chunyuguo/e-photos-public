idf_component_register(SRCS "app_controller.c"
                       INCLUDE_DIRS "include"
                       REQUIRES ephoto_common
                                bsp_ephoto_p4c6
                                display_jd9365
                                services
                                freertos
                                esp_timer)
