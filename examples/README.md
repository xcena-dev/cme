# examples/ — runnable programs against the public API

Three programs, each showing something the one before it does not.
They link `cme` and nothing from `tests/`, so they compile exactly as your own code does.

Every one defaults to a POSIX shm URI, so they run on any machine with no CXL hardware and no `config.yaml`.
Pass a URI as the only argument to run one somewhere else.

## Build and run

```sh
cmake -S . -B build && cmake --build build -j
build/examples/cme-example-hello
build/examples/cme-example-multi-process
build/examples/cme-example-threads
```

They are registered with ctest, so this runs all three and checks the results:

```sh
ctest --test-dir build -R '^example_'
```

## What each one shows

### `01_hello_lock.cpp` → `cme-example-hello`

The smallest program that takes a lock: format, open, create a domain, join it, hold it.

```
domain: inventory
holding 'inventory'
released
```

### `02_multi_process.cpp` → `cme-example-multi-process`

Four processes increment one counter under one domain.
This is the shape the library exists for, since a peer is a process rather than a thread and each child claims its own peer slot.
The counter lives in a mapping the program makes itself, because CME owns no user data and only decides who may write.

```
counter = 800, expected 800
```

Take the lock out and the total comes up short, which is the whole demonstration.

### `03_threads.cpp` → `cme-example-threads`

Eight threads of one process, over one peer slot, using `SharedSession`.

```
counter = 4000, expected 4000
```

The ownership token is per peer, and a process is one peer.
So `Session::lock` does not exclude a process from itself: a second thread finds the domain already resident and walks into the critical section without throwing and without timing out.
`SharedSession` adds the intra-node tier that closes the gap.
Reach for it whenever more than one thread of a process locks the same domain.

## Two behaviours that surprise people

**`format()` zeroes the region.**
It is not a create-if-absent.
Only whoever creates the region calls it; a process joining one that is already live calls `open()` alone.
Each example owns its own shm name and formats it on entry, which is why re-running one is safe and why running two at once is not.

**A domain nobody participates in is reclaimed.**
The poll thread sweeps data domains with no participants and frees the slot, so `createDomain` followed by nothing leaves a name that disappears underneath you.
The creator joins and stays for as long as the name has to exist.

## Not here

Recovery has no API surface: nothing you write makes it happen, and nothing you write can observe it except by finding that a lock is available when its holder died.
[`tests/`](../tests/) covers it across four strategies and three backends, and Section 11.5 of the [design record](../docs/design/technical_report.md) measures how long it takes.
