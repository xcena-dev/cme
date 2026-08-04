#!/usr/bin/env python3
# sweep_plot.py -- draw the cross-backend/strategy UC sweep as a figure.
#
# Reads the CSVs produced by bench_sweep.sh (cols:
# peers,domains,mean_us,p50_us,p90_us,p99_us,samples) and plots latency vs peers,
# one subplot per domain count. Color encodes strategy, line style encodes backend
# (uc/file = solid, shm = dashed) so the two backends of a strategy sit side by side.
# Top row = mean_us, bottom row = p99_us. Series label is taken from the file basename
# sweep_<backend>_<strategy>.csv unless given explicitly as label=path.
#
# usage: sweep_plot.py [label=]CSV ... -o OUT.png [--title T]
import csv
import os
import sys
import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt


def label_of(arg):
    if "=" in arg and not os.path.exists(arg):
        lbl, _, path = arg.partition("=")
        return lbl, path
    base = os.path.basename(arg)
    if base.startswith("sweep_") and base.endswith(".csv"):
        base = base[len("sweep_"):-len(".csv")]
    return base, arg


def load(path):
    d = {}  # (peers,dom) -> row dict(float)
    try:
        with open(path) as f:
            for r in csv.DictReader(f):
                try:
                    d[(int(r["peers"]), int(r["domains"]))] = {
                        k: float(r[k]) for k in ("mean_us", "p50_us", "p90_us", "p99_us")
                    }
                except (ValueError, KeyError):
                    pass
    except FileNotFoundError:
        pass
    return d


def main():
    args = sys.argv[1:]
    out = "sweep.png"
    title = "CME UC sweep -- latency vs peers"
    xmode = "peers"  # x-axis: "peers" (facet per domain) or "domains" (facet per peer)
    items = []
    i = 0
    while i < len(args):
        a = args[i]
        if a == "-o":
            out = args[i + 1]; i += 2
        elif a == "--title":
            title = args[i + 1]; i += 2
        elif a == "--x":
            xmode = args[i + 1]; i += 2
        else:
            items.append(a); i += 1
    if not items:
        print("usage: sweep_plot.py [label=]CSV ... -o OUT.png [--title T] [--x peers|domains]",
              file=sys.stderr)
        return 2
    if xmode not in ("peers", "domains"):
        print(f"bad --x {xmode!r} (want peers|domains)", file=sys.stderr)
        return 2

    # Split "backend/strategy" (or "backend_strategy") label into its parts; a bare
    # label has no backend. "file" backend is shown as "uc" (real UC mmap).
    def split_label(lbl):
        for sep in ("/", "_"):
            if sep in lbl:
                b, _, s = lbl.partition(sep)
                return b, s
        return "", lbl

    def disp_backend(b):
        return "uc" if b == "file" else b

    series = []  # (label, backend, strategy, data)
    for a in items:
        lbl, path = label_of(a)
        data = load(path)
        if data:
            b, s = split_label(lbl)
            disp = f"{disp_backend(b)}/{s}" if b else s
            series.append((disp, b, s, data))
        else:
            print(f"skip (no data): {path}", file=sys.stderr)
    if not series:
        print("no data in any CSV", file=sys.stderr)
        return 1

    # x-axis varies; the other dimension becomes one subplot column (facet).
    if xmode == "peers":
        facets = sorted({d for *_, data in series for (_, d) in data})
        xvals = sorted({p for *_, data in series for (p, _) in data})
        cell_key = lambda x, f: (x, f)      # data keyed (peers, dom)
        facet_title = lambda f: f"domains={f}"
        xlabel = "peers"
    else:
        facets = sorted({p for *_, data in series for (p, _) in data})
        xvals = sorted({d for *_, data in series for (_, d) in data})
        cell_key = lambda x, f: (f, x)
        facet_title = lambda f: f"peers={f}"
        xlabel = "domains"
    metrics = [("mean_us", "mean latency (us)"), ("p99_us", "p99 latency (us)")]

    ncol = max(1, len(facets))
    fig, axes = plt.subplots(
        len(metrics), ncol, figsize=(3.2 * ncol, 7), squeeze=False, sharex=True
    )
    # Color = strategy (stable across backends), line style + marker = backend.
    strats_seen, backends_seen = [], []
    for _, b, s, _ in series:
        if s not in strats_seen:
            strats_seen.append(s)
        if b not in backends_seen:
            backends_seen.append(b)
    cmap = plt.get_cmap("tab10")
    colors = {s: cmap(i % 10) for i, s in enumerate(strats_seen)}
    ls_cycle, mk_cycle = ["-", "--", "-.", ":"], ["o", "s", "^", "D"]
    known_ls = {"file": "-", "shm": "--"}
    known_mk = {"file": "o", "shm": "s"}
    styles = {b: known_ls.get(b, ls_cycle[i % 4]) for i, b in enumerate(backends_seen)}
    markers = {b: known_mk.get(b, mk_cycle[i % 4]) for i, b in enumerate(backends_seen)}

    for row, (mkey, mlabel) in enumerate(metrics):
        for col, facet in enumerate(facets):
            ax = axes[row][col]
            for lbl, b, s, data in series:
                xs, ys = [], []
                for x in xvals:
                    cell = data.get(cell_key(x, facet))
                    if cell:
                        xs.append(x); ys.append(cell[mkey])
                if xs:
                    ax.plot(xs, ys, marker=markers.get(b, "o"), ms=3, lw=1.3,
                            color=colors[s], ls=styles.get(b, "-"), label=lbl)
            ax.set_xscale("log", base=2)
            ax.set_xticks(xvals)
            ax.set_xticklabels([str(x) for x in xvals], fontsize=7)
            ax.grid(True, ls=":", lw=0.5, alpha=0.6)
            ax.tick_params(labelsize=7)
            if row == 0:
                ax.set_title(facet_title(facet), fontsize=9)
            if row == len(metrics) - 1:
                ax.set_xlabel(xlabel, fontsize=8)
            if col == 0:
                ax.set_ylabel(mlabel, fontsize=8)

    handles, labels = axes[0][0].get_legend_handles_labels()
    fig.legend(handles, labels, loc="upper center", ncol=min(len(series), 6),
               fontsize=8, frameon=False, bbox_to_anchor=(0.5, 0.99))
    fig.suptitle(title, y=1.04, fontsize=11)
    fig.tight_layout(rect=[0, 0, 1, 0.96])
    fig.savefig(out, dpi=130, bbox_inches="tight")
    print(f"figure -> {out}  ({len(series)} series, {xlabel}-axis, facets={facets})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
