# sweep/ — the grid drivers and the plotters

A driver here runs one binary over the same (peers × domains) grid many times and writes a CSV.
A plotter turns that CSV into a figure.
The split matters because a sweep takes minutes and a figure takes a second, so the two are re-run at different rates.

Every device path and mount comes from `config.yaml`, read at run time through [`../harness/site_config.sh`](../harness/site_config.sh).
A medium this machine does not have is skipped with a message rather than failing.

## Drivers

| | |
|---|---|
| `run_sweep.sh` | The sweep. Runs each (backend × strategy) over the same peers × domains grid, prints one cross-strategy table per backend, and draws the comparison figures. `--backends` defaults to `shm file dax`. |
| `bench_sweep.sh` | One (backend × strategy) grid for a single binary, which is what `run_sweep.sh` calls per cell. Useful alone when you want one row rather than the whole table. |
| `stress_recovery.sh` | Repeat-runs `cme-recovery-test` and keeps only the failing logs. It halts at the first failure so the state is still there to look at; `--no-halt-on-fail` keeps going. |

`run_sweep.sh` takes its axes as options, since those say what to measure this time rather than what this machine has:

```sh
./tests/sweep/run_sweep.sh --backends shm --strategies request --no-seqlat --no-tierlat
./tests/sweep/run_sweep.sh --backends dax --trace
```

`--build` and `--tbuild` name the build trees, which keeps a sweep from reconfiguring one you are using.

## Plotting

| | |
|---|---|
| `sweep_plot.py` | Draws a sweep CSV as a faceted figure, `--x peers` or `--x domains`. |
| `latency_trace.py` | Turns a `CME_LATENCY` JSONL trace into a per-stage swimlane over a chosen time window. |
| `latency_hist.py` | Compact histogram and cliff summary for a `--lat-csv` dump. |

These need matplotlib.
Point `--python` at an interpreter that has it if the default `python3` does not.

[`../baseline/rdma_lock.sh`](../baseline/rdma_lock.sh) writes the same CSV schema on purpose, so `sweep_plot.py` draws its figures too and the two land side by side.
