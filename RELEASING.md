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
   - `tdlib-purple_<version>-1_debian-13_amd64.deb`
   - `tdlib-purple_<version>-1_ubuntu-24.04-lts_amd64.deb`
   - `tdlib-purple-<version>-1_fedora-44_x86_64.rpm`
   - `tdlib-purple-<version>-1_el9_x86_64.rpm`
6. The workflow publishes the draft only after every asset job succeeds. A
   failed asset job deletes the incomplete draft and tag.
7. The APT repository workflow retains Debian packages from the newest two
   stable releases, signs fresh repository metadata, and deploys the complete
   repository to GitHub Pages.

`fix:` commits produce patch releases, `feat:` commits produce minor releases, and commits with `!` produce major releases.

## Application Provider

The maintained default application provider is part of the tracked source and
does not depend on repository secrets. CI exercises the default path for both
Purple adapters. The release workflow verifies the exact maintained identity
before packaging every binary and before creating the source archive.

A package without an embedded provider is a broken build and must never be
published as a release asset.

Project release jobs leave `TDLIB_PURPLE_REQUIRE_CUSTOM_CREDENTIALS` at its
default `OFF` value and use the maintained identity. A downstream release
pipeline that owns a different identity should set the flag to `ON` and provide
both custom file paths. Configuration then fails instead of silently falling
back to the maintained project identity.

This is identity enforcement, not secret handling. Telegram API IDs and hashes
are public application identifiers embedded in release binaries. Pass custom
values through the validated file-path options so pipeline configuration is
unambiguous.

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

## APT Repository

GitHub Pages must use GitHub Actions as its publishing source. Create a
protected `apt-repository` environment containing these secrets before the
first repository deployment:

- `APT_SIGNING_PRIVATE_KEY`: ASCII-armored private key for a dedicated archive
  signing key.
- `APT_SIGNING_KEY_PASSPHRASE`: passphrase for that key.

Set the environment variable `APT_SIGNING_KEY_FINGERPRINT` to the full primary
fingerprint. Publication fails if the imported key does not match it.
Restrict both `apt-repository` and `github-pages` environments to the `master`
branch.

Keep an offline backup of the signing key. The workflow imports it into a
temporary GnuPG home, publishes only the minimal public key, then removes the
temporary key material. Never reuse a personal signing key.

The Release workflow invokes the reusable APT workflow after publishing a
stable release. Run **Publish APT repository** manually once to bootstrap Pages
from existing releases or to repair a failed deployment. Repository metadata
uses separate `debian-13` and `ubuntu-24.04` suites and retains packages from
up to the newest two stable releases for rollback.

Before the first run, open **Settings**, **Pages**, then select **GitHub
Actions** as the publishing source. GitHub does not allow the repository token
to enable Pages automatically.

## Windows Assets

The repository still has an NSIS target, but Windows release assets are not automated yet. The missing piece is a reproducible Windows dependency environment for TDLib, libpurple, and the plugin's image/translation dependencies.
