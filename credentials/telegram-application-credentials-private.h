/*
 * Internal provider boundary. Builds supply a generated implementation from
 * the maintained defaults or a validated custom application credential pair.
 */

#ifndef TDLIB_PURPLE_APPLICATION_CREDENTIALS_PRIVATE_H
#define TDLIB_PURPLE_APPLICATION_CREDENTIALS_PRIVATE_H

#include "telegram-application-credentials.h"

G_BEGIN_DECLS

const TdlibPurpleApplicationCredentials *
tdlib_purple_application_credentials_embedded(void);

G_END_DECLS

#endif /* TDLIB_PURPLE_APPLICATION_CREDENTIALS_PRIVATE_H */
