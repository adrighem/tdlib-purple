# Unofficial Telegram for Purple

An unofficial libpurple plugin for Telegram chats in Pidgin, Finch, BitlBee,
and other Purple clients. Uses Telegram's API through TDLib.

[Download the latest release](https://github.com/adrighem/tdlib-purple/releases/latest)

## Client status

| Client API | Status |
| --- | --- |
| Purple 2, including Pidgin 2 and BitlBee | The primary, usable plugin. |
| Purple 3, including Pidgin 3 development builds | Developer preview. QR authorization reaches the ready state, but contacts and messages are not exposed yet. |

The installed plugin and protocol are displayed as **Unofficial Telegram**.
Package, library, and internal protocol identifiers retain the `tdlib-purple`
and `telegram-tdlib` names so upgrades and existing accounts remain compatible.

## Features at a glance

The Purple 2 plugin turns Telegram features into native Purple conversations
and controls:

| Area | What is supported |
| --- | --- |
| Conversations | Private messages, group chats, channels, typing updates, and read receipts |
| Communities | Room discovery, group invitations, supergroups, and forum topics as separate rooms |
| Contacts | Telegram contact synchronization, add and remove actions, aliases, and profile information |
| Media | Send and receive files, configurable inline downloads, static stickers, and animated stickers when image support is available |
| Rich text | Bold, italic, underline, strikethrough, inline code, preformatted text, block quotes, spoilers, and supported links |
| Privacy | Optional secret chats, self-destructing-message display, and read-receipt controls where the client supports them |
| Account access | Phone-number sign-in, Telegram authentication codes, and masked two-step-verification password prompts |

Forum-enabled groups keep General on the existing room identity. Other topics
appear as separate rooms, and sends, uploads, failures, and read receipts stay
within the selected topic. Use an official Telegram client for topic
administration and notification settings.

Unsupported rich-text styling is shown as plain text instead of being discarded.
Official packages are built without voice or video calling support.

## Install

Official releases provide five credentialed Linux x86-64 packages and one
complete source archive:

| System | Asset |
| --- | --- |
| Debian stable | `tdlib-purple_*_debian-stable_amd64.deb` |
| Ubuntu 24.04 LTS | `tdlib-purple_*_ubuntu-24.04-lts_amd64.deb` |
| Fedora 44 | `tdlib-purple-*_fedora-44_x86_64.rpm` |
| Enterprise Linux 9 compatible | `tdlib-purple-*_el9_x86_64.rpm` |
| Other compatible Linux systems | `tdlib-purple-*-linux-x86_64.tar.xz` |
| Complete corresponding source | `tdlib-purple-*-source.tar.xz` |

Download the package for your system from the
[latest release](https://github.com/adrighem/tdlib-purple/releases/latest).

### Debian or Ubuntu

Install the downloaded package with APT. For example:

```sh
# Debian stable
sudo apt install ./tdlib-purple_*_debian-stable_amd64.deb

# Ubuntu 24.04 LTS
sudo apt install ./tdlib-purple_*_ubuntu-24.04-lts_amd64.deb
```

Run only the command for your distribution.

### Fedora or Enterprise Linux

Install the downloaded package with DNF. For example:

```sh
# Fedora 44
sudo dnf install ./tdlib-purple-*_fedora-44_x86_64.rpm

# Enterprise Linux 9 compatible systems
sudo dnf install ./tdlib-purple-*_el9_x86_64.rpm
```

### Generic tarball

Prefer a native package when one is available. To install the tarball manually:

```sh
mkdir -p telegram-purple-release
tar -xf tdlib-purple-*-linux-x86_64.tar.xz -C telegram-purple-release
sudo cp -a telegram-purple-release/usr/. /usr/
```

Fully quit and restart the Purple client after installing or upgrading the
plugin. A client that was already running may continue using the old library
until it exits.

Official release packages already contain the required Telegram application
provider. Users do not need to obtain or enter an API ID or API hash.

## Add a Telegram account in Pidgin 2

1. Open **Accounts > Manage Accounts > Add**.
2. Select **Unofficial Telegram** as the protocol.
3. Enter your phone number in international form, using digits and an optional
   leading `+`, with no spaces.
4. Save and enable the account.
5. Enter the authentication code delivered by Telegram.
6. Enter your Telegram two-step-verification password if requested.

Leave the API ID and API hash compatibility overrides in the Advanced tab
empty. They exist only for older Purple 2 account configurations and are not
needed with an official package.

Telegram session data is stored inside your Purple profile. Treat that profile
as private data and include it in backups only when the backup is protected.

## Troubleshooting

### "Telegram application credentials are missing or invalid"

This message means the installed plugin binary is broken or obsolete. It is not
an account-password problem, and a credentialless build is never a supported
variant.

Install the latest official package, make sure an older copy of
`libtelegram-tdlib.so` is not taking precedence in another plugin directory,
then fully quit and restart the client.

### Telegram is not listed as a protocol

Confirm that the package matches your distribution and CPU architecture, then
restart the client. If you previously installed the tarball or built from
source, check for an older plugin in a user-local plugin directory.

### Telegram rejects the login

Check that the account name contains the complete international phone number
without spaces. Authentication codes expire and can be superseded by a newer
code. If the account uses two-step verification, enter that Telegram password
when prompted.

### Forum-topic limitations

The plugin routes messages to existing topics but does not manage topic
administration, notification, or mute settings. Use an official Telegram client
for those operations.

## Building from source

Source builds require CMake 3.16 or newer, Python 3.8 or newer, and TDLib 1.8.65
or an API-compatible newer version. The pinned TDLib submodule is the supported
and tested schema.

Every build requires a valid Telegram application API ID and API hash in two
separate owner-only files outside the source tree. A build without an embedded
provider is broken, so configuration and rebuilds fail closed when either input
is missing or invalid.

```sh
TDLIB_PURPLE_API_ID_FILE=/path/to/api_id \
TDLIB_PURPLE_API_HASH_FILE=/path/to/api_hash \
  ./build_and_install.sh
```

The API ID file must contain a positive decimal integer no greater than
`2147483647`. The API hash file must contain exactly 32 hexadecimal characters.
On Unix, both files must be regular files owned by the build user, with no group
or other permission bits. Mode `0600` is recommended.

Only file paths enter CMake. Do not place credential values in source files,
CMake arguments, logs, caches, test output, or bug reports. Generated source,
objects, and plugin binaries contain extractable credentials, so protect build
directories and disable compiler caches, remote compilation, and automatic
compiler-crash uploads.

To uninstall a build installed by the convenience script:

```sh
./build_and_install.sh uninstall
```

See [CONTRIBUTING.md](CONTRIBUTING.md) for development and test commands,
[purple3/README.md](purple3/README.md) for the Purple 3 preview, and
[RELEASING.md](RELEASING.md) for the release process.

## Reporting issues

[Open an issue](https://github.com/adrighem/tdlib-purple/issues) with your
distribution, Purple client and version, package or build method, and the
smallest relevant log excerpt.

Debug logs can contain names, phone numbers, chat titles, message text, and
authorization data. Remove private data before sharing a log publicly. Never
share API credentials, login codes, session files, cookies, or tokens.

## License and acknowledgments

Except where a file or directory states otherwise, Unofficial Telegram for
Purple is free software under the GNU General Public License, version 3 or (at
your option) any later version. See [LICENSE](LICENSE).

The repository also contains third-party code under compatible licenses. TDLib
uses the Boost Software License 1.0; fmt and rlottie retain the licenses and
notices in their source directories. Release packages install those license
texts, and each release includes the exact TDLib source used to build it.

This project is powered by TDLib and builds on earlier work by the
[original tdlib-purple project](https://github.com/ars3niy/tdlib-purple) and
[Ben Wiederhake's continuation](https://github.com/BenWiederhake/tdlib-purple).

This is an independent, unofficial project and is not affiliated with or
endorsed by Telegram.
