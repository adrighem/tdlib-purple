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

1. CI modernization and full tests in CI.
2. TDLib pin consistency check.
3. Project hygiene, including version `0.9.0`.
4. Mechanical split of `td-client.cpp` with no behavior changes.
5. CMake modernization.
6. Dependency strategy for `fmt` and `rlottie`.
7. Documentation and prerelease preparation.
