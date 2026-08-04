# src/core/algo/ — the algorithms

These four pairs hold the mechanism that the policies in [`../policy/`](../policy/) plug into.
The split is deliberate: the invariants live here, so a policy can be swapped without putting them at risk.

| | |
|---|---|
| `peer.{hpp,cpp}` | One peer's instance of the algorithm, plus the `PeerGuard` that scopes it. The per-peer poll thread that drives everything else starts here. |
| `ownership_transfer.{hpp,cpp}` | The strategy-agnostic primitives: publish a domain record, wait for ownership, take a domain over. Every policy reaches ownership through these, and none of them writes an ownership record itself. |
| `lifecycle.{hpp,cpp}` | Membership: join, leave, rejoin. Membership changes while the region is live, so this is not setup code. |
| `recovery.{hpp,cpp}` | The recovery state machine, run from the poll thread. Detect a dead peer, claim the right to recover it, seize what it held, then declare its slot free. |

## Two rules worth knowing before editing

**The truth record's holder is the sole authority on ownership.**
`publishDomainRecord` writes the shadow entry before the truth record, so a shadow can name a peer that the truth does not.
`waitForOwnership` treats such a shadow only as a prompt to read the truth, and the truth settles it.
Therefore a shadow ahead of the truth is an incomplete handoff, not a transfer.

**One epoch must imply one holder.**
There is no compare-and-swap available, so a monotonically increasing epoch stands in for it.
A recovery seize must therefore jump clear of any in-flight shadow before stamping itself, which is what `RecoverySeizeEpochGap` in [`../../config.hpp`](../../config.hpp) is for.
Epochs only have to increase, so a gap costs nothing.

The formal statement of both is in [`docs/spec/`](../../../docs/spec/), which is normative for this directory.
