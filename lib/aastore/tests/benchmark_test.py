#!/usr/bin/env python3
# 文件职责：使用临时生成的小数据验证 benchmark 命令行和冷缓存矩阵脚本。
# 检查分批边界、不同模式与线程数的输出一致性、非法参数及结果统计。
"""Exercise the benchmark CLI on generated data, not a publication dataset."""

import argparse
import csv
import json
from pathlib import Path
import subprocess
import sys
import tempfile

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "benchmarks"))
from run_cold_matrix import parse_metrics


# 限时执行命令，合并输出并检查退出码，成功后提取统计字段。
def checked(command):
    result = subprocess.run([str(value) for value in command], stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, universal_newlines=True, timeout=120)
    if result.returncode:
        raise RuntimeError("{}\n{}".format(command, result.stdout))
    return parse_metrics(result.stdout)


# 按请求顺序独立计算 FNV-1a 校验值，纳入小端 ID、长度和记录内容。
def digest(records, ids):
    value = 14695981039346656037
    for index in ids:
        record = records[index].encode("ascii")
        data = index.to_bytes(8, "little") + len(record).to_bytes(8, "little") + record
        for byte in data:
            value = ((value ^ byte) * 1099511628211) & ((1 << 64) - 1)
    return str(value)


# 创建测试库并运行模式、线程和后端组合，检查分批、校验值、参数拒绝及矩阵输出。
def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("build", type=Path)
    parser.add_argument("bench", type=Path)
    parser.add_argument("--uring", action="store_true")
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    if args.output:
        args.output.mkdir(parents=True, exist_ok=False)
    results = []
    with tempfile.TemporaryDirectory(prefix="aastore-bench-test-") as directory:
        root = Path(directory)
        records = [("ACDEFGHIKLMNPQRSTVWY" * 16)[:13 + i % 271] for i in range(1000)]
        records[21] = "ACD" * 100000
        fasta, store = root / "input.fasta", root / "store"
        fasta.write_text("".join(">record{}\n{}\n".format(i, value) for i, value in enumerate(records)))
        checked([args.build, fasta, store, "--sequences-only"])
        backends = ["pread"] + (["uring"] if args.uring else [])
        expected_sum = str(sum(sum(value.encode("ascii")) for value in records))
        packed_bytes = (store / "packed.bin").stat().st_size
        storage_cases = [(["--full-memory"], packed_bytes, "full_memory")]
        for fraction in (0, 25, 50, 75):
            budget = packed_bytes * fraction // 100
            storage_cases.append((["--memory-bytes", budget], budget,
                                  "prefix_memory" if budget else "byte_range"))
        paired = {}
        for backend in backends:
            for threads in (1, 4, 16, 64):
                for storage_args, budget, mode in storage_cases:
                    for percent in ("10", "100"):
                        command = [args.bench, store, "--sample-percent", percent, "--threads", threads,
                                   "--io-backend", backend, "--drop-cache",
                                   "--batch-bytes", 4096] + storage_args
                        metrics = checked(command)
                        assert metrics["storage_mode"] == mode
                        assert int(metrics["resident_bytes"]) == budget
                        assert metrics["cold_cache_verified"] == "1"
                        assert metrics["collect_diagnostics"] == "1"
                        assert metrics["diagnostics_collected"] == "1"
                        assert float(metrics["layout_ms"]) > 0
                        assert float(metrics["worker_decode_ms"]) > 0
                        assert metrics["requests"] == ("100" if percent == "10" else "1000")
                        assert metrics["sampling"] == "without_replacement"
                        assert int(metrics["workers"]) <= threads
                        signature = tuple(metrics[key] for key in (
                            "trace_hash_fnv1a64", "output_hash_fnv1a64", "residues", "batches_per_round"))
                        if percent in paired:
                            assert paired[percent] == signature
                        paired[percent] = signature
                        if percent == "100":
                            assert metrics["checksum"] == expected_sum
                            assert metrics["max_batch_output_bytes"] == "300001"
                        if int(metrics["ranges"]):
                            assert metrics["active_io_backend"] == ("io_uring" if backend == "uring" else "pread")
                        else:
                            assert metrics["active_io_backend"] == "none"
                        results.append(metrics)
        ids = [21, 4, 4, 600, 0, 999]
        ids_file = root / "ids.txt"
        ids_file.write_text("\n".join(map(str, ids)))
        base = [args.bench, store, "--ids-file", ids_file, "--io-backend", "pread"]
        for limit in (1, 4096, 1048576):
            metrics = checked(base + ["--batch-bytes", limit, "--rounds", 2])
            assert metrics["output_hash_fnv1a64"] == digest(records, ids)
            assert int(metrics["residues"]) == 2 * sum(len(records[index]) for index in ids)
        single_batch = checked([args.bench, store, "--sample-percent", 100])
        assert single_batch["output_hash_fnv1a64"] == paired["100"][1]
        sequential_hash = digest(records, range(len(records)))
        for storage_args, budget, mode in storage_cases:
            metrics = checked([args.bench, store, "--fast-sequential", "--requests", len(records),
                               "--threads", 4, "--io-backend", "pread"] + storage_args)
            assert metrics["storage_mode"] == mode
            assert metrics["resident_bytes"] == str(budget)
            assert metrics["id_pattern"] == "fast_sequential_range"
            assert metrics["output_hash_fnv1a64"] == sequential_hash
            assert metrics["checksum"] == expected_sum
            assert metrics["diagnostics_collected"] == "1"
            assert float(metrics["worker_decode_ms"]) > 0
        empty = checked([args.bench, store, "--requests", 0, "--drop-cache"])
        assert empty["batches_per_round"] == "0" and empty["residues"] == "0"
        assert empty["collect_diagnostics"] == "1" and empty["diagnostics_collected"] == "0"
        for extra in (["--sample-percent", "nan"], ["--sample-percent", "101"],
                      ["--sample-percent", "0"], ["--requests", "-1"],
                      ["--batch-bytes", "0"], ["--batch-bytes", "-1"],
                      ["--sample-percent", "10", "--requests", "1"],
                      ["--drop-cache", "--rounds", "2"],
                      ["--memory-bytes", "-1"],
                      ["--memory-bytes", "1", "--memory-mb", "0"],
                      ["--memory-bytes", "1", "--full-memory"],
                      ["--fast-sequential", "--batch-bytes", "1"]):
            result = subprocess.run([str(args.bench), str(store)] + extra,
                                    stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=10)
            assert result.returncode == 2, extra
        matrix = root / "matrix"
        runner = Path(__file__).resolve().parents[1] / "benchmarks/run_cold_matrix.py"
        checked([sys.executable, runner, store, matrix, "--bench", args.bench,
                 "--label", "synthetic_smoke_only", "--percents", "10", "100",
                 "--threads", "1", "4", "16", "64", "--prefix-percents", "0", "25", "50", "75",
                 "--backend", backends[-1], "--batch-bytes", 4096])
        assert (matrix / "COMPLETE").exists()
        with (matrix / "results.csv").open(newline="") as handle:
            rows = list(csv.DictReader(handle))
        assert len(rows) == 40
        assert {row["resident_percent"] for row in rows} == {"0", "25", "50", "75", "100"}
        assert all(row["status"] == "ok" for row in rows)
        assert all(row["collect_diagnostics"] == "1" and
                   row["diagnostics_collected"] == "1" for row in rows)
        for prefixes in (["100"], ["-1"], ["nan"], ["25", "25.0"]):
            result = subprocess.run([str(sys.executable), str(runner), str(store), str(root / "invalid"),
                                     "--label", "invalid", "--prefix-percents"] + prefixes,
                                    stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=10)
            assert result.returncode == 2, prefixes
            assert not (root / "invalid").exists()
        if args.output:
            (args.output / "cold_cases.json").write_text(json.dumps(results, indent=2) + "\n")
            for name in ("manifest.json", "results.csv", "COMPLETE"):
                (args.output / name).write_bytes((matrix / name).read_bytes())
    print("PASS: {} synthetic cold cases; batching, digests, CLI validation and matrix runner".format(len(results)))


if __name__ == "__main__":
    main()
