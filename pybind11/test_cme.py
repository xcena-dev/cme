# SPDX-License-Identifier: Apache-2.0
# Copyright XCENA Inc.
"""What the binding has to answer for, on a file-backed region of its own.

Run it with the module's build directory on sys.path:

    PYTHONPATH=<build>/pybind11 pytest pybind11/test_cme.py
"""

from __future__ import annotations

import threading

import pytest

import cme


@pytest.fixture(name="region")
def make_region(tmp_path):
    """A formatted region nobody else holds, so a failure here is the binding's."""
    path = tmp_path / "region"
    uri = f"file:{path}"
    cme.Session.format(uri)
    return uri


def test_format_then_open(region):
    session = cme.Session.open(region)
    assert session is not None


def test_open_names_what_is_missing():
    with pytest.raises(cme.CmeError) as raised:
        cme.Session.open("file:/nonexistent/region")
    assert "No such file" in str(raised.value)


def test_domain_round_trip(region):
    session = cme.Session.open(region)
    session.create_domain("alpha")
    assert "alpha" in session.domain_names()

    handle = session.resolve_domain("alpha")
    assert handle.id != 0
    assert handle.incarnation != 0

    entries = {entry.name: entry.handle for entry in session.domain_entries()}
    assert "alpha" in entries
    assert entries["alpha"].id == handle.id


def test_turn_is_held_only_inside_the_block(region):
    session = cme.Session.open(region)
    session.create_domain("beta")
    session.join_domain("beta")

    held = session.lock("beta")
    assert held.held is False
    assert held.name == "beta"

    with held as inside:
        assert inside.held is True
    assert held.held is False


def test_turn_is_released_when_the_block_raises(region):
    session = cme.Session.open(region)
    session.create_domain("gamma")
    session.join_domain("gamma")

    held = session.lock("gamma")
    with pytest.raises(RuntimeError):
        with held:
            raise RuntimeError("the block fails")
    assert held.held is False

    # The proof that the turn really went back: a second acquire would block forever otherwise.
    with session.lock("gamma"):
        pass


def test_shared_session_refuses_a_domain_never_joined(region):
    """SharedSession keys its intra-node tier on the join, so a peer with no join has no mutex to
    take. Creating a domain joins it, which is why the refusal needs a second peer."""
    maker = cme.SharedSession.open(region)
    maker.create_domain("delta")

    joiner = cme.SharedSession.open(region)
    with pytest.raises(cme.CmeError, match="not joined"):
        with joiner.lock("delta"):
            pass

    joiner.join_domain("delta")
    with joiner.lock("delta"):
        pass


def test_the_gil_is_released_around_a_held_turn(region):
    """A held turn must not stop other Python threads.

    Without gil_scoped_release the interpreter freezes for the whole critical section, and the
    ticker below would never advance.
    """
    session = cme.Session.open(region)
    session.create_domain("epsilon")
    session.join_domain("epsilon")

    ticked = threading.Event()
    stop = threading.Event()

    def keep_ticking():
        while not stop.is_set():
            ticked.set()

    ticker = threading.Thread(target=keep_ticking, daemon=True)
    ticker.start()
    try:
        with session.lock("epsilon"):
            assert ticked.wait(timeout=5.0), "another thread never ran while the turn was held"
    finally:
        stop.set()
        ticker.join(timeout=5.0)


def test_shared_session_serialises_this_process_own_threads(region):
    """cme's token is per peer, so plain Session lets a second thread through. SharedSession is
    the tier that does not."""
    session = cme.SharedSession.open(region)
    session.create_domain("zeta")
    session.join_domain("zeta")

    inside = 0
    highest = 0
    guard = threading.Lock()
    barrier = threading.Barrier(4)

    def contend():
        nonlocal inside, highest
        barrier.wait()
        for _ in range(20):
            with session.lock("zeta"):
                with guard:
                    inside += 1
                    highest = max(highest, inside)
                with guard:
                    inside -= 1

    workers = [threading.Thread(target=contend) for _ in range(4)]
    for worker in workers:
        worker.start()
    for worker in workers:
        worker.join(timeout=60.0)
        assert not worker.is_alive()

    assert highest == 1, f"{highest} threads were inside the domain at once"


def test_enums_and_options_reach_the_library(tmp_path):
    uri = f"file:{tmp_path / 'tuned'}"
    opts = cme.FormatOpts()
    opts.max_domains = 4
    opts.max_peers = 2
    opts.strategy = cme.Strategy.Peterson
    cme.Session.format(uri, opts)

    opened = cme.OpenOpts()
    opened.coherency = cme.CoherencyMode.CacheCoherent
    session = cme.Session.open(uri, opened)
    assert session is not None

def test_a_handle_locks_and_leaves_without_the_walk(region):
    """resolveDomain exists so a repeated locker pays the record walk once.

    Two peers, because the library refuses to let the sole participant leave: it says to delete the
    domain instead, so a one-peer test would never reach leave_domain at all.
    """
    maker = cme.Session.open(region)
    created = maker.create_domain("eta")

    joiner = cme.Session.open(region)
    resolved = joiner.resolve_domain("eta")
    assert resolved.id == created.id
    assert resolved.incarnation == created.incarnation

    joiner.join_domain("eta")
    with joiner.lock(resolved) as held:
        assert held.held is True
    joiner.leave_domain(resolved)


def test_try_lock_answers_a_box_that_already_holds(region):
    session = cme.Session.open(region)
    session.create_domain("theta")
    session.join_domain("theta")

    taken = session.try_lock("theta", timeout_seconds=1.0)
    assert taken is not None
    assert taken.held is True
    # Entering an already-held box takes no second turn.
    with taken:
        assert taken.held is True
    assert taken.held is False


def test_try_lock_by_handle_takes_the_same_turn(region):
    session = cme.Session.open(region)
    handle = session.create_domain("iota")

    taken = session.try_lock(handle, timeout_seconds=1.0)
    assert taken is not None
    assert taken.held is True
    taken.release()
    assert taken.held is False


def test_try_lock_answers_none_on_the_deadline(region):
    """A turn another peer holds is not available, and the bounded form says so rather than
    waiting. Two sessions, because one peer's own token passes it straight through."""
    holder = cme.Session.open(region)
    holder.create_domain("kappa")

    waiter = cme.Session.open(region)
    waiter.join_domain("kappa")

    with holder.lock("kappa"):
        assert waiter.try_lock("kappa", timeout_seconds=0.2) is None


def test_try_lock_on_an_unknown_name_fails_fast(region):
    session = cme.Session.open(region)
    with pytest.raises(cme.CmeError):
        session.try_lock("no-such-domain", timeout_seconds=0.1)


def test_release_is_idempotent_and_bool_reports_it(region):
    session = cme.Session.open(region)
    session.create_domain("lambda")
    session.join_domain("lambda")

    held = session.lock("lambda")
    assert bool(held) is False
    with held:
        assert bool(held) is True
        held.release()
        assert bool(held) is False
    held.release()
    assert bool(held) is False
def test_shared_try_lock_gives_up_on_a_local_holder(region):
    """The bounded form has to be able to give up on the intra-node tier, not only on the region.

    One thread of this process holds the domain, so the deadline is the only way out.
    """
    session = cme.SharedSession.open(region)
    session.create_domain("theta")

    with session.lock("theta"):
        blocked = []
        worker = threading.Thread(
            target=lambda: blocked.append(session.try_lock("theta", timeout_seconds=0.2))
        )
        worker.start()
        worker.join(timeout=5.0)
        assert not worker.is_alive()
        assert blocked == [None]

    taken = session.try_lock("theta", timeout_seconds=1.0)
    assert taken is not None
    assert taken.held is True
    taken.release()


def test_shared_try_lock_refuses_a_domain_never_joined(region):
    joiner = cme.SharedSession.open(region)
    cme.SharedSession.open(region).create_domain("iota")
    with pytest.raises(cme.CmeError, match="not joined"):
        joiner.try_lock("iota", timeout_seconds=0.2)
