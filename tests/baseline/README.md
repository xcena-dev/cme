# baseline/ — the external reference point

Nothing here is CME.
`rdma_lock.cpp` holds no cme header and links no cme target: it is a two-node RDMA lock, and Section 11.3 of the [design record](../../docs/design/technical_report.md) compares CME's sweep against what it measures.
A control belongs with the experiment, which is why it lives under `tests/` rather than beside the library.

## What it measures

A remote lock is a single 8-byte word acquired by the NIC, so the server CPU is not in the lock path at all.
That is what makes it the fair opponent: it is not a CPU-mediated lock wearing an RDMA costume.

Three acquire policies, because the choice changes the answer by more than the medium does:

| | |
|---|---|
| `--ticket` | FAA ticket lock, the DSLR-style ordering core. FCFS, bounded turns, no re-CAS. This is the one the report compares against. |
| `--backoff` | Compare-and-swap with exponential backoff and jitter. The default. |
| `--blind` | Immediate re-CAS. Documented quadratic worst case, and the reason a blind baseline is not a fair one. |

Read `--ticket` numbers as "FAA-ticket ordering core", not "DSLR measured".
The file's header says which DSLR mechanisms are absent.

## Running it

Two hosts on the same RoCEv2 fabric, both naming the same HCA and GID index, both from `config.yaml`.
This host is the client and runs the contending threads and QPs; the peer holds the lock words and otherwise idles.

```sh
./tests/baseline/rdma_lock.sh
./tests/baseline/rdma_lock.sh --peers "4 8" --domains "1 4" --iters 200
./tests/baseline/rdma_lock.sh --verify          # correctness soak instead of latency
./tests/baseline/rdma_lock.sh --acquire backoff # a different acquire policy
```

Beyond `config.yaml` it needs passwordless ssh to the peer and a configured build tree here.
The script scp's the binary CMake built rather than compiling its own, so both ends run the same bytes.

The CMake target is guarded on libibverbs.
A machine without an RDMA stack still builds the rest of the tree, and the script says so rather than failing obscurely.
Nothing here is registered with ctest, because no build can supply a second host.
