#include "fixture.h"
#include "client-utils.h"
#include "libpurple-mock.h"
#include "module-activity.h"
#include "tdlib-purple.h"

#include <gtest/gtest.h>

#include <atomic>
#include <memory>
#include <stdexcept>
#include <thread>
#include <utility>

namespace {

PurplePluginInfo *loadedPluginInfo()
{
    fixture_init();
    PurplePluginInfo *info = getPluginInfo();
    EXPECT_NE(info, nullptr);
    if (info && info->load)
        EXPECT_TRUE(info->load(nullptr));
    return info;
}

class OrphanAccountThread final : public AccountThread {
public:
    struct Probe {
        std::atomic<bool> ran{false};
        std::atomic<bool> calledBack{false};
        std::atomic<bool> destroyed{false};
    };

    OrphanAccountThread(
        PurpleAccount *account,
        std::shared_ptr<Probe> probe,
        bool throwInRun = false)
        : AccountThread(account),
          m_probe(std::move(probe)),
          m_throwInRun(throwInRun)
    {
    }

    ~OrphanAccountThread() override
    {
        m_probe->destroyed = true;
    }

private:
    void run() override
    {
        m_probe->ran = true;
        if (m_throwInRun)
            throw std::runtime_error(
                "synthetic account worker failure");
    }

    void callback(PurpleTdClient *) override
    {
        m_probe->calledBack = true;
    }

    std::shared_ptr<Probe> m_probe;
    bool m_throwInRun;
};

class RestoreSingleThreadMode {
public:
    ~RestoreSingleThreadMode()
    {
        AccountThread::setSingleThread();
    }
};

struct IdleActivityProbe {
    bool called = false;
    bool destroyed = false;
};

gboolean runIdleActivityProbe(gpointer data)
{
    static_cast<IdleActivityProbe *>(data)->called = true;
    return FALSE;
}

void destroyIdleActivityProbe(gpointer data)
{
    static_cast<IdleActivityProbe *>(data)->destroyed = true;
}

} // namespace

TEST(PluginLifecycle, UnloadAndReloadMaintainCommands)
{
    PurplePluginInfo *info = loadedPluginInfo();
    ASSERT_NE(info, nullptr);
    ASSERT_NE(info->unload, nullptr);
    ASSERT_NE(info->load, nullptr);

    EXPECT_EQ(registeredPurpleCommandCount(), 2u);
    EXPECT_TRUE(purpleCommandRegistered("kick"));
    EXPECT_TRUE(purpleCommandRegistered("hangup"));

    EXPECT_TRUE(info->unload(nullptr));
    EXPECT_EQ(registeredPurpleCommandCount(), 0u);
    EXPECT_FALSE(purpleCommandRegistered("kick"));
    EXPECT_FALSE(purpleCommandRegistered("hangup"));

    EXPECT_TRUE(info->load(nullptr));
    EXPECT_EQ(registeredPurpleCommandCount(), 2u);
    EXPECT_TRUE(purpleCommandRegistered("kick"));
    EXPECT_TRUE(purpleCommandRegistered("hangup"));
}

TEST(PluginLifecycle, ActiveModuleWorkRefusesUnload)
{
    PurplePluginInfo *info = loadedPluginInfo();
    ASSERT_NE(info, nullptr);
    ASSERT_NE(info->unload, nullptr);
    ASSERT_NE(info->load, nullptr);

    {
        ModuleActivityGuard activity;
        EXPECT_TRUE(moduleActivityPending());
        EXPECT_FALSE(info->unload(nullptr));
        EXPECT_EQ(registeredPurpleCommandCount(), 2u);
        EXPECT_TRUE(purpleCommandRegistered("kick"));
        EXPECT_TRUE(purpleCommandRegistered("hangup"));
    }

    EXPECT_FALSE(moduleActivityPending());
    EXPECT_TRUE(info->unload(nullptr));
    EXPECT_EQ(registeredPurpleCommandCount(), 0u);
    EXPECT_TRUE(info->load(nullptr));
    EXPECT_EQ(registeredPurpleCommandCount(), 2u);
}

TEST(PluginLifecycle, FailedSecondCommandRegistrationRollsBack)
{
    PurplePluginInfo *info = loadedPluginInfo();
    ASSERT_NE(info, nullptr);
    ASSERT_NE(info->unload, nullptr);
    ASSERT_NE(info->load, nullptr);

    EXPECT_TRUE(info->unload(nullptr));
    EXPECT_EQ(registeredPurpleCommandCount(), 0u);

    failPurpleCommandRegistrationAfter(2);
    EXPECT_FALSE(info->load(nullptr));
    EXPECT_EQ(registeredPurpleCommandCount(), 0u);
    EXPECT_FALSE(purpleCommandRegistered("kick"));
    EXPECT_FALSE(purpleCommandRegistered("hangup"));

    EXPECT_TRUE(info->load(nullptr));
    EXPECT_EQ(registeredPurpleCommandCount(), 2u);
}

TEST(PluginLifecycle, TrackedIdleBlocksUnloadUntilDispatch)
{
    PurplePluginInfo *info = loadedPluginInfo();
    ASSERT_NE(info, nullptr);
    ASSERT_NE(info->unload, nullptr);
    ASSERT_NE(info->load, nullptr);

    IdleActivityProbe probe;
    EXPECT_NE(
        moduleActivityAddIdle(
            runIdleActivityProbe,
            &probe,
            destroyIdleActivityProbe),
        0u);
    EXPECT_TRUE(moduleActivityPending());
    EXPECT_FALSE(info->unload(nullptr));

    while (!probe.called &&
           g_main_context_iteration(
               g_main_context_default(), FALSE)) {
    }

    EXPECT_TRUE(probe.called);
    EXPECT_TRUE(probe.destroyed);
    EXPECT_FALSE(moduleActivityPending());
    EXPECT_TRUE(info->unload(nullptr));
    EXPECT_TRUE(info->load(nullptr));
}

TEST(PluginLifecycle, AccountThreadWithoutLiveClientCleansUp)
{
    loadedPluginInfo();

    PurpleAccount *account =
        purple_account_new("orphan-thread", "prpl-telegram");
    account->gc = nullptr;
    std::shared_ptr<OrphanAccountThread::Probe> probe(
        new OrphanAccountThread::Probe());

    (new OrphanAccountThread(account, probe))
        ->startThread();

    EXPECT_TRUE(probe->ran.load());
    EXPECT_FALSE(probe->calledBack.load());
    EXPECT_TRUE(probe->destroyed.load());
    EXPECT_FALSE(moduleActivityPending());
    purple_account_destroy(account);
}

TEST(PluginLifecycle, ThreadedAccountWorkAndFailureCleanUp)
{
    loadedPluginInfo();
    AccountThread::setSingleThread(false);
    RestoreSingleThreadMode restoreMode;

    for (bool throwInRun: {false, true}) {
        PurpleAccount *account =
            purple_account_new(
                throwInRun
                    ? "failing-orphan-worker"
                    : "orphan-worker",
                "prpl-telegram");
        account->gc = nullptr;
        std::shared_ptr<OrphanAccountThread::Probe> probe(
            new OrphanAccountThread::Probe());

        (new OrphanAccountThread(
             account,
             probe,
             throwInRun))
            ->startThread();

        const gint64 deadline =
            g_get_monotonic_time() + 5 * G_TIME_SPAN_SECOND;
        while ((!probe->destroyed.load() ||
                moduleActivityPending()) &&
               g_get_monotonic_time() < deadline) {
            while (g_main_context_iteration(
                       g_main_context_default(), FALSE)) {
            }
            std::this_thread::yield();
        }

        EXPECT_TRUE(probe->ran.load());
        EXPECT_FALSE(probe->calledBack.load());
        EXPECT_TRUE(probe->destroyed.load());
        EXPECT_FALSE(moduleActivityPending());
        purple_account_destroy(account);
    }
}
