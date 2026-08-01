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

// These helpers attach a source to the default context and retain ownership of
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

#endif
