#!/usr/bin/env python3
"""Swimlane timeline for cme handoff event trace (CME_LATENCY).

Collect once (full trace), draw any window:
    build-trace/tests/cme-fairness-test -n 4 -d 2 -t 1 -i 200 --request \\
        --trace-jsonl /tmp/trace.jsonl
    tests/sweep/latency_trace.py /tmp/trace.jsonl --from-us 0 --window-us 2000 -o /tmp/trace.png

One row per lane = (peer, role, tid, domain). Each bar is a MEASURED span with its
own [t0,t1] (1:1 with the latency breakdown counters); un-instrumented time is
left blank, so a gap means "no protocol work measured here", not a phase. Nested
spans (e.g. unlock over scan+clear) draw longest-first so inner spans stay
visible on top.
"""
import argparse
import json
import sys
from collections import defaultdict

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.patches import Patch

# Stage -> (label, color). Keys are the LatencyStage enum names verbatim (the JSONL
# "stage" field == the OBSERVE_LATENCY_BEGIN/END macro argument), so a bar's legend
# label is exactly the macro to look up in latency.hpp. Each bar is a measured span
# [t0,t1] emitted next to its CME_LAT_ADD, so the width equals the breakdown delta.
# High-contrast hues so adjacent slivers stay visible.
STAGE = {
    "Fence":         ("Fence",         "#56b4e9"),  # sky blue
    "Hold":          ("Hold",          "#e69f00"),  # orange
    "Raise":         ("Raise",         "#cc79a7"),  # purple-pink (request: raise doorbell)
    "Spin":          ("Spin",          "#0072b2"),  # blue (dominant wait)
    "AdoptLocal":    ("AdoptLocal",    "#009e73"),  # green
    "Scan":          ("Scan",          "#f0e442"),  # yellow
    "Clear":         ("Clear",         "#777777"),  # gray
    "Unlock":        ("Unlock",        "#bbbbbb"),  # light gray (outer release span)
    "ClimbAnnounce": ("ClimbAnnounce", "#9467bd"),  # purple (peterson: announce phase)
    "ClimbSpin":     ("ClimbSpin",     "#c7519c"),  # magenta (peterson: busy-spin wait)
    "Release":       ("Release",       "#8c564b"),  # brown (peterson: post-wait cleanup)
}
def load(path):
    ghz = None
    recs = []
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            o = json.loads(line)
            if "meta" in o:
                ghz = o["meta"]["ghz"]
                continue
            recs.append(o)
    if ghz is None or ghz <= 0:
        sys.exit("no/invalid ghz in trace meta line")
    if not recs:
        sys.exit("no event records")
    return ghz, recs


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("jsonl")
    ap.add_argument("--from-us", type=float, default=0.0, help="window start (us since trace t0)")
    ap.add_argument("--window-us", type=float, default=0.0, help="window length us (0 = full)")
    ap.add_argument("-o", "--out", default="trace.png")
    args = ap.parse_args()

    ghz, recs = load(args.jsonl)
    # t0 over every span start (everything is a span now).
    all_ts = [r["t0"] for r in recs]
    base = min(all_ts)
    def to_us(tsc):
        return (tsc - base) / ghz / 1000.0

    lo = args.from_us
    hi = lo + args.window_us if args.window_us > 0 else float("inf")

    def clip(a, b):  # clamp [a,b] to window, or None if outside
        if b < lo or a > hi:
            return None
        return max(a, lo), min(b, hi)

    # Lanes split per domain (a worker thread is serial, so each domain's spans
    # are a clean subsequence) -- without this, dom0/dom1 overlap in one lane.
    lanes = defaultdict(lambda: {"spans": defaultdict(list)})
    for r in recs:
        key = (r["peer"], r["role"], r["tid"], r["domain"])
        lanes[key]["spans"][r["stage"]].append((r["t0"], r["t1"]))

    def lane_key(k):
        peer, role, tid, dom = k
        return (peer, 0 if role == "worker" else 1, tid, dom)
    order = sorted(lanes.keys(), key=lane_key)

    fig, ax = plt.subplots(figsize=(15, 0.55 * len(order) + 1.5))
    ylabels = []
    for row, key in enumerate(order):
        peer, role, tid, dom = key
        ylabels.append(f"p{peer} {role}{tid} d{dom}" if role == "worker"
                       else f"p{peer} {role}{tid}")
        lane = lanes[key]

        # Measured stage spans. Some nest (e.g. unlock brackets scan +
        # clear), so flatten every span and draw longest-first: the outer span
        # lays down as background, shorter inner spans paint over it on top.
        spans = []  # (dur_us, x0_us, color)
        for stage, pairs in lane["spans"].items():
            meta = STAGE.get(stage)
            if meta is None:
                continue
            for t0, t1 in pairs:
                c = clip(to_us(t0), to_us(t1))
                if c:
                    spans.append((max(c[1] - c[0], 0.0), c[0], meta[1]))
        for dur, x0, color in sorted(spans, key=lambda s: s[0], reverse=True):
            ax.broken_barh([(x0, dur)], (row - 0.4, 0.8),
                           facecolors=color, edgecolors="none")

    ax.set_yticks(range(len(order)))
    ax.set_yticklabels(ylabels, fontsize=8)
    ax.invert_yaxis()
    ax.set_xlabel("us since trace t0")
    if hi != float("inf"):
        ax.set_xlim(lo, hi)
    ax.set_title(f"cme handoff trace  ({len(recs)} recs, {len(order)} lanes, tsc={ghz:.3f} GHz)")
    legend = [Patch(facecolor=color, label=label) for (label, color) in STAGE.values()]
    ax.legend(handles=legend, loc="upper center", bbox_to_anchor=(0.5, -0.10),
              ncol=6, fontsize=7, frameon=False)
    fig.tight_layout()
    fig.savefig(args.out, dpi=130, bbox_inches="tight")
    print(f"wrote {args.out}  (window [{lo}, {hi}] us, {len(order)} lanes)")


if __name__ == "__main__":
    main()
