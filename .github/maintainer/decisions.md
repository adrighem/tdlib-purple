# Maintenance Decisions

## Active Items Under Review

### PR:24 (fix: do not look up a username when filing a known contact into a group)
- **Status:** Merged [2026-08-14]
- **Summary:** Fixes contact regrouping bug where filing a contact into a new group asks the server to resolve internal id/secret usernames, causing failure and deleting the contact from the UI. Also fixes parser bug in `purpleBuddyNameToSecretChatId`.
- **Verdict:** Merged successfully. Excellent quality and correct fix.

### PR:25 (fix: do not delete a contact that is only unfiled from one group)
- **Status:** Merged [2026-08-14]
- **Summary:** Fixes buddy removal where unfiling from one group deletes the contact completely from Telegram. Uses standard `purple_find_buddies()` count to ensure contact is only deleted if count falls to 1.
- **Verdict:** Merged successfully. Correct and essential fix.

### ISSUE:27 (Removing a contact deletes it and its history with no confirmation. Was disabling the prompt deliberate?)
- **Status:** Triaged
- **Summary:** Restores the prompt when `g_requestUiOperations` is true, keeping headless/scripted clients functional while improving standard client UX.
- **Recommendation:** Implement a conditional prompt with `purple_request_action` when UI operations are available.

### ISSUE:26 (Scheduling bypasses PurpleEventLoopUiOps, so the plugin does nothing on a UI with its own event loop)
- **Status:** Triaged - PR Requested [2026-08-14]
- **Summary:** Bypassing libpurple's event loop abstraction (`PurpleEventLoopUiOps`) by using GLib `GSource` directly causes the plugin to hang indefinitely on non-GLib clients (like `bitlbee`).
- **Recommendation:** Commented on the issue requesting the custom Purple-2 scheduling backend PR. Detailed analysis saved in `notes/issue-26.md`.

---

## Handover & Repository Configuration Changes

### Release-Please Author Attribution Configuration
- **Date:** [2026-08-15]
- **Change:** Enabled `"include-commit-authors": true` in `release-please-config.json`.
- **Reasoning:** In accordance with repository policies (documented in the global `GEMINI.md`), PR author/contributor names must always be included in the generated changelog for every bullet point they were involved in. This configuration setting ensures that future release-please PRs automatically resolve and append GitHub usernames to changelog bullet points, reducing manual editing effort.
- **Manual Corrections:** Corrected current release-please PR #28's `CHANGELOG.md` to append the missing `@KnutMann` mentions for all corresponding commits.

