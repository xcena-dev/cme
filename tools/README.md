# tools/ — what a user of the library runs

One program, and the only executable the install step lays down beside the library.

`cme_top.cpp` builds `cme-top`, an htop-style read-only monitor for a live region.
It never joins and never writes, so watching a region costs the peers in it nothing.

```sh
build/tools/cme-top shm:/name
build/tools/cme-top dax:/dev/dax0.0
```

Four peers on three domains, one of them queued behind the control domain:

```
cme-top  shm:/cme_fairness_test_2153625  [06:06:49]
  strategy=REQUEST   domains=3  peers=4  total=2112B  fmt_gen=1785737208108153048

DOMAINS
   id  name          holder     generation
    0  control       2<-3       25
    1  lane0         1<-2       20
    2  lane1         0<-1       19

PEERS
   id  status  role      poll%    worker%    wait%    spin%    hb_age     pending
    0  ACTIVE  HOLDING   3.4%     1.1%       26.7%    4.1%     0ms        -
    1  ACTIVE  HOLDING   3.4%     1.1%       26.7%    4.2%     0ms        -
    2  ACTIVE  HOLDING   3.2%     1.1%       26.7%    4.1%     0ms        -
    3  ACTIVE  WAITING   3.3%     0.5%       13.4%    4.0%     0ms        {d0}

[Ctrl-C to quit, refresh 600ms]
```

`DOMAINS`

| | |
|---|---|
| `name` | As stored in the record. Domain 0 is the control domain and carries no stored name; an unnamed data slot shows `-`. |
| `holder` | The peer holding it. `2<-3` means it changed hands since the previous frame, from 3 to 2. |
| `generation` | Transfer counter, incremented on every handoff. |

`PEERS`

| | |
|---|---|
| `status` | `ACTIVE` once the peer has joined membership. |
| `role` | `HOLDING` outranks `WAITING`, so a peer holding one domain while queued for another reads as holding. `STALE` overrides both. |
| `poll%` | Frame share the poll thread spent on CPU. |
| `worker%` | Frame share spent in the peer's own work. |
| `wait%` | Frame share spent blocked on ownership. |
| `spin%` | Share of that wait spent busy-spinning. 27% waiting with 4% spinning means the blocked time is not burning a core. |
| `hb_age` | Age of the liveness stamp. Past `DeadWindowEffective` (`src/config.hpp`) the role reads `STALE`, which is the window the region's own failure detector uses, so `--interval` does not change the diagnosis. |
| `pending` | Domains this peer is queued for. |

A `-` in any column means the value could not be derived, not zero: no previous frame yet, a counter that went backwards, or an instrumentation axis that is off.

The four CPU-share columns need both axes, which is where the numbers above come from.
`CME_STATS` accumulates them and `CME_PROFILE` publishes them, and `CME_PROFILE` changes the slot layout, so the monitor and what it watches must come from the same build tree.

```sh
cmake -S . -B build-prof -DCME_STATS=ON -DCME_PROFILE=ON && cmake --build build-prof -j
```
