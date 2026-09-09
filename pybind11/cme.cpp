// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// cme.cpp -- the peer session as a CPython extension.
//
// Session is the whole surface. SharedSession is here too because a process whose threads lock the
// same domain needs it: cme's ownership token is per peer, so plain Session::lock lets a second
// thread of this process straight through.
//
// The GIL is released around every call that waits on the region. pybind11 holds it by default,
// unlike ctypes, and an acquire that keeps it stops the interpreter until a remote peer lets go.
//
// withLock is not bound: it takes a lambda and runs it under a guard, which is what `with` is.
// flush is not bound either: it is private, so no caller outside the library reaches it.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <pybind11/chrono.h>
#include <pybind11/functional.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include "cme/errors.hpp"
#include "cme/shared.hpp"
#include "cme/shared_session.hpp"

namespace pybind = pybind11;

namespace
{

// Session's guard is a free class and SharedSession's is nested, so the box asks for it by trait
// rather than by a name only one of them has.
template <typename T_Session>
struct GuardOf_t
{
    using Type = typename T_Session::Guard;
};

template <>
struct GuardOf_t<cme::Session>
{
    using Type = cme::Guard;
};

// A held turn the interpreter can own. cme's Guard has no move-assign, so it cannot itself be the
// box pybind11 needs, and the acquire is a callable so one box serves both name and handle.
template <typename T_Session>
class HeldDomain
{
public:
    using GuardType = typename GuardOf_t<T_Session>::Type;
    using Acquire = std::function<GuardType()>;

    HeldDomain(Acquire acquire, std::string label)
        : acquire_{std::move(acquire)},
          label_{std::move(label)}
    {
    }

    // Already holding, which is what a bounded acquire that succeeded hands back.
    HeldDomain(GuardType guard, std::string label)
        : label_{std::move(label)},
          held_{std::move(guard)}
    {
    }

    // Entering an already-held box changes nothing, so `with session.try_lock(name):` reads the
    // same as `with session.lock(name):` and takes one turn either way.
    HeldDomain& takeTurn()
    {
        if (held_.has_value())
        {
            return *this;
        }

        pybind::gil_scoped_release released;
        held_.emplace(acquire_());
        return *this;
    }

    // The three arguments are what the with-protocol hands back. None is read.
    void dropTurn(const pybind::object&, const pybind::object&, const pybind::object&)
    {
        release();
    }

    // Early release, before the block ends. Idempotent, as cme::Guard::unlock is.
    void release()
    {
        pybind::gil_scoped_release released;
        held_.reset();
    }

    [[nodiscard]] bool isHeld() const noexcept
    {
        return held_.has_value();
    }

    [[nodiscard]] const std::string& readLabel() const noexcept
    {
        return label_;
    }

private:
    Acquire acquire_;
    std::string label_;
    std::optional<GuardType> held_;
};

template <typename T_Session>
void bindHeldDomain(pybind::module_& module, const char* named)
{
    using Held = HeldDomain<T_Session>;
    pybind::class_<Held>(module, named)
        .def("__enter__", &Held::takeTurn, pybind::return_value_policy::reference_internal)
        .def("__exit__", &Held::dropTurn)
        .def("release", &Held::release)
        .def("__bool__", &Held::isHeld)
        .def_property_readonly("held", &Held::isHeld)
        .def_property_readonly("name", &Held::readLabel);
}

std::chrono::nanoseconds readNanoseconds(double seconds)
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::duration<double>{seconds});
}

std::string nameHandle(const cme::DomainHandle_t& handle)
{
    return "handle:" + std::to_string(handle.id);
}

}  // namespace

PYBIND11_MODULE(cme, module)
{
    module.doc() = "cme peer session: open a region, join a domain, take its turn.";

    // One Python exception for the whole family, carrying the C++ message, so a caller writes one
    // except clause instead of reading a returned code.
    pybind::register_exception<cme::Error>(module, "CmeError");

    pybind::enum_<cme::Strategy>(module, "Strategy")
        .value("Order", cme::Strategy::Order)
        .value("Request", cme::Strategy::Request)
        .value("RequestAgg", cme::Strategy::RequestAgg)
        .value("Peterson", cme::Strategy::Peterson);

    pybind::enum_<cme::CoherencyMode>(module, "CoherencyMode")
        .value("CacheCoherent", cme::CoherencyMode::CacheCoherent)
        .value("Uncached", cme::CoherencyMode::Uncached)
        .value("Flush", cme::CoherencyMode::Flush);

    // Read-only: a handle is what a join or a resolve answered, and a caller hands it back rather
    // than building one. Zero names no domain, which the library refuses.
    pybind::class_<cme::DomainHandle_t>(module, "DomainHandle")
        .def_readonly("id", &cme::DomainHandle_t::id)
        .def_readonly("incarnation", &cme::DomainHandle_t::incarnation)
        .def("__repr__", [](const cme::DomainHandle_t& handle) {
            return "<DomainHandle id=" + std::to_string(handle.id) +
                   " incarnation=" + std::to_string(handle.incarnation) + ">";
        });

    pybind::class_<cme::DomainEntry_t>(module, "DomainEntry")
        .def_readonly("name", &cme::DomainEntry_t::name)
        .def_readonly("handle", &cme::DomainEntry_t::handle)
        .def("__repr__", [](const cme::DomainEntry_t& entry) {
            return "<DomainEntry " + entry.name + " id=" + std::to_string(entry.handle.id) + ">";
        });

    pybind::class_<cme::Session::FormatOpts_t>(module, "FormatOpts")
        .def(pybind::init<>())
        .def_readwrite("max_domains", &cme::Session::FormatOpts_t::maxDomains)
        .def_readwrite("max_peers", &cme::Session::FormatOpts_t::maxPeers)
        .def_readwrite("strategy", &cme::Session::FormatOpts_t::strategy)
        .def_readwrite("aggregator_groups", &cme::Session::FormatOpts_t::aggregatorGroups);

    pybind::class_<cme::Session::OpenOpts_t>(module, "OpenOpts")
        .def(pybind::init<>())
        .def_readwrite("format_timeout", &cme::Session::OpenOpts_t::formatTimeout)
        .def_readwrite("coherency", &cme::Session::OpenOpts_t::coherency);

    bindHeldDomain<cme::Session>(module, "HeldDomain");
    bindHeldDomain<cme::SharedSession>(module, "SharedHeldDomain");

    using SessionHeld = HeldDomain<cme::Session>;
    using SharedHeld = HeldDomain<cme::SharedSession>;

    pybind::class_<cme::Session>(module, "Session")
        // mkfs and not create-if-absent: it discards what the region's peers are using.
        .def_static(
            "format",
            [](const std::string& uri, const cme::Session::FormatOpts_t& opts) {
                pybind::gil_scoped_release released;
                cme::Session::format(uri, opts);
            },
            pybind::arg("uri"), pybind::arg("opts") = cme::Session::FormatOpts_t{})
        // Blocking: it opens the region and takes this node's peer slot.
        .def_static(
            "open",
            [](const std::string& uri, const cme::Session::OpenOpts_t& opts) {
                pybind::gil_scoped_release released;
                return cme::Session::open(uri, opts);
            },
            pybind::arg("uri"), pybind::arg("opts") = cme::Session::OpenOpts_t{})

        // ── participation ──────────────────────────────────────────
        .def(
            "join_domain",
            [](cme::Session& session, const std::string& name) {
                pybind::gil_scoped_release released;
                return session.joinDomain(name);
            },
            pybind::arg("name"))
        // PRECONDITION: this thread holds no turn on the domain. Leaving takes the same lock a held
        // turn keeps, so calling it inside a `with` block deadlocks on a non-recursive mutex.
        .def(
            "leave_domain",
            [](cme::Session& session, const std::string& name) {
                pybind::gil_scoped_release released;
                session.leaveDomain(name);
            },
            pybind::arg("name"))
        .def(
            "leave_domain",
            [](cme::Session& session, const cme::DomainHandle_t& handle) {
                pybind::gil_scoped_release released;
                session.leaveDomain(handle);
            },
            pybind::arg("handle"))

        // ── by handle ──────────────────────────────────────────────
        .def(
            "resolve_domain",
            [](const cme::Session& session, const std::string& name) {
                pybind::gil_scoped_release released;
                return session.resolveDomain(name);
            },
            pybind::arg("name"))

        // ── dynamic domains ────────────────────────────────────────
        .def(
            "create_domain",
            [](cme::Session& session, const std::string& name) {
                pybind::gil_scoped_release released;
                return session.createDomain(name);
            },
            pybind::arg("name"))
        .def(
            "delete_domain",
            [](cme::Session& session, const std::string& name) {
                pybind::gil_scoped_release released;
                session.deleteDomain(name);
            },
            pybind::arg("name"))

        // ── accessors ──────────────────────────────────────────────
        .def("domain_entries",
             [](const cme::Session& session) {
                 pybind::gil_scoped_release released;
                 return session.getDomainEntries();
             })
        .def("domain_names",
             [](const cme::Session& session) {
                 pybind::gil_scoped_release released;
                 return session.getDomainNames();
             })

        // ── acquire ────────────────────────────────────────────────
        // Hands back the box rather than acquiring, so the turn is taken by entering `with` and
        // returned by leaving it. keep_alive ties the box's life to the session's.
        .def(
            "lock",
            [](cme::Session& session, const std::string& name) {
                return std::make_unique<SessionHeld>([&session, name] { return session.lock(name); }, name);
            },
            pybind::arg("name"), pybind::keep_alive<0, 1>())
        .def(
            "lock",
            [](cme::Session& session, const cme::DomainHandle_t& handle) {
                return std::make_unique<SessionHeld>([&session, handle] { return session.lock(handle); },
                                                     nameHandle(handle));
            },
            pybind::arg("handle"), pybind::keep_alive<0, 1>())
        // None is the deadline alone. A name or handle that no live domain answers throws, so a
        // retry loop on a bad one fails fast rather than spinning.
        .def(
            "try_lock",
            [](cme::Session& session, const std::string& name,
               double timeout_seconds) -> std::unique_ptr<SessionHeld> {
                std::optional<cme::Guard> taken;
                {
                    pybind::gil_scoped_release released;
                    taken = session.tryLock(name, readNanoseconds(timeout_seconds));
                }
                if (!taken.has_value())
                {
                    return nullptr;
                }
                return std::make_unique<SessionHeld>(std::move(*taken), name);
            },
            pybind::arg("name"), pybind::arg("timeout_seconds") = 5.0, pybind::keep_alive<0, 1>())
        .def(
            "try_lock",
            [](cme::Session& session, const cme::DomainHandle_t& handle,
               double timeout_seconds) -> std::unique_ptr<SessionHeld> {
                std::optional<cme::Guard> taken;
                {
                    pybind::gil_scoped_release released;
                    taken = session.tryLock(handle, readNanoseconds(timeout_seconds));
                }
                if (!taken.has_value())
                {
                    return nullptr;
                }
                return std::make_unique<SessionHeld>(std::move(*taken), nameHandle(handle));
            },
            pybind::arg("handle"), pybind::arg("timeout_seconds") = 5.0, pybind::keep_alive<0, 1>())
;

    // The same region, for a process whose threads contend for one domain. Its lock adds the
    // intra-node tier that Session's per-peer token does not provide.
    pybind::class_<cme::SharedSession>(module, "SharedSession")
        .def_static(
            "open",
            [](const std::string& uri, const cme::Session::OpenOpts_t& opts) {
                pybind::gil_scoped_release released;
                return cme::SharedSession::open(uri, opts);
            },
            pybind::arg("uri"), pybind::arg("opts") = cme::Session::OpenOpts_t{})
        .def(
            "join_domain",
            [](cme::SharedSession& session, const std::string& name) {
                pybind::gil_scoped_release released;
                return session.joinDomain(name);
            },
            pybind::arg("name"))
        .def(
            "leave_domain",
            [](cme::SharedSession& session, const std::string& name) {
                pybind::gil_scoped_release released;
                session.leaveDomain(name);
            },
            pybind::arg("name"))
        .def(
            "create_domain",
            [](cme::SharedSession& session, const std::string& name) {
                pybind::gil_scoped_release released;
                return session.createDomain(name);
            },
            pybind::arg("name"))
        .def(
            "delete_domain",
            [](cme::SharedSession& session, const std::string& name) {
                pybind::gil_scoped_release released;
                session.deleteDomain(name);
            },
            pybind::arg("name"))
        .def(
            "lock",
            [](cme::SharedSession& session, const std::string& name) {
                return std::make_unique<SharedHeld>([&session, name] { return session.lock(name); }, name);
            },
            pybind::arg("name"), pybind::keep_alive<0, 1>())
        // None is the deadline alone, and the one budget covers both tiers. A name this session
        // never joined throws, as it does through lock.
        .def(
            "try_lock",
            [](cme::SharedSession& session, const std::string& name,
               double timeout_seconds) -> std::unique_ptr<SharedHeld> {
                // Constructed from the call rather than assigned: this Guard has no move-assign,
                // so an optional of it cannot be filled in a second step.
                auto taken = [&]() {
                    pybind::gil_scoped_release released;
                    return session.tryLock(name, readNanoseconds(timeout_seconds));
                }();
                if (!taken.has_value())
                {
                    return nullptr;
                }
                return std::make_unique<SharedHeld>(std::move(*taken), name);
            },
            pybind::arg("name"), pybind::arg("timeout_seconds") = 5.0, pybind::keep_alive<0, 1>())
        .def("set_cohort_cap", &cme::SharedSession::setCohortCap, pybind::arg("cap"));
}
