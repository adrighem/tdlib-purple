# Purple 3 Development

This directory contains the experimental libpurple 3 adapter. It is separate
from the production Purple 2 plugin so that both frontends can evolve around
shared Telegram and TDLib code without version conditionals throughout the
codebase.

The current milestone:

- exposes the existing `telegram-tdlib` protocol identity
- builds against the `purple-3` pkg-config interface
- loads and unloads through GPlugin
- registers and removes a native `PurpleProtocol`
- uses the Pidgin account name only as a local display label
- exposes only an advanced secret-chat option
- creates a protocol-specific asynchronous `PurpleConnection` backed by the
  shared TDLib transport and authorization controller
- presents Telegram's rotating login QR code and, when required, a masked
  two-step-verification password prompt
- stores each TDLib session under the stable Purple account UUID

A default build can now connect an existing Telegram account through QR-only
authorization. Phone-number and SMS login and new-account registration are not
offered, so another
already-authorized Telegram client is required to scan the code. Telegram's
API ID and API hash identify the application even during QR login. They are
deliberately not Purple account settings, environment variables, or raw CMake
values. The optional two-step-verification password is requested when Telegram
requires it; it is masked and is not saved as a Purple account setting. Proxy
configuration is also not exposed as an account setting until Purple 3 has a
stable account proxy API.

This phase ends when TDLib reports the account ready. Contacts, chat lists,
incoming messages, and outgoing messages are not connected to Purple 3 yet, so
a successfully connected account does not provide normal Telegram chat
functionality.

The pinned Pidgin revision needs the tracked
[`pidgin-a412f2-qrcode-rendering.patch`](patches/pidgin-a412f2-qrcode-rendering.patch)
QR renderer compatibility patch.
Without its four-module quiet zone, Telegram's device-linking scanner rejects
the displayed pattern. The patch also makes failed encoding, QR rotation, and
widget cleanup safe. It is applied in CI to keep it compatible with the pinned
source revision, but it still needs to be applied to a local Pidgin checkout as
described below.

Pidgin preserves settings written by earlier development builds even after a
protocol stops advertising them. On load, the adapter removes the obsolete
phone-number, API-ID, and API-hash copies from its Pidgin 3 accounts and
persists the cleanup. Pidgin 3 stores these accounts separately from Purple 2,
so this does not alter the production plugin's profile. This is logical
removal from the active settings database, not forensic erasure of SQLite
storage or backups.

TDLib data is kept below the Purple data directory at
`telegram-tdlib/accounts/<account-uuid>`. The UUID is normalized to lowercase,
and the account directory is created with mode `0700`. Renaming the local
Pidgin account label does not change this location. The directory contains the
authorization state for that account and must be treated as private data. Each
Purple 3 account UUID gets an independent TDLib session. Purple 2 TDLib data is
not reused or migrated automatically.

## Build against a Pidgin development checkout

The commands below assume the Pidgin checkout is in `~/src/pidgin` and use its
uninstalled libpurple libraries and pkg-config files. This bootstrap is
currently tested against Pidgin revision `a412f2ead95d`, whose `purple-3`
pkg-config version is `2.96.1-dev`.

After checking out that Pidgin revision, apply the compatibility patch from the
tdlib-purple source root and rebuild Pidgin:

```sh
patch -d "$HOME/src/pidgin" -p1 \
  < "$PWD/purple3/patches/pidgin-a412f2-qrcode-rendering.patch"
meson compile -C "$HOME/src/pidgin/_build" \
  'pidgin/pidgin3:executable'
```

The patch is temporary integration work and should be dropped once the pinned
Pidgin revision contains an equivalent renderer fix.

In addition to the dependencies supplied by the Pidgin development
environment, this build requires CMake, Ninja, Python 3.8 or newer, and a TDLib
CMake package compatible with TDLib 1.8.65 or newer. If TDLib is not installed
in a standard CMake search location, add
`-DTd_DIR=/path/to/lib/cmake/Td` to the configure command.

The maintained default application provider needs no extra configuration:

```sh
cd "$HOME/src/tdlib-purple"

meson devenv -C "$HOME/src/pidgin/_build" \
  --workdir "$PWD" \
  cmake -S purple3 -B purple3/build -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug

meson devenv -C "$HOME/src/pidgin/_build" \
  --workdir "$PWD" \
  cmake --build purple3/build

meson devenv -C "$HOME/src/pidgin/_build" \
  --workdir "$PWD" \
  ctest --test-dir purple3/build --output-on-failure
```

To use a different Telegram application, set
`TDLIB_PURPLE_API_ID_FILE` and `TDLIB_PURPLE_API_HASH_FILE` to two distinct,
owner-only files outside the source tree. They must be readable regular files,
not symbolic links, and owned by the build user. On Unix, they must have no
group or other permission bits; mode `0600` is recommended. Each file may
contain one ASCII value followed by an optional LF or CRLF and must not exceed
64 bytes. The API ID must be a canonical positive decimal integer no greater
than `2147483647`; the API hash must contain exactly 32 hexadecimal characters.

Downstream pipelines that must always use their own application identity should
add all three options to the configure command:

```sh
-DTDLIB_PURPLE_REQUIRE_CUSTOM_CREDENTIALS=ON \
-DTDLIB_PURPLE_API_ID_FILE=/path/to/api_id \
-DTDLIB_PURPLE_API_HASH_FILE=/path/to/api_hash
```

`TDLIB_PURPLE_REQUIRE_CUSTOM_CREDENTIALS` defaults to `OFF`. When enabled,
configuration fails unless both custom files are supplied and valid. This
prevents an incomplete downstream configuration from silently using the
maintained project identity. It is a build-policy flag for identity enforcement,
not a secrecy control. Telegram API IDs and hashes are public identifiers and
remain extractable from the built plugin.

CMake stores custom file paths, but not their contents, in the build directory's
cache. Omitting the `-D` options during a later reconfiguration does not clear
them. Set both paths to empty to return to the maintained defaults:

```sh
meson devenv -C "$HOME/src/pidgin/_build" \
  --workdir "$PWD" \
  cmake -S purple3 -B purple3/build \
  -DTDLIB_PURPLE_REQUIRE_CUSTOM_CREDENTIALS=OFF \
  -DTDLIB_PURPLE_API_ID_FILE= \
  -DTDLIB_PURPLE_API_HASH_FILE=
```

The former raw `API_ID`, `API_HASH`, and `STUFF` CMake cache variables are not
accepted. If an old build directory still contains one, configuration removes
the entry and stops with `CREDENTIAL_LEGACY_CACHE_REMOVED`. Remove the raw
option from build automation, configure again with the two file-path options,
and never move the values themselves onto the command line.

The provider refreshes whenever the plugin is built. Rebuilding after either
custom value changes embeds the new pair. A single configured path, a missing
custom file, or an invalid custom value stops the build and removes the
generated provider. With fail-closed custom mode enabled, omitting both paths
also stops configuration.

Generated source, object files, and the plugin contain extractable application
identifiers. The maintained defaults are intentionally distributed, and custom
API IDs and hashes are public too. Previously built objects or binaries may
retain an older identity after returning to defaults. Use a clean build directory
when switching identities. File ownership and mode checks protect build input
integrity; they do not make the identifiers secret. POSIX ownership guarantees
do not have an equivalent ACL check on Windows.

Credential tests cover the maintained identity, synthetic overrides,
fail-closed custom mode, and test backends. CI build coverage exercises both the
maintained default and synthetic custom identities. The production smoke test
verifies generated provider state without connecting accounts to Telegram or
accessing the network. It uses in-memory Purple backends and an isolated
temporary profile.

### Credential diagnostics

Credential setup failures report stable, value-free diagnostic codes:

| Diagnostic | Meaning |
| --- | --- |
| `CREDENTIAL_PATHS_INCOMPLETE` | Only one credential path was configured. |
| `CREDENTIAL_PATHS_REQUIRED` | Fail-closed custom mode was enabled without either credential path. |
| `CREDENTIAL_DEFAULT_INVALID` | The maintained default pair is malformed. |
| `CREDENTIAL_LEGACY_CACHE_REMOVED` | A legacy raw credential cache entry was removed; migrate the build to the two file-path options and configure again. |
| `CREDENTIAL_INPUT_MISSING` | The configured file pair is incomplete on disk. |
| `CREDENTIAL_INPUT_DUPLICATE` | Both paths resolve to the same file. |
| `CREDENTIAL_INPUT_IN_SOURCE_TREE` | A credential file is inside the source tree. |
| `CREDENTIAL_INPUT_UNSAFE` | An input is unreadable, oversized, linked, not a regular file, incorrectly owned, or too broadly accessible. |
| `CREDENTIAL_API_ID_INVALID` | The API ID has the wrong decimal format or range. |
| `CREDENTIAL_API_HASH_INVALID` | The API hash is not exactly 32 hexadecimal characters. |
| `CREDENTIAL_OUTPUT_ERROR` | The private generated provider could not be written safely. |
| `CREDENTIAL_GENERATOR_FAILED` | The credential generator failed unexpectedly. |

Current production targets cannot create a plugin without an embedded provider.
A plugin without that provider is a broken build, not a supported credentialless
variant. The runtime `Telegram application credentials are unavailable in this
build` message therefore indicates an obsolete or externally modified binary.
A valid build starts Telegram authorization and presents a QR code. Subsequent
connection errors describe an authorization, Purple UI, account-storage, or
TDLib backend failure without including the configured credential values.

## Inspect and verify in an isolated Pidgin 3 profile

Use a dedicated profile so the account database and TDLib data do not mix with
your normal Pidgin profiles. Create it once with owner-only access:

```sh
mkdir -p "$HOME/.local/share/tdlib-purple-pidgin3-profile"
chmod 700 "$HOME/.local/share/tdlib-purple-pidgin3-profile"
```

Close any existing Pidgin 3 process first. From the tdlib-purple source root,
inspect the account editor without connecting accounts:

```sh
PURPLE_PLUGIN_PATH="$PWD/purple3/build" \
  meson devenv -C "$HOME/src/pidgin/_build" \
  --workdir "$HOME/src/pidgin" \
  pidgin3 \
  --config="$HOME/.local/share/tdlib-purple-pidgin3-profile" \
  --nologin
```

Open the account editor and confirm that `Telegram` is available in
the protocol chooser, requires only a local Account Name, and shows no
phone-number, API-ID, or API-hash setting. The advanced view contains the
secret-chat option. `--nologin` prevents existing accounts from connecting.

To verify authorization, build with the maintained defaults or the optional
custom override described above, then launch without `--nologin`:

```sh
PURPLE_PLUGIN_PATH="$PWD/purple3/build" \
  meson devenv -C "$HOME/src/pidgin/_build" \
  --workdir "$HOME/src/pidgin" \
  pidgin3 \
  --config="$HOME/.local/share/tdlib-purple-pidgin3-profile"
```

Create or enable a `Telegram` account. On an already-authorized phone,
open Telegram's **Settings > Devices > Link Desktop Device** screen and scan
the displayed QR code. Do not use the phone's normal camera or a generic QR
scanner. Telegram refreshes the short-lived code automatically, so scan the
currently displayed pattern. If the Telegram account uses two-step
verification, complete the masked password prompt. An account whose
UUID-specific TDLib data is already authorized may reconnect without showing a
new QR code. Treat a displayed QR code as a temporary login credential and do
not share it. The pinned Pidgin UI also offers `Open Link` and
`Copy Link Address` from the QR context menu. Do not use those actions for a
Telegram login QR code: an external URI handler or clipboard history can
retain the temporary login token.

Successful authorization currently proves only the connection and
authorization lifecycle: the account reaches Purple's ready state, but
contacts and messages are still outside this milestone.

Reuse the same isolated profile to test reconnection. Choose a different empty
profile directory for a fresh authorization test. There is no automatic
migration of Purple 2 accounts, account databases, or TDLib data into this
profile.
