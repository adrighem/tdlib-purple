#!/usr/bin/env bash

set -euo pipefail

if [ "$#" -lt 1 ]; then
    echo "Usage: $0 source | $0 tarball|deb|rpm DISTRO_ID" >&2
    exit 2
fi

asset_type="$1"
distro_id="${2:-}"

: "${VERSION:?VERSION must be set}"
: "${TD_TAG:?TD_TAG must be set}"

td_mark="${TD_MARK:-release}"
asset_dir="${ASSET_DIR:-release-assets}"
package_revision="${PACKAGE_REVISION:-1}"
repo_root="$(cd "$(dirname "$0")/../.." && pwd)"

verify_maintained_application_provider() (
    generator="$1"
    source_root="$2"
    provider_source="$3"
    state_header="$4"

    if ! grep -q \
        '^#define TDLIB_PURPLE_APPLICATION_CREDENTIALS_AVAILABLE 1$' \
        "$state_header"; then
        echo "Application credential provider is unavailable." >&2
        exit 1
    fi

    verification_dir="$(env -i PATH="$PATH" mktemp -d)"
    chmod 700 "$verification_dir"
    trap 'rm -f "$verification_dir/expected-api-id" \
        "$verification_dir/expected-api-hash" \
        "$verification_dir/expected-provider.c"; \
        rmdir "$verification_dir" 2>/dev/null || true' EXIT
    umask 077
    printf '%s\n' '32769927' > "$verification_dir/expected-api-id"
    printf '%s\n' 'a0d6d0f8a610fb055903fd0e94d8eeb0' \
        > "$verification_dir/expected-api-hash"
    env -i PATH="$PATH" python3 "$generator" \
        --api-id-file "$verification_dir/expected-api-id" \
        --api-hash-file "$verification_dir/expected-api-hash" \
        --source-root "$source_root" \
        --output "$verification_dir/expected-provider.c"
    if ! cmp -s "$provider_source" \
        "$verification_dir/expected-provider.c"; then
        echo "Provider does not use the maintained application identity." >&2
        exit 1
    fi
)

cd "$repo_root"
mkdir -p "$asset_dir"

if [ "$asset_type" = "source" ]; then
    td_gitlink="$(git ls-tree HEAD td | awk '{print $3}')"
    if [ -z "$td_gitlink" ] || [ "$td_gitlink" != "$TD_TAG" ]; then
        echo "The td submodule commit does not match TD_TAG." >&2
        exit 1
    fi
    if ! td_checkout="$(git -C td rev-parse HEAD 2>/dev/null)" || \
       [ "$td_checkout" != "$td_gitlink" ]; then
        echo "The exact td submodule commit must be checked out for the source asset." >&2
        exit 1
    fi

    source_workdir="$(mktemp -d)"
    trap 'rm -rf "$source_workdir"' EXIT
    source_root="tdlib-purple-${VERSION}"
    git archive --format=tar --prefix="${source_root}/" HEAD |
        tar -xf - -C "$source_workdir"
    mkdir -p "$source_workdir/$source_root/td"
    git -C td archive --format=tar --prefix="${source_root}/td/" "$td_gitlink" |
        tar -xf - -C "$source_workdir"

    if [ ! -s "$source_workdir/$source_root/td/LICENSE_1_0.txt" ]; then
        echo "The complete source asset is missing the TDLib license." >&2
        exit 1
    fi

    provider_check_dir="$source_workdir/provider-check"
    env -i PATH="$PATH" python3 \
        "$source_workdir/$source_root/credentials/generate-application-credentials.py" \
        --source-root "$source_workdir/$source_root" \
        --output "$provider_check_dir/provider.c" \
        --state-output "$provider_check_dir/provider-state.h"
    verify_maintained_application_provider \
        "$source_workdir/$source_root/credentials/generate-application-credentials.py" \
        "$source_workdir/$source_root" \
        "$provider_check_dir/provider.c" \
        "$provider_check_dir/provider-state.h"
    rm -f "$provider_check_dir/provider.c" \
        "$provider_check_dir/provider-state.h"
    rmdir "$provider_check_dir"

    source_date_epoch="$(git show -s --format=%ct HEAD)"
    asset="$asset_dir/tdlib-purple-${VERSION}-source.tar.xz"
    tar --sort=name --owner=0 --group=0 --numeric-owner \
        --mtime="@${source_date_epoch}" -C "$source_workdir" \
        -cJf "$asset" "$source_root"
    echo "Created $asset"
    exit 0
fi

if [ -z "$distro_id" ]; then
    echo "DISTRO_ID is required for $asset_type assets." >&2
    exit 2
fi

if [ ! -s "$repo_root/td/LICENSE_1_0.txt" ]; then
    echo "The exact td submodule and its license must be checked out." >&2
    exit 1
fi

build_dir="build-release-${asset_type}-${distro_id}"
staging_dir="$repo_root/package-root-${asset_type}-${distro_id}"

rm -rf "$build_dir" "$staging_dir"

.github/ga_build_td.sh "$TD_TAG" "$td_mark"

cmake -S . -B "$build_dir" -GNinja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX=/usr \
    -DCMAKE_DISABLE_FIND_PACKAGE_fmt=TRUE \
    -DNoVoip=TRUE \
    -DTDLIB_PURPLE_TDLIB_LICENSE_FILE="$repo_root/td/LICENSE_1_0.txt" \
    -DTd_DIR="$repo_root/td_destdir/usr/local/lib/cmake/Td"

cmake --build "$build_dir" --target telegram-tdlib
credential_provider_source="$build_dir/.private/telegram-application-credentials-embedded.c"
credential_state_header="$build_dir/.private/telegram-application-credentials-state.h"
verify_maintained_application_provider \
    "$repo_root/credentials/generate-application-credentials.py" \
    "$repo_root" \
    "$credential_provider_source" \
    "$credential_state_header"
DESTDIR="$staging_dir" cmake --build "$build_dir" --target install

license_root="$staging_dir/usr/share/licenses/tdlib-purple"
required_licenses=(
    "LICENSE"
    "fmt/LICENSE.rst"
    "rlottie/COPYING.rlottie"
    "rlottie/COPYING.FTL"
    "rlottie/COPYING.LGPL"
    "rlottie/COPYING.PIX"
    "rlottie/COPYING.RPD"
    "rlottie/COPYING.SKIA"
    "rlottie/COPYING.STB"
    "tdlib/LICENSE_1_0.txt"
)
for required_license in "${required_licenses[@]}"; do
    if [ ! -s "$license_root/$required_license" ]; then
        echo "Release package is missing license notice: $required_license" >&2
        exit 1
    fi
done

case "$asset_type" in
tarball)
    asset="$asset_dir/tdlib-purple-${VERSION}-${distro_id}.tar.xz"
    tar --sort=name --owner=0 --group=0 --numeric-owner -C "$staging_dir" -cJf "$asset" .
    echo "Created $asset"
    ;;

deb)
    arch="$(dpkg --print-architecture)"
    plugin_so="$(find "$staging_dir" -path '*/purple-2/libtelegram-tdlib.so' -print -quit)"
    if [ -z "$plugin_so" ]; then
        echo "Could not find installed libtelegram-tdlib.so in $staging_dir" >&2
        exit 1
    fi

    install -d -m 0755 "$staging_dir/DEBIAN"

    shlib_workdir="$(mktemp -d)"
    trap 'rm -rf "$shlib_workdir"' EXIT
    mkdir -p "$shlib_workdir/debian"
    cat > "$shlib_workdir/debian/control" <<'CONTROL'
Source: tdlib-purple
Section: net
Priority: optional
Maintainer: tdlib-purple contributors <noreply@example.invalid>
Standards-Version: 4.6.2

Package: tdlib-purple
Architecture: any
Depends: ${shlibs:Depends}
Description: Telegram client for libpurple using TDLib
 A Telegram client for libpurple applications such as Pidgin, powered by TDLib.
CONTROL

    depends="$(
        cd "$shlib_workdir"
        dpkg-shlibdeps -O -e "$plugin_so" | sed 's/^shlibs:Depends=//'
    )"
    install -Dm0644 "$repo_root/LICENSE" \
        "$staging_dir/usr/share/doc/tdlib-purple/copyright"
    installed_size="$(du -sk "$staging_dir/usr" | awk '{print $1}')"

    cat > "$staging_dir/DEBIAN/control" <<CONTROL
Package: tdlib-purple
Version: ${VERSION}-${package_revision}
Section: net
Priority: optional
Architecture: ${arch}
Maintainer: tdlib-purple contributors <noreply@example.invalid>
Depends: ${depends}
Installed-Size: ${installed_size}
Homepage: https://github.com/adrighem/tdlib-purple
Description: Telegram client for libpurple using TDLib
 A Telegram client for libpurple applications such as Pidgin, powered by TDLib.
 This package was built for ${distro_id}.
CONTROL

    (
        cd "$staging_dir"
        find usr -type f -exec md5sum '{}' + | sort -k 2 > DEBIAN/md5sums
    )
    chmod 0644 "$staging_dir/DEBIAN/control" "$staging_dir/DEBIAN/md5sums"

    asset="$asset_dir/tdlib-purple_${VERSION}-${package_revision}_${distro_id}_${arch}.deb"
    fakeroot dpkg-deb --build --root-owner-group "$staging_dir" "$asset"
    echo "Created $asset"
    ;;

rpm)
    arch="$(rpm --eval '%{_arch}')"
    rpm_topdir="$(mktemp -d)"
    trap 'rm -rf "$rpm_topdir"' EXIT
    mkdir -p "$rpm_topdir"/{BUILD,BUILDROOT,RPMS,SOURCES,SPECS,SRPMS}

    spec="$rpm_topdir/SPECS/tdlib-purple.spec"
    cat > "$spec" <<SPEC
%global debug_package %{nil}

Name: tdlib-purple
Version: ${VERSION}
Release: ${package_revision}%{?dist}
Summary: Telegram client for libpurple using TDLib
License: GPL-3.0-or-later
URL: https://github.com/adrighem/tdlib-purple
Requires: libpurple

%description
A Telegram client for libpurple applications such as Pidgin, powered by TDLib.
This package was built for ${distro_id}.

%prep

%build

%install
mkdir -p %{buildroot}
cp -a ${staging_dir}/usr %{buildroot}/

%files
%license %{_licensedir}/%{name}/LICENSE
%license %{_licensedir}/%{name}/fmt/LICENSE.rst
%license %{_licensedir}/%{name}/rlottie/COPYING.FTL
%license %{_licensedir}/%{name}/rlottie/COPYING.LGPL
%license %{_licensedir}/%{name}/rlottie/COPYING.PIX
%license %{_licensedir}/%{name}/rlottie/COPYING.RPD
%license %{_licensedir}/%{name}/rlottie/COPYING.SKIA
%license %{_licensedir}/%{name}/rlottie/COPYING.STB
%license %{_licensedir}/%{name}/rlottie/COPYING.rlottie
%license %{_licensedir}/%{name}/tdlib/LICENSE_1_0.txt
%{_libdir}/purple-2/libtelegram-tdlib.so
%{_datadir}/pixmaps/pidgin/protocols/*/telegram.png
%{_datadir}/metainfo/tdlib-purple.metainfo.xml
%{_datadir}/locale/*/LC_MESSAGES/tdlib-purple.mo

%changelog
* $(LC_ALL=C date "+%a %b %d %Y") tdlib-purple contributors <noreply@example.invalid> - ${VERSION}-${package_revision}
- Automated release build for ${distro_id}
SPEC

    rpmbuild -bb --define "_topdir $rpm_topdir" "$spec"
    rpm_path="$(find "$rpm_topdir/RPMS" -name 'tdlib-purple-*.rpm' -print -quit)"
    if [ -z "$rpm_path" ]; then
        echo "Could not find built RPM in $rpm_topdir/RPMS" >&2
        exit 1
    fi

    asset="$asset_dir/tdlib-purple-${VERSION}-${package_revision}_${distro_id}_${arch}.rpm"
    cp "$rpm_path" "$asset"
    echo "Created $asset"
    ;;

*)
    echo "Unknown asset type: $asset_type" >&2
    exit 2
    ;;
esac
