# src/observe/ — instrumentation

Four axes, each compiled in or out on its own.
Call sites do not know which are enabled.

```cpp
OBSERVE_EVENT(Event::RecoveryTakeover, peerState, domainId);
```

`observe.hpp` fans one `OBSERVE_EVENT` out to every axis through an overload set.
An axis that has no overload for a given event falls back to a no-op template, so adding an event does not require touching all four.

| axis | build flag | what it costs when off |
|---|---|---|
| stats | `CME_STATS` | `stats.cpp` compiles empty stubs, which link-time optimization elides. |
| logging | `CME_LOGGING` | `logging.cpp` compiles empty stubs. |
| latency | `CME_LATENCY` | The lane registry is not populated. |
| inspector | always available | Nothing; it is a separate reader, not a call site. |

| | |
|---|---|
| `observe.hpp` | The dispatcher. Header-only, and there is no `observe.cpp`. |
| `event.hpp` | The `Event` enum and its tag dispatcher, kept separate from `observe.hpp` to break a circular include. |
| `telemetry.hpp` | `PeerTelemetry_t`, the per-peer atomic counters. Fields are always allocated; the build flags decide only whether anything bumps them. |
| `stats.{hpp,cpp}` | Per-event counter bumps. The header is invariant and the gate is in the `.cpp`. |
| `logging.{hpp,cpp}` | Per-event stderr trace, same header-invariant arrangement. |
| `latency.{hpp,cpp}` | Handoff-latency breakdown plus a per-thread event trace, dumped as JSONL for [`tests/sweep/latency_trace.py`](../../tests/sweep/latency_trace.py). |
| `inspector.{hpp,cpp}` | Read-only observer of a live region, for monitoring and forensic dumps. Rebinds `Geometry` before each sample and returns `nullopt` on a window it cannot read. Backs `cme-top`. |

## One caution

An observation that runs on the hot path changes what it measures.
A syscall in the acquire path costs more than the handoff it is timing, so the number it reports is mostly itself.
Prefer a counter bump, and put anything heavier behind `CME_LATENCY` where the cost is opt-in.
