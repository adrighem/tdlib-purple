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

When reporting bugs or proposing fixes, include:

- what you tried
- your OS or distro
- the TDLib version or commit used
- the relevant build or runtime log

Debug logs may contain private names, phone numbers, chat titles, and message text. Remove sensitive data before posting logs publicly.
