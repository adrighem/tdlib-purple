# Technology Stack - tdlib-purple

## Core Languages & Standards
- **C++14**: The primary programming language, adhering to the C++14 standard as required by TDLib.

## Primary Frameworks
- **libpurple**: The core plugin framework, providing abstractions for IM protocols and client integration.
- **TDLib (Telegram Database Library)**: Handles all network communication, encryption, and local data storage for Telegram.
- **GLib**: Utilized for core data structures and event loops common in the libpurple ecosystem.

## Key Libraries & Dependencies
- **rlottie**: Used for rendering and converting Telegram's animated stickers.
- **libwebp & libpng**: Handles various sticker and image formats.
- **OpenSSL**: Provides cryptographic primitives for secure communication.
- **libtgvoip**: Enables support for Telegram voice calls.
- **fmt**: A modern C++ formatting library for string manipulation.
- **Gettext**: Handles internationalization and localization.

## Infrastructure & Tooling
- **CMake**: The build system used for configuration and compilation across platforms.
- **Google Test (GTest)**: The framework used for regression testing.
