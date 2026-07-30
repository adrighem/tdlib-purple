# Purple 3 Bootstrap

This directory contains the experimental libpurple 3 adapter. It is separate
from the production Purple 2 plugin so that both frontends can evolve around
shared Telegram and TDLib code without version conditionals throughout the
codebase.

The current milestone only:

- exposes the existing `telegram-tdlib` protocol identity
- builds against the `purple-3` pkg-config interface
- loads and unloads through GPlugin
- registers and removes a native `PurpleProtocol`
- uses the Pidgin account name only as a local display label
- exposes only an advanced secret-chat option
- creates a protocol-specific asynchronous `PurpleConnection`

The connection currently stops after checking the shared application-level
credential provider because the Purple-neutral TDLib authentication transport
is the next implementation phase. A credentialless build reports that its
application credentials are unavailable; a configured build reaches the
existing not-supported transport error. Telegram's API ID and API hash
identify the application even during QR login. They are deliberately not
Purple account settings, environment variables, or raw CMake values.

Pidgin preserves settings written by earlier development builds even after a
protocol stops advertising them. On load, the adapter removes the obsolete
phone-number, API-ID, and API-hash copies from its Pidgin 3 accounts and
persists the cleanup. Pidgin 3 stores these accounts separately from Purple 2,
so this does not alter the production plugin's profile. This is logical
removal from the active settings database, not forensic erasure of SQLite
storage or backups.

## Build against a Pidgin development checkout

The commands below assume the Pidgin checkout is in `~/src/pidgin` and use its
uninstalled libpurple libraries and pkg-config files. This bootstrap is
currently tested against Pidgin revision `a412f2ead95d`, whose `purple-3`
pkg-config version is `2.96.1-dev`.

In addition to the dependencies supplied by the Pidgin development
environment, this build requires CMake, Ninja, and Python 3.8 or newer.

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

This creates a credentialless build, which is sufficient for automated tests
and account-editor inspection. For a private build, store the application API
ID and API hash in separate files outside the source tree and pass only their
paths when configuring:

```sh
meson devenv -C "$HOME/src/pidgin/_build" \
  --workdir "$PWD" \
  cmake -S purple3 -B purple3/build -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DTDLIB_PURPLE_API_ID_FILE=/path/to/api_id \
  -DTDLIB_PURPLE_API_HASH_FILE=/path/to/api_hash
```

The two files must be distinct, readable regular files, not symbolic links,
and owned by the user running the build. On Unix, they must have no group or
other permission bits; mode `0600` is recommended. Each file may contain one
ASCII value followed by an optional LF or CRLF and must not exceed 64 bytes.
The API ID must be a canonical positive decimal integer no greater than
`2147483647`, with no sign, whitespace, or leading zero. The API hash must
contain exactly 32 hexadecimal characters.

CMake stores these file paths, but not their contents, in the build
directory's cache. Omitting the `-D` options during a later reconfiguration
does not clear the cached paths. To return an existing build to credentialless
mode, configure both variables as empty:

```sh
meson devenv -C "$HOME/src/pidgin/_build" \
  --workdir "$PWD" \
  cmake -S purple3 -B purple3/build \
  -DTDLIB_PURPLE_API_ID_FILE= \
  -DTDLIB_PURPLE_API_HASH_FILE=
```

Using a fresh build directory has the same effect.

The provider refreshes whenever the plugin is built. Rebuilding after either
value changes embeds the new pair. Removing both input files replaces the
provider with the credential-unavailable stub. Removing only one file stops
the build because a partial pair is never accepted.

The generated source, object files, and plugin contain extractable application
credentials. The generated source is protected inside the private build
directory, but compilation does not make the values secret. Keep the entire
build directory and resulting binary private, and do not publish them unless
you intend to distribute these application credentials. Previously built
objects or binaries may retain an older pair even after the input files are
removed.

The credential tests use only synthetic values and the credential-unavailable
stub. The production smoke test verifies the generated provider state without
inspecting or logging credential values, connecting accounts to Telegram, or
accessing the network. It uses in-memory Purple backends and an isolated
temporary profile.

### Credential diagnostics

Credential setup failures report stable, value-free diagnostic codes:

| Diagnostic | Meaning |
| --- | --- |
| `CREDENTIAL_PATHS_INCOMPLETE` | Only one credential path was configured. |
| `CREDENTIAL_INPUT_MISSING` | The configured file pair is incomplete on disk. |
| `CREDENTIAL_INPUT_DUPLICATE` | Both paths resolve to the same file. |
| `CREDENTIAL_INPUT_IN_SOURCE_TREE` | A credential file is inside the source tree. |
| `CREDENTIAL_INPUT_UNSAFE` | An input is unreadable, oversized, linked, not a regular file, incorrectly owned, or too broadly accessible. |
| `CREDENTIAL_API_ID_INVALID` | The API ID has the wrong decimal format or range. |
| `CREDENTIAL_API_HASH_INVALID` | The API hash is not exactly 32 hexadecimal characters. |
| `CREDENTIAL_OUTPUT_ERROR` | The private generated provider could not be written safely. |
| `CREDENTIAL_GENERATOR_FAILED` | The credential generator failed unexpectedly. |

At runtime, `Telegram application credentials are unavailable in this build`
means the credentialless stub is active. The current `not supported`
connection error means valid application credentials were loaded and the
Purple-neutral TDLib authentication transport is the next implementation
step.

## Inspect in Pidgin 3

Close any existing Pidgin 3 process first, then run:

```sh
PURPLE_PLUGIN_PATH="$PWD/purple3/build" \
  meson devenv -C "$HOME/src/pidgin/_build" \
  --workdir "$HOME/src/pidgin" \
  pidgin3 --nologin
```

Open the account editor and confirm that `Telegram (tdlib)` is available in the
protocol chooser, requires only a local Account Name, and shows no phone-number,
API-ID, or API-hash setting. The advanced view contains the secret-chat option.
`--nologin` prevents existing accounts from connecting while TDLib
authentication is still unimplemented. If an account is enabled accidentally,
the adapter refuses the connection with a clear credential-unavailable or
not-supported error, depending on the build.
