# Improvement and Modernization Plan

This fork is focused on getting tdlib-purple building and working with newer TDLib releases while keeping the project maintainable for future upgrades.

## Top Recommendations

1. Fix CI first.
   - Move off retired GitHub Actions images.
   - Use current supported action majors.
   - Re-enable the test build and full test run in CI.
   - Treat passing CI as the gate for larger refactors.

2. Split `td-client.cpp` by responsibility.
   - Extract authentication, proxy handling, chat sync, contacts, message sending, message updates, history fetching, and room list behavior into separate modules.
   - Keep the public `PurpleTdClient` surface stable while moving private implementation details out of the large translation unit.

3. Create a TDLib compatibility layer.
   - Centralize TDLib request construction and update interpretation.
   - Keep TDLib API churn out of libpurple-facing code and most tests.
   - Future TDLib upgrades should mostly touch this layer.

## High Value Refactors

1. Turn `tdlib-purple.cpp` into a thin libpurple protocol adapter.
   - Keep libpurple callback registration and callback glue there.
   - Move behavior into focused modules that can be tested without the plugin table.

2. Modernize CMake incrementally.
   - Prefer target-scoped includes, compile definitions, compile options, and link libraries.
   - Reduce global `include_directories()` and `link_directories()` usage.
   - Keep compatibility with the distributions this fork realistically supports.

3. Make the dependency strategy explicit.
   - Prefer system `fmt` and `rlottie` when available.
   - Keep bundled fallbacks for simple source builds unless CI proves system-only builds are reliable everywhere this fork supports.

4. Add a TDLib pin consistency check.
   - CI should fail if the `td` submodule commit and the workflow TDLib commit drift apart.
   - This prevents testing against a different TDLib version than the one checked into the repo.

5. Keep tests central.
   - Preserve and expand the existing TDLib/libpurple mock tests.
   - Add tests for each compatibility-layer behavior before and during future TDLib upgrades.

## Project Hygiene

1. Move the project version to `0.9.0`.
   - This fork is no longer just upstream `0.8.1`.
   - Use `0.9.0` as the first fork-maintained upgrade line.

2. Keep public docs short but useful.
   - README should state fork status, quick build notes, known rough edges, privacy guidance for logs, and contribution expectations.

3. Add contributor guidance.
   - Ask contributors for small changes, relevant logs, test results, and the TDLib version or commit used.

4. Clean repository metadata.
   - Remove duplicate ignore entries.
   - Keep generated build outputs, local planning artifacts, and large scratch files out of git.

## Implementation Order

1. [x] CI modernization and full tests in CI.
2. [x] TDLib pin consistency check.
3. [x] Project hygiene, including version `0.9.0`.
4. [x] First mechanical split of `td-client.cpp` with no behavior changes.
5. [x] CMake modernization.
6. [x] Dependency strategy for `fmt` and `rlottie`.
7. [x] Documentation and prerelease preparation.

## Implemented in This Pass

- GitHub Actions now uses supported runners/actions, verifies the TDLib pin, builds tests, runs the primary test matrix, and keeps CodeQL enabled.
- The fork version is `0.9.0`, with refreshed README, contribution guidance, project metadata, and ignore rules.
- Authentication, proxy setup, and TDLib parameter handling were split from `td-client.cpp` into `td-client-auth.cpp`.
- CMake now uses target-scoped include/link wiring, a shared plugin source list for the library and tests, and CMake 3.16 as the baseline.
- `fmt` and `rlottie` now prefer system packages when present and keep bundled fallbacks for simple local builds.

## Remaining Architecture Work

- Continue splitting `td-client.cpp` by domain: contacts, chats, message send/update flow, history, and room lists.
- Add a TDLib compatibility layer before the next large TDLib upgrade so API churn is isolated.
- Thin `tdlib-purple.cpp` down to libpurple registration and callback glue.
- Add focused tests around each extracted domain before changing behavior.
