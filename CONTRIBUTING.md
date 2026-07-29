# Contributing

Thanks for helping with this tdlib-purple upgrade fork.

Please keep changes small and focused. This makes it easier to review behavior changes and keep the TDLib upgrade work moving.

Before sending a change, please run:

```sh
cmake --build build --target telegram-tdlib
cmake --build build --target tests
cmake --build build --target run-tests
```

If you use a different build directory, adjust the commands accordingly.

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

Never commit Telegram API credentials, login codes, authorization data, or session files. Configure API credentials through the local Pidgin account settings and remove credentials from logs before sharing them.
