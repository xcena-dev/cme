#!/usr/bin/env python3
"""Run the cmed tiered bench over a grid and print it the way the libcme sweep prints its own.

The point of matching the layout is that a cell here and the cell with the same peers/threads/domains
there differ by one thing, which is the daemon in the path.

  sweep_tiered.py --total 64 --domains 1,4,16,32,63 --splits 2,4,8 --iters 200

--total is peers x threads, held fixed so every cell carries the same load. A split that does not divide
it is skipped rather than run at a different load.
"""
import argparse
import csv
import pathlib
import subprocess
import sys
import tempfile

STRATEGIES = ("request", "peterson")


def cell(bench, strategy, nodes, threads, domains, iters, uri, coherency, csvPath):
    """One bench run. Returns (mean_us, p99_us) or None when the run failed."""
    before = rows(csvPath)
    outcome = subprocess.run(
        [
            str(bench),
            "--strategy", strategy,
            "--peers", str(nodes),
            "--threads", str(threads),
            "--domains", str(domains),
            "--iters", str(iters),
            "--uri", uri,
            "--coherency", coherency,
            "--csv", str(csvPath),
        ],
        capture_output=True,
        text=True,
        cwd=str(bench.parent),
    )
    after = rows(csvPath)
    if outcome.returncode != 0 or len(after) == len(before):
        tail = (outcome.stdout + outcome.stderr).strip().splitlines()
        print(f"    ({strategy} {nodes}p x {threads}t {domains}dom FAILED: {tail[-1] if tail else '?'})")
        return None
    return float(after[-1]["mean_us"]), float(after[-1]["p99_us"])


def rows(csvPath):
    if not csvPath.exists():
        return []
    with csvPath.open() as handle:
        return list(csv.DictReader(handle))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--bench", default="build/daemon/tests/cmed-tiered-lock-bench")
    parser.add_argument("--total", type=int, default=16)
    parser.add_argument("--domains", default="1,4,16")
    parser.add_argument("--splits", default="2,4")
    parser.add_argument("--iters", type=int, default=200)
    parser.add_argument("--uri", default="shm:/cmed-tiered-bench")
    # A bare open assumes cache_coherent, which is the wrong barrier discipline on devdax and on an
    # uncacheable mount, so a --uri that names one of those needs this too.
    parser.add_argument("--coherency", default="cache_coherent")
    chosen = parser.parse_args()

    bench = pathlib.Path(chosen.bench).resolve()
    if not bench.exists():
        print(f"no bench at {bench}")
        return 1

    domains = [int(text) for text in chosen.domains.split(",")]
    splits = [int(text) for text in chosen.splits.split(",")]

    print(f"=== cmed tiered (total={chosen.total}, domains={domains}) -- mean_us / p99_us ===")
    width = 18
    header = "%-5s %-4s %-4s | " % ("peers", "thr", "dom")
    header += "| ".join("%-*s" % (width, name) for name in STRATEGIES) + "| winner"
    print(header)
    print("-" * len(header))

    with tempfile.TemporaryDirectory() as scratch:
        csvPath = pathlib.Path(scratch) / "tiered.csv"
        with csvPath.open("w") as handle:
            handle.write("peers,threads,domains,mean_us,p50_us,p90_us,p99_us,samples\n")

        for nodes in splits:
            threads, remainder = divmod(chosen.total, nodes)
            if remainder != 0 or threads < 1:
                continue
            for count in domains:
                measured = {
                    strategy: cell(bench, strategy, nodes, threads, count, chosen.iters,
                                   chosen.uri, chosen.coherency, csvPath)
                    for strategy in STRATEGIES
                }

                cells = ""
                for strategy in STRATEGIES:
                    got = measured[strategy]
                    text = "-" if got is None else "%-7.1f %-7.1f" % got
                    cells += "%-*s| " % (width, text)

                known = {name: got for name, got in measured.items() if got is not None}
                if len(known) < 2:
                    verdict = "-"
                else:
                    best = min(known, key=lambda name: known[name][0])
                    other = [name for name in known if name != best][0]
                    gain = (known[other][0] / known[best][0] - 1.0) * 100.0
                    verdict = "%s  (%.0f%%)" % (best, gain)

                print("%-5d %-4d %-4d | %s%s" % (nodes, threads, count, cells, verdict))
                sys.stdout.flush()

    return 0


if __name__ == "__main__":
    sys.exit(main())
