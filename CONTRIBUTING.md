# Contributing

Any contribution is welcome.
A typo fix, a question that turns out to be a documentation gap, a measurement that contradicts ours, a bug report with no patch attached: all of it helps, and none of it needs to be large.

If you are unsure whether something is worth raising, raise it.

## Where things go

Questions and design discussion belong in [Discussions](../../discussions).
Bugs and concrete proposals belong in [Issues](../../issues).

For a big change, an issue first saves you a rewrite, because the on-region layout and the recovery protocol are both still moving.
For anything small, just open the pull request.

## Style

Two tools decide it, so there is nothing to memorise:

```sh
find src tests tools examples -name '*.cpp' -o -name '*.hpp' | xargs clang-format-18 -i
```

`clang-format` is pinned to 18, because its output changes between major releases.
`clang-tidy` runs from [`.clang-tidy`](.clang-tidy), and CI checks only the lines you touched.

The one rule that changes how you write rather than how it formats: every identifier is at least 3 characters.
`domainId`, not `id`.
`file`, not `fd`.

Comments say what the code does now, never what it used to do.
That belongs in the commit message.

## Testing

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build -L shm --output-on-failure
```

`shm` needs no special hardware and is what CI runs.
The `dax` and `uc` labels need a devdax node and an uncacheable mount ([`BUILD.md`](BUILD.md)).
Run those too if your change touches the medium, the fences, or recovery.

Run the contended cases serially.
Several assert a fairness bound, so a loaded machine can fail them for reasons unrelated to your change.

If you change the protocol, update [`docs/spec/`](docs/spec/) with it.
It is normative and `src/core/` implements it.

## Licence

Apache-2.0, as in [`LICENSE`](LICENSE).
Keep the SPDX line on new files.
