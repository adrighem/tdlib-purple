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

void setPurpleRequestUiCapabilities(
    bool uiOperations,
    bool iconActions,
    bool closeRequests);
void setPurpleRequestIconHandleAvailable(bool available);
void resetPurpleRequestUi();
std::size_t purpleRequestIconCount();
std::size_t purpleRequestIconSize(std::size_t index);
std::vector<unsigned char> purpleRequestIconCopy(std::size_t index);
bool purpleRequestIconClosed(std::size_t index);
void invokePurpleRequestIconAction(std::size_t index);
void resetPurpleAccountLifecycle();
void setPurpleAccountLifecycleSimulation(bool enabled);
void setPurpleAccountEnabled(PurpleAccount *account, bool enabled);
void setPurpleAccountDisableDuringDisconnect(bool enabled);
unsigned purpleAccountDisconnectCount();
unsigned purpleAccountConnectCount();

// A user interface's event loop, faked.
//
// None is the default and reports no event loop ui ops at all, which is what
// every test that does not care about scheduling wants: code that asks whether
// it can schedule through libpurple is told it cannot, and behaves as it did
// before there was anything to ask.
//
// Recording answers as a user interface with its own event loop would: it hands
// out watches and timeouts, remembers them, and runs them only when a test says
// so. Deliberately not backed by GLib, because a fake that dispatched through
// GLib could not tell the difference between working and being carried by a
// GLib main loop, which is the whole subject.
enum class PurpleEventLoopMode {
    None,
    Recording
};

void setPurpleEventLoopMode(PurpleEventLoopMode mode);
void resetPurpleEventLoop();

std::size_t purpleEventLoopWatchCount();
std::size_t purpleEventLoopTimeoutCount();
// The condition each live watch was registered with, in registration order.
std::vector<PurpleInputCondition> purpleEventLoopWatchConditions();
// Run every watch whose descriptor is ready right now. Returns how many ran.
std::size_t purpleEventLoopRunReadyWatches();
// Move the fake clock and run whatever falls due. Returns how many ran.
std::size_t purpleEventLoopAdvance(unsigned milliseconds);
// The shortest interval any live timeout was registered with, or -1 for none.
int purpleEventLoopShortestTimeout();
// Calls into the event loop from a thread other than the one that reset it.
unsigned purpleEventLoopThreadViolations();
// Handles removed after they had already fired, which libpurple complains
// about and no correct caller does.
unsigned purpleEventLoopStaleRemovals();

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
