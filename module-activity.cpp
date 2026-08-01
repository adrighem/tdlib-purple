#include "module-activity.h"

#include <atomic>
#include <new>
#include <utility>

namespace {

std::atomic<unsigned> &activityCount()
{
    // Unloading is only allowed once this counter reaches zero, so its
    // module-local lifetime is sufficient and does not leak across reloads.
    static std::atomic<unsigned> count(0);
    return count;
}

struct TrackedSource {
    TrackedSource(
        GSourceFunc sourceCallback,
        gpointer sourceData,
        GDestroyNotify sourceDestroy) noexcept
        : callback(sourceCallback),
          userData(sourceData),
          destroy(sourceDestroy)
    {
    }

    ModuleActivityGuard activity;
    GSourceFunc callback;
    gpointer userData;
    GDestroyNotify destroy;
    std::atomic<bool> published{false};
};

gboolean dispatchTrackedSource(gpointer userData)
{
    TrackedSource *source =
        static_cast<TrackedSource *>(userData);
    source->published.load(std::memory_order_acquire);
    try {
        return source->callback
                   ? source->callback(source->userData)
                   : FALSE;
    } catch (...) {
        return FALSE;
    }
}

void destroyTrackedSource(gpointer userData)
{
    TrackedSource *source =
        static_cast<TrackedSource *>(userData);
    if (!source)
        return;

    source->published.load(std::memory_order_acquire);
    try {
        if (source->destroy)
            source->destroy(source->userData);
    } catch (...) {
    }
    delete source;
}

guint attachTrackedSource(
    GSource *source,
    gint priority,
    GSourceFunc callback,
    gpointer userData,
    GDestroyNotify destroy)
{
    TrackedSource *tracked = new (std::nothrow) TrackedSource(
        callback, userData, destroy);
    if (!tracked) {
        if (destroy) {
            try {
                destroy(userData);
            } catch (...) {
            }
        }
        g_source_unref(source);
        return 0;
    }

    g_source_set_priority(source, priority);
    g_source_set_callback(
        source,
        dispatchTrackedSource,
        tracked,
        destroyTrackedSource);
    // GLib does not provide a C++ memory-model synchronization edge between
    // the attaching thread and the thread dispatching the source. Publish the
    // callback state explicitly before making the source dispatchable.
    tracked->published.store(true, std::memory_order_release);
    const guint id =
        g_source_attach(source, g_main_context_default());
    if (id == 0)
        g_source_destroy(source);
    g_source_unref(source);
    return id;
}

} // namespace

ModuleActivityGuard::ModuleActivityGuard() noexcept
    : m_active(true)
{
    activityCount().fetch_add(1, std::memory_order_acq_rel);
}

ModuleActivityGuard::~ModuleActivityGuard()
{
    if (m_active)
        activityCount().fetch_sub(1, std::memory_order_acq_rel);
}

ModuleActivityGuard::ModuleActivityGuard(
    ModuleActivityGuard &&other) noexcept
    : m_active(other.m_active)
{
    other.m_active = false;
}

ModuleActivityGuard &ModuleActivityGuard::operator=(
    ModuleActivityGuard &&other) noexcept
{
    if (this == &other)
        return *this;

    if (m_active)
        activityCount().fetch_sub(1, std::memory_order_acq_rel);
    m_active = other.m_active;
    other.m_active = false;
    return *this;
}

bool moduleActivityPending() noexcept
{
    return activityCount().load(std::memory_order_acquire) != 0;
}

guint moduleActivityAddIdle(
    GSourceFunc callback,
    gpointer userData,
    GDestroyNotify destroy)
{
    return attachTrackedSource(
        g_idle_source_new(),
        G_PRIORITY_DEFAULT_IDLE,
        callback,
        userData,
        destroy);
}

guint moduleActivityAddTimeout(
    guint intervalMilliseconds,
    GSourceFunc callback,
    gpointer userData,
    GDestroyNotify destroy)
{
    return attachTrackedSource(
        g_timeout_source_new(intervalMilliseconds),
        G_PRIORITY_DEFAULT,
        callback,
        userData,
        destroy);
}
