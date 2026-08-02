/*
 * tdlib-purple - Unofficial Telegram protocol plugin for libpurple
 * Copyright (C) tdlib-purple contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 3 of the License, or (at your option)
 * any later version.
 */

#include <glib/gi18n-lib.h>

#include "telegram-purple3-application-credentials.h"

gboolean
telegram_tdlib_copy_application_credentials(
    TdlibPurpleApplicationCredentials *credentials,
    GError **error)
{
    const TdlibPurpleApplicationCredentials *provided_credentials = NULL;

    g_return_val_if_fail(credentials != NULL, FALSE);
    g_return_val_if_fail(error == NULL || *error == NULL, FALSE);

    *credentials = (TdlibPurpleApplicationCredentials){0};
    provided_credentials = tdlib_purple_application_credentials_get();
    if (provided_credentials == NULL) {
        g_set_error_literal(
            error,
            G_IO_ERROR,
            G_IO_ERROR_NOT_INITIALIZED,
            _("Telegram application credentials are unavailable in this "
              "build."));
        return FALSE;
    }

    *credentials = *provided_credentials;

    return TRUE;
}
