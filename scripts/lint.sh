#!/usr/bin/env bash
set -euo pipefail

project_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="${ASHIATO_SYNC_LINT_BUILD_DIR:-${project_dir}/build-lint}"
ashiato_source_dir="${ASHIATO_SYNC_ASHIATO_SOURCE_DIR:-${project_dir}/../ashiato}"
spdlog_source_dir="${FETCHCONTENT_SOURCE_DIR_SPDLOG:-${project_dir}/../spdlog}"

cmake -S "${project_dir}" -B "${build_dir}" \
    -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
    -DBUILD_TESTING=OFF \
    -DASHIATO_SYNC_BUILD_EXAMPLES=OFF \
    -DASHIATO_SYNC_BUILD_BENCHMARKS=OFF \
    -DASHIATO_SYNC_BUILD_FUZZERS=OFF \
    -DASHIATO_SYNC_BUILD_TRACE_VIEWER=OFF \
    -DASHIATO_SYNC_BUILD_STATIC_LIBRARY=ON \
    -DASHIATO_SYNC_ENABLE_CLANG_TIDY=ON \
    -DASHIATO_SYNC_ENABLE_CPPCHECK=ON \
    -DASHIATO_SYNC_ASHIATO_SOURCE_DIR="${ashiato_source_dir}" \
    -DFETCHCONTENT_SOURCE_DIR_SPDLOG="${spdlog_source_dir}"

cmake --build "${build_dir}" --target ashiato_sync
