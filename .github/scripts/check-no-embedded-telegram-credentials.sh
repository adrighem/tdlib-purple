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
        echo "Missing optional credential override ${variable}." >&2
        exit 1
    fi
done
if ! grep -q 'TDLIB_PURPLE_REQUIRE_CUSTOM_CREDENTIALS' \
    cmake/TelegramApplicationCredentials.cmake; then
    echo "Missing required-custom credential mode." >&2
    exit 1
fi

provider_check_dir="$(env -i PATH="${PATH}" mktemp -d)"
chmod 700 "${provider_check_dir}"
trap 'rm -f "${provider_check_dir}/provider.c" \
    "${provider_check_dir}/provider-state.h" \
    "${provider_check_dir}/expected-api-id" \
    "${provider_check_dir}/expected-api-hash" \
    "${provider_check_dir}/expected-provider.c"; \
    rmdir "${provider_check_dir}" 2>/dev/null || true' EXIT
env -i PATH="${PATH}" python3 \
    credentials/generate-application-credentials.py \
    --source-root "$(pwd)" \
    --output "${provider_check_dir}/provider.c" \
    --state-output "${provider_check_dir}/provider-state.h"
if ! grep -q \
    '^#define TDLIB_PURPLE_APPLICATION_CREDENTIALS_AVAILABLE 1$' \
    "${provider_check_dir}/provider-state.h"; then
    echo "The default application credential provider is unavailable." >&2
    exit 1
fi

umask 077
printf '%s\n' '32769927' \
    > "${provider_check_dir}/expected-api-id"
printf '%s\n' 'a0d6d0f8a610fb055903fd0e94d8eeb0' \
    > "${provider_check_dir}/expected-api-hash"
env -i PATH="${PATH}" python3 \
    credentials/generate-application-credentials.py \
    --api-id-file "${provider_check_dir}/expected-api-id" \
    --api-hash-file "${provider_check_dir}/expected-api-hash" \
    --source-root "$(pwd)" \
    --output "${provider_check_dir}/expected-provider.c"
if ! cmp -s "${provider_check_dir}/provider.c" \
    "${provider_check_dir}/expected-provider.c"; then
    echo "The default provider does not use the maintained application identity." >&2
    exit 1
fi

if git ls-files | grep -q \
    'telegram-application-credentials-embedded\.c$'; then
    echo "Generated Telegram application credentials must not be committed." >&2
    exit 1
fi

git grep -l \
    'tdlib_purple_application_credentials_embedded' \
    -- '*.c' '*.cc' '*.cpp' '*.cxx' '*.h' '*.hh' '*.hpp' '*.hxx' |
while IFS= read -r provider_file; do
    case "${provider_file}" in
        credentials/telegram-application-credentials-private.h | \
        credentials/telegram-application-credentials.c | \
        purple3/test/application-credentials-test-backend.c | \
        test/application-credentials-test-backend.c)
            ;;
        *)
            echo "Unexpected tracked application credential provider symbol in ${provider_file}." >&2
            exit 1
            ;;
    esac
done
