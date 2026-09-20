#!/usr/bin/env bash
set -euo pipefail

project_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
tier="${1:-pr}"
build_dir="${2:-${project_dir}/build}"

case "${tier}" in
    pr)
        seed_count=4
        ;;
    nightly)
        seed_count=256
        ;;
    *)
        echo "usage: $0 [pr|nightly] [build-dir]" >&2
        exit 2
        ;;
esac

cmake --build "${build_dir}" --parallel --target ashiato_sync_tests
ASHIATO_SYNC_STRESS_SEED_COUNT="${seed_count}" \
    "${build_dir}/tests/ashiato_sync_tests" "[.stress]~[known-bug]" --reporter compact
