/*
 * Purple 3 credential-unavailable provider test.
 */

#include <string.h>

#include <glib.h>

#include "telegram-purple3-application-credentials.h"

static void
purple3_test_application_credentials_stub(void)
{
    TdlibPurpleApplicationCredentials credentials;
    GError *error = NULL;

    memset(&credentials, 0x7f, sizeof(credentials));
    g_assert_false(telegram_tdlib_copy_application_credentials(
        &credentials, &error));

    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_NOT_INITIALIZED);
    g_assert_cmpint(credentials.api_id, ==, 0);
    g_assert_cmpint(credentials.api_hash[0], ==, '\0');
    g_clear_error(&error);
}

int
main(int argc, char *argv[])
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func(
        "/purple3/application-credentials/stub",
        purple3_test_application_credentials_stub);

    return g_test_run();
}
