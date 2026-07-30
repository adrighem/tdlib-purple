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
        TdTransport::UpdateCallback updateCallback =
            TdTransport::UpdateCallback(),
        TdTransport::TimeoutSourceFactory timeoutSourceFactory =
            TdTransport::TimeoutSourceFactory())
    {
        g_main_context_push_thread_default(m_context);
        std::unique_ptr<TdTransport> transport(
            new TdTransport(
                std::move(sendCallback),
                std::move(updateCallback),
                std::move(timeoutSourceFactory)));
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

TdTransport::TimeoutSourceFactory immediateTimeoutSourceFactory()
{
    return [](unsigned) {
        return g_idle_source_new();
    };
}

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

TEST_F(TdTransportTest, QueuedResponseBeatsReadyTerminalTimeout)
{
    unsigned callbackCount = 0;
    bool receivedObject = false;
    std::unique_ptr<TdTransport> transport = makeTransport(
        [](uint64_t, object_ptr<td::td_api::Function>) {},
        TdTransport::UpdateCallback(),
        immediateTimeoutSourceFactory());
    const uint64_t requestId = transport->sendWithTimeout(
        make_object<getMe>(),
        [&](uint64_t, object_ptr<Object> object) {
            ++callbackCount;
            receivedObject = object != nullptr;
        },
        1);

    transport->receive(requestId, make_object<ok>());
    drainContext(m_context);

    EXPECT_EQ(1u, callbackCount);
    EXPECT_TRUE(receivedObject);
}

TEST_F(TdTransportTest, TerminalTimeoutDropsLateAndDuplicateResponses)
{
    std::vector<bool> receivedObjects;
    std::unique_ptr<TdTransport> transport = makeTransport(
        [](uint64_t, object_ptr<td::td_api::Function>) {},
        TdTransport::UpdateCallback(),
        immediateTimeoutSourceFactory());
    const uint64_t requestId = transport->sendWithTimeout(
        make_object<getMe>(),
        [&](uint64_t, object_ptr<Object> object) {
            receivedObjects.push_back(object != nullptr);
        },
        1);

    drainContext(m_context);
    transport->receive(requestId, make_object<ok>());
    transport->receive(requestId, make_object<ok>());
    drainContext(m_context);

    EXPECT_EQ((std::vector<bool>{false}), receivedObjects);
}

TEST_F(TdTransportTest, SynchronousResponseCancelsPreinstalledTimeout)
{
    TdTransport *transportPtr = nullptr;
    unsigned callbackCount = 0;
    bool receivedObject = false;
    std::unique_ptr<TdTransport> transport = makeTransport(
        [&](uint64_t requestId, object_ptr<td::td_api::Function>) {
            transportPtr->receive(requestId, make_object<ok>());
        },
        TdTransport::UpdateCallback(),
        immediateTimeoutSourceFactory());
    transportPtr = transport.get();

    transport->sendWithTimeout(
        make_object<getMe>(),
        [&](uint64_t, object_ptr<Object> object) {
            ++callbackCount;
            receivedObject = object != nullptr;
        },
        1);

    EXPECT_EQ(0u, callbackCount);
    drainContext(m_context);
    EXPECT_EQ(1u, callbackCount);
    EXPECT_TRUE(receivedObject);
}

TEST_F(TdTransportTest, TimeoutCannotFireBeforeBackendSendReturns)
{
    bool senderReturned = false;
    bool callbackCalled = false;
    bool callbackObservedSenderReturn = false;
    std::unique_ptr<TdTransport> transport = makeTransport(
        [&](uint64_t, object_ptr<td::td_api::Function>) {
            g_main_context_iteration(m_context, FALSE);
            senderReturned = true;
        },
        TdTransport::UpdateCallback(),
        immediateTimeoutSourceFactory());

    ASSERT_NE(
        0u,
        transport->sendWithTimeout(
            make_object<getMe>(),
            [&](uint64_t, object_ptr<Object>) {
                callbackCalled = true;
                callbackObservedSenderReturn = senderReturned;
            },
            1));

    EXPECT_FALSE(callbackCalled);
    drainContext(m_context);
    EXPECT_TRUE(callbackCalled);
    EXPECT_TRUE(callbackObservedSenderReturn);
}

TEST_F(TdTransportTest, NotificationTimeoutPreservesNormalResponse)
{
    unsigned timeoutCount = 0;
    unsigned responseCount = 0;
    std::unique_ptr<TdTransport> transport = makeTransport(
        [](uint64_t, object_ptr<td::td_api::Function>) {},
        TdTransport::UpdateCallback(),
        immediateTimeoutSourceFactory());
    const uint64_t requestId = transport->send(
        make_object<getMe>(),
        [&](uint64_t, object_ptr<Object>) {
            ++responseCount;
        });

    ASSERT_TRUE(transport->setNotificationTimeout(
        requestId, 1, [&](uint64_t) {
            ++timeoutCount;
        }));
    drainContext(m_context);
    EXPECT_EQ(1u, timeoutCount);
    EXPECT_EQ(0u, responseCount);

    transport->receive(requestId, make_object<ok>());
    transport->receive(requestId, make_object<ok>());
    drainContext(m_context);
    EXPECT_EQ(1u, timeoutCount);
    EXPECT_EQ(1u, responseCount);
}

TEST_F(TdTransportTest, ResponseCancelsNotificationAndReleasesCapture)
{
    std::shared_ptr<int> capture = std::make_shared<int>(1);
    std::weak_ptr<int> weakCapture = capture;
    unsigned responseCount = 0;
    std::unique_ptr<TdTransport> transport = makeTransport(
        [](uint64_t, object_ptr<td::td_api::Function>) {},
        TdTransport::UpdateCallback(),
        immediateTimeoutSourceFactory());
    const uint64_t requestId = transport->send(
        make_object<getMe>(),
        [&](uint64_t, object_ptr<Object>) {
            ++responseCount;
        });

    ASSERT_TRUE(transport->setNotificationTimeout(
        requestId, 1, [capture](uint64_t) {}));
    capture.reset();
    EXPECT_FALSE(weakCapture.expired());

    transport->receive(requestId, make_object<ok>());
    EXPECT_FALSE(weakCapture.expired());
    drainContext(m_context);
    EXPECT_TRUE(weakCapture.expired());
    EXPECT_EQ(1u, responseCount);
}

TEST_F(TdTransportTest, NullResponseDoesNotBeatRealResponseOrTimeout)
{
    unsigned callbackCount = 0;
    bool receivedObject = false;
    std::unique_ptr<TdTransport> transport = makeTransport(
        [](uint64_t, object_ptr<td::td_api::Function>) {},
        TdTransport::UpdateCallback(),
        immediateTimeoutSourceFactory());
    const uint64_t requestId = transport->sendWithTimeout(
        make_object<getMe>(),
        [&](uint64_t, object_ptr<Object> object) {
            ++callbackCount;
            receivedObject = object != nullptr;
        },
        1);

    transport->receive(requestId, nullptr);
    transport->receive(requestId, make_object<ok>());
    drainContext(m_context);
    EXPECT_EQ(1u, callbackCount);
    EXPECT_TRUE(receivedObject);
}

TEST_F(TdTransportTest, NullResponseLeavesTerminalTimeoutActive)
{
    unsigned callbackCount = 0;
    bool receivedObject = true;
    std::unique_ptr<TdTransport> transport = makeTransport(
        [](uint64_t, object_ptr<td::td_api::Function>) {},
        TdTransport::UpdateCallback(),
        immediateTimeoutSourceFactory());
    const uint64_t requestId = transport->sendWithTimeout(
        make_object<getMe>(),
        [&](uint64_t, object_ptr<Object> object) {
            ++callbackCount;
            receivedObject = object != nullptr;
        },
        1);

    transport->receive(requestId, nullptr);
    drainContext(m_context);
    EXPECT_EQ(1u, callbackCount);
    EXPECT_FALSE(receivedObject);
}

TEST_F(TdTransportTest, TerminalTimeoutCallbackMayDestroyTransport)
{
    std::unique_ptr<TdTransport> transport = makeTransport(
        [](uint64_t, object_ptr<td::td_api::Function>) {},
        TdTransport::UpdateCallback(),
        immediateTimeoutSourceFactory());
    transport->sendWithTimeout(
        make_object<getMe>(),
        [&](uint64_t, object_ptr<Object>) {
            transport.reset();
        },
        1);

    drainContext(m_context);
    EXPECT_EQ(nullptr, transport);
}

TEST_F(TdTransportTest, NotificationTimeoutCallbackMayDestroyTransport)
{
    std::unique_ptr<TdTransport> transport = makeTransport(
        [](uint64_t, object_ptr<td::td_api::Function>) {},
        TdTransport::UpdateCallback(),
        immediateTimeoutSourceFactory());
    const uint64_t requestId = transport->send(
        make_object<getMe>(),
        [](uint64_t, object_ptr<Object>) {});
    ASSERT_TRUE(transport->setNotificationTimeout(
        requestId, 1, [&](uint64_t) {
            transport.reset();
        }));

    drainContext(m_context);
    EXPECT_EQ(nullptr, transport);
}

TEST_F(TdTransportTest, ShutdownCancelsTimeoutAndReleasesCaptures)
{
    std::shared_ptr<int> responseCapture = std::make_shared<int>(1);
    std::shared_ptr<int> factoryCapture = std::make_shared<int>(2);
    std::weak_ptr<int> weakResponseCapture = responseCapture;
    std::weak_ptr<int> weakFactoryCapture = factoryCapture;
    TdTransport::TimeoutSourceFactory factory =
        [factoryCapture](unsigned) {
            return g_idle_source_new();
        };
    std::unique_ptr<TdTransport> transport = makeTransport(
        [](uint64_t, object_ptr<td::td_api::Function>) {},
        TdTransport::UpdateCallback(),
        std::move(factory));
    transport->sendWithTimeout(
        make_object<getMe>(),
        [responseCapture](uint64_t, object_ptr<Object>) {},
        1);
    responseCapture.reset();
    factoryCapture.reset();

    EXPECT_FALSE(weakResponseCapture.expired());
    EXPECT_FALSE(weakFactoryCapture.expired());
    transport->shutdown();
    transport->shutdown();

    EXPECT_TRUE(weakResponseCapture.expired());
    EXPECT_TRUE(weakFactoryCapture.expired());
    EXPECT_FALSE(g_main_context_pending(m_context));
}

TEST_F(TdTransportTest, RejectsDuplicateAndCompletedRequestTimers)
{
    std::shared_ptr<int> rejectedCapture = std::make_shared<int>(1);
    std::weak_ptr<int> weakRejectedCapture = rejectedCapture;
    std::unique_ptr<TdTransport> transport = makeTransport(
        [](uint64_t, object_ptr<td::td_api::Function>) {},
        TdTransport::UpdateCallback(),
        immediateTimeoutSourceFactory());
    const uint64_t requestId = transport->send(
        make_object<getMe>(),
        [](uint64_t, object_ptr<Object>) {});

    ASSERT_TRUE(transport->setNotificationTimeout(
        requestId, 1, [](uint64_t) {}));
    EXPECT_FALSE(transport->setNotificationTimeout(
        requestId, 1, [rejectedCapture](uint64_t) {}));
    rejectedCapture.reset();
    EXPECT_TRUE(weakRejectedCapture.expired());

    transport->receive(requestId, make_object<ok>());
    drainContext(m_context);
    EXPECT_FALSE(transport->setResponseTimeout(requestId, 1));
    EXPECT_FALSE(transport->setResponseTimeout(requestId + 100, 1));
}

TEST_F(TdTransportTest, TimeoutUsesCapturedContext)
{
    bool callbackCalled = false;
    bool ownedCapturedContext = false;
    std::unique_ptr<TdTransport> transport = makeTransport(
        [](uint64_t, object_ptr<td::td_api::Function>) {});
    transport->sendWithTimeout(
        make_object<getMe>(),
        [&](uint64_t, object_ptr<Object>) {
            callbackCalled = true;
            ownedCapturedContext =
                g_main_context_is_owner(m_context);
        },
        0);

    g_main_context_iteration(nullptr, FALSE);
    EXPECT_FALSE(callbackCalled);
    drainContext(m_context);
    EXPECT_TRUE(callbackCalled);
    EXPECT_TRUE(ownedCapturedContext);
}

TEST_F(TdTransportTest, ThrowingTimeoutDoesNotStopLaterTimeout)
{
    unsigned finalTimeoutCount = 0;
    std::unique_ptr<TdTransport> transport = makeTransport(
        [](uint64_t, object_ptr<td::td_api::Function>) {},
        TdTransport::UpdateCallback(),
        immediateTimeoutSourceFactory());
    const uint64_t firstId = transport->send(
        make_object<getMe>(),
        [](uint64_t, object_ptr<Object>) {});
    const uint64_t secondId = transport->send(
        make_object<getMe>(),
        [](uint64_t, object_ptr<Object>) {});

    ASSERT_TRUE(transport->setNotificationTimeout(
        firstId, 1, [](uint64_t) {
            throw std::runtime_error("synthetic timeout failure");
        }));
    ASSERT_TRUE(transport->setNotificationTimeout(
        secondId, 1, [&](uint64_t) {
            ++finalTimeoutCount;
        }));

    EXPECT_NO_THROW(drainContext(m_context));
    EXPECT_EQ(1u, finalTimeoutCount);
}

TEST_F(TdTransportTest, ExistingRequestCanReceiveTerminalTimeout)
{
    unsigned callbackCount = 0;
    bool receivedObject = true;
    std::unique_ptr<TdTransport> transport = makeTransport(
        [](uint64_t, object_ptr<td::td_api::Function>) {},
        TdTransport::UpdateCallback(),
        immediateTimeoutSourceFactory());
    const uint64_t requestId = transport->send(
        make_object<getMe>(),
        [&](uint64_t, object_ptr<Object> object) {
            ++callbackCount;
            receivedObject = object != nullptr;
        });

    ASSERT_TRUE(transport->setResponseTimeout(requestId, 1));
    drainContext(m_context);
    EXPECT_EQ(1u, callbackCount);
    EXPECT_FALSE(receivedObject);
}

TEST_F(TdTransportTest, TerminalTimeoutCanOverrideResponseCallback)
{
    unsigned responseCount = 0;
    unsigned timeoutCount = 0;
    std::unique_ptr<TdTransport> transport = makeTransport(
        [](uint64_t, object_ptr<td::td_api::Function>) {},
        TdTransport::UpdateCallback(),
        immediateTimeoutSourceFactory());
    const uint64_t requestId = transport->send(
        make_object<getMe>(),
        [&](uint64_t, object_ptr<Object>) {
            ++responseCount;
        });

    ASSERT_TRUE(transport->setResponseTimeout(
        requestId,
        1,
        [&](uint64_t, object_ptr<Object> object) {
            EXPECT_EQ(nullptr, object);
            ++timeoutCount;
        }));
    drainContext(m_context);
    transport->receive(requestId, make_object<ok>());
    drainContext(m_context);

    EXPECT_EQ(0u, responseCount);
    EXPECT_EQ(1u, timeoutCount);
}

TEST_F(TdTransportTest, FailedTimeoutSourcePreventsSend)
{
    unsigned sendCount = 0;
    std::shared_ptr<int> capture = std::make_shared<int>(1);
    std::weak_ptr<int> weakCapture = capture;
    std::unique_ptr<TdTransport> transport = makeTransport(
        [&](uint64_t, object_ptr<td::td_api::Function>) {
            ++sendCount;
        },
        TdTransport::UpdateCallback(),
        [](unsigned) -> GSource * {
            return nullptr;
        });

    const uint64_t requestId = transport->sendWithTimeout(
        make_object<getMe>(),
        [capture](uint64_t, object_ptr<Object>) {},
        1);
    capture.reset();

    EXPECT_EQ(0u, requestId);
    EXPECT_EQ(0u, sendCount);
    EXPECT_TRUE(weakCapture.expired());
}

TEST_F(TdTransportTest, ThrowingSenderRollsBackPreinstalledTimeout)
{
    unsigned callbackCount = 0;
    std::shared_ptr<int> capture = std::make_shared<int>(1);
    std::weak_ptr<int> weakCapture = capture;
    std::unique_ptr<TdTransport> transport = makeTransport(
        [](uint64_t, object_ptr<td::td_api::Function>) {
            throw std::runtime_error("synthetic send failure");
        },
        TdTransport::UpdateCallback(),
        immediateTimeoutSourceFactory());

    const uint64_t requestId = transport->sendWithTimeout(
        make_object<getMe>(),
        [&, capture](uint64_t, object_ptr<Object>) {
            ++callbackCount;
        },
        1);
    capture.reset();

    EXPECT_EQ(0u, requestId);
    EXPECT_TRUE(weakCapture.expired());
    drainContext(m_context);
    EXPECT_EQ(0u, callbackCount);
    EXPECT_FALSE(g_main_context_pending(m_context));
}

TEST_F(TdTransportTest, ResponseAndTimeoutRaceInvokesExactlyOnce)
{
    const unsigned iterationCount = 500;
    unsigned callbackCount = 0;
    std::unique_ptr<TdTransport> transport = makeTransport(
        [](uint64_t, object_ptr<td::td_api::Function>) {},
        TdTransport::UpdateCallback(),
        immediateTimeoutSourceFactory());

    for (unsigned iteration = 0;
         iteration < iterationCount;
         ++iteration) {
        const uint64_t requestId = transport->sendWithTimeout(
            make_object<getMe>(),
            [&](uint64_t, object_ptr<Object>) {
                ++callbackCount;
            },
            1);
        ASSERT_NE(0u, requestId);

        std::atomic_bool start(false);
        std::thread responder([&]() {
            while (!start.load())
                std::this_thread::yield();
            transport->receive(requestId, make_object<ok>());
        });
        start = true;
        g_main_context_iteration(m_context, FALSE);
        responder.join();
        drainContext(m_context);

        ASSERT_EQ(iteration + 1, callbackCount);
    }
}

TEST_F(TdTransportTest, RejectsInvalidAndThrowingTimeoutSources)
{
    GSource *attachedSource = nullptr;
    std::unique_ptr<TdTransport> attachedTransport = makeTransport(
        [](uint64_t, object_ptr<td::td_api::Function>) {},
        TdTransport::UpdateCallback(),
        [&](unsigned) {
            attachedSource = g_idle_source_new();
            g_source_attach(attachedSource, m_context);
            return attachedSource;
        });
    EXPECT_EQ(
        0u,
        attachedTransport->sendWithTimeout(
            make_object<getMe>(),
            [](uint64_t, object_ptr<Object>) {},
            1));
    ASSERT_NE(nullptr, attachedSource);
    g_source_destroy(attachedSource);

    std::unique_ptr<TdTransport> destroyedTransport = makeTransport(
        [](uint64_t, object_ptr<td::td_api::Function>) {},
        TdTransport::UpdateCallback(),
        [](unsigned) {
            GSource *source = g_idle_source_new();
            g_source_destroy(source);
            return source;
        });
    EXPECT_EQ(
        0u,
        destroyedTransport->sendWithTimeout(
            make_object<getMe>(),
            [](uint64_t, object_ptr<Object>) {},
            1));

    std::unique_ptr<TdTransport> throwingTransport = makeTransport(
        [](uint64_t, object_ptr<td::td_api::Function>) {},
        TdTransport::UpdateCallback(),
        [](unsigned) -> GSource * {
            throw std::runtime_error("synthetic factory failure");
        });
    EXPECT_EQ(
        0u,
        throwingTransport->sendWithTimeout(
            make_object<getMe>(),
            [](uint64_t, object_ptr<Object>) {},
            1));
}

TEST_F(TdTransportTest, PassesTimeoutIntervalToFactoryUnchanged)
{
    unsigned observedTimeout = 0;
    std::unique_ptr<TdTransport> transport = makeTransport(
        [](uint64_t, object_ptr<td::td_api::Function>) {},
        TdTransport::UpdateCallback(),
        [&](unsigned timeoutSeconds) {
            observedTimeout = timeoutSeconds;
            return g_idle_source_new();
        });

    ASSERT_NE(
        0u,
        transport->sendWithTimeout(
            make_object<getMe>(),
            [](uint64_t, object_ptr<Object>) {},
            37));
    EXPECT_EQ(37u, observedTimeout);
}

} // namespace
