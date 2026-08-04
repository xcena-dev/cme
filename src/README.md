# src/ — implementation map

The public API in [`include/cme/`](../include/cme/) is a thin surface.
Everything below it lives here.

Reading order for a newcomer: `shared.cpp` for what a caller triggers, then `core/algo/` for what happens next, then `core/layout/` for what lands in shared memory.

| | |
|---|---|
| `shared.cpp` | `Session` and `Guard`. The entry point every public call passes through. |
| `config.hpp` | Tunable algorithm constants: timeouts, poll intervals, retry counts. One place to look when a deadline needs changing. |

## Subdirectories

| | |
|---|---|
| [`core/algo/`](core/algo/) | The algorithms: membership lifecycle, ownership transfer, the per-peer engine, the recovery state machine. |
| [`core/policy/`](core/policy/) | The interchangeable parts: who succeeds, who is alive, who may recover. |
| `core/layout/` | `Geometry`: the on-region record types and the `format()` writer. Everything persisted to shared memory has its layout defined here. |
| `core/runtime/` | Per-peer DRAM state that is *not* authoritative. `LocalPeerState` and `LocalDomainView` hold a peer's own belief about ownership; the record in `core/layout/` settles it. |
| `core/` | `types.hpp` for private identifier types, `domain_bitmap.hpp` for the packed domain-id set backing the participation and demand bitmaps. |
| `admission/` | Peer-slot claim, on the `Session::open` path. A process takes a slot in the region before it can own anything. |
| `memory/` | The mmap backends. `shm.cpp`, `dax.cpp`, and `file.cpp` each turn a URI into mapped bytes; `memory.cpp` picks between them. Layout interpretation is not here, it is in `core/layout/`. |
| [`observe/`](observe/) | Instrumentation. Counters, stderr tracing, latency breakdown, and a read-only region inspector, each independently compiled in or out. |
| `serializers/` | `SharedSession`, which lets many threads of one process share a single `Session`. |
| `util/` | Cross-cutting helpers: `coherency.hpp` for the cross-host visibility fences, `endian.hpp` for tear-free access to region-resident scalars, plus time and small utilities. |

## What is authoritative

Two directories hold state and they are not equal.

`core/layout/` defines the records in shared memory, and those records decide ownership.
`core/runtime/` holds a peer's local view, which is a cache and a poll filter.
When the two disagree, the record wins.

[`docs/spec/`](../docs/spec/) is normative and `core/` implements it.
