# tdlib-purple upgrade fork

This is a new fork of tdlib-purple. Its purpose is simple: get tdlib-purple building and working with a newer TDLib.

The goal is to keep Telegram support available for libpurple clients such as Pidgin and BitlBee while the codebase catches up with TDLib API changes.

This fork is currently in upgrade mode. Please expect some rough edges while the work settles.

## Focus

- update tdlib-purple for newer TDLib releases
- keep the plugin buildable and usable
- keep changes small enough to review and maintain

## Build

For a default local build and install:

```sh
./build_and_install.sh
```

That script builds the pinned TDLib submodule, builds tdlib-purple without VoIP support, and installs the plugin system-wide.

Manual CMake builds need CMake 3.16 or newer.

## Help

Bug reports and fixes are welcome. When reporting an issue, please include:

- what you tried
- your OS or distro
- the TDLib version or commit you built with
- the relevant build or runtime log

Debug logs can contain private data, including names, phone numbers, chat titles, and message text. Please remove sensitive data before sharing logs publicly.

For contribution guidelines, see [CONTRIBUTING.md](CONTRIBUTING.md).

Thanks for testing and helping move tdlib-purple forward.

## Upstream history

- https://github.com/ars3niy/tdlib-purple
- https://github.com/BenWiederhake/tdlib-purple
