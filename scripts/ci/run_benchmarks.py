#!/usr/bin/env python3
"""Run the CI benchmark subset and write normalized result files."""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path


CPU_CHARACTERISTIC_FILTERS = [
    "BM_ServerTickSerializedDelta/16384/8",
    "BM_ServerTickPackedAckedDeltaShared/16384/8",
    "BM_ServerTickMutatingAckedDelta/16384/8",
    "BM_ServerTickBudgetLimited/16384/8/64",
    "BM_ClientReceivePredict/4096/16",
    "BM_ClientApplyBufferedInterpolation/4096/16",
    "BM_ClientSampleFractionalTickLargePayload/1024/16",
    "BM_BitBufferAppendBits/131072",
]

BANDWIDTH_SCENARIOS = {
    "ball_snap_baseline": {
        "executable": "kage_sync_ball_stress",
        "category": "ball",
        "args": [
            "--duration-seconds",
            "5",
            "--clients",
            "4",
            "--max-balls",
            "4096",
            "--spawn-interval-ms",
            "5",
            "--client-mode",
            "snap",
            "--wire-diagnostics",
            "--report",
            "json",
        ],
    },
    "ball_buffered_latency": {
        "executable": "kage_sync_ball_stress",
        "category": "ball",
        "args": [
            "--duration-seconds",
            "5",
            "--clients",
            "4",
            "--max-balls",
            "4096",
            "--spawn-interval-ms",
            "5",
            "--server-to-client-latency-ms",
            "80",
            "--server-to-client-jitter-ms",
            "20",
            "--client-to-server-latency-ms",
            "40",
            "--client-to-server-jitter-ms",
            "10",
            "--client-mode",
            "buffered-interpolation",
            "--wire-diagnostics",
            "--report",
            "json",
        ],
    },
    "prediction_rollback_mixed": {
        "executable": "kage_sync_prediction_stress",
        "category": "prediction",
        "args": [
            "--entities",
            "2048",
            "--ticks",
            "600",
            "--latency-frames",
            "10",
            "--misprediction-percent",
            "5",
            "--rollback-policy",
            "all",
            "--report",
            "json",
        ],
    },
}


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--build-dir", required=True, type=Path)
    parser.add_argument("--output-dir", required=True, type=Path)
    parser.add_argument(
        "--subset",
        default="characteristic",
        choices=("characteristic", "cpu", "bandwidth", "all"),
    )
    parser.add_argument("--cpu-filter", default="")
    parser.add_argument("--bandwidth-scenarios", default="")
    parser.add_argument("--commit", default=os.environ.get("GITHUB_SHA", "unknown"))
    parser.add_argument("--ref", default=os.environ.get("GITHUB_REF_NAME", "unknown"))
    parser.add_argument("--run-id", default=os.environ.get("GITHUB_RUN_ID", ""))
    parser.add_argument("--run-attempt", default=os.environ.get("GITHUB_RUN_ATTEMPT", ""))
    parser.add_argument("--repository", default=os.environ.get("GITHUB_REPOSITORY", ""))
    return parser.parse_args()


def executable_path(build_dir: Path, name: str) -> Path:
    path = build_dir / "benchmarks" / name
    if not path.exists():
        raise FileNotFoundError(f"benchmark executable not found: {path}")
    return path


def run_command(command: list[str], stdout_path: Path | None = None) -> None:
    print("+ " + " ".join(command), flush=True)
    if stdout_path is None:
        subprocess.run(command, check=True)
        return
    stdout_path.parent.mkdir(parents=True, exist_ok=True)
    with stdout_path.open("w", encoding="utf-8") as stdout:
        subprocess.run(command, check=True, stdout=stdout)


def cpu_filter(args: argparse.Namespace) -> str:
    if args.cpu_filter:
        return args.cpu_filter
    names = CPU_CHARACTERISTIC_FILTERS
    if args.subset == "all":
        return "."
    return "^(" + "|".join(names) + ")$"


def selected_bandwidth_scenarios(args: argparse.Namespace) -> list[str]:
    if args.bandwidth_scenarios:
        names = [name.strip() for name in args.bandwidth_scenarios.split(",") if name.strip()]
    elif args.subset == "all":
        names = sorted(BANDWIDTH_SCENARIOS)
    else:
        names = ["ball_snap_baseline", "ball_buffered_latency", "prediction_rollback_mixed"]

    unknown = [name for name in names if name not in BANDWIDTH_SCENARIOS]
    if unknown:
        valid = ", ".join(sorted(BANDWIDTH_SCENARIOS))
        raise ValueError(f"unknown bandwidth scenario(s): {', '.join(unknown)}; valid: {valid}")
    return names


def summarize_cpu(cpu_json_path: Path) -> list[dict[str, object]]:
    data = json.loads(cpu_json_path.read_text(encoding="utf-8"))
    benchmarks = []
    for item in data.get("benchmarks", []):
        if item.get("run_type") != "iteration":
            continue
        benchmarks.append(
            {
                "name": item.get("name"),
                "real_time": item.get("real_time"),
                "cpu_time": item.get("cpu_time"),
                "time_unit": item.get("time_unit"),
                "items_per_second": item.get("items_per_second"),
                "bytes_per_second": item.get("bytes_per_second"),
                "iterations": item.get("iterations"),
            }
        )
    return benchmarks


def summarize_bandwidth(scenario: str, data: dict[str, object]) -> dict[str, object]:
    timing = data.get("timing", {})
    bandwidth = data.get("bandwidth", {})
    server_to_clients = bandwidth.get("server_to_clients", {}) if isinstance(bandwidth, dict) else {}
    clients_to_server = bandwidth.get("clients_to_server", {}) if isinstance(bandwidth, dict) else {}

    def timing_value(name: str) -> object:
        if isinstance(timing, dict) and timing.get(name) is not None:
            return timing.get(name)
        return data.get(name)

    return {
        "scenario": scenario,
        "wall_seconds": timing_value("wall_seconds"),
        "server_replication_seconds": timing_value("server_replication_seconds"),
        "client_receive_seconds": timing_value("client_receive_seconds"),
        "server_to_clients_bytes": server_to_clients.get("bytes"),
        "server_to_clients_packets": server_to_clients.get("packets"),
        "clients_to_server_bytes": clients_to_server.get("bytes"),
        "clients_to_server_packets": clients_to_server.get("packets"),
        "server_bytes": data.get("server_bytes"),
        "server_packets": data.get("server_packets"),
        "client_bytes": data.get("client_bytes"),
        "client_packets": data.get("client_packets"),
    }


def main() -> int:
    args = parse_args()
    args.output_dir.mkdir(parents=True, exist_ok=True)
    (args.output_dir / "cpu").mkdir(exist_ok=True)
    (args.output_dir / "bandwidth").mkdir(exist_ok=True)

    started_at = datetime.now(timezone.utc).isoformat()
    cpu_summary: list[dict[str, object]] = []
    bandwidth_summary: list[dict[str, object]] = []

    run_cpu = args.subset in ("characteristic", "cpu", "all") or bool(args.cpu_filter)
    run_bandwidth = args.subset in ("characteristic", "bandwidth", "all") or bool(args.bandwidth_scenarios)

    if run_cpu:
        benchmark = executable_path(args.build_dir, "kage_sync_benchmark")
        cpu_json = args.output_dir / "cpu" / "google-benchmark.json"
        run_command(
            [
                str(benchmark),
                f"--benchmark_filter={cpu_filter(args)}",
                "--benchmark_format=json",
                f"--benchmark_out={cpu_json}",
                "--benchmark_out_format=json",
            ]
        )
        cpu_summary = summarize_cpu(cpu_json)
        (args.output_dir / "cpu" / "summary.json").write_text(
            json.dumps(cpu_summary, indent=2) + "\n",
            encoding="utf-8",
        )

    if run_bandwidth:
        for scenario in selected_bandwidth_scenarios(args):
            config = BANDWIDTH_SCENARIOS[scenario]
            benchmark = executable_path(args.build_dir, config["executable"])
            output_path = args.output_dir / "bandwidth" / f"{scenario}.json"
            run_command([str(benchmark), *config["args"]], output_path)
            data = json.loads(output_path.read_text(encoding="utf-8"))
            bandwidth_summary.append(summarize_bandwidth(scenario, data))

    result = {
        "schema_version": 1,
        "commit": args.commit,
        "ref": args.ref,
        "repository": args.repository,
        "run_id": args.run_id,
        "run_attempt": args.run_attempt,
        "started_at": started_at,
        "finished_at": datetime.now(timezone.utc).isoformat(),
        "subset": args.subset,
        "cpu_filter": cpu_filter(args) if run_cpu else "",
        "bandwidth_scenarios": [item["scenario"] for item in bandwidth_summary],
        "cpu": cpu_summary,
        "bandwidth": bandwidth_summary,
    }
    (args.output_dir / "result.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    metadata_keys = (
        "schema_version",
        "commit",
        "ref",
        "repository",
        "run_id",
        "run_attempt",
        "started_at",
        "finished_at",
        "subset",
    )
    (args.output_dir / "metadata.json").write_text(
        json.dumps({key: result[key] for key in metadata_keys}, indent=2) + "\n",
        encoding="utf-8",
    )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as error:
        print(f"error: {error}", file=sys.stderr)
        raise SystemExit(1)
