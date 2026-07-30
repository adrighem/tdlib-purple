/*
 * tdlib-purple - Telegram protocol plugin for libpurple
 * Copyright (C) tdlib-purple contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, see <https://www.gnu.org/licenses/>.
 */

#include <gio/gio.h>
#include <glib/gi18n-lib.h>

#include "telegram-purple3-protocol.h"

struct _TelegramTdlibProtocol {
    PurpleProtocol parent;
};

G_DEFINE_DYNAMIC_TYPE_EXTENDED(
    TelegramTdlibProtocol,
    telegram_tdlib_protocol,
    PURPLE_TYPE_PROTOCOL,
    G_TYPE_FLAG_FINAL,
    {})

static PurpleAccountSettings *
telegram_tdlib_protocol_get_default_account_settings(
    G_GNUC_UNUSED PurpleProtocol *protocol)
{
    return purple_account_settings_new();
}

static PurpleConnection *
telegram_tdlib_protocol_create_connection(
    G_GNUC_UNUSED PurpleProtocol *protocol,
    G_GNUC_UNUSED PurpleAccount *account,
    GError **error)
{
    g_set_error_literal(
        error,
        G_IO_ERROR,
        G_IO_ERROR_NOT_SUPPORTED,
        _("Telegram connectivity is not implemented in the Purple 3 adapter "
          "yet."));

    return NULL;
}

static void
telegram_tdlib_protocol_init(G_GNUC_UNUSED TelegramTdlibProtocol *protocol)
{
}

static void
telegram_tdlib_protocol_class_finalize(
    G_GNUC_UNUSED TelegramTdlibProtocolClass *klass)
{
}

static void
telegram_tdlib_protocol_class_init(TelegramTdlibProtocolClass *klass)
{
    PurpleProtocolClass *protocol_class = PURPLE_PROTOCOL_CLASS(klass);

    protocol_class->create_connection =
        telegram_tdlib_protocol_create_connection;
    protocol_class->get_default_account_settings =
        telegram_tdlib_protocol_get_default_account_settings;
}

void
telegram_tdlib_protocol_register(GPluginNativePlugin *plugin)
{
    telegram_tdlib_protocol_register_type(G_TYPE_MODULE(plugin));
}

PurpleProtocol *
telegram_tdlib_protocol_new(void)
{
    return g_object_new(
        TELEGRAM_TDLIB_TYPE_PROTOCOL,
        "id", "telegram-tdlib",
        "name", "Telegram (tdlib)",
        "description", _("Telegram protocol support using TDLib"),
        "icon-name", "im-telegram",
        "icon-resource-path", "/im/tdlib-purple/protocols/telegram/icons",
        NULL);
}
