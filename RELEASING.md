# Releasing

This project uses release-please to manage release pull requests, changelog updates, version bumps, Git tags, and GitHub Releases.

## How Releases Happen

1. Land changes on `master` using Conventional Commit messages.
2. The Release workflow opens or updates a release PR.
3. Review and merge the release PR when ready to ship.
4. release-please creates the GitHub Release.
5. The release pipeline builds and uploads:
   - `tdlib-purple-<version>-linux-x86_64.tar.xz`
   - `tdlib-purple_<version>-1_debian-stable_amd64.deb`
   - `tdlib-purple_<version>-1_ubuntu-24.04-lts_amd64.deb`
   - `tdlib-purple-<version>-1_fedora-44_x86_64.rpm`
   - `tdlib-purple-<version>-1_el9_x86_64.rpm`

`fix:` commits produce patch releases, `feat:` commits produce minor releases, and commits with `!` produce major releases.

## Version Source

The current version lives in `CMakeLists.txt` and `.release-please-manifest.json`.
release-please updates both through `release-please-config.json`.

## Linux Assets

The Linux tarball is a staged install tree rooted at `usr/`.
The `.deb` packages are built in distro-specific environments and use `dpkg-shlibdeps` to derive runtime dependencies from the built plugin.
The RPM packages target Fedora 44 and Enterprise Linux 9 via AlmaLinux 9. Fedora 44 tracks the current Fedora stable release; EL9 is the conservative Red Hat compatible baseline for broad enterprise users.

## Windows Assets

The repository still has an NSIS target, but Windows release assets are not automated yet. The missing piece is a reproducible Windows dependency environment for TDLib, libpurple, and the plugin's image/translation dependencies.
