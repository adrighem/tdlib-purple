#!/usr/bin/env bash

set -euo pipefail

if [ "$#" -ne 1 ]; then
    echo "Usage: $0 PACKAGE.deb" >&2
    exit 2
fi

deb="$1"
if [ ! -f "$deb" ]; then
    echo "Package not found: $deb" >&2
    exit 1
fi

case "$deb" in
    /*|./*|../*) deb_arg="$deb" ;;
    *) deb_arg="./$deb" ;;
esac

apt-get update
apt-get install -y --no-install-recommends "$deb_arg"

plugin_path="$(dpkg-query -L tdlib-purple | grep '/purple-2/libtelegram-tdlib\.so$' | head -n 1 || true)"
if [ -z "$plugin_path" ]; then
    echo "Could not find installed libtelegram-tdlib.so" >&2
    exit 1
fi

ldd_output="$(ldd "$plugin_path")"
printf '%s\n' "$ldd_output"

if printf '%s\n' "$ldd_output" | grep -F 'not found' >/dev/null; then
    echo "Unresolved shared library dependencies in $plugin_path" >&2
    exit 1
fi
