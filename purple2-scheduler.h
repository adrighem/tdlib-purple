#ifndef PURPLE2_SCHEDULER_H
#define PURPLE2_SCHEDULER_H

#include <glib.h>

// Why this exists.
//
// The Purple-neutral core schedules by creating GLib sources and attaching them
// to a GMainContext. Something has to iterate that context or the sources are
// never dispatched. Pidgin runs a GLib main loop and its PurpleEventLoopUiOps
// are the GLib functions themselves, so under Pidgin the default context is
// iterated and this is invisible. A user interface that supplies its own event
// loop, which is what PurpleEventLoopUiOps is for, iterates no GLib context at
// all: Adium runs a CFRunLoop, bitlbee runs its own select loop. There the
// plugin loads, TDLib starts its threads, and nothing else ever happens. The
// account stays on Connecting with no error, because the callback that would
// deliver the first authorization update is never called.
//
// The fix is to stop assuming and start driving. The plugin dispatches on a
// context of its own and this pump drives that context from whatever event loop
// libpurple was given, through purple_input_add and purple_timeout_add. A
// GMainContext already carries the thread-safe wakeup that makes this work: it
// is a pipe on POSIX, it is signalled by g_source_attach from any thread, and
// g_main_context_query hands out its descriptor. So a worker thread keeps
// talking to GLib alone, which is thread-safe, and every libpurple call happens
// on the thread libpurple is driven from, which is what libpurple requires.
//
// A context of its own rather than the default one. Owning the process default
// context would mean permanently acquiring something the plugin does not own,
// blocking anybody else from iterating it, and dispatching every source in the
// process that was dormant until now. None of that is needed: the plugin knows
// which context its own work goes to.
//
// One path for every user interface, Pidgin included. Under Pidgin the pump's
// watches are GLib watches on the default context, so the private context is
// nested inside the loop that was already running. That is one code path
// exercised everywhere instead of two, one of which would only ever run on the
// platforms with no continuous integration.

enum class Purple2SchedulerDecision {
    NotAttempted,
    Installed,
    // No event loop ui ops, or ops without the calls the pump needs. libpurple
    // dereferences those members without checking, so this is also the guard
    // that keeps the plugin from crashing a user interface that supplies none.
    NoEventLoopUiOps,
    // TDLIB_PURPLE_SCHEDULER=glib. The escape hatch back to the old behaviour.
    DisabledByEnvironment,
    // Windows. purple_input_add there wants a socket, and a GLib wakeup is an
    // event handle.
    UnsupportedPlatform,
    Failed
};

// Install the pump. Idempotent, and safe to call when it will decline: a
// decline leaves the plugin scheduling exactly as it did before there was a
// pump. Must be called on the thread libpurple runs on.
bool purple2SchedulerInstall();

// Take the pump down again. Safe from inside a dispatched callback, in which
// case it takes effect once that callback returns. Must be called on the thread
// that installed it.
void purple2SchedulerUninstall();

bool purple2SchedulerInstalled();
Purple2SchedulerDecision purple2SchedulerLastDecision();

// The context the plugin's sources belong to, or null when no pump is
// installed. Null means the thread default, which is where they went before.
GMainContext *purple2SchedulerContext();

#endif
