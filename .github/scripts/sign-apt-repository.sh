#!/usr/bin/env bash

set -euo pipefail

if [ "$#" -ne 1 ]; then
    echo "Usage: $0 SITE_DIR" >&2
    exit 2
fi

site_dir="$1"
archive_root="$site_dir/apt"
keyring_name="tdlib-purple-archive-keyring.gpg"

: "${APT_SIGNING_KEY_FINGERPRINT:?APT_SIGNING_KEY_FINGERPRINT must be set}"
: "${APT_SIGNING_KEY_PASSPHRASE:?APT_SIGNING_KEY_PASSPHRASE must be set}"

if [[ ! "$APT_SIGNING_KEY_FINGERPRINT" =~ ^[0-9A-F]{40,64}$ ]]; then
    echo "APT_SIGNING_KEY_FINGERPRINT is invalid." >&2
    exit 1
fi

if [ ! -d "$archive_root" ]; then
    echo "APT repository does not exist: $archive_root" >&2
    exit 1
fi
if ! gpg --batch --list-secret-keys "$APT_SIGNING_KEY_FINGERPRINT" \
    >/dev/null 2>&1; then
    echo "APT signing key is unavailable." >&2
    exit 1
fi
if [ -n "$(find "$site_dir" ! -type d ! -type f -print -quit)" ]; then
    echo "APT repository contains an unsupported file type." >&2
    exit 1
fi
if [ -e "$archive_root/$keyring_name" ] ||
   [ -e "$archive_root/tdlib-purple-archive-keyring.fingerprint" ]; then
    echo "Repository already contains signing-key output." >&2
    exit 1
fi

gpg --batch --quiet --export-options export-minimal \
    --export "$APT_SIGNING_KEY_FINGERPRINT" \
    > "$archive_root/$keyring_name"
if [ ! -s "$archive_root/$keyring_name" ]; then
    echo "Could not export APT signing public key." >&2
    exit 1
fi

gpg_arguments=(
    --batch
    --yes
    --quiet
    --pinentry-mode loopback
    --passphrase-fd 0
    --local-user "$APT_SIGNING_KEY_FINGERPRINT"
    --digest-algo SHA256
)

for suite in debian-13 ubuntu-24.04; do
    distribution_dir="$archive_root/dists/$suite"
    release_file="$distribution_dir/Release"
    if [ ! -s "$release_file" ]; then
        echo "Release metadata is missing for $suite." >&2
        exit 1
    fi
    if [ -e "$distribution_dir/InRelease" ] ||
       [ -e "$distribution_dir/Release.gpg" ]; then
        echo "Repository already contains signatures for $suite." >&2
        exit 1
    fi

    printf '%s\n' "${APT_SIGNING_KEY_PASSPHRASE:-}" |
        gpg "${gpg_arguments[@]}" --clearsign \
            --output "$distribution_dir/InRelease" \
            "$release_file"
    printf '%s\n' "${APT_SIGNING_KEY_PASSPHRASE:-}" |
        gpg "${gpg_arguments[@]}" --armor --detach-sign \
            --output "$distribution_dir/Release.gpg" \
            "$release_file"
    gpgv --keyring "$archive_root/$keyring_name" \
        "$distribution_dir/InRelease" >/dev/null
    gpgv --keyring "$archive_root/$keyring_name" \
        "$distribution_dir/Release.gpg" "$release_file" >/dev/null
done

printf '%s\n' "$APT_SIGNING_KEY_FINGERPRINT" \
    > "$archive_root/tdlib-purple-archive-keyring.fingerprint"
