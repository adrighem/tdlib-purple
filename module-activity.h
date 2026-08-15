#ifndef TDLIB_PURPLE_MODULE_ACTIVITY_H
#define TDLIB_PURPLE_MODULE_ACTIVITY_H

#include <glib.h>

// Keeps the plugin module loaded while code owned by it can still execute.
// Guards may be created on any thread.
class ModuleActivityGuard {
public:
    ModuleActivityGuard() noexcept;
    ~ModuleActivityGuard();

    ModuleActivityGuard(const ModuleActivityGuard &) = delete;
    ModuleActivityGuard &operator=(const ModuleActivityGuard &) = delete;

    ModuleActivityGuard(ModuleActivityGuard &&other) noexcept;
    ModuleActivityGuard &operator=(
        ModuleActivityGuard &&other) noexcept;

private:
    bool m_active;
};

bool moduleActivityPending() noexcept;

// The context these helpers attach to. Borrowed, and null means the default
// context, which is where they went when there was nothing else to attach to.
// Set on the thread the sources are dispatched on, before any of them exist.
void moduleActivitySetContext(GMainContext *context);

// These helpers attach a source to the context above and retain ownership of
// userData until the source is destroyed. The destroy callback, when supplied,
// also runs if the source cannot be attached.
guint moduleActivityAddIdle(
    GSourceFunc callback,
    gpointer userData,
    GDestroyNotify destroy = nullptr);
guint moduleActivityAddTimeout(
    guint intervalMilliseconds,
    GSourceFunc callback,
    gpointer userData,
    GDestroyNotify destroy = nullptr);

// Cancel one of the above by the id it returned. g_source_remove would only
// work while these attach to the default context, and it complains about an id
// whose source has already run; this reports that plainly instead.
gboolean moduleActivityRemove(guint id);

#endif
