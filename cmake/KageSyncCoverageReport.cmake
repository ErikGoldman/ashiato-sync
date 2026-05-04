cmake_minimum_required(VERSION 3.16)

foreach(required_var IN ITEMS
    KAGE_SYNC_SOURCE_DIR
    KAGE_SYNC_BINARY_DIR
    KAGE_SYNC_GCOV_EXECUTABLE
    KAGE_SYNC_COVERAGE_OUTPUT_DIR
)
    if(NOT DEFINED ${required_var} OR "${${required_var}}" STREQUAL "")
        message(FATAL_ERROR "${required_var} is required")
    endif()
endforeach()

file(REMOVE_RECURSE "${KAGE_SYNC_COVERAGE_OUTPUT_DIR}")
file(MAKE_DIRECTORY "${KAGE_SYNC_COVERAGE_OUTPUT_DIR}")

file(GLOB_RECURSE kage_sync_gcda_files
    "${KAGE_SYNC_BINARY_DIR}/src/CMakeFiles/kage_sync.dir/*.gcda"
    "${KAGE_SYNC_BINARY_DIR}/tests/CMakeFiles/kage_sync_tests.dir/*.gcda"
)

if(NOT kage_sync_gcda_files)
    message(FATAL_ERROR "No .gcda files found. Build with KAGE_SYNC_ENABLE_COVERAGE=ON and run tests before generating coverage.")
endif()

set(kage_sync_line_total 0)
set(kage_sync_line_covered 0)
set(kage_sync_branch_total 0)
set(kage_sync_branch_covered 0)
set(kage_sync_function_total 0)
set(kage_sync_function_covered 0)
set(kage_sync_seen_sources "")
set(kage_sync_seen_lines "")
set(kage_sync_seen_branches "")
set(kage_sync_seen_functions "")

function(kage_sync_project_source source_path out_var)
    if(source_path MATCHES "^${KAGE_SYNC_SOURCE_DIR}/(include/kage/sync/.*|src/.*\\.(cpp|hpp|h))$"
        AND NOT source_path MATCHES "\\.test\\.cpp$")
        set(${out_var} TRUE PARENT_SCOPE)
    else()
        set(${out_var} FALSE PARENT_SCOPE)
    endif()
endfunction()

function(kage_sync_record_covered_key list_var prefix key is_covered)
    string(MAKE_C_IDENTIFIER "${key}" key_id)
    set(value_var "${prefix}_${key_id}")

    if(NOT key IN_LIST ${list_var})
        list(APPEND ${list_var} "${key}")
        set(${value_var} FALSE PARENT_SCOPE)
    endif()

    if(is_covered)
        set(${value_var} TRUE PARENT_SCOPE)
    endif()

    set(${list_var} "${${list_var}}" PARENT_SCOPE)
endfunction()

foreach(gcda_file IN LISTS kage_sync_gcda_files)
    get_filename_component(gcda_dir "${gcda_file}" DIRECTORY)
    execute_process(
        COMMAND "${KAGE_SYNC_GCOV_EXECUTABLE}" -b -c -p -o "${gcda_dir}" "${gcda_file}"
        WORKING_DIRECTORY "${KAGE_SYNC_COVERAGE_OUTPUT_DIR}"
        RESULT_VARIABLE gcov_result
        OUTPUT_VARIABLE gcov_output
        ERROR_VARIABLE gcov_error
    )

    if(NOT gcov_result EQUAL 0)
        message(FATAL_ERROR "gcov failed for ${gcda_file}\n${gcov_output}\n${gcov_error}")
    endif()
endforeach()

file(GLOB kage_sync_gcov_reports "${KAGE_SYNC_COVERAGE_OUTPUT_DIR}/*.gcov")

foreach(gcov_report IN LISTS kage_sync_gcov_reports)
    file(READ "${gcov_report}" gcov_report_content)
    string(REPLACE "\r\n" "\n" gcov_report_content "${gcov_report_content}")
    string(REPLACE "\n" ";" gcov_lines "${gcov_report_content}")

    set(current_source "")
    set(current_line 0)
    set(include_current_source FALSE)

    foreach(gcov_line IN LISTS gcov_lines)
        if(gcov_line MATCHES "^ *-: *0:Source:(.+)$")
            set(current_source "${CMAKE_MATCH_1}")
            kage_sync_project_source("${current_source}" include_current_source)
            if(include_current_source)
                list(APPEND kage_sync_seen_sources "${current_source}")
            endif()
        elseif(include_current_source AND gcov_line MATCHES "^function ([^ ]+) called ([0-9]+)")
            set(function_key "${current_source}:${CMAKE_MATCH_1}")
            set(is_covered FALSE)
            if(CMAKE_MATCH_2 GREATER 0)
                set(is_covered TRUE)
            endif()
            kage_sync_record_covered_key(kage_sync_seen_functions "kage_sync_function_covered_key" "${function_key}" "${is_covered}")
        elseif(include_current_source AND gcov_line MATCHES "^ *([^:]+): *([0-9]+):(.*)$")
            set(line_count "${CMAKE_MATCH_1}")
            set(current_line "${CMAKE_MATCH_2}")
            if(NOT line_count STREQUAL "-")
                set(line_key "${current_source}:${current_line}")
                set(is_covered FALSE)
                if(line_count MATCHES "^[1-9]")
                    set(is_covered TRUE)
                endif()
                kage_sync_record_covered_key(kage_sync_seen_lines "kage_sync_line_covered_key" "${line_key}" "${is_covered}")
            endif()
        elseif(include_current_source AND gcov_line MATCHES "^branch +([0-9]+) +taken +([0-9]+)")
            set(branch_key "${current_source}:${current_line}:${CMAKE_MATCH_1}")
            set(is_covered FALSE)
            if(CMAKE_MATCH_2 GREATER 0)
                set(is_covered TRUE)
            endif()
            kage_sync_record_covered_key(kage_sync_seen_branches "kage_sync_branch_covered_key" "${branch_key}" "${is_covered}")
        elseif(include_current_source AND gcov_line MATCHES "^branch +([0-9]+) +never executed")
            set(branch_key "${current_source}:${current_line}:${CMAKE_MATCH_1}")
            kage_sync_record_covered_key(kage_sync_seen_branches "kage_sync_branch_covered_key" "${branch_key}" FALSE)
        endif()
    endforeach()
endforeach()

list(REMOVE_DUPLICATES kage_sync_seen_sources)
list(LENGTH kage_sync_seen_sources kage_sync_source_count)

foreach(line_key IN LISTS kage_sync_seen_lines)
    string(MAKE_C_IDENTIFIER "${line_key}" key_id)
    math(EXPR kage_sync_line_total "${kage_sync_line_total} + 1")
    if(kage_sync_line_covered_key_${key_id})
        math(EXPR kage_sync_line_covered "${kage_sync_line_covered} + 1")
    endif()
endforeach()

foreach(branch_key IN LISTS kage_sync_seen_branches)
    string(MAKE_C_IDENTIFIER "${branch_key}" key_id)
    math(EXPR kage_sync_branch_total "${kage_sync_branch_total} + 1")
    if(kage_sync_branch_covered_key_${key_id})
        math(EXPR kage_sync_branch_covered "${kage_sync_branch_covered} + 1")
    endif()
endforeach()

foreach(function_key IN LISTS kage_sync_seen_functions)
    string(MAKE_C_IDENTIFIER "${function_key}" key_id)
    math(EXPR kage_sync_function_total "${kage_sync_function_total} + 1")
    if(kage_sync_function_covered_key_${key_id})
        math(EXPR kage_sync_function_covered "${kage_sync_function_covered} + 1")
    endif()
endforeach()

function(kage_sync_coverage_percent covered total out_var)
    if(${total} EQUAL 0)
        set(${out_var} "n/a" PARENT_SCOPE)
    else()
        math(EXPR percent_x100 "(${covered} * 10000 + (${total} / 2)) / ${total}")
        math(EXPR percent_whole "${percent_x100} / 100")
        math(EXPR percent_fraction "${percent_x100} % 100")
        if(percent_fraction LESS 10)
            set(percent_fraction "0${percent_fraction}")
        endif()
        set(${out_var} "${percent_whole}.${percent_fraction}" PARENT_SCOPE)
    endif()
endfunction()

function(kage_sync_print_coverage label covered total)
    kage_sync_coverage_percent("${covered}" "${total}" percent)
    if(percent STREQUAL "n/a")
        message(STATUS "${label}: n/a")
    else()
        message(STATUS "${label}: ${percent}% (${covered}/${total})")
    endif()
endfunction()

kage_sync_coverage_percent("${kage_sync_line_covered}" "${kage_sync_line_total}" kage_sync_line_percent)
kage_sync_coverage_percent("${kage_sync_branch_covered}" "${kage_sync_branch_total}" kage_sync_branch_percent)
kage_sync_coverage_percent("${kage_sync_function_covered}" "${kage_sync_function_total}" kage_sync_function_percent)

set(kage_sync_badge_color "red")
if(NOT kage_sync_line_percent STREQUAL "n/a")
    string(REGEX REPLACE "\\..*$" "" kage_sync_line_percent_whole "${kage_sync_line_percent}")
    if(kage_sync_line_percent_whole GREATER_EQUAL 90)
        set(kage_sync_badge_color "brightgreen")
    elseif(kage_sync_line_percent_whole GREATER_EQUAL 80)
        set(kage_sync_badge_color "green")
    elseif(kage_sync_line_percent_whole GREATER_EQUAL 70)
        set(kage_sync_badge_color "yellowgreen")
    elseif(kage_sync_line_percent_whole GREATER_EQUAL 60)
        set(kage_sync_badge_color "yellow")
    elseif(kage_sync_line_percent_whole GREATER_EQUAL 50)
        set(kage_sync_badge_color "orange")
    endif()
endif()

file(WRITE "${KAGE_SYNC_COVERAGE_OUTPUT_DIR}/coverage.json"
    "{\"schemaVersion\":1,\"label\":\"coverage\",\"message\":\"${kage_sync_line_percent}%\",\"color\":\"${kage_sync_badge_color}\"}\n")

file(WRITE "${KAGE_SYNC_COVERAGE_OUTPUT_DIR}/index.html"
    "<!doctype html>\n"
    "<html lang=\"en\">\n"
    "<head>\n"
    "  <meta charset=\"utf-8\">\n"
    "  <meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n"
    "  <title>Kage Sync Coverage</title>\n"
    "  <style>body{font-family:system-ui,sans-serif;margin:2rem;line-height:1.5;color:#1f2933}table{border-collapse:collapse;margin-top:1rem}td,th{border:1px solid #d9e2ec;padding:.45rem .7rem;text-align:left}th{background:#f0f4f8}.metric{font-variant-numeric:tabular-nums}</style>\n"
    "</head>\n"
    "<body>\n"
    "  <h1>Kage Sync Coverage</h1>\n"
    "  <table>\n"
    "    <thead><tr><th>Metric</th><th>Coverage</th><th>Covered</th><th>Total</th></tr></thead>\n"
    "    <tbody>\n"
    "      <tr><td>Lines</td><td class=\"metric\">${kage_sync_line_percent}%</td><td class=\"metric\">${kage_sync_line_covered}</td><td class=\"metric\">${kage_sync_line_total}</td></tr>\n"
    "      <tr><td>Branches</td><td class=\"metric\">${kage_sync_branch_percent}%</td><td class=\"metric\">${kage_sync_branch_covered}</td><td class=\"metric\">${kage_sync_branch_total}</td></tr>\n"
    "      <tr><td>Functions</td><td class=\"metric\">${kage_sync_function_percent}%</td><td class=\"metric\">${kage_sync_function_covered}</td><td class=\"metric\">${kage_sync_function_total}</td></tr>\n"
    "    </tbody>\n"
    "  </table>\n"
    "  <p>Project source files reported: ${kage_sync_source_count}</p>\n"
    "</body>\n"
    "</html>\n")

message(STATUS "Coverage report files: ${KAGE_SYNC_COVERAGE_OUTPUT_DIR}")
message(STATUS "Project source files reported: ${kage_sync_source_count}")
kage_sync_print_coverage("Line coverage" "${kage_sync_line_covered}" "${kage_sync_line_total}")
kage_sync_print_coverage("Branch coverage" "${kage_sync_branch_covered}" "${kage_sync_branch_total}")
kage_sync_print_coverage("Function coverage" "${kage_sync_function_covered}" "${kage_sync_function_total}")
