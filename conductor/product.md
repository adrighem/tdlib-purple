# Initial Concept
A libpurple plugin for Telegram that succeeds telegram-purple, using TDLib.

# Product Definition - tdlib-purple

## Product Vision
Tdlib-purple aims to provide a stable, efficient, and feature-rich Telegram experience for users of libpurple-based IM clients (such as Pidgin, Finch, and Adium). As the successor to telegram-purple, it prioritizes a modern implementation using TDLib to ensure robust protocol support and improved performance.

## Target Audience
- **Desktop IM Users:** Individuals who rely on multi-protocol clients like Pidgin and Adium and need a reliable way to access Telegram.
- **Unified Messaging Proponents:** Users who prefer consolidating all their communication into a single, cohesive desktop application.

## Core Goals
- **Stability & Performance:** Deliver a crash-free experience with efficient resource usage, addressing long-standing issues in older implementations.
- **Modern Feature Support:** Integrate contemporary Telegram features such as animated stickers, supergroups, and media handling.
- **Protocol Parity:** Ensure that all essential Telegram messaging functionality is seamlessly available within the libpurple framework.

## Key Features
- **Comprehensive Media Support:** High-quality handling of photos, videos, files, and various sticker formats (including animated stickers).
- **Secret Chats:** Support for Telegram's end-to-end encrypted private conversations on the desktop.
- **Advanced Group Management:** Robust handling of Supergroups and the ability to display non-member chats effectively.

## Technical Constraints & Considerations
- **TDLib Synchronization:** Continuous alignment with the latest stable versions of TDLib (currently targeting 1.8.x) to maintain compatibility and security.
- **CPU Efficiency:** Careful optimization of computationally intensive tasks, such as the conversion of animated stickers (rlottie) and complex media processing.
- **Build Portability:** Ensuring the plugin remains buildable across various Linux distributions and other supported environments via CMake.
