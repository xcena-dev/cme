# cme spec (TLA+)

Formal model of the CME ownership + crash-recovery protocol that the C++ in `src/` implements —
specifically the §4 state machine of the technical report (`../report/technical_report.md`, ch. 8). The model
is the source of truth for the concurrency design; code changes that touch protocol semantics should
be reflected here and re-checked with TLC.

## What is TLA+ / TLC?

- **TLA+** is a specification language for concurrent/distributed systems. A spec defines the state
  variables, an `Init` predicate, and a `Next` relation (a disjunction of atomic *actions*). The set
  of behaviors is every sequence of states reachable from `Init` by repeatedly applying `Next`.
- **TLC** is the model checker. Given a `.cfg` that binds the `CONSTANTS` to small finite values, it
  does an exhaustive breadth-first exploration of **all reachable states** and checks every declared
  `INVARIANT` (safety — one state) and `PROPERTY` (temporal — over whole behaviors), reporting a
  concrete counterexample trace on failure.

## Files

| File | Purpose |
|---|---|
| `CME_state_machine.tla` | The report's §4 state machine, modeled directly: two coupled machines — ownership (per domain, over the *DomainRecord*) and membership (per peer, over the *MemberSlot*) — plus crash recovery. Policy and recovery-authority are nondeterministic oracles (one proof covers every policy); failure detection is crash-stop. The recovery-authority *claim* is **not** core state here — it is policy-private FAM state in the code (`recovery_authority.cpp`); the sole core-visible marker of recovery-in-progress is the `MemberSlot` status `Recovering`. |
| `CME_state_machine.cfg` | Safety config (`NumPeers=4`, `NumDomains=2`): checks `TypeOK`, `DeadHolderPending` (I2), `RecoveringTargetsDead` (well-formedness), and deadlock-freedom. I1 is **not** checked here (see below). |
| `CME_state_machine_live.cfg` | Liveness *without* the recovery-first schedule (`FairSpec` = weak fairness only). `RecoveryTerminates` is **expected to FAIL** here — the counterexample that shows why the schedule is needed. |
| `CME_state_machine_liveRF.cfg` | Liveness *with* the recovery-first schedule (`FairSpecRF`). `RecoveryTerminates` holds — recovery completes once churn is bounded. |
| `tla2tools.jar` | Bundled TLC + parser (no separate install). |

## Setup

Only a JRE is needed (TLC is pure Java); `tla2tools.jar` is vendored here.

```sh
java -version   # any JRE 11+ works
```

To refresh `tla2tools.jar` (official TLA+ releases):

```sh
curl -L -o tla2tools.jar \
  https://github.com/tlaplus/tlaplus/releases/latest/download/tla2tools.jar
```

## Running TLC

From this directory:

```sh
# Safety: invariants + deadlock-freedom, exhaustive (N=4, D=2)
java -jar tla2tools.jar -workers 16 -config CME_state_machine.cfg CME_state_machine.tla

# Liveness WITHOUT recovery-first -- expected to FAIL (the boundary counterexample)
java -jar tla2tools.jar -workers 16 -config CME_state_machine_live.cfg CME_state_machine.tla

# Liveness WITH recovery-first -- holds
java -jar tla2tools.jar -workers 16 -config CME_state_machine_liveRF.cfg CME_state_machine.tla
```

- `-workers N` sets the parallel worker count (use a fixed number, not `$(nproc)`).
- A clean run ends with `Model checking completed. No error has been found.` A violation prints the
  failing invariant/property and a step-by-step trace. `CME_state_machine_live.cfg` **fails by
  design** — the churn counterexample it produces is the point.
- TLC writes `states/` and `*.log` working files here; scratch output, safe to delete.

## What the model proves

`CME_state_machine.tla` is the §4 state machine and nothing more — no control/data split, no
create/delete, no notification transport (those are implementation, §9). Checked exhaustively:

- **I1 — exactly one RA per peer**: **not model-checked at the core level.** The arbitrating claim
  is policy-private FAM state (`recovery_authority.cpp`, LWW winner id), outside core, so I1 is an
  *assumption on the policy oracle* (at most one confirmed RA per dead peer). The spec models every
  live peer as a possible RA (`\E ra`); TLC checks that safety survives any RA the oracle could
  confirm, which is strictly weaker than trusting single-winner arbitration. Claim-LWW correctness
  itself is a policy-layer obligation, not checked here.
- **I2 — recovery completes** (`DeadHolderPending` + deadlock-freedom): a dead holder is never
  stranded; it stays pending recovery (`Member` or `Recovering`) until taken over, and recovery can
  always make progress. An RA may die mid-recovery — the `Recovering` target survives and any later
  live RA resumes it.
- **Well-formedness** (`RecoveringTargetsDead`): only a dead member is `Recovering` — a live peer is
  never recovered (the crash-stop assumption).
- **R1 / R2** (exclusive ownership, ownership continuity): hold by construction — the record is
  single-valued and a crashed owner stays owner until recovery moves it.
- **Liveness — recovery *completes*, conditionally**: under weak fairness it needs the recovery-first
  schedule (bounded churn); without it, unbounded churn can starve a ready recovery.
  `CME_state_machine_live.cfg` is that counterexample; `CME_state_machine_liveRF.cfg` shows it holds
  with the schedule.
