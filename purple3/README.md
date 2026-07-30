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

The connection currently reports a clear not-supported error because the
Purple-neutral TDLib authentication transport is the next implementation
phase. Telegram's API ID and API hash identify the application even during QR
login, so they will be supplied together by an application-level provider.
They are deliberately not Purple account settings, environment variables, or
raw CMake values.

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

The smoke test uses in-memory Purple backends and an isolated temporary
profile. It does not read Telegram credentials, connect accounts, or access
the network.

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
the adapter refuses the connection with a clear not-supported error.
