# tests/harness

What a case compiles against, and what the scripts under `tests/` source.
Nothing here is part of libcme.
The library reads no configuration and takes no flags; the harness is where both live.

## What a case compiles against

| | |
|---|---|
| [`test_options.hpp`](test_options.hpp) | The flags one run was given: `--backend shm\|dax\|uc`, `--slot N`, `--strategy`, `--config`, `--cleanup`. ctest passes them one registration at a time. |
| [`config_reader.hpp`](config_reader.hpp) | `config.yaml`, read once and checked against the machine: which devdax node this host has, where the uncacheable mount is. |
| [`test_memory.hpp`](test_memory.hpp) | One named area on the medium, owned by one run. Names it, starts it clean, hands out a URI or a mapped region, removes it afterwards. |
| [`test_context.hpp`](test_context.hpp) | The three above in the one order that works, plus the verdict and `runCase`. Also `SharedBuffer<T>`, a named shm segment for a forked child to report through. |
| [`helper.hpp`](helper.hpp) | What a case wants whatever medium it runs on: randomness, `waitUntil`, `percentile`, `threw<T>`, `seedDataDomains`. |
| [`args.hpp`](args.hpp) | `--flag value` lookup over `argv`, with no dependency on libcme. |

The order in `TestContext` is the point of it.
Flags come before config, config before medium, medium before region, and the verdict after everything.
Member declaration order is what enforces that, so a run cannot read a fact that is not settled yet.

`args.hpp` sits apart from `helper.hpp` for one reason.
The standalone probes in `bench/` need argument parsing and must not link libcme, because they measure the medium with `mmap` and intrinsics and pulling in `cme/shared.hpp` would defeat what they exist to measure.
So `args.hpp` holds the part that needs nothing, and `helper.hpp` includes it and adds the part that needs the library.

## What the scripts source

| | |
|---|---|
| [`script_args.sh`](script_args.sh) | `--flag value` parsing for the shell scripts under `tests/`, so a script takes options the way the binaries it drives take them. |
| [`site_config.sh`](site_config.sh) | `config.yaml` from a shell script. A missing file or key yields an empty string. |

An environment variable cannot be wrong, which is why neither of these reads one.
`REPEAT=5` where the script reads `REPEATS` is silently the default, and the run then reports a number nobody asked for.
A flag can be wrong, and both of these say so by name.

## A tool

| | |
|---|---|
| [`thp_exec.cpp`](thp_exec.cpp) | Clears `PR_SET_THP_DISABLE`, then execs the command it was given. |

Some interactive shells start with transparent huge pages disabled and pass that to every child.
That breaks devdax `mmap` on a PMD-aligned chardev: the fault path cannot install a huge mapping, and the first store takes SIGBUS.
Anonymous and file mappings are unaffected, so the state goes unnoticed until dax work hits it.
Every `*_dax` registration runs under this wrapper.

## The three readers of config.yaml

`cmake/SiteConfig.cmake` reads it at configure time, `config_reader.hpp` at run time from C++, and `site_config.sh` at run time from a script.
`config.yaml` is restricted to flat `key: value` lines precisely so that each of the three reads it with what it already has, and none of them needs a YAML parser.
A fourth reader is a reason to keep the format flat, not a reason to add a dependency.
