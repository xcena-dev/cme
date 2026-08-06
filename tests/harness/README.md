# tests/harness

What a case compiles against, and what the scripts under `tests/` source.
Nothing here is part of libcme.
The library reads no configuration and takes no flags; the harness is where both live.

## What a case compiles against

| | |
|---|---|
| [`test_options.hpp`](test_options.hpp) | The flags one run was given: which medium, which slot on it, which successor policy, where the config file is, and whether this invocation is the cleanup pass rather than the run. ctest passes them one registration at a time. |
| [`config_reader.hpp`](config_reader.hpp) | `config.yaml`, read once and checked against the machine: which devdax node this host has, where the uncacheable mount is. |
| [`test_memory.hpp`](test_memory.hpp) | One named area on the medium, owned by one run. Names it, starts it clean, hands out a URI or a mapped region, removes it afterwards. |
| [`test_context.hpp`](test_context.hpp) | The three above in the one order that works, plus the verdict a case records against and the entry point that runs one. Also a named shm segment for a forked child to report through. |
| [`helper.hpp`](helper.hpp) | The three below together, so a case includes one name and gets all of `harness`. |
| [`helper_util.hpp`](helper_util.hpp) | What a case needs that has nothing to do with cme: randomness, a timestamped log line, waiting for a predicate to hold and holding one across a window, running a fixed set of threads to completion, catching a single exception type, and one summary statistic. No cme header, no `TestContext`. |
| [`helper_process.hpp`](helper_process.hpp) | Forking a case into several processes over one region, and collecting them afterwards. POSIX only. |
| [`helper_cme.hpp`](helper_cme.hpp) | The library calls every case makes the same way: formatting a region, opening one at each tier, seeding it with domains, asking it what it holds, and reading a member slot or a domain record past the public API. |
| [`args.hpp`](args.hpp) | `--flag value` lookup over `argv`, with no dependency on libcme. |

The order in `TestContext` is the point of it.
Flags come before config, config before medium, medium before region, and the verdict after everything.
Member declaration order is what enforces that, so a run cannot read a fact that is not settled yet.

The helper split follows the same rule, and `args.hpp` is why the rule exists.
The standalone probes in `bench/` need argument parsing and must not link libcme, because they measure the medium with `mmap` and intrinsics and pulling in `cme/shared.hpp` would defeat what they exist to measure.
So each of these headers is separated by what it drags in rather than by what it is about: `args.hpp` and `helper_util.hpp` need nothing, `helper_process.hpp` needs POSIX, and `helper_cme.hpp` needs the library.
A file that includes `helper.hpp` takes all three, which is what a case wants and what a probe must not do.

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
