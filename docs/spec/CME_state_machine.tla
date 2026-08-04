----------------------- MODULE CME_state_machine -----------------------
\* State machine over DomainRecord (per-domain owner) and MemberSlot (per-peer: Free/Member/Leaving/Recovering).
\* alive[peer] is the oracle; Crashed status is DERIVED (crash writes nothing).
\* RA arbitration is policy-private; this spec models the confirmed RA as a nondeterministic oracle.
\* Out of scope: local belief/epoch, create/delete, shadow, concrete policies. Detection = crash-stop.

EXTENDS Integers, FiniteSets

CONSTANTS NumPeers, NumDomains
ASSUME NumPeers   \in Nat /\ NumPeers   >= 2
ASSUME NumDomains \in Nat /\ NumDomains >= 1

Peers   == 0 .. (NumPeers - 1)
Domains == 0 .. (NumDomains - 1)

\* ============================ State (§4.1 objects) ============================

\* Oracle: process liveness (Crash/Revive via adversary).
VARIABLES
    alive              \* [Peers -> BOOLEAN]; the oracle's flag, never a slot write

\* MemberSlot per peer: Free/Member/Leaving/Recovering (RA oracle, no claim field stored).
VARIABLES
    MemberSlotStatus   \* [Peers -> {"Free","Member","Leaving","Recovering"}]; Crashed is derived

\* DomainRecord per domain: the sole owner authority.
VARIABLES
    DomainRecord       \* [Domains -> Peers]; the named owner IS the ownership

vars == << alive, MemberSlotStatus, DomainRecord >>

TypeOK ==
    /\ alive              \in [Peers -> BOOLEAN]
    /\ MemberSlotStatus   \in [Peers -> {"Free","Member","Leaving","Recovering"}]
    /\ DomainRecord       \in [Domains -> Peers]

Init ==
    /\ alive              = [p \in Peers |-> TRUE]
    /\ MemberSlotStatus   = [p \in Peers |-> "Member"]
    /\ DomainRecord       = [d \in Domains |-> 0]                  \* peer 0 holds all

\* ============================ Helpers ============================

IsActive(peer) == alive[peer] /\ MemberSlotStatus[peer] = "Member"

\* Dead slot, not yet Free: recovery still owes work.
PendingRecovery(peer) == ~alive[peer] /\ MemberSlotStatus[peer] \in {"Member","Leaving","Recovering"}

\* Live holder (Member or Leaving): only one may release the domain.  "Leaving" is included (SuccessorCandidates
\* filters on "Member" at write time) because the impl picks successors off stale member view; a departing
\* peer must stay able to release until all domains are handed off.
IsLiveHolder(domain, peer) ==
    /\ alive[peer]
    /\ MemberSlotStatus[peer] \in {"Member","Leaving"}
    /\ DomainRecord[domain] = peer

\* Policy oracle: successor is any Member (policy-neutral; \E explores all picks).
SuccessorCandidates(from) ==
    { p \in Peers : p # from /\ MemberSlotStatus[p] = "Member" }

\* ======================= Ownership machine (§4.3) =======================

\* Atomic record write: ownership transfers at the write (no belief/adopt).
PublishOwnership(domain, from, to) ==
    /\ IsLiveHolder(domain, from)
    /\ to \in SuccessorCandidates(from)
    /\ DomainRecord' = [DomainRecord EXCEPT ![domain] = to]
    /\ UNCHANGED << alive, MemberSlotStatus >>

\* ======================= Member machine (§4.4/4.5) =======================

\* Join: Free -> Member (lowest free slot, mirroring reserveMemberSlot in admission/claim.cpp).
JoinMembership(peer) ==
    /\ alive[peer]
    /\ MemberSlotStatus[peer] = "Free"
    /\ \A q \in Peers : q < peer => MemberSlotStatus[q] # "Free"
    /\ MemberSlotStatus' = [MemberSlotStatus EXCEPT ![peer] = "Member"]
    /\ UNCHANGED << alive, DomainRecord >>

\* BeginLeave: Member -> Leaving (dropped from SuccessorCandidates; PendingRecovery still covers it).
BeginLeave(peer) ==
    /\ IsActive(peer)
    \* Must have an active successor; emptying region is out of scope.
    /\ \E s \in Peers : s # peer /\ IsActive(s)
    /\ MemberSlotStatus' = [MemberSlotStatus EXCEPT ![peer] = "Leaving"]
    /\ UNCHANGED << alive, DomainRecord >>

\* PublishNone: Leaving -> Free (LAST write: Free slot is no RA target, so write it after all releases).
PublishNone(peer) ==
    /\ alive[peer]
    /\ MemberSlotStatus[peer] = "Leaving"
    /\ \A d \in Domains : DomainRecord[d] # peer
    /\ MemberSlotStatus' = [MemberSlotStatus EXCEPT ![peer] = "Free"]
    /\ UNCHANGED << alive, DomainRecord >>

\* Crash: Member or Leaving only (Free is not included: once PublishNone lands, no FAM writes remain).
Crash(peer) ==
    /\ alive[peer]
    /\ MemberSlotStatus[peer] \in {"Member","Leaving"}
    /\ \E s \in Peers : s # peer /\ IsActive(s)
    /\ alive' = [alive EXCEPT ![peer] = FALSE]
    /\ UNCHANGED << MemberSlotStatus, DomainRecord >>

\* Revive: Free process restarts, then re-admits via JoinMembership.
Revive(peer) ==
    /\ ~alive[peer]
    /\ MemberSlotStatus[peer] = "Free"
    /\ alive' = [alive EXCEPT ![peer] = TRUE]
    /\ UNCHANGED << MemberSlotStatus, DomainRecord >>

\* ======================= Recovery (§4.5 Detect/Recover, I1/I2) =======================

\* Detect + seize: RA marks slot Recovering (fires once per dead peer).
SeizeRecovery(ra, dead) ==
    /\ IsActive(ra)
    /\ ~alive[dead]
    /\ MemberSlotStatus[dead] \in {"Member","Leaving"}
    /\ MemberSlotStatus' = [MemberSlotStatus EXCEPT ![dead] = "Recovering"]
    /\ UNCHANGED << alive, DomainRecord >>

\* Recover, part 1: RA takes one domain from the seized dead holder.
TakeoverOwnership(domain, ra, dead) ==
    /\ IsActive(ra)
    /\ MemberSlotStatus[dead] = "Recovering"
    /\ dead = DomainRecord[domain]
    /\ DomainRecord' = [DomainRecord EXCEPT ![domain] = ra]
    /\ UNCHANGED << alive, MemberSlotStatus >>

\* Recover, part 2: all domains taken; scrub Recovering -> Free.
CompleteRecovery(ra, dead) ==
    /\ IsActive(ra)
    /\ MemberSlotStatus[dead] = "Recovering"
    /\ \A d \in Domains : DomainRecord[d] # dead
    /\ MemberSlotStatus' = [MemberSlotStatus EXCEPT ![dead] = "Free"]
    /\ UNCHANGED << alive, DomainRecord >>

\* ============================ Next-state ============================

OwnershipActions ==
    \/ \E d \in Domains, from \in Peers, to \in Peers : PublishOwnership(d, from, to)

MembershipActions ==
    \/ \E p \in Peers : JoinMembership(p)
    \/ \E p \in Peers : BeginLeave(p)
    \/ \E p \in Peers : PublishNone(p)

OracleActions ==
    \/ \E p \in Peers : Crash(p)
    \/ \E p \in Peers : Revive(p)

RecoveryActions ==
    \/ \E ra \in Peers, dead \in Peers : SeizeRecovery(ra, dead)
    \/ \E d \in Domains, ra \in Peers, dead \in Peers : TakeoverOwnership(d, ra, dead)
    \/ \E ra \in Peers, dead \in Peers : CompleteRecovery(ra, dead)

Next ==
    \/ OwnershipActions
    \/ MembershipActions
    \/ OracleActions
    \/ RecoveryActions

Spec == Init /\ [][Next]_vars

\* ============================ Invariants (§4.7) ============================

\* R1/R2 (exclusive, continuous ownership) hold by construction: DomainRecord is single-valued,
\* only the live owner writes it, and crashed owners stay owner until recovery moves them.

\* Uniqueness (one RA per dead peer) is the policy oracle's guarantee (not checked here).

\* I1: dead holder stays in pending recovery until taken over. Unconditional: BeginLeave ensures
\* a Member is always available; emptying region is out of scope.
DeadHolderPending ==
    \A d \in Domains :
        ~alive[DomainRecord[d]] => PendingRecovery(DomainRecord[d])

\* Only dead members are Recovering (crash-stop: live peers are never recovered).
RecoveringTargetsDead ==
    \A p \in Peers : MemberSlotStatus[p] = "Recovering" => ~alive[p]

\* Liveness: recovery completes under fairness; holds under bounded churn only.
FairSpec == Spec /\ WF_vars(RecoveryActions)

\* I2: seized peers eventually reach Free.
RecoveryTerminates ==
    \A p \in Peers : (MemberSlotStatus[p] = "Recovering") ~> (MemberSlotStatus[p] = "Free")

\* Recovery-first: preempt normal work while recovery is pending (bounds churn, starves RA crashes only).
RecoveryPending == \E p \in Peers : PendingRecovery(p)
NextRecoveryFirst ==
    \/ RecoveryPending  /\ (RecoveryActions \/ OracleActions)
    \/ ~RecoveryPending /\ Next
FairSpecRF == Init /\ [][NextRecoveryFirst]_vars /\ WF_vars(RecoveryActions)

=============================================================================
