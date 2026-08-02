/*
 * tdlib-purple - Unofficial Telegram protocol plugin for libpurple
 * Copyright (C) tdlib-purple contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 3 of the License, or (at your option)
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

#include "telegram-purple3-connection.h"
#include "telegram-purple3-protocol.h"

#define TELEGRAM_TDLIB_SETTING_ENABLE_SECRET_CHATS "enable-secret-chats"

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
    PurpleAccountSetting *setting = NULL;
    PurpleAccountSettings *settings = NULL;

    settings = purple_account_settings_new();

    setting = purple_account_setting_boolean_new(
        TELEGRAM_TDLIB_SETTING_ENABLE_SECRET_CHATS,
        _("Enable secret chats (takes effect at reconnect)"),
        TRUE);
    purple_account_setting_set_weight(setting, 10);
    purple_account_setting_set_advanced(setting, TRUE);
    purple_account_settings_add_setting(settings, setting);

    return settings;
}

static gboolean
telegram_tdlib_protocol_validate_account(
    G_GNUC_UNUSED PurpleProtocol *protocol,
    PurpleAccount *account,
    GError **error)
{
    g_return_val_if_fail(PURPLE_IS_ACCOUNT(account), FALSE);
    g_return_val_if_fail(error == NULL || *error == NULL, FALSE);

    return TRUE;
}

static PurpleConnection *
telegram_tdlib_protocol_create_connection(
    G_GNUC_UNUSED PurpleProtocol *protocol,
    PurpleAccount *account,
    G_GNUC_UNUSED GError **error)
{
    g_return_val_if_fail(PURPLE_IS_ACCOUNT(account), NULL);

    return g_object_new(
        TELEGRAM_TDLIB_TYPE_CONNECTION,
        "account", account,
        NULL);
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
    protocol_class->validate_account =
        telegram_tdlib_protocol_validate_account;
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
        "name", "Unofficial Telegram",
        "description", _("Unofficial Telegram protocol support using TDLib"),
        "icon-name", "im-telegram",
        "icon-resource-path", "/im/tdlib-purple/protocols/telegram/icons",
        NULL);
}
