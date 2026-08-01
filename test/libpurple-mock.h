#ifndef _LIBPURPLE_MOCK_H
#define _LIBPURPLE_MOCK_H

#include <glib.h>
#include <cstddef>
#include <vector>
#include <string>
#include <purple.h>

extern "C" {

void setFakeFileSize(const char *path, size_t size);
void clearFakeFiles();
int  getLastImgstoreId();
guint8 *arrayDup(gpointer data, size_t size);
void setUiName(const char *name);

};

std::size_t registeredPurpleCommandCount();
bool purpleCommandRegistered(const std::string &command);
// A value of 1 fails the next registration, 2 fails the one after that.
// The failure setting clears itself when it triggers.
void failPurpleCommandRegistrationAfter(unsigned callsUntilFailure);

#if !GLIB_CHECK_VERSION(2, 34, 0)
static void g_list_free_full (GList *list, GDestroyNotify free_func)
{
    if (free_func)
        for (GList *item = list; item; item = item->next)
            free_func(item->data);
    g_list_free(list);
}
#endif

#endif
