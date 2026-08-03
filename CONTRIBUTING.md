# Contributing

Thanks for helping improve this Purple plugin for Telegram.

Please keep changes small and focused. This makes behavior changes easier to
review and maintain.

Before sending a change, please run:

```sh
cmake --build build --target telegram-tdlib
cmake --build build --target tests
cmake --build build --target run-tests
```

The default application provider needs no credential setup. If you use a
different build directory, adjust the commands accordingly.

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

Never commit private Telegram API credentials, login codes, authorization data,
or session files. The maintained public application defaults are the only
credential pair intentionally distributed in the repository and source
releases. Custom builds take paths to two owner-only files outside the source
tree and fail if only one input is configured or either value is invalid.
Purple 3 uses only the generated application provider. Purple 2 first accepts a
complete legacy per-account override for compatibility, then falls back to the
provider; the override remains in Purple 2's plaintext account settings. Never
put private override values in CMake options, compiler caches, logs, tests, or
bug reports.

## Licensing contributions

Except where explicitly agreed otherwise, contributions are accepted under the
same `GPL-3.0-or-later` terms as the project. By submitting a contribution, you
confirm that you have the right to license it on those terms. This is an
inbound-equals-outbound policy, not a copyright assignment.

Keep all existing copyright, attribution, and third-party license notices.
Identify copied or adapted third-party code in the contribution and make sure
its license is compatible before submitting it.
