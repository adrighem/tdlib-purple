/*
 * Credential-unavailable provider for public and unconfigured builds.
 */

#include "telegram-application-credentials-private.h"

const TdlibPurpleApplicationCredentials *
tdlib_purple_application_credentials_embedded(void)
{
    return NULL;
}
