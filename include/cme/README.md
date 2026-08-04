# include/cme/ — the public API

Everything a caller needs is here.
Nothing else in the tree is a supported include.

Include the umbrella and you get the rest:

```cpp
#include "cme/cme.hpp"
```

| | |
|---|---|
| `cme.hpp` | Umbrella include. Pulls in the three headers below. |
| `shared.hpp` | `Session`, `Guard`, and `Strategy`. The usage model is written out in this header's doc comment, which is the place to read before the design record. |
| `shared_session.hpp` | `SharedSession`, for many threads of one process over a single `Session`. Use it when threads share a peer slot instead of taking one each. |
| `errors.hpp` | The exception hierarchy. Every error derives from `std::runtime_error`, so a caller that only wants "it failed" can catch that. |

## Shape of the API

A `Session` is a peer slot inside the region, not a connection to a service.
`lock()` is loads and stores against mapped memory, so there is no server to reach and no lease to renew.

A `Guard` releases in its destructor.
`lock()` throws `LockTimeoutError` when the deadline passes rather than blocking forever.

`joinDomain()` is required before `lock()` on that domain, and `lock()` throws without it.
Domains are discoverable by name but not self-describing: the region arbitrates a name, and what that name protects stays a convention between callers.
