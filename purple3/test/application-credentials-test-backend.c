/*
 * Synthetic application-credential backend for Purple 3 tests.
 */

#include <string.h>

#include "application-credentials-test-backend.h"
#include "telegram-application-credentials-private.h"

static TdlibPurpleApplicationCredentials purple3_test_credentials;
static gboolean purple3_test_credentials_available = FALSE;

void
purple3_test_application_credentials_set_unavailable(void)
{
    memset(&purple3_test_credentials, 0, sizeof(purple3_test_credentials));
    purple3_test_credentials_available = FALSE;
}

void
purple3_test_application_credentials_set(
    gint32 api_id,
    const gchar *api_hash,
    gsize api_hash_length)
{
    gsize copy_length = MIN(api_hash_length,
                            sizeof(purple3_test_credentials.api_hash));

    memset(&purple3_test_credentials, 0, sizeof(purple3_test_credentials));
    purple3_test_credentials.api_id = api_id;
    if (api_hash != NULL && copy_length > 0) {
        memcpy(purple3_test_credentials.api_hash, api_hash, copy_length);
    }
    purple3_test_credentials_available = TRUE;
}

const TdlibPurpleApplicationCredentials *
tdlib_purple_application_credentials_embedded(void)
{
    if (!purple3_test_credentials_available) {
        return NULL;
    }

    return &purple3_test_credentials;
}
