# Contributing

Thanks for helping with this tdlib-purple upgrade fork.

Please keep changes small and focused. This makes it easier to review behavior changes and keep the TDLib upgrade work moving.

Before sending a change, please run:

```sh
cmake --build build --target telegram-tdlib
cmake --build build --target tests
cmake --build build --target run-tests
```

Configure that build directory first with the two owner-only credential file
paths documented in the README. If you use a different build directory, adjust
the commands accordingly.

For a focused check of Telegram forum-topic behavior:

```sh
./build/test/tests --gtest_filter='ForumTopic*.*'
```

Build and test against TDLib 1.8.65 or an API-compatible newer release. The pinned TDLib submodule is the supported baseline.

When reporting bugs or proposing fixes, include:

- what you tried
- your OS or distro
- the TDLib version or commit used
- the relevant build or runtime log

Debug logs may contain private names, phone numbers, chat titles, and message text. Remove sensitive data before posting logs publicly.

Never commit Telegram API credentials, login codes, authorization data, or
session files. Every production build takes only paths to two owner-only files
outside the source tree and fails if either input is unavailable or invalid.
CI uses synthetic values; releases use encrypted repository secrets. Purple 3
uses only the generated application provider. Purple 2 first accepts a complete
legacy per-account override for compatibility, then falls back to the provider;
the override remains in Purple 2's plaintext account settings. Never put
credential values in CMake options, compiler caches, logs, tests, or bug
reports.
