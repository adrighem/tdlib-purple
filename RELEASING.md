# Releasing

This project uses release-please to manage release pull requests, changelog updates, version bumps, Git tags, and GitHub Releases.

## How Releases Happen

1. Land changes on `master` using Conventional Commit messages.
2. The Release workflow opens or updates a release PR.
3. Review and merge the release PR when ready to ship.
4. release-please creates a draft GitHub Release and tag.
5. The release pipeline builds and verifies assets with the maintained default
   application provider, then uploads:
   - `tdlib-purple-<version>-source.tar.xz`
   - `tdlib-purple-<version>-linux-x86_64.tar.xz`
   - `tdlib-purple_<version>-1_debian-stable_amd64.deb`
   - `tdlib-purple_<version>-1_ubuntu-24.04-lts_amd64.deb`
   - `tdlib-purple-<version>-1_fedora-44_x86_64.rpm`
   - `tdlib-purple-<version>-1_el9_x86_64.rpm`
6. The workflow publishes the draft only after every asset job succeeds. A
   failed asset job deletes the incomplete draft and tag.

`fix:` commits produce patch releases, `feat:` commits produce minor releases, and commits with `!` produce major releases.

## Application Provider

The maintained default application provider is part of the tracked source and
does not depend on repository secrets. CI exercises the default path for both
Purple adapters. The release workflow verifies the generated provider before
packaging every binary and before creating the source archive.

A package without an embedded provider is a broken build and must never be
published as a release asset.

Never place private override values in workflow inputs, repository variables,
command lines, caches, logs, or release notes.

## Version Source

The current version lives in `CMakeLists.txt`, `purple3/CMakeLists.txt`,
`package.nix`, and `.release-please-manifest.json`. release-please updates all
four through `release-please-config.json`.

## Linux Assets

The Linux tarball is a staged install tree rooted at `usr/`, and packaging
verifies the generated provider state before creating an asset.
The source archive includes the maintained default application provider, the
complete repository source, and the exact TDLib submodule source selected by
the release commit. Its provider is generated and validated before the archive
is created, so downstream package builds need no extra credential setup.
All binary packages contain the GPL and bundled dependency license notices.
The `.deb` packages are built in distro-specific environments and use `dpkg-shlibdeps` to derive runtime dependencies from the built plugin.
The RPM packages target Fedora 44 and Enterprise Linux 9 via AlmaLinux 9. Fedora 44 tracks the current Fedora stable release; EL9 is the conservative Red Hat compatible baseline for broad enterprise users.

## Windows Assets

The repository still has an NSIS target, but Windows release assets are not automated yet. The missing piece is a reproducible Windows dependency environment for TDLib, libpurple, and the plugin's image/translation dependencies.
