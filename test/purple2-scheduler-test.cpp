#include "libpurple-mock.h"
#include "module-activity.h"
#include "purple2-scheduler.h"

#include <gtest/gtest.h>

#include <atomic>
#include <cstdlib>
#include <thread>
#include <vector>

namespace {

std::atomic<unsigned> g_ran(0);

gboolean countOne(gpointer)
{
    g_ran.fetch_add(1, std::memory_order_acq_rel);
    return FALSE;
}

void attachIdleFrom(GMainContext *context, bool onAnotherThread)
{
    const auto attach = [context]() {
        GSource *source = g_idle_source_new();
        g_source_set_callback(source, countOne, nullptr, nullptr);
        g_source_attach(source, context);
        g_main_context_wakeup(context);
        g_source_unref(source);
    };

    if (onAnotherThread) {
        std::thread worker(attach);
        worker.join();
    } else {
        attach();
    }
}

// How much the default GLib context can be persuaded to do. Under a user
// interface with its own event loop nobody iterates it at all; here it is
// iterated on purpose, to show that doing so does not run the plugin's work.
std::size_t drainDefaultContext()
{
    std::size_t iterations = 0;

    while (g_main_context_iteration(g_main_context_default(), FALSE))
        iterations++;

    return iterations;
}

class Purple2SchedulerTest : public testing::Test {
protected:
    void SetUp() override
    {
        unsetenv("TDLIB_PURPLE_SCHEDULER");
        resetPurpleEventLoop();
        g_ran.store(0, std::memory_order_release);
    }

    void TearDown() override
    {
        purple2SchedulerUninstall();
        resetPurpleEventLoop();
        unsetenv("TDLIB_PURPLE_SCHEDULER");
    }
};

} // namespace

// A user interface that supplies no event loop ops is one the plugin cannot
// schedule through, and libpurple would dereference the missing members rather
// than fall back, so this has to be a decline and not a crash.
TEST_F(Purple2SchedulerTest, DeclinesWithoutEventLoopUiOps)
{
    setPurpleEventLoopMode(PurpleEventLoopMode::None);

    EXPECT_FALSE(purple2SchedulerInstall());
    EXPECT_FALSE(purple2SchedulerInstalled());
    EXPECT_EQ(
        Purple2SchedulerDecision::NoEventLoopUiOps,
        purple2SchedulerLastDecision());
    EXPECT_EQ(nullptr, purple2SchedulerContext());
    EXPECT_EQ(0u, purpleEventLoopWatchCount());
}

TEST_F(Purple2SchedulerTest, DeclinesWhenTheEnvironmentAsksForGLib)
{
    setPurpleEventLoopMode(PurpleEventLoopMode::Recording);
    setenv("TDLIB_PURPLE_SCHEDULER", "glib", 1);

    EXPECT_FALSE(purple2SchedulerInstall());
    EXPECT_EQ(
        Purple2SchedulerDecision::DisabledByEnvironment,
        purple2SchedulerLastDecision());
    EXPECT_EQ(nullptr, purple2SchedulerContext());
}

TEST_F(Purple2SchedulerTest, InstallingWatchesTheContextThroughLibpurple)
{
    setPurpleEventLoopMode(PurpleEventLoopMode::Recording);

    ASSERT_TRUE(purple2SchedulerInstall());
    EXPECT_TRUE(purple2SchedulerInstalled());
    EXPECT_EQ(
        Purple2SchedulerDecision::Installed,
        purple2SchedulerLastDecision());
    ASSERT_NE(nullptr, purple2SchedulerContext());

    // One watch, on the context's own wakeup. The plugin's sources are idles
    // and timeouts, which bring no descriptors of their own.
    EXPECT_EQ(1u, purpleEventLoopWatchCount());
    EXPECT_TRUE(purple2SchedulerInstall());
    EXPECT_EQ(1u, purpleEventLoopWatchCount());
}

// The report in issue 26, as a test. A worker thread hands work to the context
// the plugin dispatches on. Nothing runs it, and iterating GLib's default
// context does not run it either, which is exactly what a user interface with
// its own event loop would find. It runs when, and only when, libpurple's event
// loop reports the watch.
TEST_F(Purple2SchedulerTest, WorkFromAWorkerThreadRunsWhenLibpurpleSaysSo)
{
    setPurpleEventLoopMode(PurpleEventLoopMode::Recording);
    ASSERT_TRUE(purple2SchedulerInstall());

    attachIdleFrom(purple2SchedulerContext(), true);

    EXPECT_EQ(0u, g_ran.load(std::memory_order_acquire));
    drainDefaultContext();
    EXPECT_EQ(0u, g_ran.load(std::memory_order_acquire));

    EXPECT_EQ(1u, purpleEventLoopRunReadyWatches());
    EXPECT_EQ(1u, g_ran.load(std::memory_order_acquire));
}

// The same for work scheduled by the thread libpurple itself runs on, which is
// where most of the plugin's own scheduling comes from. GLib does not signal a
// context's wakeup for an attach by its owner, so this only works because every
// attach site says so explicitly.
TEST_F(Purple2SchedulerTest, WorkFromTheLibpurpleThreadRunsToo)
{
    setPurpleEventLoopMode(PurpleEventLoopMode::Recording);
    ASSERT_TRUE(purple2SchedulerInstall());

    attachIdleFrom(purple2SchedulerContext(), false);

    EXPECT_EQ(0u, g_ran.load(std::memory_order_acquire));
    EXPECT_EQ(1u, purpleEventLoopRunReadyWatches());
    EXPECT_EQ(1u, g_ran.load(std::memory_order_acquire));
}

// Work put on by module-activity, which is how read receipts, the download
// wrap-up and the reauthorization watchdog are scheduled, has to land on the
// same context as everything else.
TEST_F(Purple2SchedulerTest, ModuleActivityWorkGoesToTheSameContext)
{
    setPurpleEventLoopMode(PurpleEventLoopMode::Recording);
    ASSERT_TRUE(purple2SchedulerInstall());

    ASSERT_NE(0u, moduleActivityAddIdle(countOne, nullptr));

    drainDefaultContext();
    EXPECT_EQ(0u, g_ran.load(std::memory_order_acquire));

    EXPECT_EQ(1u, purpleEventLoopRunReadyWatches());
    EXPECT_EQ(1u, g_ran.load(std::memory_order_acquire));
}

TEST_F(Purple2SchedulerTest, ModuleActivityWorkCanBeCancelledAgain)
{
    setPurpleEventLoopMode(PurpleEventLoopMode::Recording);
    ASSERT_TRUE(purple2SchedulerInstall());

    const guint id = moduleActivityAddTimeout(50, countOne, nullptr);
    ASSERT_NE(0u, id);

    EXPECT_TRUE(moduleActivityRemove(id));
    EXPECT_FALSE(moduleActivityRemove(id));

    purpleEventLoopRunReadyWatches();
    purpleEventLoopAdvance(100);
    EXPECT_EQ(0u, g_ran.load(std::memory_order_acquire));
}

// A timeout is a time, not a descriptor, so it has to reach libpurple as one.
TEST_F(Purple2SchedulerTest, ATimeoutIsHandedToLibpurpleAsATimeout)
{
    setPurpleEventLoopMode(PurpleEventLoopMode::Recording);
    ASSERT_TRUE(purple2SchedulerInstall());

    GSource *source = g_timeout_source_new(120);
    g_source_set_callback(source, countOne, nullptr, nullptr);
    g_source_attach(source, purple2SchedulerContext());
    g_main_context_wakeup(purple2SchedulerContext());
    g_source_unref(source);

    // The wakeup makes the pump replan, and the replan is what notices the
    // clock.
    ASSERT_EQ(1u, purpleEventLoopRunReadyWatches());
    ASSERT_EQ(1u, purpleEventLoopTimeoutCount());
    EXPECT_NEAR(120, purpleEventLoopShortestTimeout(), 5);
    EXPECT_EQ(0u, g_ran.load(std::memory_order_acquire));
}

// And when that timeout comes back, what was waiting on it runs. The clock in
// the fake event loop and the clock GLib reads are not the same one, so this
// waits for the real one rather than pretending.
TEST_F(Purple2SchedulerTest, WhatWaitsOnTheClockRunsWhenTheClockSaysSo)
{
    setPurpleEventLoopMode(PurpleEventLoopMode::Recording);
    ASSERT_TRUE(purple2SchedulerInstall());

    GSource *source = g_timeout_source_new(1);
    g_source_set_callback(source, countOne, nullptr, nullptr);
    g_source_attach(source, purple2SchedulerContext());
    g_main_context_wakeup(purple2SchedulerContext());
    g_source_unref(source);

    ASSERT_EQ(1u, purpleEventLoopRunReadyWatches());
    ASSERT_EQ(1u, purpleEventLoopTimeoutCount());
    EXPECT_EQ(0u, g_ran.load(std::memory_order_acquire));

    g_usleep(20 * 1000);
    purpleEventLoopAdvance(20);

    EXPECT_EQ(1u, g_ran.load(std::memory_order_acquire));
    EXPECT_EQ(0u, purpleEventLoopStaleRemovals());
}

// Some event loops compare the condition they were given rather than masking
// it, so a watch asking for read and write at once silently becomes a write
// watch and never fires.
TEST_F(Purple2SchedulerTest, NeverAsksForReadAndWriteInOneWatch)
{
    setPurpleEventLoopMode(PurpleEventLoopMode::Recording);
    ASSERT_TRUE(purple2SchedulerInstall());

    for (PurpleInputCondition condition : purpleEventLoopWatchConditions()) {
        EXPECT_TRUE(
            condition == PURPLE_INPUT_READ ||
            condition == PURPLE_INPUT_WRITE);
    }
}

// A watch that fires with nothing to do must leave nothing behind. Every event
// loop here is level triggered, so a descriptor left readable would be a busy
// loop in the user interface.
TEST_F(Purple2SchedulerTest, AWakeupWithNothingToDoSettlesAgain)
{
    setPurpleEventLoopMode(PurpleEventLoopMode::Recording);
    ASSERT_TRUE(purple2SchedulerInstall());

    g_main_context_wakeup(purple2SchedulerContext());

    EXPECT_EQ(1u, purpleEventLoopRunReadyWatches());
    EXPECT_EQ(0u, g_ran.load(std::memory_order_acquire));

    // Nothing was queued, so the descriptor has to be quiet now.
    EXPECT_EQ(0u, purpleEventLoopRunReadyWatches());
    EXPECT_EQ(1u, purpleEventLoopWatchCount());
}

TEST_F(Purple2SchedulerTest, EveryLibpurpleCallHappensOnTheInstallingThread)
{
    setPurpleEventLoopMode(PurpleEventLoopMode::Recording);
    ASSERT_TRUE(purple2SchedulerInstall());

    std::vector<std::thread> workers;
    for (unsigned i = 0; i < 8; i++) {
        workers.emplace_back([]() {
            attachIdleFrom(purple2SchedulerContext(), false);
        });
    }
    for (std::thread &worker : workers)
        worker.join();

    purpleEventLoopRunReadyWatches();

    EXPECT_EQ(8u, g_ran.load(std::memory_order_acquire));
    EXPECT_EQ(0u, purpleEventLoopThreadViolations());
    EXPECT_EQ(0u, purpleEventLoopStaleRemovals());
}

namespace {

gboolean uninstallFromInside(gpointer)
{
    purple2SchedulerUninstall();
    g_ran.fetch_add(1, std::memory_order_acq_rel);
    return FALSE;
}

} // namespace

// Unloading can be asked for by something the pump is running, and freeing the
// pump underneath its own cycle would be a use after free.
TEST_F(Purple2SchedulerTest, UninstallFromInsideACallbackWaitsItsTurn)
{
    setPurpleEventLoopMode(PurpleEventLoopMode::Recording);
    ASSERT_TRUE(purple2SchedulerInstall());

    GSource *source = g_idle_source_new();
    g_source_set_callback(source, uninstallFromInside, nullptr, nullptr);
    g_source_attach(source, purple2SchedulerContext());
    g_main_context_wakeup(purple2SchedulerContext());
    g_source_unref(source);

    purpleEventLoopRunReadyWatches();

    EXPECT_EQ(1u, g_ran.load(std::memory_order_acquire));
    EXPECT_FALSE(purple2SchedulerInstalled());
    EXPECT_EQ(0u, purpleEventLoopWatchCount());
    EXPECT_EQ(0u, purpleEventLoopTimeoutCount());
}

TEST_F(Purple2SchedulerTest, UninstallingGivesEverythingBack)
{
    setPurpleEventLoopMode(PurpleEventLoopMode::Recording);
    ASSERT_TRUE(purple2SchedulerInstall());
    ASSERT_EQ(1u, purpleEventLoopWatchCount());

    purple2SchedulerUninstall();

    EXPECT_FALSE(purple2SchedulerInstalled());
    EXPECT_EQ(nullptr, purple2SchedulerContext());
    EXPECT_EQ(0u, purpleEventLoopWatchCount());
    EXPECT_EQ(0u, purpleEventLoopTimeoutCount());
    EXPECT_EQ(0u, purpleEventLoopStaleRemovals());

    // And module-activity is back on the default context, which is where it
    // went before there was anything else.
    ASSERT_NE(0u, moduleActivityAddIdle(countOne, nullptr));
    drainDefaultContext();
    EXPECT_EQ(1u, g_ran.load(std::memory_order_acquire));
}
