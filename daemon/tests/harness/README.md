# daemon/tests/harness

What a cmed probe compiles against.
Nothing here is part of `cmed_core`, `cmed_client` or `cmed_service`.

A probe includes `harness/helper.hpp` and links `cmed_test_harness`, which carries the include path and the daemon-side library the fixture needs.
It also carries `cme_harness`, so a probe reaches the cme harness headers that resolve a medium name against the site config.

| | |
|---|---|
| [`probe_context.hpp`](probe_context.hpp) | The checks one probe records, and the entry point that runs it. A check names itself as it goes, so a red probe says which expectation failed rather than only returning 1. The exit code is derived from the tally. |
| [`helper_area.hpp`](helper_area.hpp) | One area owned by one probe: named, started clean, removed afterwards. Also the registry writes a case needs before any name resolves, the questions a case asks about a domain's state or its mutex, and the one surface that names a field of the shared structs. |
| [`helper_daemon.hpp`](helper_daemon.hpp) | The daemon's half of the exchange, as much of it as a requester can tell apart. One stub, with the answer to an acquire injected. |
| [`helper_handler.hpp`](helper_handler.hpp) | The handler a probe hands a daemon loop, and the per-domain tally it kept. Which ids reached it and how many times each, without the stub or the serve turn. |
| [`helper_process.hpp`](helper_process.hpp) | Forking a case into several processes over one area, and reaping them. POSIX only. |
| [`helper.hpp`](helper.hpp) | Everything above together, so a probe includes one name and gets all of `cmed::harness`. |
| [`helper_scratch.hpp`](helper_scratch.hpp) | One run's own directory and the shm names it made, both removed when the run ends. The pid in every name is what keeps two runs of the same probe apart. |
| [`helper_requester.hpp`](helper_requester.hpp) | A requester on an area a fixture already laid down, at the compiled-in deadlines. Needs the client library. |
| [`helper_socket.hpp`](helper_socket.hpp) | Both ends of one connection in one process, and the listening socket that owns the name. A case drops either end and reads what the other one then sees. |
| [`helper_cme_region.hpp`](helper_cme_region.hpp) | One cme region a probe owns for as long as it runs, formatted and unlinked by the fixture. Needs libcme. |
| [`helper_medium.hpp`](helper_medium.hpp) | Which medium a probe that starts a real daemon runs on: a backend name and a window index become a URI and the coherency word a daemon config takes. Needs libcme and the cme harness. |

Each header is separated by what it drags in rather than by what it is about.
`probe_context.hpp` needs no cmed header at all, `helper_area.hpp` needs the daemon's half because formatting zeroes the area, `helper_daemon.hpp` reaches the area through `helper_area.hpp` and so needs the same, `helper_handler.hpp` needs the callback type and that same area header for the domain ceiling and the poll step, and `helper_process.hpp` needs POSIX.

The headers below `helper.hpp` in the table stay out of it for that same reason.
A probe naming a scratch path needs neither library, and the ones that reach for the client library or libcme would put that dependency on every probe if the umbrella carried them.
`helper_socket.hpp` stays out for the opposite reason: both of its callers link `cmed_shared` alone, and the umbrella reaches the daemon's library through `helper_area.hpp`.

## The stub daemon takes a policy

The three answers a requester can get are not variations on one happy path.
A grant, a refusal carrying an errno, and no answer at all each drive a different exit from `CmedSession::tryLock`.
So `StubDaemon` takes a `ServePolicy` and there is one stub rather than three: the drain loop, and the rule that the doorbell is read before draining, live in one place.

`grantEveryLock()` is the default and is what a case whose subject is the requester wants.
`refuseEveryLock(-EPERM)` and `answerNoLock()` are the other two.
A release is served whatever the policy says.
A stub that withheld one would hold the requester inside its release exchange for the whole `ReleaseTimeout`, and a case measuring the caller's own deadline would measure that instead.

## What a child may do

A child reports through the shared area and never through `ProbeContext`.
Its checks would print, and its tally would die with it.
The parent reads the words the child wrote and records the checks itself.
