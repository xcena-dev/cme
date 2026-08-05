# CME

## Recovery-aware ownership coordination framework for atomic-free, non-coherent shared memory.

[![test](https://github.com/xcena-dev/cme/actions/workflows/test.yml/badge.svg)](https://github.com/xcena-dev/cme/actions/workflows/test.yml)
[![style](https://github.com/xcena-dev/cme/actions/workflows/style.yml/badge.svg)](https://github.com/xcena-dev/cme/actions/workflows/style.yml)
[![License](https://img.shields.io/badge/license-Apache--2.0-blue.svg)](LICENSE)
![C++17](https://img.shields.io/badge/C%2B%2B-17-blue.svg)
[![spec](https://img.shields.io/badge/spec-TLA%2B-blueviolet.svg)](docs/spec/)
![Status](https://img.shields.io/badge/status-experimental-orange.svg)

![Without CME, two hosts writing one cacheline race. With CME, one current owner writes.](docs/articles/figs/medium-cover.png)

CXL lets several hosts address the same memory.
It does not answer *"who may write this, right now?"*
CME, short for CXL Mutual Exclusion, answers it.

CME assumes *non-coherent* shared memory: multiple hosts with byte-addressable access to the same physical memory, no cross-host cache coherence, no cross-host atomics.
That is CXL 2.0 fabric-attached memory, and CXL 3.x wherever hardware coherence (BISnp / HDM-DB) is unsupported, unconfigured, or too costly to rely on.
Every classical synchronization primitive needs coherence or atomics.
CME needs neither.
Therefore it also runs unchanged where they exist.

Under that assumption, the answer is named, recoverable, exclusive ownership of logical domains, built from plain loads and stores, with no coordinator.

> [!WARNING]
> Experimental pre-release.
> The API, the on-region layout, and the recovery protocol are all subject to change.
> Not for production use.

## Quick start

The example uses devdax.
Swap the URI for `shm:/cme-demo` to run it on a single host without CXL hardware.

| URI scheme | Backend |
|---|---|
| `shm:/name` | POSIX shm, single host |
| `dax:/dev/daxN.M` | CXL devdax chardev |
| `file:/path` | a file on a mapped filesystem, uncacheable where the mount is |

One process sets the region up:

```cpp
#include "cme/cme.hpp"

cme::Session::OpenOpts_t how;
how.coherency = cme::CoherencyMode::Flush;   // devdax: cached, and outside the
                                             // coherence domain the peers share

cme::Session::FormatOpts_t opts;
opts.maxPeers   = 8;
opts.maxDomains = 8;
opts.strategy   = cme::Strategy::Request;    // recorded in the region; joiners
                                             // pick up the matching policy
cme::Session::format("dax:/dev/dax0.0", opts);

auto session = cme::Session::open("dax:/dev/dax0.0", how);
session.createDomain("inv");
session.joinDomain("inv");
```

Every other process attaches to what is already there:

```cpp
auto session = cme::Session::open("dax:/dev/dax0.0", how);
for (const auto& name : session.getDomainNames())
    session.joinDomain(name);           // lock() without this throws
```

From there the two are the same, and this is the part that repeats:

```cpp
{
    auto guard = session.lock("inv");   // throws LockTimeoutError on deadline
    // ... critical section on your own data ...
    cme::flush(buf, nbytes, how.coherency);   // publish before the Guard releases
}                                       // Guard dtor releases
```

Two things in the first block are easy to get wrong.
`format()` zeroes the region rather than creating it if absent, so only the process that sets the region up calls it.
And a data domain no peer has joined is swept as an orphan and its slot freed, which is why the creator joins the name it just made and keeps the Session.

Domains are discoverable but not self-describing: the region hands back names and arbitrates them, and what each one protects stays a convention between callers.

## Ownership, not just a lock

The API is a lock.
The semantics are not.
A lock is a transient bit held by a live process; ownership here is a durable record in the shared region:

- it outlives the death of its holder and is reclaimed rather than lost;
- membership changes under it at runtime;
- the arbitration discipline is a policy choice, not a fixed algorithm.

Nor is there a lock service in the picture.
`Session` is a peer slot inside the region, not a connection, and `lock()` is loads and stores against mapped memory — no server, no quorum, no lease.

The name says what CME provides.
It does not say what CME is.
CME is a coordination layer rather than an application, and it owns no user data.
It is built to sit under the data planes already running on CXL shared memory: a FAM-native filesystem, an MPI/OpenSHMEM transport, a distributed database.

## Why not something that already exists

**A lock service, such as ZooKeeper, etcd, or a distributed mutex?**
Those arbitrate over a network, so an acquire costs a round trip to a service that sits outside the data path.
CME arbitrates in the shared memory the peers already map, so an acquire is loads and stores rather than a message.

**RDMA atomics, or a fabric compare-and-swap?**
CME targets memory where cross-host atomics are unavailable.
Where they are available, Section 11.3 of the [design record](docs/design/technical_report.md) measures CME against an RDMA ticket lock.

**CXL 3.x hardware coherence (BISnp / HDM-DB)?**
Use it where you have it.
CME exists for the deployments where it is unsupported, unconfigured, or too costly to depend on, and it runs unchanged on top of coherent memory.

## Architecture

![CME layering: public API, coordination engine, ownership state machine over CXL shared memory, with the policies consulted from the side](docs/articles/figs/medium-framework.png)

CME is four parts, left to right: the public API a peer calls, a per-peer coordination engine that drives it autonomously, the state machine defining ownership × membership state and the events that move it, and the geometry that persists that state as records in shared memory.

Policies sit outside that chain.
The engine *consults* them — for a successor, a liveness verdict, a recovery authority — and they manage only their own private storage.
A policy never writes the ownership record, which is why the invariants can live in the core while the policy stays interchangeable.

Skim the diagram for the shape.
Section 5 of the [design record](docs/design/technical_report.md) names each part.

## Failure model

CME assumes crash-stop: a peer declared dead is assumed to have stopped.
It cannot make that true on its own.
Recovery is safe against a declared-dead peer that only reads; one that still writes is stopped only from below, by a fabric access revoke, a host reset, or the OS unmapping the region.
**CME decides who is recovered; the platform enforces the stop.**

## Build, test, install

```sh
cmake -S . -B build && cmake --build build -j
ctest --test-dir build
cmake --install build --prefix /usr/local
```

That is everything on a machine with no CXL hardware.
Each test case is registered on all three backends, and one this machine cannot provide reports skipped rather than failed, so the shm suite runs anywhere with no arguments.

Reaching devdax or an uncacheable mount takes one more file:

```sh
cp config.example.yaml config.yaml
```

[`BUILD.md`](BUILD.md) has the rest: the config keys, the build options, devdax setup, and how to run the benchmarks.

## Documentation

**Using CME**

| | |
|---|---|
| What to include | [`include/cme/cme.hpp`](include/cme/cme.hpp) |
| The API, with the usage model in its header doc | [`include/cme/shared.hpp`](include/cme/shared.hpp) |
| Build, test, benchmark, devdax setup | [`BUILD.md`](BUILD.md) |
| Runnable examples | [`examples/`](examples/) |

**Understanding CME**

| | |
|---|---|
| Design record — why it is shaped this way | [`docs/design/technical_report.md`](docs/design/technical_report.md) |
| Normative specification, machine-checked | [`docs/spec/`](docs/spec/) |
| Article drafts, derived from the design record | [`docs/articles/`](docs/articles/) |
| The vocabulary, in the sense used here | [`docs/GLOSSARY.md`](docs/GLOSSARY.md) |

`docs/spec/` is normative; `src/core/` implements it.

**Taking part**

| | |
|---|---|
| Style rules, tests to run, what a PR needs | [`CONTRIBUTING.md`](CONTRIBUTING.md) |
| How to cite it | [`CITATION.cff`](CITATION.cff) |

## Troubleshooting

**A devdax run takes `SIGBUS` on its first store.**

The mapping succeeded and the first write killed the process, which points at the huge page rather than at the device.
A devdax chardev is mapped at PMD granularity, so the fault path has to install a 2 MiB page.
A process carrying `PR_SET_THP_DISABLE` cannot, and the flag is inherited, so a shell started with it hands the condition to everything it launches.
The error says nothing about any of this.

`tests/harness/thp_exec.cpp` builds a wrapper that clears the flag and then execs what it was given:

```sh
build/tests/thp_exec build/tools/cme-top dax:/dev/dax0.0
```

An ordinary login shell does not need it, which is why no other example here uses it.
Reach for it when a devdax mapping dies on the first touch and `daxctl list` says the node is fine.
`ctest` and the benchmark scripts already run everything through it, so a case that passes under `ctest` and dies when run by hand is this and nothing else.

## License

Apache-2.0. Copyright XCENA.
