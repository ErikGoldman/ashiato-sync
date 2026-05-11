add_executable(ashiato_sync_protocol_fuzzer
    protocol_fuzzer.cpp
)

target_link_libraries(ashiato_sync_protocol_fuzzer
    PRIVATE
        ashiato::sync
)

target_include_directories(ashiato_sync_protocol_fuzzer
    PRIVATE
        ${PROJECT_SOURCE_DIR}/src
        ${PROJECT_SOURCE_DIR}/tests
)

target_compile_options(ashiato_sync_protocol_fuzzer PRIVATE
    -g
    -O1
    -fno-omit-frame-pointer
    -fsanitize=fuzzer-no-link,address,undefined
)

target_link_options(ashiato_sync_protocol_fuzzer PRIVATE
    -fsanitize=fuzzer,address,undefined
)
