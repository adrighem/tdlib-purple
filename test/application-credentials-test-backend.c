#include "application-credentials-test-backend.h"

#include "telegram-application-credentials-private.h"

#include <string.h>

static TdlibPurpleApplicationCredentials test_credentials;
static gboolean test_credentials_available = FALSE;

void
tdlib_purple_test_application_credentials_set_unavailable(void)
{
    memset(&test_credentials, 0, sizeof(test_credentials));
    test_credentials_available = FALSE;
}

void
tdlib_purple_test_application_credentials_set(
    gint32 api_id,
    const gchar *api_hash,
    gsize api_hash_length)
{
    gsize copy_length = MIN(api_hash_length,
                            sizeof(test_credentials.api_hash));

    memset(&test_credentials, 0, sizeof(test_credentials));
    test_credentials.api_id = api_id;
    if (api_hash != NULL && copy_length > 0) {
        memcpy(test_credentials.api_hash, api_hash, copy_length);
    }
    test_credentials_available = TRUE;
}

const TdlibPurpleApplicationCredentials *
tdlib_purple_application_credentials_embedded(void)
{
    return test_credentials_available ? &test_credentials : NULL;
}
