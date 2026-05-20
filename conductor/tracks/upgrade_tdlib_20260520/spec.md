# Specification - Upgrade to tdlib 1.8.64

## Overview
This track involves upgrading the core Telegram Database Library (TDLib) from its current version to 1.8.64. This is a maintenance task to ensure the plugin benefits from the latest Telegram API features, security patches, and performance improvements.

## Goals
- Update the TDLib submodule or dependency to version 1.8.64.
- Adapt `tdlib-purple` code to any breaking changes in TDLib 1.8.64.
- Verify all existing functionality remains intact after the upgrade.
- Update build configurations to reflect the new version requirement.

## Requirements
- The plugin must compile successfully with TDLib 1.8.64.
- All existing unit tests and regression tests must pass.
- Core features (messaging, media, secret chats) must be verified manually.
- `CMakeLists.txt` version checks must be updated if applicable.

## Success Criteria
- [ ] Build system updated to require TDLib 1.8.64.
- [ ] Successful compilation of `libtelegram-tdlib.so`.
- [ ] All automated tests pass with the new TDLib version.
- [ ] Manual verification of key Telegram features is successful.
