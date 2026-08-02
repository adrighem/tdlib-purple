# Releasing

This project uses release-please to manage release pull requests, changelog updates, version bumps, Git tags, and GitHub Releases.

## How Releases Happen

1. Land changes on `master` using Conventional Commit messages.
2. The Release workflow opens or updates a release PR.
3. Review and merge the release PR when ready to ship.
4. release-please creates a draft GitHub Release and tag.
5. The release pipeline builds and verifies credentialed assets from encrypted
   repository secrets, then uploads:
   - `tdlib-purple-<version>-source.tar.xz`
   - `tdlib-purple-<version>-linux-x86_64.tar.xz`
   - `tdlib-purple_<version>-1_debian-stable_amd64.deb`
   - `tdlib-purple_<version>-1_ubuntu-24.04-lts_amd64.deb`
   - `tdlib-purple-<version>-1_fedora-44_x86_64.rpm`
   - `tdlib-purple-<version>-1_el9_x86_64.rpm`
6. The workflow publishes the draft only after every asset job succeeds. A
   failed asset job deletes the incomplete draft and tag.

`fix:` commits produce patch releases, `feat:` commits produce minor releases, and commits with `!` produce major releases.

## Required Release Credentials

The repository Actions secrets `TDLIB_PURPLE_API_ID` and
`TDLIB_PURPLE_API_HASH` must contain the release application credential pair.
The workflow checks that both secrets exist before release-please can create a
tag or draft. Each asset job writes them without logging to owner-only temporary
files, and removes those files after the build.

A package without an embedded provider is a broken build and must never be
published as a release asset.

Never place credential values in workflow inputs, repository variables,
committed files, command lines, caches, logs, or release notes.

## Version Source

The current version lives in `CMakeLists.txt`, `purple3/CMakeLists.txt`,
`package.nix`, and `.release-please-manifest.json`. release-please updates all
four through `release-please-config.json`.

## Linux Assets

The Linux tarball is a staged install tree rooted at `usr/`. Configuration fails
unless both credential files are present and valid, and packaging verifies the
generated provider state before creating an asset.
The source archive is not a binary build and contains no application
credentials. It includes the complete repository source plus the exact TDLib
submodule source selected by the release commit.
All binary packages contain the GPL and bundled dependency license notices.
The `.deb` packages are built in distro-specific environments and use `dpkg-shlibdeps` to derive runtime dependencies from the built plugin.
The RPM packages target Fedora 44 and Enterprise Linux 9 via AlmaLinux 9. Fedora 44 tracks the current Fedora stable release; EL9 is the conservative Red Hat compatible baseline for broad enterprise users.

## Windows Assets

The repository still has an NSIS target, but Windows release assets are not automated yet. The missing piece is a reproducible Windows dependency environment for TDLib, libpurple, and the plugin's image/translation dependencies.
