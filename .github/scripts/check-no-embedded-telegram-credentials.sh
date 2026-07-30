#!/bin/sh

set -eu

for variable in API_ID API_HASH STUFF; do
    if git grep -Eq \
        "set\\([[:space:]]*${variable}([[:space:]]|$)" \
        -- '*.cmake' 'CMakeLists.txt'; then
        echo "CMakeLists.txt must not define legacy raw ${variable} input." >&2
        exit 1
    fi
done

for variable in TDLIB_PURPLE_API_ID_FILE TDLIB_PURPLE_API_HASH_FILE; do
    if ! grep -q "${variable}" cmake/TelegramApplicationCredentials.cmake; then
        echo "Missing path-only credential input ${variable}." >&2
        exit 1
    fi
done

if git ls-files | grep -q \
    'telegram-application-credentials-embedded\.c$'; then
    echo "Generated Telegram application credentials must not be committed." >&2
    exit 1
fi
