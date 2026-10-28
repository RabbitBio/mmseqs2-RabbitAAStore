#!/usr/bin/env python3
# 文件职责：按抽样比例、线程数和前缀驻留比例运行独立进程冷缓存实验。
# 保存实验配置、逐例日志和 CSV，并检查同组请求及输出校验值一致。
"""Paired, fresh-process RabbitAAStore random-access pilot. Standard library only."""

import argparse
import csv
import datetime
from decimal import Decimal, InvalidOperation
import hashlib
import json
import math
import os
from pathlib import Path
import platform
import random
import struct
import subprocess
import sys
import time


# 解析命令行正整数，拒绝零和负数。
def positive(value):
    result = int(value)
    if result <= 0:
        raise argparse.ArgumentTypeError("must be positive")
    return result


# 校验抽样百分比位于 (0, 100] 且为有限值，保留原字符串供基准程序解析。
def percentage(value):
    result = float(value)
    if not math.isfinite(result) or not 0 < result <= 100:
        raise argparse.ArgumentTypeError("must be in (0, 100]")
    return value


# 从程序输出中提取 key=value 形式的统计字段。
def parse_metrics(text):
    return dict(line.split("=", 1) for line in text.splitlines() if "=" in line)


# 校验并规范化 [0, 100) 的前缀比例，便于识别重复值；100% 单独作为全内存模式。
def prefix_percentage(value):
    try:
        number = Decimal(value)
    except InvalidOperation:
        raise argparse.ArgumentTypeError("prefix percentage must be a number")
    if not number.is_finite() or not 0 <= number < 100:
        raise argparse.ArgumentTypeError("prefix percentage must be in [0, 100); FullMemory covers 100%")
    return "0" if number == 0 else format(number.normalize(), "f")


# 取主机可用内存与各级可见 cgroup 剩余额度中的最小值。
def available_memory():
    values = {}
    for line in Path("/proc/meminfo").read_text().splitlines():
        key, value = line.split(":", 1)
        values[key] = int(value.split()[0]) * 1024
    available = values["MemAvailable"]
    # 同时考虑 cgroup v1/v2 及所有可见祖先节点的内存限制。
    for line in Path("/proc/self/cgroup").read_text().splitlines():
        _, controllers, relative = line.split(":", 2)
        if controllers == "":
            root = Path("/sys/fs/cgroup")
            limit_name, usage_name = "memory.max", "memory.current"
        elif "memory" in controllers.split(","):
            root = Path("/sys/fs/cgroup/memory")
            limit_name, usage_name = "memory.limit_in_bytes", "memory.usage_in_bytes"
        else:
            continue
        directory = root / relative.lstrip("/")
        while directory == root or root in directory.parents:
            try:
                limit = (directory / limit_name).read_text().strip()
                usage = int((directory / usage_name).read_text())
                if limit != "max":
                    available = min(available, max(0, int(limit) - usage))
            except FileNotFoundError:
                pass
            if directory == root:
                break
            directory = directory.parent
    return available


# 打乱实验组合后逐例运行，验证冷缓存、后端、驻留预算和配对结果，再汇总指标。
def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("store", type=Path)
    parser.add_argument("output", type=Path, help="new results directory")
    parser.add_argument("--bench", type=Path, default=Path(__file__).resolve().parents[1] / "build/aastore_bench")
    parser.add_argument("--label", required=True)
    parser.add_argument("--percents", nargs="+", type=percentage, default=["0.01", "0.1"])
    parser.add_argument("--threads", nargs="+", type=positive, default=[1, 8, 64])
    parser.add_argument("--prefix-percents", nargs="+", type=prefix_percentage, default=["0"],
                        help="Preload fractions below 100% of encoded bytes; a full-preload case is added automatically")
    parser.add_argument("--repeats", type=positive, default=1)
    parser.add_argument("--seed", type=int, default=12345)
    parser.add_argument("--batch-bytes", type=positive, default=64 * 1024**2)
    parser.add_argument("--io-buffer-bytes", type=positive, default=16 * 1024**2)
    parser.add_argument("--queue-depth", type=positive, default=64)
    parser.add_argument("--backend", choices=["uring", "pread"], default="uring")
    parser.add_argument("--timeout", type=positive, default=1800, help="seconds per case")
    args = parser.parse_args()
    args.store, args.bench, args.output = args.store.resolve(), args.bench.resolve(), args.output.resolve()
    if not 0 <= args.seed <= 2**64 - args.repeats:
        parser.error("seed and repeats exceed uint64 range")
    if len(set(args.prefix_percents)) != len(args.prefix_percents):
        parser.error("duplicate prefix percentages")
    files = {}
    for name in ("meta.bin", "packed.bin"):
        status = (args.store / name).stat()
        files[name] = {"bytes": status.st_size, "mtime_ns": status.st_mtime_ns}
    with (args.store / "meta.bin").open("rb") as handle:
        magic, version, records, residues = struct.unpack("<8sQQQ", handle.read(32))
    if magic != b"AAPCMP3\0" or version != 3:
        parser.error("expected an AAPCMP3 store")
    packed_bytes = (residues * 5 + 7) // 8
    if files["packed.bin"]["bytes"] < packed_bytes:
        parser.error("packed file is shorter than metadata")
    storage_cases = [("full", "100", packed_bytes)] + [
        ("nonfull", percent, int(Decimal(packed_bytes) * Decimal(percent) / 100))
        for percent in args.prefix_percents]
    args.output.mkdir(parents=True, exist_ok=False)
    cases = [(repeat, percent, threads, mode, resident_percent, resident_budget)
             for repeat in range(args.repeats) for percent in args.percents
             for threads in args.threads for mode, resident_percent, resident_budget in storage_cases]
    random.Random(args.seed).shuffle(cases)
    manifest = {
        "started_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
        "arguments": {key: str(value) if isinstance(value, Path) else value
                      for key, value in vars(args).items()},
        "platform": platform.platform(), "cpu_count": os.cpu_count(),
        "cpu_affinity": sorted(os.sched_getaffinity(0)),
        "bench_sha256": hashlib.sha256(args.bench.read_bytes()).hexdigest(),
        "store_files": files, "records": records, "case_order": cases,
        "cold_scope": "packed.bin and meta.bin evicted and mincore-verified before each process opens Store",
        "nonfull": "NoMemory at zero budget; PartialMemory at positive sub-full budget",
        "prefix_basis": "floor(metadata packed bytes * percent / 100), not record count",
        "full": "FullMemory from max(1, packed_bytes) preload budget; preload included in service_ms",
        "resident_budget_bytes": "Expected resident encoded bytes; preload_bytes records the requested budget",
        "packed_bytes": packed_bytes,
    }
    (args.output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    fields = ["case", "repeat", "percent", "mode", "resident_percent", "resident_budget_bytes",
              "threads", "status", "wall_s", "storage_mode", "packed_bytes", "preload_bytes",
              "requests", "residues", "trace_hash_fnv1a64", "output_hash_fnv1a64",
              "cold_cache_verified", "batches_per_round", "max_batch_output_bytes",
              "open_ms", "preload_ms", "fetch_ms", "service_ms", "end_to_end_ms",
              "prepare_ms", "consume_ms", "peak_rss_kib", "resident_bytes",
              "open_storage_read_bytes", "fetch_storage_read_bytes", "read_bytes",
              "ranges", "active_io_backend", "workers", "io_workers",
              "collect_diagnostics", "diagnostics_collected",
              "io_submissions", "io_completions", "io_submit_calls", "io_wait_calls",
              "cache_hits", "cache_misses", "cache_hit_residues", "cache_miss_residues"]
    paired = {}
    with (args.output / "results.csv").open("w", newline="") as output:
        writer = csv.DictWriter(output, fieldnames=fields, extrasaction="ignore")
        writer.writeheader()
        output.flush()
        for number, (repeat, percent, threads, mode, resident_percent, resident_budget) in enumerate(cases, 1):
            case = "{:03d}_{}_resident{}_p{}_t{}_r{}".format(
                number, mode, resident_percent, percent, threads, repeat + 1)
            command = [str(args.bench), str(args.store), "--sample-percent", percent,
                       "--threads", str(threads),
                       "--io-backend", args.backend, "--batch-bytes", str(args.batch_bytes),
                       "--io-buffer-bytes", str(args.io_buffer_bytes), "--queue-depth", str(args.queue_depth),
                       "--seed", str(args.seed + repeat), "--rounds", "1", "--drop-cache"]
            # An empty encoded file still needs a positive request to select
            # FullMemory. Keep the expected residency separate from the budget.
            preload_budget = max(1, packed_bytes) if mode == "full" else resident_budget
            command += ["--memory-bytes", str(preload_budget)]
            (args.output / (case + ".command.json")).write_text(json.dumps(command) + "\n")
            # 保守的启动检查，并非对任意长度序列都成立的内存上界。
            sample_count = max(1, int(records * float(percent) / 100)) if records else 0
            estimate = (files["meta.bin"]["bytes"] + sample_count * 120 +
                        2 * args.batch_bytes + args.io_buffer_bytes + 1024**3)
            estimate += 2 * resident_budget
            if available_memory() < estimate:
                raise RuntimeError("insufficient available memory for " + case)
            print("[{}/{}] {}".format(number, len(cases), case), flush=True)
            started = time.monotonic()
            row = {"case": case, "repeat": repeat + 1, "percent": percent,
                   "mode": mode, "resident_percent": resident_percent,
                   "resident_budget_bytes": resident_budget, "threads": threads, "status": "ok"}
            with (args.output / (case + ".log")).open("w") as log:
                try:
                    result = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, timeout=args.timeout)
                    if result.returncode:
                        row["status"] = "exit_{}".format(result.returncode)
                except subprocess.TimeoutExpired:
                    row["status"] = "timeout"
            metrics = parse_metrics((args.output / (case + ".log")).read_text())
            row.update(metrics)
            row["wall_s"] = round(time.monotonic() - started, 3)
            if row["status"] == "ok":
                try:
                    if metrics["cold_cache_verified"] != "1":
                        raise ValueError("cache was not verified cold")
                    expected_mode = ("no_memory" if preload_budget == 0 else
                                     "full_memory" if preload_budget >= packed_bytes else
                                     "partial_memory")
                    if metrics["storage_mode"] != expected_mode:
                        raise ValueError("unexpected storage mode")
                    if int(metrics["preload_bytes"]) != preload_budget:
                        raise ValueError("preload budget differs from requested budget")
                    if int(metrics["resident_bytes"]) != min(preload_budget, packed_bytes):
                        raise ValueError("resident bytes differ from expected residency")
                    expected_backend = "io_uring" if args.backend == "uring" else "pread"
                    if int(metrics["ranges"]) and metrics["active_io_backend"] != expected_backend:
                        raise ValueError("backend fell back")
                    signature = tuple(metrics[key] for key in (
                        "requests", "residues", "trace_hash_fnv1a64", "output_hash_fnv1a64",
                        "batches_per_round", "max_batch_output_bytes"))
                    key = (repeat, percent)
                    if key in paired and paired[key] != signature:
                        raise ValueError("paired trace/output mismatch")
                    paired[key] = signature
                except (KeyError, ValueError) as error:
                    row["status"] = "invalid: " + str(error)
            writer.writerow(row)
            output.flush()
            if row["status"] != "ok":
                raise RuntimeError(case + ": " + row["status"])
    (args.output / "COMPLETE").write_text("{} cold cases; paired outputs matched\n".format(len(cases)))
    print("Completed: " + str(args.output / "results.csv"), flush=True)


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, ValueError) as error:
        sys.exit(str(error))
