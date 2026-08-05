# Glossary

Ordinary words with a narrow meaning here.

**Region** - the shared-memory area every peer maps: header, membership table, per-domain records, laid out as 64-byte cachelines by `Geometry`.
The file, devdax node, or shm object behind it is the *backend*.

**Peer** - one process that has joined a region.
Its `PeerId` is also its membership slot index, so peer 7 and slot 7 are the same thing.
Ceiling 64.

**Domain** - one independent unit of mutual exclusion.
Domains do not nest and do not interact.
Domain 0 is the control domain, reserved for `createDomain`/`deleteDomain` and not addressable by an application.
Ceiling 64.

**Membership** - the table of peer slots, plus the rules for entering and leaving.
`peerScanBound` records how far a scan must look, so scans do not walk 64 slots when 3 are in use.

**Holder**, also **owner** - the peer that currently owns a domain and may write what it protects.
The two words mean the same thing here.
The code and the TLA+ spec say *holder* (`getHolder`, `DeadHolderPending`).
The design record says *owner* about as often.
Exactly one at a time, and that is the property the protocol exists to maintain.
`cme::Guard` is what an application holds.

*Ownership* is not a third word for the same thing: the holder is the peer, and ownership is what it holds.
`getHolder()` returns a `PeerId`, `readOwnership()` returns a record.

**Ownership record** - the 64-byte record naming a domain's holder and epoch.
Being durable is what makes ownership recoverable rather than only current.

**Epoch** - a counter on that record, increasing whenever ownership changes.
Gaps cost nothing, and a recovery seize jumps several deliberately.

**Shadow record** - a second copy written *before* the record it shadows, so a handoff interrupted midway leaves a state a reader recognises as in-progress.
It can sit one epoch above the truth record, and only one.

**Handoff** - transfer of a domain to the next holder.
A *torn handoff* is one interrupted by the holder's death, leaving the two records disagreeing.
Recovery resolves it.

**Successor policy** - the rule picking which waiting peer goes next, recorded in the header so every joiner uses the matching implementation.
Four exist: `Order`, `Request`, `RequestAgg`, `Peterson`.

**Demand** - a peer's published statement that it wants a domain, written to the region so the holder sees it without being asked.

**Heartbeat** - a timestamp each peer refreshes in its slot.
A stamp that stops advancing past a timeout means the peer is *judged dead*, which is an observers' decision rather than a fact about the process.

**Recovery authority (RA)** - the single peer responsible for recovering one dead peer.
Unique per dead peer, available whenever anyone is alive, chosen deterministically.
The default picks the first live peer clockwise after the dead one.

**Stake / settle / confirm** - what replaces compare-and-swap.
Write the claim, wait long enough for it to become visible to the other hosts, then re-read and check it still stands.
The wait does the work: with no coherence there is no instant at which a write is simultaneously visible, so the protocol pays a bounded delay instead.

**Orphan** - a domain with no live participant, usually left by a dead peer.
An *orphan sweep* reclaims them.

**Crash-stop** - the failure assumption: a peer stops, and does not later resume acting on pre-death state.
CME does not enforce this.
The platform does.

**Coherency mode** - how *this* peer mapped the region: `CacheCoherent`, `Flush`, or `Uncached`.
A property of the mapping, not the region, so two peers over one device can differ.
It selects which fences `wmb` and `rmb` emit.

**wmb / rmb** - publish and acquire barriers for cross-host visibility, not just compiler or CPU ordering.
Every cross-host read and write in the library goes through one.

**Settle time** - the bounded delay a two-phase claim waits before re-reading.
Measured, not guessed: `tests/bench/lww_settle_bench.cpp` sets the constant in `src/config.hpp`.

**Non-coherent shared memory** - the substrate assumption everything above follows from: several hosts with byte-addressable access to the same physical memory, no cross-host cache coherence, no cross-host atomics.
