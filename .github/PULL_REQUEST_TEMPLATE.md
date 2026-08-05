<!-- CONTRIBUTING.md has the style rules and the test commands. Small PRs need little here. -->

## What and why

<!-- What changed, and what problem it solves. Link an issue if there is one. -->

## Testing

<!-- What you ran and what it said. "ctest -L shm: 61/61" is checkable, and "works for me" is not. -->

## Checklist

- [ ] `clang-format-18` leaves the tree unchanged.
- [ ] `ctest -L shm` passes.
  Plus `dax`/`uc` if this reaches the medium, the fences, or recovery.
- [ ] `docs/spec/` matches, if the protocol or the region layout changed.
