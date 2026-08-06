#!/usr/bin/env bash

set -euo pipefail

if [ "$#" -ne 2 ]; then
    echo "Usage: $0 PACKAGE_DIR SITE_DIR" >&2
    exit 2
fi

package_dir="$1"
site_dir="$2"
archive_root="$site_dir/apt"
project_name="tdlib-purple"
repository_url="https://adrighem.github.io/tdlib-purple/apt"
keyring_name="tdlib-purple-archive-keyring.gpg"

for command_name in apt-ftparchive dpkg-deb gzip install sha256sum; do
    if ! command -v "$command_name" >/dev/null 2>&1; then
        echo "Required command is unavailable: $command_name" >&2
        exit 1
    fi
done

if [ ! -d "$package_dir" ]; then
    echo "Package directory does not exist: $package_dir" >&2
    exit 1
fi

if [ -e "$site_dir" ]; then
    if [ ! -d "$site_dir" ] ||
       [ -n "$(find "$site_dir" -mindepth 1 -print -quit)" ]; then
        echo "Site directory must be absent or empty: $site_dir" >&2
        exit 1
    fi
else
    mkdir -p "$site_dir"
fi

declare -A package_identities=()
declare -A suite_package_counts=(
    [debian-13]=0
    [ubuntu-24.04]=0
)

while IFS= read -r -d '' package_path; do
    package_filename="$(basename "$package_path")"
    case "$package_filename" in
    tdlib-purple_*_debian-13_amd64.deb|tdlib-purple_*_debian-stable_amd64.deb)
        suite="debian-13"
        ;;
    tdlib-purple_*_ubuntu-24.04-lts_amd64.deb)
        suite="ubuntu-24.04"
        ;;
    *)
        continue
        ;;
    esac

    package_name="$(dpkg-deb --field "$package_path" Package)"
    package_version="$(dpkg-deb --field "$package_path" Version)"
    package_architecture="$(dpkg-deb --field "$package_path" Architecture)"
    if [ "$package_name" != "$project_name" ] ||
       [ "$package_architecture" != "amd64" ] ||
       [ -z "$package_version" ]; then
        echo "Unexpected Debian package metadata: $package_filename" >&2
        exit 1
    fi

    release_tag="$(basename "$(dirname "$package_path")")"
    case "$release_tag" in
    tdlib-purple-v*)
        release_version="${release_tag#tdlib-purple-v}"
        ;;
    v*)
        release_version="${release_tag#v}"
        ;;
    *)
        echo "Unrecognized release tag for $package_filename." >&2
        exit 1
        ;;
    esac
    package_revision="${package_version#"$release_version"-}"
    if [ "$package_revision" = "$package_version" ] ||
       [[ ! "$package_revision" =~ ^[0-9]+$ ]]; then
        echo "Package version does not match release tag: $package_filename" >&2
        exit 1
    fi

    package_identity="$suite:$package_name:$package_version:$package_architecture"
    if [ -n "${package_identities[$package_identity]:-}" ]; then
        echo "Duplicate Debian package identity: $package_identity" >&2
        exit 1
    fi
    package_identities[$package_identity]="$package_filename"

    pool_dir="$archive_root/pool/$suite/main/t/$project_name"
    install -d -m 0755 "$pool_dir"
    install -m 0644 "$package_path" "$pool_dir/$package_filename"
    suite_package_counts[$suite]=$((suite_package_counts[$suite] + 1))
done < <(find "$package_dir" -type f -name '*.deb' -print0 | sort -z)

for suite in debian-13 ubuntu-24.04; do
    if [ "${suite_package_counts[$suite]}" -eq 0 ]; then
        echo "No $project_name packages found for $suite." >&2
        exit 1
    fi

    index_dir="$archive_root/dists/$suite/main/binary-amd64"
    install -d -m 0755 "$index_dir"
    (
        cd "$archive_root"
        apt-ftparchive packages "pool/$suite" \
            > "dists/$suite/main/binary-amd64/Packages"
    )
    gzip -9n -c "$index_dir/Packages" > "$index_dir/Packages.gz"

    for index_file in "$index_dir/Packages" "$index_dir/Packages.gz"; do
        index_hash="$(sha256sum "$index_file")"
        index_hash="${index_hash%% *}"
        by_hash_dir="$index_dir/by-hash/SHA256"
        install -d -m 0755 "$by_hash_dir"
        install -m 0644 "$index_file" "$by_hash_dir/$index_hash"
    done

    (
        cd "$archive_root"
        apt-ftparchive \
            -o "APT::FTPArchive::Release::Origin=$project_name" \
            -o "APT::FTPArchive::Release::Label=$project_name" \
            -o "APT::FTPArchive::Release::Suite=$suite" \
            -o "APT::FTPArchive::Release::Codename=$suite" \
            -o "APT::FTPArchive::Release::Architectures=amd64" \
            -o "APT::FTPArchive::Release::Components=main" \
            -o "APT::FTPArchive::Release::Acquire-By-Hash=yes" \
            -o "APT::FTPArchive::Release::Description=$project_name packages for $suite" \
            release "dists/$suite" > "dists/$suite/Release"
    )

done

for suite in debian-13 ubuntu-24.04; do
    cat > "$archive_root/$project_name-$suite.sources" <<EOF
Types: deb
URIs: $repository_url
Suites: $suite
Components: main
Architectures: amd64
Signed-By: /etc/apt/keyrings/$keyring_name
EOF
done
