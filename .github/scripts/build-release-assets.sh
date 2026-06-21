#!/usr/bin/env bash

set -euo pipefail

if [ "$#" -lt 2 ]; then
    echo "Usage: $0 tarball|deb|rpm DISTRO_ID" >&2
    exit 2
fi

asset_type="$1"
distro_id="$2"

: "${VERSION:?VERSION must be set}"
: "${TD_TAG:?TD_TAG must be set}"

td_mark="${TD_MARK:-release}"
asset_dir="${ASSET_DIR:-release-assets}"
package_revision="${PACKAGE_REVISION:-1}"
repo_root="$(cd "$(dirname "$0")/../.." && pwd)"

cd "$repo_root"
mkdir -p "$asset_dir"

build_dir="build-release-${asset_type}-${distro_id}"
staging_dir="$repo_root/package-root-${asset_type}-${distro_id}"

rm -rf "$build_dir" "$staging_dir"

.github/ga_build_td.sh "$TD_TAG" "$td_mark"

cmake -S . -B "$build_dir" -GNinja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX=/usr \
    -DCMAKE_DISABLE_FIND_PACKAGE_fmt=TRUE \
    -DNoVoip=TRUE \
    -DTd_DIR="$repo_root/td_destdir/usr/local/lib/cmake/Td"

cmake --build "$build_dir" --target telegram-tdlib
DESTDIR="$staging_dir" cmake --build "$build_dir" --target install

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

    mkdir -p "$staging_dir/DEBIAN"

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
Description: Telegram plugin for libpurple using TDLib
 Telegram protocol plugin for libpurple clients such as Pidgin.
CONTROL

    depends="$(
        cd "$shlib_workdir"
        dpkg-shlibdeps -O -e "$plugin_so" | sed 's/^shlibs:Depends=//'
    )"
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
Description: Telegram plugin for libpurple using TDLib
 Telegram protocol plugin for libpurple clients such as Pidgin.
 This package was built for ${distro_id}.
CONTROL

    (
        cd "$staging_dir"
        find usr -type f -exec md5sum '{}' + | sort -k 2 > DEBIAN/md5sums
    )

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
Summary: Telegram plugin for libpurple using TDLib
License: GPL-2.0-or-later
URL: https://github.com/adrighem/tdlib-purple
Requires: libpurple

%description
Telegram protocol plugin for libpurple clients such as Pidgin.
This package was built for ${distro_id}.

%prep

%build

%install
mkdir -p %{buildroot}
cp -a ${staging_dir}/usr %{buildroot}/
install -Dm0644 ${repo_root}/LICENSE %{buildroot}%{_licensedir}/%{name}/LICENSE

%files
%license %{_licensedir}/%{name}/LICENSE
%{_libdir}/purple-2/libtelegram-tdlib.so
%{_datadir}/pixmaps/pidgin/protocols/*/telegram.png
/usr/local/share/metainfo/tdlib-purple.metainfo.xml
/usr/local/share/locale/*/LC_MESSAGES/tdlib-purple.mo

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
