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

#include "telegram-purple3-connection.h"
#include "telegram-purple3-protocol.h"

#define TELEGRAM_TDLIB_SETTING_PHONE_NUMBER "phone-number"
#define TELEGRAM_TDLIB_SETTING_API_ID "api-id"
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

    /* TRANSLATOR: Account settings, key (string) */
    setting = purple_account_setting_string_new(
        TELEGRAM_TDLIB_SETTING_PHONE_NUMBER,
        _("Phone number"),
        NULL);
    purple_account_setting_set_hint(
        setting, _("International format, for example +15551234567"));
    purple_account_setting_set_weight(setting, 10);
    purple_account_settings_add_setting(settings, setting);

    setting = purple_account_setting_string_new(
        TELEGRAM_TDLIB_SETTING_API_ID,
        _("API ID"),
        NULL);
    purple_account_setting_set_hint(
        setting, _("Numeric Telegram API ID from my.telegram.org"));
    purple_account_setting_set_weight(setting, 20);
    purple_account_settings_add_setting(settings, setting);

    setting = purple_account_setting_boolean_new(
        TELEGRAM_TDLIB_SETTING_ENABLE_SECRET_CHATS,
        _("Enable secret chats (takes effect at reconnect)"),
        TRUE);
    purple_account_setting_set_weight(setting, 30);
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
    PurpleAccountSettings *settings = NULL;
    const char *api_id = NULL;
    const char *phone_number = NULL;
    guint64 numeric_api_id = 0;

    settings = purple_account_get_settings(account);
    phone_number = purple_account_settings_get_string(
        settings, TELEGRAM_TDLIB_SETTING_PHONE_NUMBER, NULL);
    if (phone_number == NULL || phone_number[0] == '\0') {
        g_set_error_literal(
            error,
            PURPLE_ACCOUNT_ERROR,
            PURPLE_ACCOUNT_ERROR_NOT_VALID,
            _("A phone number is required."));
        return FALSE;
    }

    api_id = purple_account_settings_get_string(
        settings, TELEGRAM_TDLIB_SETTING_API_ID, NULL);
    if (api_id == NULL || api_id[0] == '\0') {
        g_set_error_literal(
            error,
            PURPLE_ACCOUNT_ERROR,
            PURPLE_ACCOUNT_ERROR_NOT_VALID,
            _("A Telegram API ID is required."));
        return FALSE;
    }

    if (api_id[0] < '1' || api_id[0] > '9') {
        g_set_error_literal(
            error,
            PURPLE_ACCOUNT_ERROR,
            PURPLE_ACCOUNT_ERROR_NOT_VALID,
            _("The Telegram API ID must be a positive decimal number without "
              "signs, spaces, or leading zeros."));
        return FALSE;
    }

    for (const char *cursor = api_id; *cursor != '\0'; cursor++) {
        guint digit = 0;

        if (*cursor < '0' || *cursor > '9') {
            g_set_error_literal(
                error,
                PURPLE_ACCOUNT_ERROR,
                PURPLE_ACCOUNT_ERROR_NOT_VALID,
                _("The Telegram API ID must contain decimal digits only."));
            return FALSE;
        }

        digit = (guint)(*cursor - '0');
        if (numeric_api_id >
            (((guint64)G_MAXINT32 - digit) / 10U))
        {
            g_set_error_literal(
                error,
                PURPLE_ACCOUNT_ERROR,
                PURPLE_ACCOUNT_ERROR_NOT_VALID,
                _("The Telegram API ID must be between 1 and 2147483647."));
            return FALSE;
        }

        numeric_api_id = (numeric_api_id * 10U) + digit;
    }

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
        "name", "Telegram (tdlib)",
        "description", _("Telegram protocol support using TDLib"),
        "icon-name", "im-telegram",
        "icon-resource-path", "/im/tdlib-purple/protocols/telegram/icons",
        NULL);
}
