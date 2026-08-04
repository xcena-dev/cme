# bench/ — the microbenchmarks behind the numbers

Each one isolates a single cost the [design record](../../docs/design/technical_report.md) reasons about.
None is registered with ctest: they measure rather than assert, so a slow run here is a result, not a failure.

| | |
|---|---|
| `cacheline_bench` | Load and store ns/iter on one 64-byte line, uncacheable against write-back over one device. The floor everything else is built on. |
| `read_tail_bench` | Per-access read latency on one line, in four modes that each leave one candidate cause of a multi-microsecond read in place. Untimed poller threads sweep the pile-up. |
| `lww_settle_bench` | How long a last-writer-wins claim takes to settle without atomics. Sets `ClaimSettle` in `src/config.hpp`, so a port to other FAM hardware re-runs it. |
| `recovery_latency_bench` | End-to-end recovery latency in phases, swept over the dead peer's owned-domain count, with the strategies side by side. Section 11.5. |

Each has a script beside it with the same name, which runs it the way the numbers were taken, and documents its own environment variables in its header.
Run the script unless you are changing what is measured.
`bench_common.sh` runs nothing; the four scripts source it for the tree root, the build tree, and the checks they share.

Every target comes from `config.yaml`, and a medium this machine lacks is reported and skipped rather than failed.
A slot sits in the tail `dax_slot_reserve` holds back, so a filesystem at the front of the same device is never written over.

To check that a script still drives its binary, cut its counts down and put it on a slot no other run is using.
That takes seconds instead of minutes and still exercises target resolution, the skip branch, and each phase.
Numbers from a run like that are not results: each script's defaults are the counts the design record's figures were taken at.

Output goes to files this directory's `.gitignore` excludes, so keep a result worth keeping somewhere else.
