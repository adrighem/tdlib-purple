# Maintenance Run Ledger

## Run [2026-08-14]
- **Target Repository:** `adrighem/tdlib-purple`
- **Actions Completed:**
  - Triaged and merged PR #25 (fix: do not delete a contact that is only unfiled from one group).
  - Triaged and merged PR #24 (fix: do not look up a username when filing a known contact into a group).
  - Resolved ISSUE #27 (Removing a contact deletes it with no confirmation) by implementing a clean conditional deletion confirmation prompt (`purple_request_action`) that checks for the presence of UI ops before prompting.
  - Added full test coverage for the conditional prompt with the new `DeleteContactWithPrompt` test case in `test/private-chat-test.cpp`.
  - Analyzed and triaged ISSUE #26 (GLib main context / event loop bypass) and responded to KnutMann on GitHub requesting his custom Purple-2 scheduling backend PR.

## Run [2026-08-15]
- **Target Repository:** `adrighem/tdlib-purple`
- **Actions Completed:**
  - Triaged, built, tested, and merged PR #29 (fix: check request ui ops members that exist) by @KnutMann.
  - Triaged and merged PR #30 (fix: clear the connection's protocol data before closing the client) by @KnutMann.
  - Triaged and merged PR #31 (feat: drive the plugin's GLib context from libpurple's event loop) by @KnutMann. Closes ISSUE #26.
