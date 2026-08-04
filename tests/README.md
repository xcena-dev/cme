# cme tests

Everything that exercises or measures the library lives here, in five directories by what a file is rather than by what it targets.

| | |
|---|---|
| `cases/` | One `test_*.cpp` per scenario, each a standalone executable that `tests/CMakeLists.txt` registers as a ctest case. The catalogue below is theirs. |
| [`harness/`](harness/) | What the cases compile against, what the scripts source, and `thp_exec`. Its own README lists the files. |
| [`bench/`](bench/) | One microbenchmark per isolated cost, each with a script named after it. No ctest registration: a benchmark has no pass or fail. |
| [`sweep/`](sweep/) | The grid drivers and the plotters that turn their CSVs into figures. |
| [`baseline/`](baseline/) | The external reference point those numbers are read against. Holds no CME code. |

Only `cases/` gates a change.
The other four measure, and a slow run there is a result rather than a failure.

[`local_ci.sh`](local_ci.sh) runs all of it here, because a hosted runner has neither a devdax node nor an uncacheable mount and can only ever cover the shm third.
It refuses to start when a medium is absent instead of skipping it, which is the opposite of the rule everywhere else and the only reason it exists.

Each `cases/test_*.cpp` is a standalone executable; `tests/CMakeLists.txt` registers them as ctest cases.

- **Backend**: POSIX shm by default. Most tests are also registered as `*_dax` and `*_file`
  variants, passing `--backend dax` or `--backend uc` and running under the `thp_exec` wrapper,
  each on its own reserved slot.
- **Strategy**: `--strategy order|request|request-agg|peterson`. Recovery/fairness tests register
  all four.
- **Verdict**: `ctx.check(cond, msg)` and `ctx.checkf(cond, fmt, ...)` print a line and tally.
  Both are non-fatal, so one run reports every invariant it broke rather than only the first.
- **Contention / stress coverage**: `admission_stress` (admission slot claim — the only lock-free
  multi-writer race — many rounds + over-subscription), `admission_race` / `create_race` (single-round
  cross-process races), `fairness` (N×T×D contended workload), `recovery` (10 s random freeze/thaw
  churn), `util_test` (primitive-level torn-pair + multi-writer hammer, in-process and cross-process).

## Test summary

Every case is a standalone executable, registered on shm, dax and uc unless the note says otherwise.
A case registered with all four strategies appears four times per backend.

| Category | File | ctest case(s) | What it covers |
|---|---|---|---|
| Primitives | `test_util.cpp` | `util_test` (shm) | `endian.hpp` + `coherency.hpp`: Field_t round-trip, wmb/rmb ordering, torn-pair, publishCas, cross-process |
| Primitives | `test_region_smoke.cpp` | `region_smoke` (shm) | Memory + formatRegion + `Memory::header()`; no Peer, no policy |
| Public API | `test_shared_smoke.cpp` | `shared_smoke` | `Session::format/open`, lock + Guard RAII, tryLock, withLock, getDomainNames, re-format idempotency |
| Public API | `test_shared_session.cpp` | `shared_session` x4 strategies | One session shared by N threads of one process still excludes, which a per-peer token does not cover alone |
| Public API | `test_tiered_lock.cpp` | `tiered` x4 strategies | Both tiers together: N sessions x T threads, inter-node ownership over intra-node mutex |
| Exclusion | `test_mutual_exclusion.cpp` | `mutex` x4 strategies | The core contract. N peers non-atomically RMW one counter in the CS; a second holder loses an update and the total falls short |
| Exclusion | `test_fairness_smoke.cpp` | `fairness_smoke` | ORDER and REQUEST round-trip at two sizes; no timeout, per-peer spread within bound |
| Exclusion | `test_fairness.cpp` | `fairness_order_sym`, `fairness_request_sym` | N x T x D contended workload; max-min per-peer spread within `--bound * mean` |
| Admission | `test_admission_race.cpp` | `admission_race` | N processes claim peer slots at once, slots >= N: distinct slots, no spurious NoFreeSlot |
| Admission | `test_admission_stress.cpp` | `admission_stress` | The same protocol over many rounds behind a spin barrier, exact-fit and over-subscribed. The only lock-free multi-writer race in cme |
| Admission | `test_readmit_gate.cpp` | `readmit_gate` (request) | C1: a dead peer's slot is re-claimable only after recovery marks it None |
| Registry | `test_domain_lifecycle.cpp` | `domain_lifecycle` | create/delete over the control domain: duplicate, ceiling, delete, slot reuse, foreign participant, unknown name |
| Registry | `test_create_race.cpp` | `create_race` | N processes createDomain at once; the control lock serialises the registry, so none is lost |
| Registry | `test_participation.cpp` | `participation` | Opt-in participation: join, leave, rejoin, sole-participant leave refused, unknown name |
| Recovery | `test_recovery.cpp` | `recovery` x4 strategies | Scripted timeline: freeze, thaw, leave, rejoin, multi-freeze, 10 s churn |
| Recovery | `test_recovery_orphan.cpp` | `recovery_orphan` x4 strategies | The orphan-free path: a sole participant crashes, and the participation scrub unblocks delete |
| Recovery | `test_recovery_resume.cpp` | `recovery_resume` x4 strategies | A peer stranded in Recovering by an RA that died mid-recovery is resumed to None |
| Recovery | `test_recovery_stake_gate.cpp` | `recovery_stake_gate` x4 strategies | A live claim word is left alone; a dead RA's is taken over |
| Recovery | `test_recovery_zombie_claim.cpp` | `recovery_zombie_claim` x4 strategies | A ghost claim authored by a peer that has since re-admitted is retracted at FINISH |
| Recovery | `test_orphan_churn.cpp` | `orphan_churn` x4 strategies | Recovery while the registry mutates, which the scripted tests never do |
| Recovery | `test_departure_strand.cpp` | `departure_strand` (order) | C4: a grant landing on a departing peer must not strand the domain |
| Policy | `test_agg_recovery.cpp` | `agg_recovery` | RequestAgg only: a crashed aggregator is re-elected to a live group member, and reaches NoPeer once the group is gone |
| Reference | `test_mutex_baseline.cpp` | `mutex_baseline` (shm) | Process-shared pthread_mutex latency floor, the coherent-DRAM reference CME's own numbers are read against |
| Reference | `test_seq_latency.cpp` | `seq_latency_*` | Uncontended acquire latency, one lock at a time, dominated by the handoff |

What each case does in detail is the comment at the top of its own file.
That is one copy rather than two: this table drifted once already, naming three files the tree no longer has while missing fourteen it does.
