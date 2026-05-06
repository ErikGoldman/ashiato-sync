add_executable(kage_sync_protocol_fuzzer
    protocol_fuzzer.cpp
)

target_link_libraries(kage_sync_protocol_fuzzer
    PRIVATE
        kage::sync
)

target_include_directories(kage_sync_protocol_fuzzer
    PRIVATE
        ${PROJECT_SOURCE_DIR}/tests
)

target_compile_options(kage_sync_protocol_fuzzer PRIVATE
    -g
    -O1
    -fno-omit-frame-pointer
    -fsanitize=fuzzer-no-link,address,undefined
)

target_link_options(kage_sync_protocol_fuzzer PRIVATE
    -fsanitize=fuzzer,address,undefined
)
