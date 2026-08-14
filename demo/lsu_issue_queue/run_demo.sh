#!/usr/bin/env bash

set -euo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
repo_root=$(cd -- "${script_dir}/../.." && pwd)
build_dir=${OLYMPIA_DEMO_BUILD_DIR:-"${repo_root}/build-demo"}
sparta_prefix=${SPARTA_SEARCH_DIR:-"${repo_root}/../map/sparta/install"}
mode=${1:-all}

case "${mode}" in
    all|conservative|speculative) ;;
    *)
        echo "Usage: $0 [all|conservative|speculative]" >&2
        exit 2
        ;;
esac

cmake -S "${repo_root}" -B "${build_dir}" -G Ninja \
    -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
    -DSPARTA_SEARCH_DIR="${sparta_prefix}"
cmake --build "${build_dir}" --target lsu_issue_queue_demo -j "$(nproc)"

binary="${build_dir}/demo/lsu_issue_queue/lsu_issue_queue_demo"
output_dir="${build_dir}/demo/lsu_issue_queue/output"
mkdir -p "${output_dir}"

run_mode() {
    local name=$1
    local speculative=$2
    local observer_out="${output_dir}/${name}.observer.txt"
    local lsu_log="${output_dir}/${name}.lsu.log"

    echo
    echo "================ ${name^^} MODE ================"
    (
        cd "${build_dir}"
        "${binary}" \
            --input-file "${script_dir}/trace.json" \
            -c "${script_dir}/demo.yaml" \
            -p top.cpu.core0.lsu.params.allow_speculative_load_exec "${speculative}" \
            -l top.cpu.core0.lsu info "${lsu_log}"
    ) | tee "${observer_out}"

    echo
    echo "Key LSU log events (${name}):"
    rg "Arbitrated inst|Aborted younger load|Replay inst ready|Found forwarding store" \
        "${lsu_log}" || true
}

if [[ "${mode}" == "all" || "${mode}" == "conservative" ]]; then
    run_mode conservative false
fi

if [[ "${mode}" == "all" || "${mode}" == "speculative" ]]; then
    run_mode speculative true
fi

echo
echo "Demo outputs: ${output_dir}"
