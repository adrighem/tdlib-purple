#include "purple2-scheduler.h"
#include "module-activity.h"

#include <purple.h>

#include <cstdlib>
#include <cstring>
#include <new>
#include <thread>
#include <vector>

#if !defined(_WIN32)
#include <poll.h>
#endif

// What can go wrong here, and where each one is dealt with.
//
//   A wakeup arrives with nothing ready. poll() reports nothing, check()
//     returns false, nothing is dispatched, the cycle re-arms. fire().
//   A descriptor stays readable because nobody consumed it. Every watch these
//     event loops offer is level triggered, so an undrained wakeup is a busy
//     loop in the host's event loop. g_main_context_check consumes it, but only
//     when it is handed a descriptor with revents set, which is why the
//     descriptors are polled here rather than trusting the condition the event
//     loop passed in. pollDescriptors().
//   A condition of read and write at once. Some implementations compare the
//     condition rather than masking it, so a combined watch silently becomes a
//     write watch. Read and write are always separate watches. armWatches().
//   A callback schedules more work. That is the normal case: the attach
//     signals the context's wakeup, the watch fires again, the next cycle
//     dispatches it.
//   A callback takes the pump down. uninstall() during a dispatch defers to the
//     end of the cycle rather than freeing state the cycle is standing on.
//     purple2SchedulerUninstall(), fire().
//   A watch outlives the pump it belongs to. Every callback checks that its
//     state is still the installed one before touching it. inputReady(),
//     timeoutReady().
//   A timeout handle is removed after it has already fired. libpurple complains
//     about an unknown handle, so the handle is cleared as the timeout fires
//     and never removed afterwards. timeoutReady().
//   A callback throws. GLib is C and an exception crossing it is undefined, so
//     the cycle is wrapped the same way module-activity wraps its own.
//   The event loop has no ops, or ops with holes in them. libpurple calls those
//     members without checking, so this declines instead. eventLoopOpsUsable().
//   Windows. purple_input_add there takes a socket and a GLib wakeup is an
//     event handle, so the pump compiles out and the plugin behaves as before.

namespace {

// Every context has at least its own wakeup descriptor, and the plugin's own
// sources are idles and timeouts, which have none. This is a starting size, not
// a limit: query() reports what it needs and the buffer grows to match.
const guint INITIAL_POLL_DESCRIPTORS = 4;

struct Watch {
    int fd;
    PurpleInputCondition condition;
    guint handle;
};

struct PumpState {
    GMainContext *context = nullptr;
    bool acquired = false;
    std::vector<GPollFD> fds;
    std::vector<Watch> watches;
    guint timeoutHandle = 0;
    gint priority = 0;
    bool firing = false;
    bool uninstallRequested = false;
    std::thread::id thread;
};

// Heap allocated rather than a static object: the plugin can be unloaded, and a
// static with a destructor would leave the atexit handler for it pointing into
// a module that is no longer mapped.
PumpState *g_pump = nullptr;
Purple2SchedulerDecision g_decision =
    Purple2SchedulerDecision::NotAttempted;

void plan(PumpState *pump);
void fire(PumpState *pump);
void finishUninstall(PumpState *pump);

bool eventLoopOpsUsable()
{
    PurpleEventLoopUiOps *ops = purple_eventloop_get_ui_ops();

    // Each of these is called through without a null check by the purple_*
    // wrapper, so a hole in the table is a crash rather than a fallback.
    return ops && ops->timeout_add && ops->timeout_remove &&
           ops->input_add && ops->input_remove;
}

#if !defined(_WIN32)

short toPollEvents(gushort events)
{
    short result = 0;

    if (events & G_IO_IN)
        result |= POLLIN;
    if (events & G_IO_OUT)
        result |= POLLOUT;
    if (events & G_IO_PRI)
        result |= POLLPRI;

    return result;
}

gushort fromPollEvents(short events)
{
    gushort result = 0;

    if (events & POLLIN)
        result |= G_IO_IN;
    if (events & POLLOUT)
        result |= G_IO_OUT;
    if (events & POLLPRI)
        result |= G_IO_PRI;
    if (events & POLLERR)
        result |= G_IO_ERR;
    if (events & POLLHUP)
        result |= G_IO_HUP;
    if (events & POLLNVAL)
        result |= G_IO_NVAL;

    return result;
}

// Ask the descriptors what is actually ready. The event loop told us one of
// them woke us, but g_main_context_check needs the state of all of them, and it
// only consumes the context's wakeup when it is handed that descriptor with
// revents set.
void pollDescriptors(PumpState *pump)
{
    if (pump->fds.empty())
        return;

    std::vector<struct pollfd> descriptors(pump->fds.size());
    for (std::size_t i = 0; i < pump->fds.size(); i++) {
        descriptors[i].fd = pump->fds[i].fd;
        descriptors[i].events = toPollEvents(pump->fds[i].events);
        descriptors[i].revents = 0;
    }

    const int ready =
        poll(descriptors.data(), descriptors.size(), 0);

    for (std::size_t i = 0; i < pump->fds.size(); i++) {
        pump->fds[i].revents =
            (ready > 0) ? fromPollEvents(descriptors[i].revents) : 0;
    }
}

#else

void pollDescriptors(PumpState *)
{
}

#endif

void inputReady(
    gpointer userData, gint, PurpleInputCondition)
{
    PumpState *pump = static_cast<PumpState *>(userData);

    if (pump != g_pump)
        return;

    fire(pump);
}

gboolean timeoutReady(gpointer userData)
{
    PumpState *pump = static_cast<PumpState *>(userData);

    if (pump != g_pump)
        return FALSE;

    // Cleared before anything else can re-arm it. A handle that has fired is
    // not ours to remove any more.
    pump->timeoutHandle = 0;
    fire(pump);

    return FALSE;
}

bool watchesStillMatch(PumpState *pump)
{
    std::size_t wanted = 0;

    for (std::size_t i = 0; i < pump->fds.size(); i++) {
        const gushort events = pump->fds[i].events;
        if (events & (G_IO_IN | G_IO_PRI | G_IO_ERR | G_IO_HUP))
            wanted++;
        if (events & G_IO_OUT)
            wanted++;
    }

    if (wanted != pump->watches.size())
        return false;

    std::size_t index = 0;
    for (std::size_t i = 0; i < pump->fds.size(); i++) {
        const gushort events = pump->fds[i].events;

        if (events & (G_IO_IN | G_IO_PRI | G_IO_ERR | G_IO_HUP)) {
            if (pump->watches[index].fd != pump->fds[i].fd ||
                pump->watches[index].condition != PURPLE_INPUT_READ) {
                return false;
            }
            index++;
        }
        if (events & G_IO_OUT) {
            if (pump->watches[index].fd != pump->fds[i].fd ||
                pump->watches[index].condition != PURPLE_INPUT_WRITE) {
                return false;
            }
            index++;
        }
    }

    return true;
}

void addWatch(
    PumpState *pump, int fd, PurpleInputCondition condition)
{
    Watch watch;

    watch.fd = fd;
    watch.condition = condition;
    watch.handle =
        purple_input_add(fd, condition, inputReady, pump);

    if (watch.handle)
        pump->watches.push_back(watch);
}

void disarmWatches(PumpState *pump)
{
    for (std::size_t i = 0; i < pump->watches.size(); i++)
        purple_input_remove(pump->watches[i].handle);

    pump->watches.clear();
}

// The descriptor set of a context holding only idles and timeouts is one
// wakeup and never changes, so the usual outcome here is that nothing happens.
// Re-registering every cycle would churn one event source per wakeup in the
// host's event loop for no reason.
void armWatches(PumpState *pump)
{
    if (watchesStillMatch(pump))
        return;

    disarmWatches(pump);

    for (std::size_t i = 0; i < pump->fds.size(); i++) {
        const gushort events = pump->fds[i].events;

        if (events & (G_IO_IN | G_IO_PRI | G_IO_ERR | G_IO_HUP))
            addWatch(pump, pump->fds[i].fd, PURPLE_INPUT_READ);
        if (events & G_IO_OUT)
            addWatch(pump, pump->fds[i].fd, PURPLE_INPUT_WRITE);
    }
}

void disarmTimeout(PumpState *pump)
{
    if (pump->timeoutHandle) {
        purple_timeout_remove(pump->timeoutHandle);
        pump->timeoutHandle = 0;
    }
}

void armTimeout(PumpState *pump, gint timeout)
{
    disarmTimeout(pump);

    // A negative timeout is GLib saying nothing is waiting on the clock. Zero
    // is GLib saying something is ready now, which libpurple understands as
    // soon as possible, and which yields to the host's event loop in between
    // exactly as a GLib main loop would.
    if (timeout < 0)
        return;

    pump->timeoutHandle = purple_timeout_add(
        static_cast<guint>(timeout), timeoutReady, pump);
}

// Work out what the context wants next and ask the host's event loop for it.
void plan(PumpState *pump)
{
    gint timeout = -1;

    g_main_context_prepare(pump->context, &pump->priority);

    if (pump->fds.size() < INITIAL_POLL_DESCRIPTORS)
        pump->fds.resize(INITIAL_POLL_DESCRIPTORS);

    for (;;) {
        const gint needed = g_main_context_query(
            pump->context,
            pump->priority,
            &timeout,
            pump->fds.data(),
            static_cast<gint>(pump->fds.size()));

        if (needed <= static_cast<gint>(pump->fds.size())) {
            pump->fds.resize(needed > 0 ? needed : 0);
            break;
        }

        pump->fds.resize(needed);
    }

    armWatches(pump);
    armTimeout(pump, timeout);
}

// One turn: find out what is ready, run it, work out what is next.
void fire(PumpState *pump)
{
    // A dispatched callback that gets back in here would drive the context from
    // inside itself. GLib warns about that and drops work; there is nothing to
    // gain by it, because the cycle re-plans on its way out anyway.
    if (pump->firing)
        return;

    pump->firing = true;

    try {
        pollDescriptors(pump);

        if (g_main_context_check(
                pump->context,
                pump->priority,
                pump->fds.data(),
                static_cast<gint>(pump->fds.size()))) {
            g_main_context_dispatch(pump->context);
        }
    } catch (...) {
    }

    pump->firing = false;

    if (pump->uninstallRequested) {
        finishUninstall(pump);
        return;
    }

    try {
        plan(pump);
    } catch (...) {
    }
}

void finishUninstall(PumpState *pump)
{
    g_pump = nullptr;
    moduleActivitySetContext(nullptr);

    disarmTimeout(pump);
    disarmWatches(pump);

    if (pump->acquired)
        g_main_context_release(pump->context);

    g_main_context_unref(pump->context);
    delete pump;
}

} // namespace

bool purple2SchedulerInstall()
{
    if (g_pump)
        return true;

#if defined(_WIN32)
    g_decision = Purple2SchedulerDecision::UnsupportedPlatform;
    return false;
#else
    const char *requested = std::getenv("TDLIB_PURPLE_SCHEDULER");
    if (requested && !std::strcmp(requested, "glib")) {
        g_decision = Purple2SchedulerDecision::DisabledByEnvironment;
        return false;
    }

    if (!eventLoopOpsUsable()) {
        g_decision = Purple2SchedulerDecision::NoEventLoopUiOps;
        return false;
    }

    GMainContext *context = g_main_context_new();
    if (!context) {
        g_decision = Purple2SchedulerDecision::Failed;
        return false;
    }

    // Held for as long as the pump lives, on the thread the pump runs on.
    // g_main_context_prepare requires it, and a context with no owner does not
    // signal its wakeup when a source is attached from another thread, which is
    // the one thing the whole arrangement rests on.
    if (!g_main_context_acquire(context)) {
        g_main_context_unref(context);
        g_decision = Purple2SchedulerDecision::Failed;
        return false;
    }

    PumpState *pump = new (std::nothrow) PumpState();
    if (!pump) {
        g_main_context_release(context);
        g_main_context_unref(context);
        g_decision = Purple2SchedulerDecision::Failed;
        return false;
    }

    pump->context = context;
    pump->acquired = true;
    pump->thread = std::this_thread::get_id();

    g_pump = pump;
    moduleActivitySetContext(context);

    plan(pump);

    g_decision = Purple2SchedulerDecision::Installed;
    return true;
#endif
}

void purple2SchedulerUninstall()
{
    PumpState *pump = g_pump;
    if (!pump)
        return;

    if (pump->firing) {
        pump->uninstallRequested = true;
        return;
    }

    finishUninstall(pump);
}

bool purple2SchedulerInstalled()
{
    return g_pump != nullptr;
}

Purple2SchedulerDecision purple2SchedulerLastDecision()
{
    return g_decision;
}

GMainContext *purple2SchedulerContext()
{
    return g_pump ? g_pump->context : nullptr;
}
