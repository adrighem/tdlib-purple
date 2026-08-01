/*
 * Synthetic application-credential backend for Purple 3 tests.
 */

#ifndef PURPLE3_APPLICATION_CREDENTIALS_TEST_BACKEND_H
#define PURPLE3_APPLICATION_CREDENTIALS_TEST_BACKEND_H

#include <glib.h>

G_BEGIN_DECLS

void purple3_test_application_credentials_set_unavailable(void);
void purple3_test_application_credentials_set(
    gint32 api_id,
    const gchar *api_hash,
    gsize api_hash_length);

G_END_DECLS

#endif /* PURPLE3_APPLICATION_CREDENTIALS_TEST_BACKEND_H */
