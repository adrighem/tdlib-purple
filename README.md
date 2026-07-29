# tdlib-purple upgrade fork

This repository is an updated fork of tdlib-purple, a Telegram plugin for libpurple clients such as Pidgin and BitlBee.

The main goal is to keep tdlib-purple usable with newer TDLib versions while making it easier to install and test on current Linux systems.

## What This Fork Provides

- an actively updated tdlib-purple fork
- a pinned TDLib submodule for reproducible builds
- CI builds and tests for common build options
- release-please managed changelogs and version bumps
- GitHub Releases with ready-to-install Linux packages
- conservative rich text translation between libpurple HTML and TDLib `formattedText`

Release assets currently include:

- Linux x86-64 tarball
- Debian stable `.deb`
- Ubuntu 24.04 LTS `.deb`
- Fedora 44 `.rpm`
- Enterprise Linux 9 compatible `.rpm`

Rich text support intentionally uses a small libpurple-compatible HTML subset:

- bold, italic, underline, strikethrough, inline code, preformatted text, block quotes, and spoilers
- HTTP(S), Telegram user, and matching `mailto:` links
- incoming Telegram URL, email, mention, pre-code, and spoiler entities are shown as libpurple HTML

Unsupported styling such as font face, size, color, arbitrary spans, lists, and tables is treated as plain text.

## Telegram Forum Topics

Forum-enabled supergroups use a compatibility-first room layout:

- General retains the legacy libpurple room identity, preserving existing room entries and conversation logs.
- Each non-General topic is exposed as a separate libpurple chat room.
- Incoming messages, text sends and send failures, document uploads, and read receipts retain their exact topic routing.
- If a child topic becomes unavailable, the plugin refuses the send instead of silently falling back to General.
- When a group changes between ordinary and forum mode, its existing base room maps to or from General without changing identity.

Topic-specific administration actions and notification or mute controls are intentionally deferred. Use an official Telegram client for those operations.

Download packages from the latest GitHub Release:

https://github.com/adrighem/tdlib-purple/releases/latest

## Building Locally

For a default local build and install:

```sh
./build_and_install.sh
```

That script builds the pinned TDLib submodule, builds tdlib-purple without VoIP support, and installs the plugin system-wide.

To uninstall a local build installed this way:

```sh
./build_and_install.sh uninstall
```

For manual CMake builds, use `sudo cmake --build build --target uninstall` from the repository root, or `sudo make uninstall` inside a Makefile-generated build directory.

Manual CMake builds need CMake 3.16 or newer and TDLib 1.8.65 or an API-compatible newer release. CMake prefers system `fmt` and `rlottie` when available, with bundled fallbacks for local builds. The pinned TDLib submodule is the supported and tested schema.

The repository and release packages intentionally do not embed Telegram API credentials. Register an application through [my.telegram.org](https://my.telegram.org), then enter its API ID and API hash in the Telegram account's Advanced settings in Pidgin. Do not put these values in source files, CMake command lines, logs, or bug reports.

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
