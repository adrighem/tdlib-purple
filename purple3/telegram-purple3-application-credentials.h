/*
 * tdlib-purple - Unofficial Telegram protocol plugin for libpurple
 * Copyright (C) tdlib-purple contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 3 of the License, or (at your option)
 * any later version.
 */

#ifndef TELEGRAM_PURPLE3_APPLICATION_CREDENTIALS_H
#define TELEGRAM_PURPLE3_APPLICATION_CREDENTIALS_H

#include <gio/gio.h>
#include <glib.h>

#include "telegram-application-credentials.h"

G_BEGIN_DECLS

G_GNUC_INTERNAL gboolean
telegram_tdlib_copy_application_credentials(
    TdlibPurpleApplicationCredentials *credentials,
    GError **error);

G_END_DECLS

#endif /* TELEGRAM_PURPLE3_APPLICATION_CREDENTIALS_H */
