/*
 * tdlib-purple - Telegram protocol plugin for libpurple
 * Copyright (C) tdlib-purple contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#ifndef TDLIB_PURPLE_APPLICATION_CREDENTIALS_H
#define TDLIB_PURPLE_APPLICATION_CREDENTIALS_H

#include <glib.h>

G_BEGIN_DECLS

#define TDLIB_PURPLE_API_HASH_LENGTH 32

typedef struct {
    gint32 api_id;
    gchar api_hash[TDLIB_PURPLE_API_HASH_LENGTH + 1];
} TdlibPurpleApplicationCredentials;

const TdlibPurpleApplicationCredentials *
tdlib_purple_application_credentials_get(void);

gboolean
tdlib_purple_application_credentials_parse_compatibility_override(
    const gchar *api_id,
    const gchar *api_hash,
    TdlibPurpleApplicationCredentials *credentials);

G_END_DECLS

#endif /* TDLIB_PURPLE_APPLICATION_CREDENTIALS_H */
