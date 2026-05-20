# Product Guidelines - tdlib-purple

## Core Principles
1.  **Stability First:** The plugin must be robust and reliable. Crashes are unacceptable in an IM client.
2.  **Performance & Efficiency:** Minimize CPU and memory usage, especially for resource-intensive tasks like sticker processing and encryption.
3.  **Protocol Fidelity:** Strive to accurately represent Telegram's features and behaviors within the constraints of libpurple.
4.  **Security by Design:** Prioritize the security of user data and communications, especially for Secret Chats.

## UX & Interaction
- **Seamless Integration:** The plugin should feel like a native part of the host IM client (e.g., Pidgin). Use standard libpurple metaphors for UI elements.
- **Clear Status Feedback:** Provide informative status messages for connection states, message delivery, and encryption status.
- **Graceful Degradation:** When a Telegram feature cannot be fully represented in libpurple, provide a reasonable fallback or clear indication.

## Visual & Branding
- **Telegram Aesthetic:** Use official Telegram icons and terminology where appropriate and supported by the client.
- **Consistent Layout:** Ensure that messages, stickers, and media are displayed consistently and clearly.

## Technical Standards
- **Modern C++:** Adhere to C++14 standards and maintain clean, readable code.
- **Robust Error Handling:** Use appropriate error handling mechanisms to prevent crashes and provide useful diagnostic information.
- **Documentation:** Maintain clear internal documentation for complex logic and protocol mappings.
- **Test-Driven Mentality:** Ensure that new features and bug fixes are accompanied by relevant tests (GTest).
