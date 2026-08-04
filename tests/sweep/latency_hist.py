#!/usr/bin/env python3
# latency_hist.py -- compact histogram + cliff summary for fairness-test --lat-csv dumps.
#
# Reads a one-column CSV of per-acquire latencies (ns, header "latency_ns") and
# prints a SMALL fixed-size summary: percentiles, a coarse bucket histogram, and
# (given --window-us) the hot/sleep split + nanosleep-cliff detection. Raw data
# never leaves the script -- only the summary is printed, so it stays token-cheap
# even for millions of samples.
#
# usage: latency_hist.py CSV [--window-us W] [--label L]
import sys
import numpy as np


def main() -> int:
    args = sys.argv[1:]
    if not args:
        print("usage: latency_hist.py CSV [--window-us W] [--label L]", file=sys.stderr)
        return 2
    path = args[0]
    window = None
    label = path
    i = 1
    while i < len(args):
        if args[i] == "--window-us":
            window = float(args[i + 1]); i += 2
        elif args[i] == "--label":
            label = args[i + 1]; i += 2
        else:
            i += 1

    v = np.loadtxt(path, skiprows=1) / 1000.0  # ns -> us
    v = np.atleast_1d(v)
    n = len(v)
    if n == 0:
        print(f"{label}: empty"); return 0

    p = lambda q: np.percentile(v, q)
    head = (f"{label}: n={n} mean={v.mean():.1f} p50={p(50):.1f} "
            f"p90={p(90):.1f} p99={p(99):.1f} p999={p(99.9):.1f} max={v.max():.1f} us")

    edges = [0, 2, 5, 10, 20, 30, 50, 75, 100, 150, 200, 300, 500, 1000, 2000, np.inf]
    h, _ = np.histogram(v, bins=edges)
    rows = []
    for j in range(len(h)):
        if h[j]:
            hi = "inf" if edges[j + 1] == np.inf else f"{edges[j+1]:.0f}"
            rows.append(f"  [{edges[j]:>5.0f},{hi:>5}) us : {h[j]:>8d}  {100*h[j]/n:5.1f}%")

    extra = ""
    if window is not None:
        hot = int((v < window).sum())
        slp = n - hot
        # nanosleep cliff = fraction landing in the typical 50-120us sleep floor
        # among the sleep-phase samples (only meaningful when window < that floor)
        cliff = int(((v >= window) & (v >= 50) & (v < 150)).sum())
        extra = (f"\n  window={window:.0f}us  hot={100*hot/n:.1f}%  "
                 f"sleep={100*slp/n:.1f}%  sleep@[50,150)us={100*cliff/n:.1f}%")

    print(head)
    print("\n".join(rows))
    if extra:
        print(extra)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
