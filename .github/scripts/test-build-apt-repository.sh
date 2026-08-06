#!/usr/bin/env bash

set -euo pipefail

repo_root="$(cd "$(dirname "$0")/../.." && pwd)"
work_dir="$(mktemp -d)"
trap 'rm -rf "$work_dir"' EXIT

package_dir="$work_dir/packages"
site_dir="$work_dir/site"
gnupg_home="$work_dir/gnupg"
install -d -m 0755 "$package_dir"
install -d -m 0700 "$gnupg_home"

build_test_package() {
    distro_id="$1"
    version="$2"
    release_tag="tdlib-purple-v${version%-*}"
    package_root="$work_dir/package-$distro_id-$version"
    install -d -m 0755 \
        "$package_root/DEBIAN" \
        "$package_root/usr/share/doc/tdlib-purple"
    cat > "$package_root/DEBIAN/control" <<EOF
Package: tdlib-purple
Version: $version
Section: net
Priority: optional
Architecture: amd64
Maintainer: tdlib-purple tests <noreply@example.invalid>
Description: Synthetic tdlib-purple APT repository test package
EOF
    printf '%s\n' "$distro_id $version" \
        > "$package_root/usr/share/doc/tdlib-purple/repository-test"
    install -d -m 0755 "$package_dir/$release_tag"
    dpkg-deb --build --root-owner-group "$package_root" \
        "$package_dir/$release_tag/tdlib-purple_${version}_${distro_id}_amd64.deb" \
        >/dev/null
}

build_test_package debian-stable 1.0.0-1
build_test_package debian-13 1.1.0-1
build_test_package ubuntu-24.04-lts 1.0.0-1
build_test_package ubuntu-24.04-lts 1.1.0-1

export GNUPGHOME="$gnupg_home"
test_passphrase='synthetic-test-passphrase'
gpg --batch --quiet --pinentry-mode loopback \
    --passphrase "$test_passphrase" --quick-generate-key \
    'tdlib-purple APT repository test <noreply@example.invalid>' \
    rsa2048 sign 1d
fingerprint="$(
    gpg --batch --with-colons --list-secret-keys |
        awk -F: '$1 == "fpr" { print $10; exit }'
)"
if [ -z "$fingerprint" ]; then
    echo "Could not create test signing key." >&2
    exit 1
fi

"$repo_root/.github/scripts/build-apt-repository.sh" \
    "$package_dir" "$site_dir"
APT_SIGNING_KEY_FINGERPRINT="$fingerprint" \
APT_SIGNING_KEY_PASSPHRASE="$test_passphrase" \
    "$repo_root/.github/scripts/sign-apt-repository.sh" "$site_dir"

archive_root="$site_dir/apt"
keyring="$archive_root/tdlib-purple-archive-keyring.gpg"
test "$(cat "$archive_root/tdlib-purple-archive-keyring.fingerprint")" = \
    "$fingerprint"
for suite in debian-13 ubuntu-24.04; do
    distribution_dir="$archive_root/dists/$suite"
    packages_file="$distribution_dir/main/binary-amd64/Packages"
    packages_gzip="$packages_file.gz"

    test -s "$distribution_dir/InRelease"
    test -s "$distribution_dir/Release.gpg"
    test -s "$packages_file"
    test -s "$packages_gzip"
    test "$(grep -c '^Package: tdlib-purple$' "$packages_file")" -eq 2
    grep -q '^Acquire-By-Hash: yes$' "$distribution_dir/Release"
    grep -q "^Suite: $suite$" "$distribution_dir/Release"

    for index_file in "$packages_file" "$packages_gzip"; do
        index_hash="$(sha256sum "$index_file")"
        index_hash="${index_hash%% *}"
        test -f "$(dirname "$packages_file")/by-hash/SHA256/$index_hash"
    done

    gpgv --keyring "$keyring" "$distribution_dir/InRelease" >/dev/null 2>&1
    gpgv --keyring "$keyring" "$distribution_dir/Release.gpg" \
        "$distribution_dir/Release" >/dev/null 2>&1
done

grep -q '^Suites: debian-13$' \
    "$archive_root/tdlib-purple-debian-13.sources"
grep -q '^Suites: ubuntu-24.04$' \
    "$archive_root/tdlib-purple-ubuntu-24.04.sources"

for suite in debian-13 ubuntu-24.04; do
    apt_root="$work_dir/apt-client-$suite"
    apt_source="$apt_root/repository.sources"
    install -d -m 0755 \
        "$apt_root/lists/partial" \
        "$apt_root/cache/archives/partial"
    touch "$apt_root/status"
    cat > "$apt_source" <<EOF
Types: deb
URIs: file:$archive_root
Suites: $suite
Components: main
Architectures: amd64
Signed-By: $keyring
EOF
    apt_options=(
        -o Debug::NoLocking=true
        -o "APT::Sandbox::User=$(id -un)"
        -o "Dir::Etc::sourcelist=$apt_source"
        -o Dir::Etc::sourceparts=-
        -o "Dir::State::status=$apt_root/status"
        -o "Dir::State::Lists=$apt_root/lists"
        -o "Dir::Cache=$apt_root/cache"
    )
    apt-get "${apt_options[@]}" update >/dev/null
    candidate_version="$(
        apt-cache "${apt_options[@]}" policy tdlib-purple |
            awk '$1 == "Candidate:" { print $2 }'
    )"
    test "$candidate_version" = 1.1.0-1
    test "$(apt-cache "${apt_options[@]}" madison tdlib-purple | wc -l)" -eq 2

    download_dir="$work_dir/download-$suite"
    install -d -m 0755 "$download_dir"
    (
        cd "$download_dir"
        apt-get "${apt_options[@]}" download tdlib-purple=1.1.0-1 \
            >/dev/null 2>&1
    )
    downloaded_package="$download_dir/tdlib-purple_1.1.0-1_amd64.deb"
    indexed_package="$(
        find "$archive_root/pool/$suite" \
            -type f -name '*_1.1.0-1_*_amd64.deb' -print -quit
    )"
    cmp "$downloaded_package" "$indexed_package"
done

wrong_key_home="$work_dir/wrong-key-home"
wrong_keyring="$work_dir/wrong-keyring.gpg"
install -d -m 0700 "$wrong_key_home"
GNUPGHOME="$wrong_key_home" gpg --batch --quiet --passphrase '' \
    --quick-generate-key \
    'wrong APT repository test key <noreply@example.invalid>' \
    rsa2048 sign 1d
GNUPGHOME="$wrong_key_home" gpg --batch --quiet --export > "$wrong_keyring"
wrong_source="$work_dir/wrong-key.sources"
cat > "$wrong_source" <<EOF
Types: deb
URIs: file:$archive_root
Suites: debian-13
Components: main
Architectures: amd64
Signed-By: $wrong_keyring
EOF
wrong_root="$work_dir/wrong-key-client"
install -d -m 0755 \
    "$wrong_root/lists/partial" \
    "$wrong_root/cache/archives/partial"
touch "$wrong_root/status"
if apt-get \
    -o Debug::NoLocking=true \
    -o "APT::Sandbox::User=$(id -un)" \
    -o "Dir::Etc::sourcelist=$wrong_source" \
    -o Dir::Etc::sourceparts=- \
    -o "Dir::State::status=$wrong_root/status" \
    -o "Dir::State::Lists=$wrong_root/lists" \
    -o "Dir::Cache=$wrong_root/cache" \
    update >/dev/null 2>&1; then
    echo "APT accepted repository signed by an untrusted key." >&2
    exit 1
fi

tampered_index_dir="$archive_root/dists/debian-13/main/binary-amd64"
find "$tampered_index_dir/by-hash" -type f -delete
printf '%s\n' tampered >> "$tampered_index_dir/Packages"
printf '%s\n' tampered >> "$tampered_index_dir/Packages.gz"
tampered_root="$work_dir/tampered-client"
tampered_source="$work_dir/tampered.sources"
install -d -m 0755 \
    "$tampered_root/lists/partial" \
    "$tampered_root/cache/archives/partial"
touch "$tampered_root/status"
cat > "$tampered_source" <<EOF
Types: deb
URIs: file:$archive_root
Suites: debian-13
Components: main
Architectures: amd64
Signed-By: $keyring
EOF
if apt-get \
    -o Debug::NoLocking=true \
    -o "APT::Sandbox::User=$(id -un)" \
    -o "Dir::Etc::sourcelist=$tampered_source" \
    -o Dir::Etc::sourceparts=- \
    -o "Dir::State::status=$tampered_root/status" \
    -o "Dir::State::Lists=$tampered_root/lists" \
    -o "Dir::Cache=$tampered_root/cache" \
    update >/dev/null 2>&1; then
    echo "APT accepted tampered repository metadata." >&2
    exit 1
fi
