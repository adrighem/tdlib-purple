#include "td-transport.h"

#include <glib.h>
#include <gtest/gtest.h>

#include <atomic>
#include <memory>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace {

using td::td_api::Object;
using td::td_api::error;
using td::td_api::getMe;
using td::td_api::make_object;
using td::td_api::object_ptr;
using td::td_api::ok;
using td::td_api::updateConnectionState;
using td::td_api::connectionStateReady;

class TdTransportTest : public testing::Test {
protected:
    TdTransportTest()
        : m_context(g_main_context_new())
    {
    }

    ~TdTransportTest() override
    {
        drainContext(m_context);
        g_main_context_unref(m_context);
    }

    std::unique_ptr<TdTransport> makeTransport(
        TdTransport::SendCallback sendCallback,
        TdTransport::UpdateCallback updateCallback = TdTransport::UpdateCallback())
    {
        g_main_context_push_thread_default(m_context);
        std::unique_ptr<TdTransport> transport(
            new TdTransport(std::move(sendCallback), std::move(updateCallback)));
        g_main_context_pop_thread_default(m_context);
        return transport;
    }

    static void drainContext(GMainContext *context)
    {
        while (g_main_context_iteration(context, FALSE)) {
        }
    }

    GMainContext *m_context;
};

TEST_F(TdTransportTest, RoutesOutOfOrderResponsesToTheirMatchingCallbacks)
{
    std::vector<uint64_t> sentIds;
    std::vector<uint64_t> callbackIds;
    std::unique_ptr<TdTransport> transport = makeTransport(
        [&](uint64_t requestId, object_ptr<td::td_api::Function>) {
            sentIds.push_back(requestId);
        });

    const uint64_t firstId = transport->send(
        make_object<getMe>(),
        [&](uint64_t requestId, object_ptr<Object>) {
            callbackIds.push_back(requestId);
        });
    const uint64_t secondId = transport->send(
        make_object<getMe>(),
        [&](uint64_t requestId, object_ptr<Object>) {
            callbackIds.push_back(requestId);
        });

    ASSERT_EQ((std::vector<uint64_t>{firstId, secondId}), sentIds);

    transport->receive(secondId, make_object<ok>());
    transport->receive(firstId, make_object<ok>());

    EXPECT_TRUE(callbackIds.empty());
    drainContext(m_context);
    EXPECT_EQ((std::vector<uint64_t>{secondId, firstId}), callbackIds);
}

TEST_F(TdTransportTest, DeliversUpdatesOnTheCapturedThreadDefaultContext)
{
    std::thread::id callbackThread;
    bool ownedCapturedContext = false;
    int updateId = 0;
    std::unique_ptr<TdTransport> transport = makeTransport(
        [](uint64_t, object_ptr<td::td_api::Function>) {},
        [&](object_ptr<Object> update) {
            callbackThread = std::this_thread::get_id();
            ownedCapturedContext =
                g_main_context_is_owner(m_context);
            updateId = update ? update->get_id() : 0;
        });
    const std::thread::id creatingThread = std::this_thread::get_id();

    std::thread worker([&]() {
        transport->receive(
            0,
            make_object<updateConnectionState>(
                make_object<connectionStateReady>()));
    });
    worker.join();

    EXPECT_EQ(0, updateId);
    g_main_context_iteration(nullptr, FALSE);
    EXPECT_EQ(0, updateId);

    drainContext(m_context);
    EXPECT_EQ(updateConnectionState::ID, updateId);
    EXPECT_EQ(creatingThread, callbackThread);
    EXPECT_TRUE(ownedCapturedContext);
}

TEST_F(TdTransportTest, KeepsUpdatesAndResponsesInReceiveOrder)
{
    std::vector<int> deliveryOrder;
    std::unique_ptr<TdTransport> transport = makeTransport(
        [](uint64_t, object_ptr<td::td_api::Function>) {},
        [&](object_ptr<Object>) {
            deliveryOrder.push_back(0);
        });
    const uint64_t requestId = transport->send(
        make_object<getMe>(),
        [&](uint64_t id, object_ptr<Object>) {
            deliveryOrder.push_back(static_cast<int>(id));
        });

    transport->receive(
        0,
        make_object<updateConnectionState>(
            make_object<connectionStateReady>()));
    transport->receive(requestId, make_object<ok>());

    drainContext(m_context);
    EXPECT_EQ((std::vector<int>{0, static_cast<int>(requestId)}),
              deliveryOrder);
}

TEST_F(TdTransportTest, SynchronousSenderResponseIsStillDeferred)
{
    TdTransport *transportPtr = nullptr;
    bool callbackCalled = false;
    std::unique_ptr<TdTransport> transport = makeTransport(
        [&](uint64_t requestId, object_ptr<td::td_api::Function>) {
            transportPtr->receive(requestId, make_object<ok>());
        });
    transportPtr = transport.get();

    transport->send(
        make_object<getMe>(),
        [&](uint64_t, object_ptr<Object>) {
            callbackCalled = true;
        });

    EXPECT_FALSE(callbackCalled);
    drainContext(m_context);
    EXPECT_TRUE(callbackCalled);
}

TEST(TdTransportGlobalContextTest, WorkerDeliveryIsNeverInvokedInline)
{
    std::atomic_bool workerReturned(false);
    std::atomic_bool callbackCalled(false);
    std::thread::id callbackThread;
    const std::thread::id creatingThread = std::this_thread::get_id();
    TdTransport transport(
        [](uint64_t, object_ptr<td::td_api::Function>) {},
        [&](object_ptr<Object>) {
            callbackThread = std::this_thread::get_id();
            callbackCalled = true;
        });

    std::thread worker([&]() {
        transport.receive(
            0,
            make_object<updateConnectionState>(
                make_object<connectionStateReady>()));
        workerReturned = true;
    });
    worker.join();

    EXPECT_TRUE(workerReturned);
    EXPECT_FALSE(callbackCalled);

    while (!callbackCalled &&
           g_main_context_iteration(g_main_context_default(), FALSE)) {
    }

    EXPECT_TRUE(callbackCalled);
    EXPECT_EQ(creatingThread, callbackThread);
}

TEST_F(TdTransportTest, IgnoresUnknownAndDuplicateResponses)
{
    unsigned callbackCount = 0;
    std::unique_ptr<TdTransport> transport = makeTransport(
        [](uint64_t, object_ptr<td::td_api::Function>) {});
    const uint64_t requestId = transport->send(
        make_object<getMe>(),
        [&](uint64_t, object_ptr<Object>) {
            ++callbackCount;
        });

    transport->receive(requestId + 100, make_object<error>(404, "unknown"));
    transport->receive(requestId, make_object<ok>());
    transport->receive(requestId, make_object<ok>());

    drainContext(m_context);
    EXPECT_EQ(1u, callbackCount);
}

TEST_F(TdTransportTest, ShutdownDropsQueuedResponses)
{
    bool callbackCalled = false;
    std::shared_ptr<int> capture = std::make_shared<int>(1);
    std::weak_ptr<int> weakCapture = capture;
    std::unique_ptr<TdTransport> transport = makeTransport(
        [](uint64_t, object_ptr<td::td_api::Function>) {});
    const uint64_t requestId = transport->send(
        make_object<getMe>(),
        [&, capture](uint64_t, object_ptr<Object>) {
            callbackCalled = true;
        });
    capture.reset();

    transport->receive(requestId, make_object<ok>());
    EXPECT_FALSE(weakCapture.expired());
    transport->shutdown();
    transport->shutdown();

    EXPECT_TRUE(weakCapture.expired());
    EXPECT_FALSE(g_main_context_pending(m_context));
    drainContext(m_context);
    EXPECT_FALSE(callbackCalled);
}

TEST_F(TdTransportTest, ShutdownReleasesPendingCallbackCaptures)
{
    std::shared_ptr<int> sendCapture = std::make_shared<int>(1);
    std::shared_ptr<int> updateCapture = std::make_shared<int>(2);
    std::shared_ptr<int> responseCapture = std::make_shared<int>(3);
    std::weak_ptr<int> weakSendCapture = sendCapture;
    std::weak_ptr<int> weakUpdateCapture = updateCapture;
    std::weak_ptr<int> weakResponseCapture = responseCapture;
    std::unique_ptr<TdTransport> transport = makeTransport(
        [sendCapture](uint64_t, object_ptr<td::td_api::Function>) {},
        [updateCapture](object_ptr<Object>) {});

    transport->send(
        make_object<getMe>(),
        [responseCapture](uint64_t, object_ptr<Object>) {});
    sendCapture.reset();
    updateCapture.reset();
    responseCapture.reset();

    EXPECT_FALSE(weakSendCapture.expired());
    EXPECT_FALSE(weakUpdateCapture.expired());
    EXPECT_FALSE(weakResponseCapture.expired());
    transport->shutdown();
    EXPECT_TRUE(weakSendCapture.expired());
    EXPECT_TRUE(weakUpdateCapture.expired());
    EXPECT_TRUE(weakResponseCapture.expired());
}

TEST_F(TdTransportTest, NullResponseDoesNotConsumeItsHandler)
{
    unsigned callbackCount = 0;
    std::unique_ptr<TdTransport> transport = makeTransport(
        [](uint64_t, object_ptr<td::td_api::Function>) {});
    const uint64_t requestId = transport->send(
        make_object<getMe>(),
        [&](uint64_t, object_ptr<Object>) {
            ++callbackCount;
        });

    transport->receive(requestId, nullptr);
    drainContext(m_context);
    EXPECT_EQ(0u, callbackCount);

    transport->receive(requestId, make_object<ok>());
    drainContext(m_context);
    EXPECT_EQ(1u, callbackCount);
}

TEST_F(TdTransportTest, CallbackMayDestroyTransportWithMoreWorkQueued)
{
    unsigned secondCallbackCount = 0;
    std::unique_ptr<TdTransport> transport = makeTransport(
        [](uint64_t, object_ptr<td::td_api::Function>) {});
    const uint64_t firstId = transport->send(
        make_object<getMe>(),
        [&](uint64_t, object_ptr<Object>) {
            transport.reset();
        });
    const uint64_t secondId = transport->send(
        make_object<getMe>(),
        [&](uint64_t, object_ptr<Object>) {
            ++secondCallbackCount;
        });

    transport->receive(firstId, make_object<ok>());
    transport->receive(secondId, make_object<ok>());

    drainContext(m_context);
    EXPECT_EQ(nullptr, transport);
    EXPECT_EQ(0u, secondCallbackCount);
}

TEST_F(TdTransportTest, CallbackExceptionsDoNotEscapeOrStopLaterDelivery)
{
    unsigned finalCallbackCount = 0;
    std::unique_ptr<TdTransport> transport = makeTransport(
        [](uint64_t, object_ptr<td::td_api::Function>) {},
        [](object_ptr<Object>) {
            throw std::runtime_error("synthetic update failure");
        });
    const uint64_t throwingId = transport->send(
        make_object<getMe>(),
        [](uint64_t, object_ptr<Object>) {
            throw std::runtime_error("synthetic response failure");
        });
    const uint64_t finalId = transport->send(
        make_object<getMe>(),
        [&](uint64_t, object_ptr<Object>) {
            ++finalCallbackCount;
        });

    transport->receive(
        0,
        make_object<updateConnectionState>(
            make_object<connectionStateReady>()));
    transport->receive(throwingId, make_object<ok>());
    transport->receive(finalId, make_object<ok>());

    EXPECT_NO_THROW(drainContext(m_context));
    EXPECT_EQ(1u, finalCallbackCount);
}

TEST_F(TdTransportTest, ConcurrentIngressCannotEscapeShutdown)
{
    for (unsigned iteration = 0; iteration < 100; ++iteration) {
        std::atomic_bool start(false);
        unsigned callbackCount = 0;
        std::unique_ptr<TdTransport> transport = makeTransport(
            [](uint64_t, object_ptr<td::td_api::Function>) {});
        const uint64_t requestId = transport->send(
            make_object<getMe>(),
            [&](uint64_t, object_ptr<Object>) {
                ++callbackCount;
            });

        std::thread worker([&]() {
            while (!start.load())
                std::this_thread::yield();
            transport->receive(requestId, make_object<ok>());
        });

        start = true;
        transport->shutdown();
        worker.join();
        drainContext(m_context);
        ASSERT_EQ(0u, callbackCount);
    }
}

TEST_F(TdTransportTest, StableReceiverIsSafeAfterTransportDestruction)
{
    bool callbackCalled = false;
    std::unique_ptr<TdTransport> transport = makeTransport(
        [](uint64_t, object_ptr<td::td_api::Function>) {});
    const uint64_t requestId = transport->send(
        make_object<getMe>(),
        [&](uint64_t, object_ptr<Object>) {
            callbackCalled = true;
        });
    TdTransport::ReceiveCallback receiver = transport->receiver();

    transport.reset();
    std::thread worker([&]() {
        receiver(requestId, make_object<ok>());
    });
    worker.join();

    drainContext(m_context);
    EXPECT_FALSE(callbackCalled);
}

TEST_F(TdTransportTest, BoundedDispatchContinuesAndCanBeRescheduled)
{
    const unsigned firstBurstSize = 65;
    unsigned callbackCount = 0;
    std::vector<uint64_t> requestIds;
    std::unique_ptr<TdTransport> transport = makeTransport(
        [](uint64_t, object_ptr<td::td_api::Function>) {});

    for (unsigned index = 0; index < firstBurstSize; ++index) {
        requestIds.push_back(transport->send(
            make_object<getMe>(),
            [&](uint64_t, object_ptr<Object>) {
                ++callbackCount;
            }));
    }
    for (uint64_t requestId: requestIds)
        transport->receive(requestId, make_object<ok>());

    ASSERT_TRUE(g_main_context_iteration(m_context, FALSE));
    EXPECT_EQ(32u, callbackCount);
    drainContext(m_context);
    EXPECT_EQ(firstBurstSize, callbackCount);
    EXPECT_FALSE(g_main_context_pending(m_context));

    const uint64_t finalId = transport->send(
        make_object<getMe>(),
        [&](uint64_t, object_ptr<Object>) {
            ++callbackCount;
        });
    transport->receive(finalId, make_object<ok>());
    drainContext(m_context);

    EXPECT_EQ(firstBurstSize + 1, callbackCount);
}

TEST_F(TdTransportTest, ConcurrentProducerAndContextDrainLoseNoDeliveries)
{
    const unsigned deliveryCount = 1000;
    unsigned callbackCount = 0;
    std::vector<uint64_t> requestIds;
    std::atomic_bool producerDone(false);
    std::unique_ptr<TdTransport> transport = makeTransport(
        [](uint64_t, object_ptr<td::td_api::Function>) {});

    for (unsigned index = 0; index < deliveryCount; ++index) {
        requestIds.push_back(transport->send(
            make_object<getMe>(),
            [&](uint64_t, object_ptr<Object>) {
                ++callbackCount;
            }));
    }

    TdTransport::ReceiveCallback receiver = transport->receiver();
    std::thread producer([&]() {
        for (uint64_t requestId: requestIds)
            receiver(requestId, make_object<ok>());
        producerDone = true;
    });

    while (!producerDone.load()) {
        g_main_context_iteration(m_context, FALSE);
        std::this_thread::yield();
    }
    producer.join();
    drainContext(m_context);

    EXPECT_EQ(deliveryCount, callbackCount);
}

} // namespace
