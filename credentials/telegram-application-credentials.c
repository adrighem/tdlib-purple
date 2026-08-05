/*
 * tdlib-purple - Telegram client for libpurple using TDLib
 * Copyright (C) tdlib-purple contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 3 of the License, or (at your option)
 * any later version.
 */

#include "telegram-application-credentials-private.h"

#include <string.h>

static gboolean
tdlib_purple_application_api_hash_is_valid(const gchar *api_hash)
{
    if (api_hash == NULL) {
        return FALSE;
    }

    for (gsize index = 0; index < TDLIB_PURPLE_API_HASH_LENGTH; index++) {
        if (!g_ascii_isxdigit(api_hash[index])) {
            return FALSE;
        }
    }

    return api_hash[TDLIB_PURPLE_API_HASH_LENGTH] == '\0';
}

gboolean
tdlib_purple_application_credentials_parse_compatibility_override(
    const gchar *api_id,
    const gchar *api_hash,
    TdlibPurpleApplicationCredentials *credentials)
{
    guint64 numeric_api_id = 0;

    g_return_val_if_fail(credentials != NULL, FALSE);

    memset(credentials, 0, sizeof(*credentials));

    if (api_id == NULL || api_id[0] < '1' || api_id[0] > '9' ||
        !tdlib_purple_application_api_hash_is_valid(api_hash))
    {
        return FALSE;
    }

    for (const gchar *cursor = api_id; *cursor != '\0'; cursor++) {
        guint digit = 0;

        if (*cursor < '0' || *cursor > '9') {
            return FALSE;
        }

        digit = (guint)(*cursor - '0');
        if (numeric_api_id >
            (((guint64)G_MAXINT32 - digit) / 10U))
        {
            return FALSE;
        }

        numeric_api_id = (numeric_api_id * 10U) + digit;
    }

    credentials->api_id = (gint32)numeric_api_id;
    memcpy(credentials->api_hash, api_hash,
           TDLIB_PURPLE_API_HASH_LENGTH + 1);

    return TRUE;
}

const TdlibPurpleApplicationCredentials *
tdlib_purple_application_credentials_get(void)
{
    const TdlibPurpleApplicationCredentials *credentials =
        tdlib_purple_application_credentials_embedded();

    if (credentials == NULL || credentials->api_id <= 0 ||
        !tdlib_purple_application_api_hash_is_valid(credentials->api_hash))
    {
        return NULL;
    }

    return credentials;
}
