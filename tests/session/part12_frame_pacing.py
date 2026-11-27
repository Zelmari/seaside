#!/usr/bin/env python3
"""Measure matched Wayland client presentation samples from a Seaside terminal."""

import argparse
import json
import math
from pathlib import Path
import subprocess
import time


def ctl(*args):
    result = json.loads(subprocess.check_output(["seasidectl", *args], timeout=5))
    if not result["ok"]:
        raise RuntimeError(f"{args}: {result}")
    return result["result"]


def dump(path, data):
    path.write_text(json.dumps(data, indent=2) + "\n")


def first_clock(path, process):
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline and process.poll() is None:
        lines = path.read_text().splitlines()
        if lines and lines[0].startswith("clock ") and any(line.startswith("present ") for line in lines):
            return int(lines[0].split()[1])
        time.sleep(0.05)
    raise RuntimeError(f"Client did not present: {path}")


def measure(lines, start, end, period_ns):
    stamps = {}
    for line in lines:
        parts = line.split()
        if parts and parts[0] == "commit" and len(parts) == 3:
            stamps[int(parts[1])] = int(parts[2])
    frames = {frame for frame, stamp in stamps.items() if start <= stamp < end}
    presented = []
    discarded = callbacks = 0
    for line in lines:
        parts = line.split()
        if len(parts) < 2 or not parts[1].isdigit() or int(parts[1]) not in frames:
            continue
        match parts[0]:
            case "present":
                presented.append(int(parts[2]))
            case "discard":
                discarded += 1
            case "callback":
                callbacks += 1
    presented.sort()
    intervals = sorted(second - first for first, second in zip(presented, presented[1:]))
    if not intervals:
        raise RuntimeError("The sample contains no presentation intervals")
    return {
        "commits": len(frames),
        "callbacks": callbacks,
        "presented": len(presented),
        "discarded": discarded,
        "p50_ms": intervals[math.ceil(0.50 * len(intervals)) - 1] / 1e6,
        "p95_ms": intervals[math.ceil(0.95 * len(intervals)) - 1] / 1e6,
        "p99_ms": intervals[math.ceil(0.99 * len(intervals)) - 1] / 1e6,
        "missed_refreshes": sum(max(0, round(interval / period_ns) - 1) for interval in intervals),
    }


def measure_trace(path, output_id):
    events = [json.loads(line) for line in path.read_text().splitlines()]
    selected = [row for row in events if row.get("output_id") == output_id]
    commits = [row for row in selected if row["event"] == "commit"]
    presented = [row for row in selected if row["event"] == "present"]
    submitted = {row["commit_seq"] for row in commits
                 if row["succeeded"] and row["presentation_mode"] is not None}
    observed = {row["commit_seq"] for row in presented}
    return {
        "failed_commits": sum(not row["succeeded"] for row in commits),
        "discarded_presents": sum(not row["succeeded"] for row in presented),
        "matched_presents": len(submitted & observed),
        "unmatched_submissions": len(submitted - observed),
        "render_paths": sorted({row["presentation_mode"] for row in presented if row["succeeded"]}),
    }


def collect(args, pair, pair_number, traced):
    stem = f"{pair[0]}-{pair[1]}-{pair_number}" + ("-trace" if traced else "")
    app_id = f"seaside.frame-pacing.{stem}"
    path = args.output / f"{stem}.log"
    ctl("workspace", str(pair[0]))
    with path.open("w") as log, (args.output / f"{stem}.err").open("w") as error:
        process = subprocess.Popen([args.client, app_id], stdout=log, stderr=error)
        try:
            return collect_samples(args, pair, app_id, path, process, traced)
        finally:
            process.terminate()
            try:
                process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()


def collect_samples(args, pair, app_id, path, process, traced):
    clock = first_clock(path, process)
    window = next(row for row in ctl("windows")["windows"] if row.get("app_id") == app_id)
    samples = []
    for workspace in pair:
        if ctl("focus")["window_id"] != window["id"]:
            raise RuntimeError("The measurement window lost keyboard focus")
        if ctl("focus")["workspace_id"] != workspace:
            ctl("window", "to-workspace", str(workspace))
        time.sleep(args.warmup)
        focus = ctl("focus")
        output = next(row for row in ctl("outputs")["outputs"] if row["id"] == focus["output_id"])
        if focus["workspace_id"] != workspace or not output["mode"]:
            raise RuntimeError(f"Unexpected measurement target: {focus}, {output}")
        before = ctl("debug", "render")
        if traced:
            ctl("trace", "start")
        try:
            start = time.clock_gettime_ns(clock)
            time.sleep(args.sample)
            end = time.clock_gettime_ns(clock)
        finally:
            if traced:
                trace = ctl("debug", "render")["trace"]
                if trace["state"] == "running":
                    trace = ctl("trace", "stop")["trace"]
        if traced:
            export = ctl("trace", "export")
            target = args.output / f"{app_id}-{workspace}.jsonl"
            target.write_bytes(Path(export["path"]).read_bytes())
            trace["frames"] = measure_trace(target, output["id"])
        after = ctl("debug", "render")
        if ctl("focus") != focus or next(
            row for row in ctl("outputs")["outputs"] if row["id"] == focus["output_id"]
        ) != output:
            raise RuntimeError(f"Measurement target changed on workspace {workspace}")
        time.sleep(0.25)
        statistics = measure(path.read_text().splitlines(), start, end, 1e12 / output["mode"]["refresh_millihz"])
        sample = dict(statistics, workspace=workspace, output=output["connector"], mode=output["mode"],
                      app_id=app_id, clock=clock, start_ns=start, end_ns=end,
                      focus=focus, workspaces=ctl("workspaces"), before=before, after=after,
                      trace=trace if traced else None)
        print(json.dumps(sample), flush=True)
        samples.append(sample)
    return samples


def parse_pair(value):
    pair = tuple(int(item) for item in value.split(":"))
    if len(pair) != 2 or pair[0] == pair[1] or any(not 1 <= item <= 10 for item in pair):
        raise argparse.ArgumentTypeError("Expected two distinct numbered workspaces, such as 1:2")
    return pair


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--client", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--group", required=True, type=parse_pair, action="append")
    parser.add_argument("--pairs", type=int, default=3)
    parser.add_argument("--warmup", type=float, default=5)
    parser.add_argument("--sample", type=float, default=20)
    parser.add_argument("--trace-pair", action="store_true")
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    initial = ctl("focus")
    dump(args.output / "environment.json", {
        "version": subprocess.check_output(["seasidectl", "--version"], text=True).strip(),
        "outputs": ctl("outputs"), "workspaces": ctl("workspaces"), "status": ctl("status"),
    })
    results = []
    try:
        for group in args.group:
            for index in range(args.pairs):
                pair = group if index % 2 == 0 else group[::-1]
                results.extend(collect(args, pair, index, traced=False))
                dump(args.output / "summary.json", results)
            if args.trace_pair:
                results.extend(collect(args, group, args.pairs, traced=True))
                dump(args.output / "summary.json", results)
    finally:
        if initial["workspace_id"] is not None:
            ctl("workspace", str(initial["workspace_id"]))


if __name__ == "__main__":
    main()
