<!--
SPDX-License-Identifier: Apache-2.0
Copyright XCENA Inc.
-->

# cmed examples

Two runnable programs against the requester API, and nothing else.

They link `cmed_client` and no test code, so they compile exactly as a downstream caller's code does.
A change that breaks the public surface breaks this build, which is why they have targets rather than living in a document.

## What they need

A daemon already serving.
Neither program opens a region: the daemon holds this node's one peer slot, and these ask it for turns.

Each takes the daemon's socket path as its one optional argument, defaulting to `/run/cmed/cmed.sock`.

```
daemon/deploy/install.sh --build build
systemctl start cmed
```

## The programs

`01_hello_lock.cpp` is the smallest one that takes a turn.
It connects, gets the domain either by creating it or by joining one another caller made, takes the lock, and lets the guard's destructor give it back.

`02_local_cohort.cpp` is what cmed is for.
Four child processes each open their own session and ask for the same domain.
The daemon settles them on this node, so the region sees one peer taking the turn rather than one per child.

```
build/daemon/examples/cmed-example-hello
build/daemon/examples/cmed-example-cohort
```

## Where the errors come from

Every call throws on refusal rather than returning a code, so both programs wrap their body in one `catch (const cmed::CmedError&)`.
A create against a name another caller already made comes back as `CmedControlRefusedError`, which is why both programs treat that one as the signal to join instead.
