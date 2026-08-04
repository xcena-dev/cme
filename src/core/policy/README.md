# src/core/policy/ — the interchangeable parts

The engine in [`../algo/`](../algo/) *consults* these.
It never lets them write an ownership record, which is why the invariants stay in the core while a policy can be replaced.
A policy manages only its own private storage.

There are three questions, answered by three independent policy kinds.

## Who goes next (successor)

The strategy is chosen at `format()` time and recorded in the region, so a joiner picks up the matching policy rather than being told.

| | |
|---|---|
| `successor_policy.hpp` | The abstract base every successor policy implements. |
| `successor.hpp` | Wrapper over the concrete kinds, so callers hold one type. |
| `successor_order.{hpp,cpp}` | `OrderPolicy`. The ring decides: the next live peer clockwise. No demand signal, so a peer with nothing to do is still offered the turn. |
| `successor_request.{hpp,cpp}` | `RequestPolicy`. Peers raise a hand, and the holder picks from those who did. |
| `successor_request_agg.{hpp,cpp}` | `RequestAggPolicy`. Hand-raising with the demand packed into fewer words, which cuts the reads a holder needs to see who is waiting. |
| `successor_peterson.{hpp,cpp}` | `PetersonPolicy`. A tournament of pairwise Peterson locks, where winning the tournament *is* holding the lock. |
| `request_demand_region.hpp` | The private region `RequestPolicy` and `RequestAggPolicy` size and format for themselves. Neither the core nor the other policies read it. |

## Who is alive (liveness)

| | |
|---|---|
| `liveness.hpp` | The `LivenessPolicy` hierarchy. |
| `liveness_heartbeat.cpp` | The heartbeat implementation: a peer that stops publishing is eventually declared dead. |

A liveness verdict is a judgement, not an observation, and CME assumes crash-stop.
A peer declared dead is assumed to have stopped, and CME cannot make that true on its own.

## Who may recover it (recovery authority)

| | |
|---|---|
| `recovery_authority.hpp` | The `RecoveryAuthorityPolicy` hierarchy. |
| `recovery_authority.cpp` | `ChainRecoveryAuthorityPolicy`: the ring walk returns the first live peer clockwise, so exactly one peer is entitled to recover a given dead one. |

Concentrating the entitlement in one peer is what keeps two recoverers from seizing the same domain.
