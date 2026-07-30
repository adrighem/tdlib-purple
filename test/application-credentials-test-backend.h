#ifndef TDLIB_PURPLE_APPLICATION_CREDENTIALS_TEST_BACKEND_H
#define TDLIB_PURPLE_APPLICATION_CREDENTIALS_TEST_BACKEND_H

#include <glib.h>

G_BEGIN_DECLS

void tdlib_purple_test_application_credentials_set_unavailable(void);
void tdlib_purple_test_application_credentials_set(
    gint32 api_id,
    const gchar *api_hash,
    gsize api_hash_length);

G_END_DECLS

#endif /* TDLIB_PURPLE_APPLICATION_CREDENTIALS_TEST_BACKEND_H */
