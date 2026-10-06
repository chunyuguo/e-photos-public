if(NOT DEFINED INPUT_FILE OR NOT DEFINED OUTPUT_FILE OR NOT DEFINED EXPECTED_SIZE)
    message(FATAL_ERROR "prepare_bootimg.cmake requires INPUT_FILE, OUTPUT_FILE and EXPECTED_SIZE")
endif()

if(NOT EXISTS "${INPUT_FILE}")
    message(FATAL_ERROR "boot image source not found: ${INPUT_FILE}")
endif()

file(SIZE "${INPUT_FILE}" INPUT_SIZE)
if(NOT INPUT_SIZE EQUAL EXPECTED_SIZE)
    message(FATAL_ERROR
            "boot image size mismatch: got ${INPUT_SIZE} bytes, expected ${EXPECTED_SIZE} bytes")
endif()

file(COPY_FILE "${INPUT_FILE}" "${OUTPUT_FILE}" ONLY_IF_DIFFERENT)
