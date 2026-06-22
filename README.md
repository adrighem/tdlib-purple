# tdlib-purple upgrade fork

This repository is an updated fork of tdlib-purple, a Telegram plugin for libpurple clients such as Pidgin and BitlBee.

The main goal is to keep tdlib-purple usable with newer TDLib versions while making it easier to install and test on current Linux systems.

## What This Fork Provides

- an actively updated tdlib-purple fork
- a pinned TDLib submodule for reproducible builds
- CI builds and tests for common build options
- release-please managed changelogs and version bumps
- GitHub Releases with ready-to-install Linux packages

Release assets currently include:

- Linux x86-64 tarball
- Debian stable `.deb`
- Ubuntu 24.04 LTS `.deb`
- Fedora 44 `.rpm`
- Enterprise Linux 9 compatible `.rpm`

Download packages from the latest GitHub Release:

https://github.com/adrighem/tdlib-purple/releases/latest

## Building Locally

For a default local build and install:

```sh
./build_and_install.sh
```

That script builds the pinned TDLib submodule, builds tdlib-purple without VoIP support, and installs the plugin system-wide.

Manual CMake builds need CMake 3.16 or newer. CMake prefers system `fmt` and `rlottie` when available, with bundled fallbacks for local builds.

## Reporting Issues

Bug reports and fixes are welcome. Please include:

- what you tried
- your OS or distro
- which package or build method you used
- the relevant build or runtime log

Debug logs can contain private data, including names, phone numbers, chat titles, and message text. Please remove sensitive data before sharing logs publicly.

For contribution guidelines, see [CONTRIBUTING.md](CONTRIBUTING.md).
For release details, see [RELEASING.md](RELEASING.md).

## Upstream History

This fork builds on the earlier tdlib-purple work:

- https://github.com/ars3niy/tdlib-purple
- https://github.com/BenWiederhake/tdlib-purple
