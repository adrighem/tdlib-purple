/*
 * Purple 3 application-credential integration tests.
 */

#include <string.h>

#include <glib.h>

#include "application-credentials-test-backend.h"
#include "telegram-purple3-application-credentials.h"

static const gchar purple3_test_api_hash[] =
    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";

static void
purple3_test_application_credentials_unavailable(void)
{
    TdlibPurpleApplicationCredentials credentials;
    GError *error = NULL;

    purple3_test_application_credentials_set_unavailable();
    memset(&credentials, 0x7f, sizeof(credentials));

    g_assert_false(telegram_tdlib_copy_application_credentials(
        &credentials, &error));

    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_NOT_INITIALIZED);
    g_assert_cmpint(credentials.api_id, ==, 0);
    g_assert_cmpint(credentials.api_hash[0], ==, '\0');
    g_clear_error(&error);
}

static void
purple3_test_application_credentials_available(void)
{
    TdlibPurpleApplicationCredentials credentials = {0};
    GError *error = NULL;

    purple3_test_application_credentials_set(
        1,
        purple3_test_api_hash,
        sizeof(purple3_test_api_hash));

    g_assert_true(telegram_tdlib_copy_application_credentials(
        &credentials, &error));

    g_assert_no_error(error);
    g_assert_cmpint(credentials.api_id, ==, 1);
    g_assert_true(
        memcmp(credentials.api_hash,
               purple3_test_api_hash,
               sizeof(purple3_test_api_hash)) == 0);

    purple3_test_application_credentials_set_unavailable();
    g_assert_cmpint(credentials.api_id, ==, 1);
    g_assert_true(
        memcmp(credentials.api_hash,
               purple3_test_api_hash,
               sizeof(purple3_test_api_hash)) == 0);
}

static void
purple3_test_application_credentials_malformed(void)
{
    TdlibPurpleApplicationCredentials credentials;
    GError *error = NULL;

    purple3_test_application_credentials_set(
        0,
        purple3_test_api_hash,
        sizeof(purple3_test_api_hash));
    memset(&credentials, 0x7f, sizeof(credentials));

    g_assert_false(telegram_tdlib_copy_application_credentials(
        &credentials, &error));

    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_NOT_INITIALIZED);
    g_assert_cmpint(credentials.api_id, ==, 0);
    g_assert_cmpint(credentials.api_hash[0], ==, '\0');
    g_clear_error(&error);
    purple3_test_application_credentials_set_unavailable();
}

int
main(int argc, char *argv[])
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func(
        "/purple3/application-credentials/unavailable",
        purple3_test_application_credentials_unavailable);
    g_test_add_func(
        "/purple3/application-credentials/available",
        purple3_test_application_credentials_available);
    g_test_add_func(
        "/purple3/application-credentials/malformed",
        purple3_test_application_credentials_malformed);

    return g_test_run();
}
