function(ephoto_create_bootimg_partition_image partition image_file)
    set(options FLASH_IN_PROJECT)
    cmake_parse_arguments(arg "${options}" "" "" "${ARGN}")

    partition_table_get_partition_info(size "--partition-name ${partition}" "size")
    partition_table_get_partition_info(offset "--partition-name ${partition}" "offset")

    if("${size}" AND "${offset}")
        set(source_file "${CMAKE_CURRENT_LIST_DIR}/${image_file}")
        set(output_file "${CMAKE_BINARY_DIR}/${partition}.bin")
        set(script_file "${CMAKE_CURRENT_LIST_DIR}/cmake/prepare_bootimg.cmake")

        add_custom_command(
            OUTPUT "${output_file}"
            COMMAND ${CMAKE_COMMAND}
                    -DINPUT_FILE=${source_file}
                    -DOUTPUT_FILE=${output_file}
                    -DEXPECTED_SIZE=${size}
                    -P "${script_file}"
            DEPENDS "${source_file}" "${script_file}"
            VERBATIM)

        add_custom_target(${partition}_bin ALL DEPENDS "${output_file}")

        idf_component_get_property(main_args esptool_py FLASH_ARGS)
        idf_component_get_property(sub_args esptool_py FLASH_SUB_ARGS)
        esptool_py_flash_target(${partition}-flash "${main_args}" "${sub_args}" ALWAYS_PLAINTEXT)
        esptool_py_flash_to_partition(${partition}-flash "${partition}" "${output_file}")
        add_dependencies(${partition}-flash ${partition}_bin)

        if(arg_FLASH_IN_PROJECT)
            esptool_py_flash_to_partition(flash "${partition}" "${output_file}")
            add_dependencies(flash ${partition}_bin)
        endif()
    else()
        set(message "Failed to create boot image for partition '${partition}'. Check partition table configuration.")
        fail_at_build_time(${partition}_bin "${message}")
    endif()
endfunction()

ephoto_create_bootimg_partition_image(bootimg_a "assets/boot_open.rgb565" FLASH_IN_PROJECT)
