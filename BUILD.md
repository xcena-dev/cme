# Build and test

## Build

```sh
cmake -S . -B build && cmake --build build -j
ctest --test-dir build
cmake --install build --prefix /usr/local
```

The install step lays down `libcme.a`, the headers under `cme/`, `cme-top`, and a pkg-config file:

```sh
c++ $(pkg-config --cflags libcme) myapp.cpp $(pkg-config --libs libcme)
```

Requires a C++17 compiler.
Nothing else is needed for the default build, and the plotting scripts in [`../tests/sweep/`](tests/sweep/) are the only part that wants Python and matplotlib.

## CMake options

Four instrumentation axes, all off by default.
Each compiles its call sites down to nothing when off, so leaving them off costs no branches.

| option | default | effect |
|---|---|---|
| `CME_STATS` | `OFF` | Bump the per-peer counters in `PeerTelemetry_t` from the event handlers. |
| `CME_LOGGING` | `OFF` | Emit a stderr trace line per event. |
| `CME_LATENCY` | `OFF` | Handoff-latency breakdown and per-thread event trace, dumped as JSONL. |
| `CME_PROFILE` | `OFF` | Accumulate worker CPU time and publish it to the profile slot. |

[`../src/observe/`](src/observe/) explains how the four axes dispatch.

One more option is not instrumentation:

| option | default | effect |
|---|---|---|
| `CME_NATIVE_ARCH` | `ON`, or `native_arch` from `config.yaml` | Build with `-march=native`. |

Read the note above the option in `CMakeLists.txt` before turning it off.
`-march=native` widens a 64-byte slot access to one AVX-512 transaction, and under SSE2 the same access becomes four 16-byte pieces.
A torn 64-byte slot is a correctness problem rather than a slower one.
So this option is semantics, and turning it off means re-checking slot atomicity on the target instruction set.

## config.yaml

Per-site facts live in `config.yaml`, which is not tracked.
Copy the template and fill in what this machine has:

```sh
cp config.example.yaml config.yaml
```

The file is a flat `key: value` subset of YAML, deliberately, because three independent readers parse it without a YAML library: `cmake/SiteConfig.cmake` for the build, `tests/harness/site_config.hpp` for the test harness, and [`../tests/harness/site_config.sh`](tests/harness/site_config.sh) for the scripts.

| key | used for |
|---|---|
| `dax_device` | The devdax node the dax tests and benchmarks open. An empty or absent value skips them. |
| `file_backend_dir` | A directory on a mapped filesystem, for the uncacheable tests. It must be a mount point; an unmounted directory is skipped. |
| `dax_slot_reserve` | Bytes at the end of `dax_device` set aside for test slots. Default 1 GiB. |
| `dax_slot_base` | A byte offset to pin slot 0. Leave it empty and the offset is derived from the device size. |
| `native_arch` | Default for `CME_NATIVE_ARCH`. |

Values are read at run time rather than baked in at configure time, so pointing the tree at a different device needs no reconfigure.

An empty key is not a failure.
It means this machine does not have that medium, and the cases needing it report skipped.

## devdax setup

`daxctl list` names the nodes and reports the mode:

```sh
daxctl list
```

A node must be in `devdax` mode and appear as a character device under `/dev/`.
Put its path in `dax_device`.

Test slots are placed in the reserved tail of the device, not at offset 0.
With `dax_slot_base` empty, slot 0 starts at `(size - dax_slot_reserve)` rounded down to a 2 MiB boundary, and each slot is 2 MiB.
Therefore a filesystem may occupy the front of the same device while the tests use the tail.
Set `dax_slot_base` explicitly when that filesystem reaches further than `dax_slot_reserve` leaves room for.

## Test

Every case is registered on all three backends, and one this machine cannot provide exits 77 and reports skipped rather than failed.
So `ctest` runs the shm suite anywhere, with no arguments and no hardware.

```sh
ctest --test-dir build                  # everything the machine can run
ctest --test-dir build -L shm           # one medium
ctest --test-dir build -R recovery      # one family
```

A hosted runner has neither a devdax node nor an uncacheable mount, so it can only ever run the shm third of that.
[`../tests/local_ci.sh`](tests/local_ci.sh) runs the rest on a machine that has the hardware, and refuses to start when a medium is absent rather than skipping it and reporting green:

```sh
./tests/local_ci.sh
```

[`../tests/README.md`](tests/README.md) is the catalogue: what each scenario puts under stress and what it asserts.
How a case is declared, and which backends and labels it gets, is in `tests/CMakeLists.txt`.

## Benchmark

[`../tests/bench/`](tests/bench/) holds the microbenchmarks, one per isolated cost.
[`../tests/sweep/`](tests/sweep/) holds the drivers that sweep the grid and draw the figures.
[`../tests/baseline/`](tests/baseline/) holds the external reference point those numbers are read against.

```sh
./tests/sweep/run_sweep.sh                                        # shm, file, dax
./tests/sweep/run_sweep.sh --backends shm --strategies request --no-seqlat --no-tierlat
./tests/bench/cacheline_bench.sh                                  # uncacheable vs write-back
```

Each benchmark has a script beside it named after it, so `tests/bench/read_tail_bench.cpp` is run by `tests/bench/read_tail_bench.sh`.
[`../tests/bench/README.md`](tests/bench/README.md) lists them and the options they take.

A sweep configures its own build tree, so pass `--build` to keep it away from one you are using.
[`../tests/sweep/README.md`](tests/sweep/README.md) lists the axes and the plotting scripts.

## Multiple hosts

The test suite runs peers as separate processes on one host, sharing one region.
That exercises the protocol, since a peer is a process rather than a thread and nothing in the algorithm assumes a shared address space.

It does not exercise the fabric.
Running peers on separate hosts needs the region on memory both hosts map, and the harness does not set that up or measure the cross-host timing.
Section 11 of the [design record](docs/design/technical_report.md) states which of its numbers come from one host and which do not.
