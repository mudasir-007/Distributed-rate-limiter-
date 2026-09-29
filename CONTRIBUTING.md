# Contributing

## Development setup

Follow the setup instructions in `README.md`.

Before opening a pull request:

```bash
cmake --preset linux-release
cmake --build --preset linux-release
ctest --preset linux-release
./build/concurrency_demo
```

If the change affects concurrency or synchronization, also run the ThreadSanitizer configuration.

## Code expectations

- Keep the implementation C++17-compatible.
- Prefer small, testable changes.
- Preserve atomicity of the rate-limit decision.
- Do not introduce retries around non-idempotent distributed operations without documenting the failure semantics.
- Add or update tests when behavior changes.
- Keep benchmark output descriptive and reproducible.
- Do not commit `.env`, build directories, generated binaries, or benchmark output.

## Pull requests

A useful pull request should describe:

1. what changed;
2. why it changed;
3. how it was tested;
4. whether Redis was required;
5. whether benchmark behavior changed;
6. any concurrency or failure-mode implications.
