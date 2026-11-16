add_executable(flac_property flac_property.c)
target_link_libraries(flac_property PRIVATE FLAC)

find_package(Python3 QUIET COMPONENTS Interpreter)
find_program(FFMPEG_EXECUTABLE ffmpeg)
find_program(FFPROBE_EXECUTABLE ffprobe)
find_program(BASH_EXECUTABLE bash)

if(Python3_Interpreter_FOUND AND FFMPEG_EXECUTABLE AND FFPROBE_EXECUTABLE AND BASH_EXECUTABLE)
    add_test(
        NAME flac_property
        COMMAND "${BASH_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/run_property_tests.sh"
            --binary "$<TARGET_FILE:flac_property>"
            --work "${CMAKE_CURRENT_BINARY_DIR}/work"
            --report "${CMAKE_CURRENT_BINARY_DIR}/report"
            --ffmpeg "${FFMPEG_EXECUTABLE}"
            --ffprobe "${FFPROBE_EXECUTABLE}"
            --python "${Python3_EXECUTABLE}"
            --source-dir "${CMAKE_CURRENT_SOURCE_DIR}"
            --malformed-count 200
            --valid-count 50
        WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
    set_tests_properties(flac_property PROPERTIES LABELS "flac;property;sanitizer" TIMEOUT 600)
else()
    message(STATUS "ffmpeg, ffprobe, bash, or Python3 not found; flac_property CTest disabled")
endif()
