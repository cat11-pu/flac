add_executable(test_seeking
    main.c
    $<$<BOOL:${WIN32}>:../../include/share/win_utf8_io.h>
    $<$<BOOL:${WIN32}>:../share/win_utf8_io/win_utf8_io.c>)
target_link_libraries(test_seeking FLAC)

add_executable(sample_seek_test
    sample_seek.c
    sample_seek_test.c)
target_include_directories(sample_seek_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(sample_seek_test FLAC)
