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

#ifndef TELEGRAM_PURPLE3_PROTOCOL_H
#define TELEGRAM_PURPLE3_PROTOCOL_H

#include <glib.h>
#include <glib-object.h>

#include <gplugin-native.h>
#include <purple.h>

G_BEGIN_DECLS

#define TELEGRAM_TDLIB_TYPE_PROTOCOL (telegram_tdlib_protocol_get_type())
G_DECLARE_FINAL_TYPE(TelegramTdlibProtocol, telegram_tdlib_protocol,
                     TELEGRAM_TDLIB, PROTOCOL, PurpleProtocol)

G_GNUC_INTERNAL void telegram_tdlib_protocol_register(
    GPluginNativePlugin *plugin);
G_GNUC_INTERNAL PurpleProtocol *telegram_tdlib_protocol_new(void);

G_END_DECLS

#endif /* TELEGRAM_PURPLE3_PROTOCOL_H */
