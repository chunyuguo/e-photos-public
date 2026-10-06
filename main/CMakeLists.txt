if(EPHOTO_SD_BENCH_APP)
    set(EPHOTO_MAIN_SRC "sd_bench_main.c")
else()
    set(EPHOTO_MAIN_SRC "main.c")
endif()

idf_component_register(SRCS "${EPHOTO_MAIN_SRC}"
                       INCLUDE_DIRS "."
                       REQUIRES app_core
                                bsp_ephoto_p4c6
                                display_jd9365
                                services)
