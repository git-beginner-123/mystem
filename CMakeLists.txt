cmake_minimum_required(VERSION 3.16)

find_package(Git QUIET)
if(GIT_FOUND)
    execute_process(
        COMMAND ${GIT_EXECUTABLE} describe --tags --always --dirty
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        OUTPUT_VARIABLE GIT_DESCRIBE_VERSION
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
    if(GIT_DESCRIBE_VERSION AND (NOT DEFINED PROJECT_VER OR PROJECT_VER STREQUAL ""))
        set(PROJECT_VER "${GIT_DESCRIBE_VERSION}")
    endif()
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(stem_framework_idf61_lcd)

# Pack generated MJPEG files into the "spiffs" data partition for runtime playback.
set(SPIFFS_LIMIT_BYTES 2023424)  # 0x1EE000 from partitions.csv
set(SPIFFS_SAFE_PAYLOAD 1900000)
set(SPIFFS_STAGE_DIR "${CMAKE_BINARY_DIR}/spiffs_payload")
set(SOLAR_MJPEG_FILES "")

file(GLOB MJPEG_OUT_FILES RELATIVE "${CMAKE_SOURCE_DIR}" "MJPEG_OUT/*.mjpeg")
file(GLOB mjpeg_out_files RELATIVE "${CMAKE_SOURCE_DIR}" "mjpeg_out/*.mjpeg")
list(APPEND SOLAR_MJPEG_FILES ${MJPEG_OUT_FILES} ${mjpeg_out_files})
list(REMOVE_DUPLICATES SOLAR_MJPEG_FILES)
list(SORT SOLAR_MJPEG_FILES)

set(SPIFFS_PAYLOAD_SIZE 0)
foreach(rel_path IN LISTS SOLAR_MJPEG_FILES)
    file(SIZE "${CMAKE_SOURCE_DIR}/${rel_path}" file_size)
    math(EXPR SPIFFS_PAYLOAD_SIZE "${SPIFFS_PAYLOAD_SIZE} + ${file_size}")
endforeach()

if(EXISTS "${CMAKE_SOURCE_DIR}/assets/spanish_history")
    file(GLOB history_files RELATIVE "${CMAKE_SOURCE_DIR}" "assets/spanish_history/*")
    foreach(rel_path IN LISTS history_files)
        file(SIZE "${CMAKE_SOURCE_DIR}/${rel_path}" file_size)
        math(EXPR SPIFFS_PAYLOAD_SIZE "${SPIFFS_PAYLOAD_SIZE} + ${file_size}")
    endforeach()
endif()

if(SPIFFS_PAYLOAD_SIZE GREATER SPIFFS_SAFE_PAYLOAD)
    message(FATAL_ERROR
        "SPIFFS payload too large. "
        "payload=${SPIFFS_PAYLOAD_SIZE} bytes, safe_limit=${SPIFFS_SAFE_PAYLOAD}, partition=${SPIFFS_LIMIT_BYTES}. "
        "Compress/remove MJPEG files or enlarge spiffs in partitions.csv.")
endif()

file(REMOVE_RECURSE "${SPIFFS_STAGE_DIR}")
file(MAKE_DIRECTORY "${SPIFFS_STAGE_DIR}")

foreach(rel_path IN LISTS SOLAR_MJPEG_FILES)
    file(COPY "${CMAKE_SOURCE_DIR}/${rel_path}" DESTINATION "${SPIFFS_STAGE_DIR}")
endforeach()

if(EXISTS "${CMAKE_SOURCE_DIR}/assets/spanish_history")
    file(COPY "${CMAKE_SOURCE_DIR}/assets/spanish_history" DESTINATION "${SPIFFS_STAGE_DIR}")
endif()

spiffs_create_partition_image(spiffs "${SPIFFS_STAGE_DIR}" FLASH_IN_PROJECT)
