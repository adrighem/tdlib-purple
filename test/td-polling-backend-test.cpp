#include "td-polling-backend.h"
#include "td-request-id.h"

#include <gtest/gtest.h>

#include <td/telegram/td_api.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace td::td_api;

constexpr unsigned CLOSE_TIMEOUT_SECONDS = 7;
constexpr double POLL_TIMEOUT_SECONDS = 0.125;

struct ClientControl {
    struct SentRequest {
        std::uint64_t requestId;
        TdPollingClient::FunctionPtr function;
    };

    void recordSend(
        std::uint64_t requestId,
        TdPollingClient::FunctionPtr function)
    {
        std::unique_lock<std::mutex> lock(mutex);
        if (sendBlocked) {
            sendEntered = true;
            condition.notify_all();
            condition.wait(lock, [this]() {
                return !sendBlocked;
            });
        }
        if (throwOnSend)
            throw std::runtime_error("synthetic send failure");
        sent.push_back({requestId, std::move(function)});
        condition.notify_all();
    }

    TdPollingClient::Response receive(double timeoutSeconds)
    {
        std::unique_lock<std::mutex> lock(mutex);
        ++receiveCalls;
        lastPollTimeoutSeconds = timeoutSeconds;
        condition.notify_all();
        condition.wait(lock, [this]() {
            return throwOnReceive || !responses.empty();
        });

        if (throwOnReceive) {
            throwOnReceive = false;
            throw std::runtime_error("synthetic receive failure");
        }

        TdPollingClient::Response response =
            std::move(responses.front());
        responses.pop_front();
        return response;
    }

    void push(TdPollingClient::Response response)
    {
        std::lock_guard<std::mutex> lock(mutex);
        responses.push_back(std::move(response));
        condition.notify_all();
    }

    bool waitForSent(std::size_t count)
    {
        std::unique_lock<std::mutex> lock(mutex);
        return condition.wait_for(
            lock, std::chrono::seconds(5),
            [this, count]() { return sent.size() >= count; });
    }

    bool waitForReceiveCalls(unsigned count)
    {
        std::unique_lock<std::mutex> lock(mutex);
        return condition.wait_for(
            lock, std::chrono::seconds(5),
            [this, count]() { return receiveCalls >= count; });
    }

    bool waitForSendEntered()
    {
        std::unique_lock<std::mutex> lock(mutex);
        return condition.wait_for(
            lock, std::chrono::seconds(5),
            [this]() { return sendEntered; });
    }

    bool waitForDestroyed()
    {
        std::unique_lock<std::mutex> lock(mutex);
        return condition.wait_for(
            lock, std::chrono::seconds(5),
            [this]() { return destroyed; });
    }

    bool waitForDestructionEntered()
    {
        std::unique_lock<std::mutex> lock(mutex);
        return condition.wait_for(
            lock, std::chrono::seconds(5),
            [this]() { return destructionEntered; });
    }

    bool isDestroyed() const
    {
        std::lock_guard<std::mutex> lock(mutex);
        return destroyed;
    }

    void failNextReceive()
    {
        std::lock_guard<std::mutex> lock(mutex);
        throwOnReceive = true;
        condition.notify_all();
    }

    void blockDestruction()
    {
        std::lock_guard<std::mutex> lock(mutex);
        destructionBlocked = true;
    }

    void blockSend()
    {
        std::lock_guard<std::mutex> lock(mutex);
        sendEntered = false;
        sendBlocked = true;
    }

    void releaseSend()
    {
        std::lock_guard<std::mutex> lock(mutex);
        sendBlocked = false;
        condition.notify_all();
    }

    void releaseDestruction()
    {
        std::lock_guard<std::mutex> lock(mutex);
        destructionBlocked = false;
        condition.notify_all();
    }

    void markDestroyed()
    {
        std::unique_lock<std::mutex> lock(mutex);
        destructionEntered = true;
        condition.notify_all();
        condition.wait(lock, [this]() {
            return !destructionBlocked;
        });
        destroyed = true;
        condition.notify_all();
    }

    mutable std::mutex mutex;
    std::condition_variable condition;
    std::vector<SentRequest> sent;
    std::deque<TdPollingClient::Response> responses;
    unsigned receiveCalls = 0;
    double lastPollTimeoutSeconds = -1.0;
    bool throwOnSend = false;
    bool throwOnReceive = false;
    bool sendBlocked = false;
    bool sendEntered = false;
    bool destructionBlocked = false;
    bool destructionEntered = false;
    bool destroyed = false;
};

class GatedClient final : public TdPollingClient {
public:
    explicit GatedClient(std::shared_ptr<ClientControl> control)
        : m_control(std::move(control))
    {
    }

    ~GatedClient() override
    {
        m_control->markDestroyed();
    }

    void send(
        std::uint64_t requestId, FunctionPtr function) override
    {
        m_control->recordSend(requestId, std::move(function));
    }

    Response receive(double timeoutSeconds) override
    {
        return m_control->receive(timeoutSeconds);
    }

private:
    std::shared_ptr<ClientControl> m_control;
};

class ManualDeadlineControl {
public:
    ~ManualDeadlineControl()
    {
        std::vector<GSource *> sources;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            sources.swap(m_sources);
        }
        for (GSource *source: sources)
            g_source_unref(source);
    }

    GSource *create(unsigned timeoutSeconds)
    {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_intervals.push_back(timeoutSeconds);
        }

        GSource *source = g_source_new(
            &sourceFunctions(), sizeof(ManualSource));
        reinterpret_cast<ManualSource *>(source)->ready = FALSE;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_sources.push_back(g_source_ref(source));
        }
        return source;
    }

    bool fireNext()
    {
        GSource *source = nullptr;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_sources.empty())
                return false;
            source = m_sources.front();
            m_sources.erase(m_sources.begin());
        }

        if (!g_source_is_destroyed(source)) {
            reinterpret_cast<ManualSource *>(source)->ready = TRUE;
            GMainContext *context = g_source_get_context(source);
            if (context)
                g_main_context_wakeup(context);
        }
        g_source_unref(source);
        return true;
    }

    std::vector<unsigned> intervals() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_intervals;
    }

private:
    struct ManualSource {
        GSource source;
        gboolean ready;
    };

    static gboolean prepare(GSource *source, gint *timeout)
    {
        *timeout = -1;
        return reinterpret_cast<ManualSource *>(source)->ready;
    }

    static gboolean check(GSource *source)
    {
        return reinterpret_cast<ManualSource *>(source)->ready;
    }

    static gboolean dispatch(
        GSource *,
        GSourceFunc callback,
        gpointer userData)
    {
        return callback ? callback(userData) : FALSE;
    }

    static GSourceFuncs &sourceFunctions()
    {
        static GSourceFuncs functions = {
            prepare,
            check,
            dispatch,
            nullptr,
            nullptr,
            nullptr};
        return functions;
    }

    mutable std::mutex m_mutex;
    std::vector<GSource *> m_sources;
    std::vector<unsigned> m_intervals;
};

class BlockingFailureSourceFactory {
public:
    GSource *create()
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_entered = true;
        m_condition.notify_all();
        m_condition.wait(lock, [this]() {
            return m_released;
        });
        return nullptr;
    }

    bool waitUntilEntered()
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        return m_condition.wait_for(
            lock,
            std::chrono::seconds(5),
            [this]() { return m_entered; });
    }

    void release()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_released = true;
        m_condition.notify_all();
    }

private:
    std::mutex m_mutex;
    std::condition_variable m_condition;
    bool m_entered = false;
    bool m_released = false;
};

class BlockingFinalizerAttacher {
public:
    guint attach(GSource *source, GMainContext *context)
    {
        const guint sourceId = g_source_attach(source, context);
        std::unique_lock<std::mutex> lock(m_mutex);
        m_entered = true;
        m_condition.notify_all();
        m_condition.wait(lock, [this]() {
            return m_released;
        });
        return sourceId;
    }

    bool waitUntilEntered()
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        return m_condition.wait_for(
            lock,
            std::chrono::seconds(5),
            [this]() { return m_entered; });
    }

    void release()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_released = true;
        m_condition.notify_all();
    }

private:
    std::mutex m_mutex;
    std::condition_variable m_condition;
    bool m_entered = false;
    bool m_released = false;
};

class ReadySignal {
public:
    void signal()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_ready = true;
        m_condition.notify_all();
    }

    bool wait()
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        return m_condition.wait_for(
            lock,
            std::chrono::seconds(5),
            [this]() { return m_ready; });
    }

private:
    std::mutex m_mutex;
    std::condition_variable m_condition;
    bool m_ready = false;
};

class DeliveryReceiptGate {
public:
    ~DeliveryReceiptGate()
    {
        settle(false);
    }

    void hold(TdTransport::DeliveryReceipt receipt)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_receipt.reset(
            new TdTransport::DeliveryReceipt(
                std::move(receipt)));
        m_condition.notify_all();
    }

    bool waitUntilHeld()
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        return m_condition.wait_for(
            lock,
            std::chrono::seconds(5),
            [this]() { return m_receipt != nullptr; });
    }

    bool settle(bool delivered)
    {
        std::unique_ptr<TdTransport::DeliveryReceipt> receipt;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            receipt.swap(m_receipt);
        }
        if (!receipt)
            return false;
        receipt->settle(delivered);
        return true;
    }

private:
    std::mutex m_mutex;
    std::condition_variable m_condition;
    std::unique_ptr<TdTransport::DeliveryReceipt> m_receipt;
};

TdPollingClient::Response makeClosed()
{
    return {
        0,
        make_object<updateAuthorizationState>(
            make_object<authorizationStateClosed>())};
}

class ReentrantCleanupProbe {
public:
    ReentrantCleanupProbe(
        std::atomic<bool> *destroyed,
        std::atomic<bool> *guarded)
        : m_destroyed(destroyed), m_guarded(guarded)
    {
    }

    ~ReentrantCleanupProbe()
    {
        *m_guarded = TdPollingBackend::hasActiveWorkers();
        *m_destroyed = true;
    }

private:
    std::atomic<bool> *m_destroyed;
    std::atomic<bool> *m_guarded;
};

class TdPollingBackendTest : public testing::Test {
protected:
    void SetUp() override
    {
        m_context = g_main_context_new();
        g_main_context_push_thread_default(m_context);
    }

    void TearDown() override
    {
        for (const auto &client: m_clients) {
            if (client) {
                client->releaseSend();
                client->releaseDestruction();
            }
            if (client && !client->isDestroyed())
                client->failNextReceive();
        }
        iterateUntil([]() {
            return !TdPollingBackend::hasActiveWorkers();
        });
        EXPECT_FALSE(TdPollingBackend::hasActiveWorkers());
        g_main_context_pop_thread_default(m_context);
        g_main_context_unref(m_context);
    }

    std::unique_ptr<TdPollingBackend> makeBackend(
        const std::string &sessionKey,
        const std::shared_ptr<ClientControl> &client,
        const std::shared_ptr<ManualDeadlineControl> &deadline)
    {
        m_clients.push_back(client);
        return std::unique_ptr<TdPollingBackend>(
            new TdPollingBackend(
                sessionKey,
                CLOSE_TIMEOUT_SECONDS,
                POLL_TIMEOUT_SECONDS,
                [client]() {
                    return std::unique_ptr<TdPollingClient>(
                        new GatedClient(client));
                },
                [deadline](unsigned seconds) {
                    return deadline->create(seconds);
                }));
    }

    void iterateUntil(const std::function<bool()> &predicate)
    {
        const gint64 deadline =
            g_get_monotonic_time() + 5 * G_TIME_SPAN_SECOND;
        while (!predicate() &&
               g_get_monotonic_time() < deadline) {
            while (g_main_context_iteration(m_context, FALSE)) {
            }
            std::this_thread::yield();
        }
        EXPECT_TRUE(predicate());
    }

    static void drainContext(GMainContext *context)
    {
        while (g_main_context_iteration(context, FALSE)) {
        }
    }

    void finishCleanly(
        TdPollingBackend &backend,
        const std::shared_ptr<ClientControl> &client,
        std::vector<TdPollingBackend::CloseResult> *results = nullptr)
    {
        std::atomic<bool> callbackCalled(false);
        backend.close([&](TdPollingBackend::CloseResult result) {
            if (results)
                results->push_back(result);
            callbackCalled = true;
        });
        ASSERT_TRUE(client->waitForSent(2));
        client->push(
            {td_request_id::CLOSE, make_object<ok>()});
        ASSERT_TRUE(client->waitForReceiveCalls(2));
        client->push(makeClosed());
        ASSERT_TRUE(client->waitForDestroyed());
        iterateUntil([&]() { return callbackCalled.load(); });
    }

    GMainContext *m_context = nullptr;
    std::vector<std::shared_ptr<ClientControl>> m_clients;
};

TEST(TdRequestIdTest, SeparatesPublicAndControlRequestIds)
{
    EXPECT_TRUE(td_request_id::isPublic(1));
    EXPECT_TRUE(td_request_id::isPublic(td_request_id::PUBLIC_MAX));
    EXPECT_FALSE(td_request_id::isPublic(0));
    EXPECT_FALSE(td_request_id::isPublic(td_request_id::ACTIVATE));
    EXPECT_FALSE(td_request_id::isPublic(td_request_id::CLOSE));
    EXPECT_TRUE(td_request_id::isControl(td_request_id::ACTIVATE));
    EXPECT_TRUE(td_request_id::isControl(td_request_id::CLOSE));
    EXPECT_NE(td_request_id::ACTIVATE, td_request_id::CLOSE);
    EXPECT_EQ(1u, td_request_id::nextPublic(0));
    EXPECT_EQ(
        td_request_id::PUBLIC_MAX,
        td_request_id::nextPublic(
            td_request_id::PUBLIC_MAX - 1));
    EXPECT_EQ(
        1u,
        td_request_id::nextPublic(td_request_id::PUBLIC_MAX));
}

TEST_F(TdPollingBackendTest, RejectsAnEmptySessionKey)
{
    std::shared_ptr<ClientControl> client(new ClientControl());
    std::shared_ptr<ManualDeadlineControl> deadline(
        new ManualDeadlineControl());

    EXPECT_THROW(
        TdPollingBackend(
            "",
            CLOSE_TIMEOUT_SECONDS,
            POLL_TIMEOUT_SECONDS,
            [client]() {
                return std::unique_ptr<TdPollingClient>(
                    new GatedClient(client));
            },
            [deadline](unsigned seconds) {
                return deadline->create(seconds);
            }),
        std::invalid_argument);
}

TEST_F(TdPollingBackendTest, RejectsInvalidPollTimeouts)
{
    std::shared_ptr<ClientControl> client(new ClientControl());
    auto construct = [&](double pollTimeoutSeconds) {
        return std::unique_ptr<TdPollingBackend>(
            new TdPollingBackend(
                "invalid-poll-timeout",
                CLOSE_TIMEOUT_SECONDS,
                pollTimeoutSeconds,
                [client]() {
                    return std::unique_ptr<TdPollingClient>(
                        new GatedClient(client));
                }));
    };

    EXPECT_THROW(construct(0.0), std::invalid_argument);
    EXPECT_THROW(construct(-0.1), std::invalid_argument);
    EXPECT_THROW(
        construct(std::numeric_limits<double>::infinity()),
        std::invalid_argument);
    EXPECT_THROW(
        construct(std::numeric_limits<double>::quiet_NaN()),
        std::invalid_argument);
}

TEST_F(TdPollingBackendTest, ActivatesWithOfficialVersionRequest)
{
    std::shared_ptr<ClientControl> client(new ClientControl());
    std::shared_ptr<ManualDeadlineControl> deadline(
        new ManualDeadlineControl());
    std::unique_ptr<TdPollingBackend> backend =
        makeBackend("activate", client, deadline);

    EXPECT_EQ(
        TdPollingBackend::StartResult::Started,
        backend->start(TdPollingBackend::Receiver()));
    ASSERT_TRUE(client->waitForSent(1));

    {
        std::lock_guard<std::mutex> lock(client->mutex);
        ASSERT_EQ(1u, client->sent.size());
        EXPECT_EQ(
            td_request_id::ACTIVATE,
            client->sent.front().requestId);
        ASSERT_NE(nullptr, client->sent.front().function);
        ASSERT_EQ(
            getOption::ID,
            client->sent.front().function->get_id());
        const getOption &request =
            static_cast<const getOption &>(
                *client->sent.front().function);
        EXPECT_EQ("version", request.name_);
    }

    ASSERT_TRUE(client->waitForReceiveCalls(1));
    {
        std::lock_guard<std::mutex> lock(client->mutex);
        EXPECT_DOUBLE_EQ(
            POLL_TIMEOUT_SECONDS,
            client->lastPollTimeoutSeconds);
    }

    finishCleanly(*backend, client);
}

TEST_F(TdPollingBackendTest, RoutesObjectsInReceiveOrderAndConsumesControls)
{
    std::shared_ptr<ClientControl> client(new ClientControl());
    std::shared_ptr<ManualDeadlineControl> deadline(
        new ManualDeadlineControl());
    std::unique_ptr<TdPollingBackend> backend =
        makeBackend("routing", client, deadline);
    std::mutex receivedMutex;
    std::vector<std::uint64_t> received;

    ASSERT_EQ(
        TdPollingBackend::StartResult::Started,
        backend->start(
            [&](std::uint64_t requestId,
                TdTransport::ObjectPtr,
                TdTransport::DeliveryReceipt receipt) {
                std::lock_guard<std::mutex> lock(receivedMutex);
                received.push_back(requestId);
                receipt.settle(true);
            }));
    ASSERT_TRUE(client->waitForSent(1));

    client->push(
        {td_request_id::ACTIVATE, make_object<optionValueString>("x")});
    client->push({0, make_object<updateConnectionState>(
                         make_object<connectionStateReady>())});
    client->push({41, make_object<ok>()});
    ASSERT_TRUE(client->waitForReceiveCalls(4));
    iterateUntil([&]() {
        std::lock_guard<std::mutex> lock(receivedMutex);
        return received.size() == 2;
    });

    {
        std::lock_guard<std::mutex> lock(receivedMutex);
        EXPECT_EQ(
            (std::vector<std::uint64_t>{0, 41}), received);
    }
    finishCleanly(*backend, client);
}

TEST_F(TdPollingBackendTest, CloseIsSingleAndAckIsNotTerminal)
{
    std::shared_ptr<ClientControl> client(new ClientControl());
    std::shared_ptr<ManualDeadlineControl> deadline(
        new ManualDeadlineControl());
    std::unique_ptr<TdPollingBackend> backend =
        makeBackend("single-close", client, deadline);
    std::vector<TdPollingBackend::CloseResult> results;

    ASSERT_EQ(
        TdPollingBackend::StartResult::Started,
        backend->start(TdPollingBackend::Receiver()));
    ASSERT_TRUE(client->waitForSent(1));
    backend->close([&](TdPollingBackend::CloseResult result) {
        results.push_back(result);
    });
    backend->close([&](TdPollingBackend::CloseResult result) {
        results.push_back(result);
    });
    ASSERT_TRUE(client->waitForSent(2));

    {
        std::lock_guard<std::mutex> lock(client->mutex);
        ASSERT_EQ(2u, client->sent.size());
        EXPECT_EQ(td_request_id::CLOSE, client->sent[1].requestId);
        ASSERT_NE(nullptr, client->sent[1].function);
        EXPECT_EQ(close::ID, client->sent[1].function->get_id());
    }

    client->push({td_request_id::CLOSE, make_object<ok>()});
    ASSERT_TRUE(client->waitForReceiveCalls(2));
    while (g_main_context_iteration(m_context, FALSE)) {
    }
    EXPECT_TRUE(results.empty());

    client->push(makeClosed());
    ASSERT_TRUE(client->waitForDestroyed());
    iterateUntil([&]() { return results.size() == 2; });
    EXPECT_EQ(TdPollingBackend::CloseResult::Closed, results[0]);
    EXPECT_EQ(TdPollingBackend::CloseResult::Closed, results[1]);
}

TEST_F(
    TdPollingBackendTest,
    CloseSendExceptionReportsFailedAndReapsWithoutChangingResult)
{
    std::shared_ptr<ClientControl> client(new ClientControl());
    std::shared_ptr<ManualDeadlineControl> deadline(
        new ManualDeadlineControl());
    std::unique_ptr<TdPollingBackend> backend =
        makeBackend("close-send-failure", client, deadline);
    std::vector<TdPollingBackend::CloseResult> results;

    ASSERT_EQ(
        TdPollingBackend::StartResult::Started,
        backend->start(TdPollingBackend::Receiver()));
    ASSERT_TRUE(client->waitForSent(1));
    ASSERT_TRUE(client->waitForReceiveCalls(1));
    {
        std::lock_guard<std::mutex> lock(client->mutex);
        client->throwOnSend = true;
    }

    backend->close([&](TdPollingBackend::CloseResult result) {
        results.push_back(result);
    });
    iterateUntil([&]() { return results.size() == 1; });
    ASSERT_EQ(1u, results.size());
    EXPECT_EQ(TdPollingBackend::CloseResult::Failed, results[0]);
    EXPECT_FALSE(client->isDestroyed());
    EXPECT_TRUE(TdPollingBackend::hasActiveWorkers());
    EXPECT_TRUE(TdPollingBackend::isSessionCleanupPending(
        "close-send-failure"));

    // Wake the already-blocked receive. The worker observes the send failure
    // before dispatching this object and exits without changing the reported
    // close result.
    client->push({td_request_id::CLOSE, make_object<ok>()});
    ASSERT_TRUE(client->waitForDestroyed());
    iterateUntil([]() {
        return !TdPollingBackend::isSessionCleanupPending(
            "close-send-failure");
    });
    EXPECT_EQ(1u, results.size());
}

TEST_F(
    TdPollingBackendTest,
    CloseSendFailureFallbackPrecedesFailedResult)
{
    std::shared_ptr<ClientControl> client(new ClientControl());
    std::shared_ptr<ManualDeadlineControl> deadline(
        new ManualDeadlineControl());
    m_clients.push_back(client);
    TdPollingBackend backend(
        "close-send-failure-fallback",
        CLOSE_TIMEOUT_SECONDS,
        POLL_TIMEOUT_SECONDS,
        [client]() {
            return std::unique_ptr<TdPollingClient>(
                new GatedClient(client));
        },
        [deadline](unsigned seconds) {
            return deadline->create(seconds);
        },
        TdPollingBackend::FinalizerSourceFactory(),
        []() -> GSource * {
            return nullptr;
        });
    std::vector<std::string> events;

    ASSERT_EQ(
        TdPollingBackend::StartResult::Started,
        backend.start(
            TdPollingBackend::Receiver(),
            [&]() { events.push_back("failure"); }));
    ASSERT_TRUE(client->waitForSent(1));
    ASSERT_TRUE(client->waitForReceiveCalls(1));
    {
        std::lock_guard<std::mutex> lock(client->mutex);
        client->throwOnSend = true;
    }

    backend.close([&](TdPollingBackend::CloseResult result) {
        EXPECT_EQ(TdPollingBackend::CloseResult::Failed, result);
        events.push_back("close");
    });
    {
        SCOPED_TRACE("failure delivery");
        iterateUntil([&]() { return events.size() == 2; });
    }
    EXPECT_EQ(
        (std::vector<std::string>{"failure", "close"}),
        events);
    EXPECT_TRUE(TdPollingBackend::hasActiveWorkers());

    client->push({td_request_id::CLOSE, make_object<ok>()});
    ASSERT_TRUE(client->waitForDestroyed());
    iterateUntil([]() {
        return !TdPollingBackend::isSessionCleanupPending(
            "close-send-failure-fallback");
    });
    deadline.reset();
    {
        SCOPED_TRACE("worker cleanup");
        iterateUntil([]() {
            return !TdPollingBackend::hasActiveWorkers();
        });
    }
    EXPECT_EQ(2u, events.size());
}

TEST_F(
    TdPollingBackendTest,
    CloseQueuesBehindConcurrentSenderWithoutBlockingFrontend)
{
    std::shared_ptr<ClientControl> client(new ClientControl());
    std::shared_ptr<ManualDeadlineControl> deadline(
        new ManualDeadlineControl());
    std::unique_ptr<TdPollingBackend> backend =
        makeBackend("queued-close", client, deadline);
    std::vector<TdPollingBackend::CloseResult> results;
    std::atomic<bool> senderFinished(false);
    std::atomic<bool> senderThrew(false);

    ASSERT_EQ(
        TdPollingBackend::StartResult::Started,
        backend->start(TdPollingBackend::Receiver()));
    ASSERT_TRUE(client->waitForSent(1));
    client->blockSend();
    TdPollingBackend::Sender sender = backend->sender();
    std::thread senderThread([&]() {
        try {
            sender(19, make_object<getMe>());
        } catch (...) {
            senderThrew = true;
        }
        senderFinished = true;
    });
    if (!client->waitForSendEntered()) {
        client->releaseSend();
        senderThread.join();
        FAIL() << "Concurrent sender did not enter its send call";
    }

    backend->close([&](TdPollingBackend::CloseResult result) {
        results.push_back(result);
    });
    EXPECT_FALSE(senderFinished.load());
    EXPECT_EQ(
        (std::vector<unsigned>{CLOSE_TIMEOUT_SECONDS}),
        deadline->intervals());
    const bool deadlineFired = deadline->fireNext();
    EXPECT_TRUE(deadlineFired);
    if (deadlineFired)
        iterateUntil([&]() { return results.size() == 1; });
    if (!results.empty()) {
        EXPECT_EQ(
            TdPollingBackend::CloseResult::TimedOut,
            results[0]);
    }

    client->releaseSend();
    senderThread.join();
    EXPECT_TRUE(senderFinished.load());
    EXPECT_FALSE(senderThrew.load());

    // Wake the fake receive so the polling worker can submit the queued
    // close request. Production receive() returns at its configured bound.
    client->push(TdPollingClient::Response());
    ASSERT_TRUE(client->waitForSent(3));
    {
        std::lock_guard<std::mutex> lock(client->mutex);
        ASSERT_EQ(3u, client->sent.size());
        EXPECT_EQ(19u, client->sent[1].requestId);
        EXPECT_EQ(td_request_id::CLOSE, client->sent[2].requestId);
    }

    client->push(makeClosed());
    ASSERT_TRUE(client->waitForDestroyed());
    iterateUntil([]() {
        return !TdPollingBackend::hasActiveWorkers();
    });
    EXPECT_EQ(1u, results.size());
}

TEST_F(
    TdPollingBackendTest,
    RuntimeFailureHandoffCannotDefeatExistingCloseDeadline)
{
    std::shared_ptr<ClientControl> client(new ClientControl());
    std::shared_ptr<ManualDeadlineControl> deadline(
        new ManualDeadlineControl());
    std::shared_ptr<BlockingFailureSourceFactory> failureFactory(
        new BlockingFailureSourceFactory());
    m_clients.push_back(client);
    TdPollingBackend backend(
        "failure-handoff-deadline",
        CLOSE_TIMEOUT_SECONDS,
        POLL_TIMEOUT_SECONDS,
        [client]() {
            return std::unique_ptr<TdPollingClient>(
                new GatedClient(client));
        },
        [deadline](unsigned seconds) {
            return deadline->create(seconds);
        },
        TdPollingBackend::FinalizerSourceFactory(),
        [failureFactory]() {
            return failureFactory->create();
        });
    std::vector<TdPollingBackend::CloseResult> results;
    std::atomic<unsigned> failureCount(0);
    std::atomic<bool> senderThrew(false);

    ASSERT_EQ(
        TdPollingBackend::StartResult::Started,
        backend.start(
            TdPollingBackend::Receiver(),
            [&]() { ++failureCount; }));
    ASSERT_TRUE(client->waitForSent(1));
    ASSERT_TRUE(client->waitForReceiveCalls(1));

    client->blockSend();
    TdPollingBackend::Sender sender = backend.sender();
    std::thread senderThread([&]() {
        try {
            sender(19, make_object<getMe>());
        } catch (...) {
            senderThrew = true;
        }
    });
    if (!client->waitForSendEntered()) {
        client->releaseSend();
        senderThread.join();
        FAIL() << "Concurrent sender did not enter its send call";
    }

    backend.close([&](TdPollingBackend::CloseResult result) {
        results.push_back(result);
    });
    EXPECT_EQ(
        (std::vector<unsigned>{CLOSE_TIMEOUT_SECONDS}),
        deadline->intervals());

    {
        std::lock_guard<std::mutex> lock(client->mutex);
        client->throwOnSend = true;
    }
    client->releaseSend();
    if (!failureFactory->waitUntilEntered()) {
        failureFactory->release();
        senderThread.join();
        FAIL() << "Runtime failure handoff did not start";
    }

    backend.close([&](TdPollingBackend::CloseResult result) {
        results.push_back(result);
    });
    ASSERT_TRUE(deadline->fireNext());
    iterateUntil([&]() { return results.size() == 2; });
    ASSERT_EQ(2u, results.size());
    EXPECT_EQ(TdPollingBackend::CloseResult::TimedOut, results[0]);
    EXPECT_EQ(TdPollingBackend::CloseResult::TimedOut, results[1]);
    EXPECT_EQ(0u, failureCount.load());

    // A later idempotent close has the known timeout result and must not
    // inherit the still-blocked runtime failure handoff.
    backend.close([&](TdPollingBackend::CloseResult result) {
        results.push_back(result);
    });
    iterateUntil([&]() { return results.size() == 3; });
    ASSERT_EQ(3u, results.size());
    EXPECT_EQ(TdPollingBackend::CloseResult::TimedOut, results[2]);
    EXPECT_EQ(0u, failureCount.load());

    client->failNextReceive();
    failureFactory->release();
    senderThread.join();
    EXPECT_TRUE(senderThrew.load());
    ASSERT_TRUE(client->waitForDestroyed());
    iterateUntil([&]() {
        return failureCount.load() == 1 &&
               !TdPollingBackend::hasActiveWorkers();
    });
    EXPECT_EQ(3u, results.size());
}

TEST_F(
    TdPollingBackendTest,
    CloseSendFailureAfterClosedIngressPreservesPhysicalResult)
{
    std::shared_ptr<ClientControl> client(new ClientControl());
    std::shared_ptr<ManualDeadlineControl> deadline(
        new ManualDeadlineControl());
    std::unique_ptr<TdPollingBackend> backend =
        makeBackend("close-send-race", client, deadline);
    std::mutex closedMutex;
    std::condition_variable closedCondition;
    bool closedDelivered = false;
    std::atomic<bool> helperSawSend(false);
    std::atomic<bool> helperSawClosed(false);
    std::atomic<unsigned> failureCount(0);
    std::vector<TdPollingBackend::CloseResult> results;

    ASSERT_EQ(
        TdPollingBackend::StartResult::Started,
        backend->start(
            [&](std::uint64_t,
                TdTransport::ObjectPtr object,
                TdTransport::DeliveryReceipt receipt) {
                if (!object ||
                    object->get_id() !=
                        updateAuthorizationState::ID) {
                    return;
                }
                {
                    std::lock_guard<std::mutex> lock(closedMutex);
                    closedDelivered = true;
                }
                closedCondition.notify_all();
                receipt.settle(true);
            },
            [&]() { ++failureCount; }));
    ASSERT_TRUE(client->waitForSent(1));
    {
        std::lock_guard<std::mutex> lock(client->mutex);
        client->throwOnSend = true;
    }
    client->blockSend();

    std::thread helper([&]() {
        helperSawSend = client->waitForSendEntered();
        if (helperSawSend.load()) {
            client->push(makeClosed());
            std::unique_lock<std::mutex> lock(closedMutex);
            helperSawClosed = closedCondition.wait_for(
                lock,
                std::chrono::seconds(5),
                [&]() { return closedDelivered; });
        }
        client->releaseSend();
    });
    backend->close([&](TdPollingBackend::CloseResult result) {
        results.push_back(result);
    });
    helper.join();

    EXPECT_TRUE(helperSawSend.load());
    EXPECT_TRUE(helperSawClosed.load());
    ASSERT_TRUE(client->waitForDestroyed());
    iterateUntil([&]() { return results.size() == 1; });

    EXPECT_EQ(TdPollingBackend::CloseResult::Closed, results[0]);
    EXPECT_EQ(0u, failureCount.load());
}

TEST_F(TdPollingBackendTest, ForwardsClosedBeforeCompletingClose)
{
    std::shared_ptr<ClientControl> client(new ClientControl());
    std::shared_ptr<ManualDeadlineControl> deadline(
        new ManualDeadlineControl());
    std::unique_ptr<TdPollingBackend> backend =
        makeBackend("closed-order", client, deadline);
    std::mutex eventsMutex;
    std::vector<std::string> events;
    std::atomic<unsigned> failureCount(0);

    ASSERT_EQ(
        TdPollingBackend::StartResult::Started,
        backend->start(
            [&](std::uint64_t,
                TdTransport::ObjectPtr object,
                TdTransport::DeliveryReceipt receipt) {
                if (object &&
                    object->get_id() ==
                        updateAuthorizationState::ID) {
                    std::lock_guard<std::mutex> lock(eventsMutex);
                    events.push_back("closed");
                }
                receipt.settle(true);
            },
            [&]() { ++failureCount; }));
    ASSERT_TRUE(client->waitForSent(1));
    backend->close([&](TdPollingBackend::CloseResult result) {
        EXPECT_EQ(TdPollingBackend::CloseResult::Closed, result);
        std::lock_guard<std::mutex> lock(eventsMutex);
        events.push_back("callback");
    });
    ASSERT_TRUE(client->waitForSent(2));
    client->push(makeClosed());
    ASSERT_TRUE(client->waitForDestroyed());
    iterateUntil([&]() {
        std::lock_guard<std::mutex> lock(eventsMutex);
        return events.size() == 2;
    });

    std::lock_guard<std::mutex> lock(eventsMutex);
    EXPECT_EQ(
        (std::vector<std::string>{"closed", "callback"}), events);
    EXPECT_EQ(0u, failureCount.load());
}

TEST_F(
    TdPollingBackendTest,
    LayeredTransportDeliversClosedBeforeCompletingClose)
{
    std::shared_ptr<ClientControl> client(new ClientControl());
    std::shared_ptr<ManualDeadlineControl> deadline(
        new ManualDeadlineControl());
    std::unique_ptr<TdPollingBackend> backend =
        makeBackend("layered-closed-order", client, deadline);
    std::vector<std::string> events;
    TdTransport transport(
        backend->sender(),
        [&](TdTransport::ObjectPtr object) {
            if (object &&
                object->get_id() ==
                    updateAuthorizationState::ID) {
                events.push_back("closed");
            }
        },
        TdTransport::TimeoutSourceFactory(),
        m_context);

    ASSERT_EQ(
        TdPollingBackend::StartResult::Started,
        backend->start(transport.acknowledgedReceiver()));
    ASSERT_TRUE(client->waitForSent(1));
    backend->close([&](TdPollingBackend::CloseResult result) {
        EXPECT_EQ(TdPollingBackend::CloseResult::Closed, result);
        events.push_back("callback");
    });
    ASSERT_TRUE(client->waitForSent(2));
    client->push(makeClosed());
    ASSERT_TRUE(client->waitForDestroyed());

    EXPECT_TRUE(events.empty());
    EXPECT_TRUE(TdPollingBackend::hasActiveWorkers());
    iterateUntil([&]() { return events.size() == 2; });

    EXPECT_EQ(
        (std::vector<std::string>{"closed", "callback"}),
        events);
}

TEST_F(
    TdPollingBackendTest,
    LayeredTransportShutdownSettlesDroppedClosed)
{
    std::shared_ptr<ClientControl> client(new ClientControl());
    std::shared_ptr<ManualDeadlineControl> deadline(
        new ManualDeadlineControl());
    std::unique_ptr<TdPollingBackend> backend =
        makeBackend("layered-dropped-closed", client, deadline);
    bool updateCalled = false;
    std::vector<TdPollingBackend::CloseResult> results;
    TdTransport transport(
        backend->sender(),
        [&](TdTransport::ObjectPtr) {
            updateCalled = true;
        },
        TdTransport::TimeoutSourceFactory(),
        m_context);

    ASSERT_EQ(
        TdPollingBackend::StartResult::Started,
        backend->start(transport.acknowledgedReceiver()));
    ASSERT_TRUE(client->waitForSent(1));
    backend->close([&](TdPollingBackend::CloseResult result) {
        results.push_back(result);
    });
    ASSERT_TRUE(client->waitForSent(2));
    client->push(makeClosed());
    ASSERT_TRUE(client->waitForDestroyed());

    transport.shutdown();
    iterateUntil([&]() { return results.size() == 1; });

    EXPECT_FALSE(updateCalled);
    EXPECT_EQ(TdPollingBackend::CloseResult::Closed, results[0]);
}

TEST_F(
    TdPollingBackendTest,
    HeldClosedReceiptDefersFinalizationAndCleanResult)
{
    std::shared_ptr<ClientControl> client(new ClientControl());
    std::shared_ptr<ManualDeadlineControl> deadline(
        new ManualDeadlineControl());
    std::unique_ptr<TdPollingBackend> backend =
        makeBackend("held-closed-receipt", client, deadline);
    DeliveryReceiptGate receiptGate;
    std::vector<TdPollingBackend::CloseResult> results;

    ASSERT_EQ(
        TdPollingBackend::StartResult::Started,
        backend->start(
            [&](std::uint64_t,
                TdTransport::ObjectPtr,
                TdTransport::DeliveryReceipt receipt) {
                receiptGate.hold(std::move(receipt));
            }));
    ASSERT_TRUE(client->waitForSent(1));
    backend->close([&](TdPollingBackend::CloseResult result) {
        results.push_back(result);
    });
    ASSERT_TRUE(client->waitForSent(2));
    client->push(makeClosed());
    ASSERT_TRUE(receiptGate.waitUntilHeld());
    ASSERT_TRUE(client->waitForDestroyed());

    drainContext(m_context);
    EXPECT_TRUE(results.empty());
    EXPECT_TRUE(TdPollingBackend::hasActiveWorkers());
    EXPECT_TRUE(TdPollingBackend::isSessionCleanupPending(
        "held-closed-receipt"));

    ASSERT_TRUE(receiptGate.settle(true));
    iterateUntil([&]() { return results.size() == 1; });
    EXPECT_EQ(TdPollingBackend::CloseResult::Closed, results[0]);
}

TEST_F(
    TdPollingBackendTest,
    DeadlineWinsWhileClosedReceiptIsHeld)
{
    std::shared_ptr<ClientControl> client(new ClientControl());
    std::shared_ptr<ManualDeadlineControl> deadline(
        new ManualDeadlineControl());
    std::unique_ptr<TdPollingBackend> backend =
        makeBackend("held-receipt-timeout", client, deadline);
    DeliveryReceiptGate receiptGate;
    std::vector<TdPollingBackend::CloseResult> results;

    ASSERT_EQ(
        TdPollingBackend::StartResult::Started,
        backend->start(
            [&](std::uint64_t,
                TdTransport::ObjectPtr,
                TdTransport::DeliveryReceipt receipt) {
                receiptGate.hold(std::move(receipt));
            }));
    ASSERT_TRUE(client->waitForSent(1));
    backend->close([&](TdPollingBackend::CloseResult result) {
        results.push_back(result);
    });
    ASSERT_TRUE(client->waitForSent(2));
    client->push(makeClosed());
    ASSERT_TRUE(receiptGate.waitUntilHeld());
    ASSERT_TRUE(client->waitForDestroyed());

    ASSERT_TRUE(deadline->fireNext());
    iterateUntil([&]() { return results.size() == 1; });
    EXPECT_EQ(TdPollingBackend::CloseResult::TimedOut, results[0]);
    EXPECT_TRUE(TdPollingBackend::hasActiveWorkers());

    ASSERT_TRUE(receiptGate.settle(true));
    iterateUntil([]() {
        return !TdPollingBackend::hasActiveWorkers();
    });
    EXPECT_EQ(1u, results.size());
}

TEST_F(
    TdPollingBackendTest,
    MissingDeadlineFailsCloseAfterPhysicalClosedReceiptIsHeld)
{
    std::shared_ptr<ClientControl> client(new ClientControl());
    m_clients.push_back(client);
    std::unique_ptr<TdPollingBackend> backend(
        new TdPollingBackend(
            "held-receipt-missing-deadline",
            CLOSE_TIMEOUT_SECONDS,
            POLL_TIMEOUT_SECONDS,
            [client]() {
                return std::unique_ptr<TdPollingClient>(
                    new GatedClient(client));
            },
            [](unsigned) -> GSource * {
                return nullptr;
            }));
    DeliveryReceiptGate receiptGate;
    std::vector<TdPollingBackend::CloseResult> results;

    ASSERT_EQ(
        TdPollingBackend::StartResult::Started,
        backend->start(
            [&](std::uint64_t,
                TdTransport::ObjectPtr,
                TdTransport::DeliveryReceipt receipt) {
                receiptGate.hold(std::move(receipt));
            }));
    ASSERT_TRUE(client->waitForSent(1));
    client->push(makeClosed());
    ASSERT_TRUE(receiptGate.waitUntilHeld());
    ASSERT_TRUE(client->waitForDestroyed());

    backend->close([&](TdPollingBackend::CloseResult result) {
        results.push_back(result);
    });
    iterateUntil([&]() { return results.size() == 1; });
    EXPECT_EQ(TdPollingBackend::CloseResult::Failed, results[0]);
    EXPECT_TRUE(TdPollingBackend::hasActiveWorkers());

    ASSERT_TRUE(receiptGate.settle(true));
    iterateUntil([]() {
        return !TdPollingBackend::hasActiveWorkers();
    });
    EXPECT_EQ(1u, results.size());
}

TEST_F(
    TdPollingBackendTest,
    ReceiptSettlementBeforePhysicalCleanupWaitsForReaper)
{
    std::shared_ptr<ClientControl> client(new ClientControl());
    std::shared_ptr<ManualDeadlineControl> deadline(
        new ManualDeadlineControl());
    std::unique_ptr<TdPollingBackend> backend =
        makeBackend("receipt-before-reaper", client, deadline);
    DeliveryReceiptGate receiptGate;
    std::vector<TdPollingBackend::CloseResult> results;
    client->blockDestruction();

    ASSERT_EQ(
        TdPollingBackend::StartResult::Started,
        backend->start(
            [&](std::uint64_t,
                TdTransport::ObjectPtr,
                TdTransport::DeliveryReceipt receipt) {
                receiptGate.hold(std::move(receipt));
            }));
    ASSERT_TRUE(client->waitForSent(1));
    backend->close([&](TdPollingBackend::CloseResult result) {
        results.push_back(result);
    });
    ASSERT_TRUE(client->waitForSent(2));
    client->push(makeClosed());
    ASSERT_TRUE(receiptGate.waitUntilHeld());
    ASSERT_TRUE(client->waitForDestructionEntered());

    ASSERT_TRUE(receiptGate.settle(true));
    drainContext(m_context);
    EXPECT_TRUE(results.empty());
    EXPECT_TRUE(TdPollingBackend::hasActiveWorkers());

    client->releaseDestruction();
    ASSERT_TRUE(client->waitForDestroyed());
    iterateUntil([&]() { return results.size() == 1; });
    EXPECT_EQ(TdPollingBackend::CloseResult::Closed, results[0]);
}

TEST_F(
    TdPollingBackendTest,
    UnexpectedClosedFailureWaitsForDeliverySettlement)
{
    std::shared_ptr<ClientControl> client(new ClientControl());
    std::shared_ptr<ManualDeadlineControl> deadline(
        new ManualDeadlineControl());
    std::unique_ptr<TdPollingBackend> backend =
        makeBackend("unexpected-held-receipt", client, deadline);
    DeliveryReceiptGate receiptGate;
    std::atomic<unsigned> failureCount(0);

    ASSERT_EQ(
        TdPollingBackend::StartResult::Started,
        backend->start(
            [&](std::uint64_t,
                TdTransport::ObjectPtr,
                TdTransport::DeliveryReceipt receipt) {
                receiptGate.hold(std::move(receipt));
            },
            [&]() { ++failureCount; }));
    ASSERT_TRUE(client->waitForSent(1));
    client->push(makeClosed());
    ASSERT_TRUE(receiptGate.waitUntilHeld());
    ASSERT_TRUE(client->waitForDestroyed());

    drainContext(m_context);
    EXPECT_EQ(0u, failureCount.load());
    EXPECT_TRUE(TdPollingBackend::hasActiveWorkers());

    ASSERT_TRUE(receiptGate.settle(true));
    iterateUntil([]() {
        return !TdPollingBackend::hasActiveWorkers();
    });
    EXPECT_EQ(1u, failureCount.load());
}

TEST_F(
    TdPollingBackendTest,
    HelperSettlementReportsUnexpectedCloseBeforeCloseCallback)
{
    std::shared_ptr<ClientControl> client(new ClientControl());
    std::shared_ptr<ManualDeadlineControl> deadline(
        new ManualDeadlineControl());
    std::unique_ptr<TdPollingBackend> backend =
        makeBackend("unexpected-helper-settlement", client, deadline);
    DeliveryReceiptGate receiptGate;
    std::vector<std::string> events;

    ASSERT_EQ(
        TdPollingBackend::StartResult::Started,
        backend->start(
            [&](std::uint64_t,
                TdTransport::ObjectPtr,
                TdTransport::DeliveryReceipt receipt) {
                receiptGate.hold(std::move(receipt));
            },
            [&]() { events.push_back("failure"); }));
    ASSERT_TRUE(client->waitForSent(1));
    client->push(makeClosed());
    ASSERT_TRUE(receiptGate.waitUntilHeld());
    ASSERT_TRUE(client->waitForDestroyed());

    backend->close([&](TdPollingBackend::CloseResult result) {
        EXPECT_EQ(TdPollingBackend::CloseResult::Closed, result);
        events.push_back("close");
    });
    std::atomic<bool> settlementSucceeded(false);
    std::thread settlementThread([&]() {
        settlementSucceeded = receiptGate.settle(true);
    });
    settlementThread.join();
    EXPECT_TRUE(settlementSucceeded.load());
    iterateUntil([&]() { return events.size() == 2; });

    EXPECT_EQ(
        (std::vector<std::string>{"failure", "close"}),
        events);
}

TEST_F(
    TdPollingBackendTest,
    FailureFallbackRunsBeforeReadyCloseDeadline)
{
    std::shared_ptr<ClientControl> client(new ClientControl());
    std::shared_ptr<ManualDeadlineControl> deadline(
        new ManualDeadlineControl());
    std::shared_ptr<ManualDeadlineControl> finalizer(
        new ManualDeadlineControl());
    m_clients.push_back(client);
    TdPollingBackend backend(
        "unexpected-failure-fallback",
        CLOSE_TIMEOUT_SECONDS,
        POLL_TIMEOUT_SECONDS,
        [client]() {
            return std::unique_ptr<TdPollingClient>(
                new GatedClient(client));
        },
        [deadline](unsigned seconds) {
            return deadline->create(seconds);
        },
        [finalizer]() {
            return finalizer->create(0);
        },
        []() -> GSource * {
            return nullptr;
        });
    DeliveryReceiptGate receiptGate;
    std::vector<std::string> events;
    std::vector<TdPollingBackend::CloseResult> results;

    ASSERT_EQ(
        TdPollingBackend::StartResult::Started,
        backend.start(
            [&](std::uint64_t,
                TdTransport::ObjectPtr,
                TdTransport::DeliveryReceipt receipt) {
                receiptGate.hold(std::move(receipt));
            },
            [&]() { events.push_back("failure"); }));
    ASSERT_TRUE(client->waitForSent(1));
    client->push(makeClosed());
    ASSERT_TRUE(receiptGate.waitUntilHeld());
    ASSERT_TRUE(client->waitForDestroyed());

    backend.close([&](TdPollingBackend::CloseResult result) {
        results.push_back(result);
        events.push_back("close");
    });
    ASSERT_TRUE(deadline->fireNext());
    std::atomic<bool> settlementSucceeded(false);
    std::thread settlementThread([&]() {
        settlementSucceeded = receiptGate.settle(true);
    });
    settlementThread.join();
    EXPECT_TRUE(settlementSucceeded.load());
    drainContext(m_context);

    ASSERT_EQ(1u, results.size());
    EXPECT_EQ(TdPollingBackend::CloseResult::Closed, results[0]);
    EXPECT_EQ(
        (std::vector<std::string>{"failure", "close"}),
        events);
    EXPECT_TRUE(TdPollingBackend::hasActiveWorkers());

    ASSERT_TRUE(finalizer->fireNext());
    iterateUntil([]() {
        return !TdPollingBackend::hasActiveWorkers();
    });
    EXPECT_EQ(2u, events.size());
}

TEST_F(
    TdPollingBackendTest,
    UnexpectedCloseFinalizationWaitsForFailureHandoff)
{
    std::shared_ptr<ClientControl> client(new ClientControl());
    std::shared_ptr<ManualDeadlineControl> deadline(
        new ManualDeadlineControl());
    std::shared_ptr<BlockingFailureSourceFactory> failureFactory(
        new BlockingFailureSourceFactory());
    m_clients.push_back(client);
    TdPollingBackend backend(
        "unexpected-failure-handoff",
        CLOSE_TIMEOUT_SECONDS,
        POLL_TIMEOUT_SECONDS,
        [client]() {
            return std::unique_ptr<TdPollingClient>(
                new GatedClient(client));
        },
        [deadline](unsigned seconds) {
            return deadline->create(seconds);
        },
        TdPollingBackend::FinalizerSourceFactory(),
        [failureFactory]() {
            return failureFactory->create();
        });
    DeliveryReceiptGate receiptGate;
    std::vector<std::string> events;

    ASSERT_EQ(
        TdPollingBackend::StartResult::Started,
        backend.start(
            [&](std::uint64_t,
                TdTransport::ObjectPtr,
                TdTransport::DeliveryReceipt receipt) {
                receiptGate.hold(std::move(receipt));
            },
            [&]() { events.push_back("failure"); }));
    ASSERT_TRUE(client->waitForSent(1));
    client->push(makeClosed());
    ASSERT_TRUE(receiptGate.waitUntilHeld());
    ASSERT_TRUE(client->waitForDestroyed());
    backend.close([&](TdPollingBackend::CloseResult result) {
        EXPECT_EQ(TdPollingBackend::CloseResult::Closed, result);
        events.push_back("close");
    });

    std::atomic<bool> settlementReturned(false);
    std::thread settlementThread([&]() {
        receiptGate.settle(true);
        settlementReturned = true;
    });
    ASSERT_TRUE(failureFactory->waitUntilEntered());
    drainContext(m_context);
    EXPECT_FALSE(settlementReturned.load());
    EXPECT_TRUE(events.empty());
    EXPECT_TRUE(TdPollingBackend::hasActiveWorkers());

    failureFactory->release();
    settlementThread.join();
    iterateUntil([&]() { return events.size() == 2; });
    EXPECT_EQ(
        (std::vector<std::string>{"failure", "close"}),
        events);
    iterateUntil([]() {
        return !TdPollingBackend::hasActiveWorkers();
    });
}

TEST_F(
    TdPollingBackendTest,
    FinalizerWaitsForHelperReceiptSettlementToExit)
{
    std::shared_ptr<ClientControl> client(new ClientControl());
    std::shared_ptr<ManualDeadlineControl> deadline(
        new ManualDeadlineControl());
    std::shared_ptr<BlockingFinalizerAttacher> attacher(
        new BlockingFinalizerAttacher());
    std::shared_ptr<ReadySignal> reaperReady(
        new ReadySignal());
    m_clients.push_back(client);
    TdPollingBackend backend(
        "settlement-exit-latch",
        CLOSE_TIMEOUT_SECONDS,
        POLL_TIMEOUT_SECONDS,
        [client]() {
            return std::unique_ptr<TdPollingClient>(
                new GatedClient(client));
        },
        [deadline](unsigned seconds) {
            return deadline->create(seconds);
        },
        TdPollingBackend::FinalizerSourceFactory(),
        TdPollingBackend::FailureSourceFactory(),
        [attacher](GSource *source, GMainContext *context) {
            return attacher->attach(source, context);
        },
        [reaperReady]() {
            reaperReady->signal();
        });
    DeliveryReceiptGate receiptGate;
    std::vector<TdPollingBackend::CloseResult> results;

    ASSERT_EQ(
        TdPollingBackend::StartResult::Started,
        backend.start(
            [&](std::uint64_t,
                TdTransport::ObjectPtr,
                TdTransport::DeliveryReceipt receipt) {
                receiptGate.hold(std::move(receipt));
            }));
    ASSERT_TRUE(client->waitForSent(1));
    backend.close([&](TdPollingBackend::CloseResult result) {
        results.push_back(result);
    });
    ASSERT_TRUE(client->waitForSent(2));
    client->push(makeClosed());
    ASSERT_TRUE(receiptGate.waitUntilHeld());
    ASSERT_TRUE(client->waitForDestroyed());
    ASSERT_TRUE(reaperReady->wait());

    std::atomic<bool> settlementReturned(false);
    std::atomic<bool> settlementSucceeded(false);
    std::thread settlementThread([&]() {
        settlementSucceeded = receiptGate.settle(true);
        settlementReturned = true;
    });
    ASSERT_TRUE(attacher->waitUntilEntered());
    EXPECT_TRUE(g_main_context_iteration(m_context, FALSE));
    EXPECT_FALSE(settlementReturned.load());
    EXPECT_TRUE(results.empty());
    EXPECT_TRUE(TdPollingBackend::hasActiveWorkers());
    EXPECT_TRUE(TdPollingBackend::isSessionCleanupPending(
        "settlement-exit-latch"));

    attacher->release();
    settlementThread.join();
    EXPECT_TRUE(settlementSucceeded.load());
    iterateUntil([&]() { return results.size() == 1; });
    EXPECT_EQ(TdPollingBackend::CloseResult::Closed, results[0]);
    iterateUntil([]() {
        return !TdPollingBackend::hasActiveWorkers();
    });
}

TEST_F(TdPollingBackendTest, UnsolicitedClosedReportsRuntimeFailure)
{
    std::shared_ptr<ClientControl> client(new ClientControl());
    std::shared_ptr<ManualDeadlineControl> deadline(
        new ManualDeadlineControl());
    std::unique_ptr<TdPollingBackend> backend =
        makeBackend("unsolicited-closed", client, deadline);
    std::atomic<unsigned> failureCount(0);
    std::vector<TdPollingBackend::CloseResult> results;

    ASSERT_EQ(
        TdPollingBackend::StartResult::Started,
        backend->start(
            TdPollingBackend::Receiver(),
            [&]() { ++failureCount; }));
    ASSERT_TRUE(client->waitForSent(1));
    client->push(makeClosed());
    ASSERT_TRUE(client->waitForDestroyed());
    iterateUntil([&]() {
        return failureCount.load() == 1 &&
               !TdPollingBackend::hasActiveWorkers();
    });

    backend->close([&](TdPollingBackend::CloseResult result) {
        results.push_back(result);
    });
    iterateUntil([&]() { return results.size() == 1; });
    EXPECT_EQ(TdPollingBackend::CloseResult::Closed, results[0]);
    EXPECT_EQ(1u, failureCount.load());
}

TEST_F(TdPollingBackendTest, DeadlineWinsExactlyOnceAndLateClosedReaps)
{
    std::shared_ptr<ClientControl> client(new ClientControl());
    std::shared_ptr<ManualDeadlineControl> deadline(
        new ManualDeadlineControl());
    std::unique_ptr<TdPollingBackend> backend =
        makeBackend("late-close", client, deadline);
    std::vector<TdPollingBackend::CloseResult> results;

    ASSERT_EQ(
        TdPollingBackend::StartResult::Started,
        backend->start(TdPollingBackend::Receiver()));
    ASSERT_TRUE(client->waitForSent(1));
    backend->close([&](TdPollingBackend::CloseResult result) {
        results.push_back(result);
    });
    ASSERT_TRUE(client->waitForSent(2));
    ASSERT_EQ(
        (std::vector<unsigned>{CLOSE_TIMEOUT_SECONDS}),
        deadline->intervals());
    ASSERT_TRUE(deadline->fireNext());
    iterateUntil([&]() { return results.size() == 1; });
    EXPECT_EQ(TdPollingBackend::CloseResult::TimedOut, results[0]);
    EXPECT_TRUE(TdPollingBackend::hasActiveWorkers());
    EXPECT_TRUE(
        TdPollingBackend::isSessionCleanupPending("late-close"));

    client->push(makeClosed());
    ASSERT_TRUE(client->waitForDestroyed());
    iterateUntil([]() {
        return !TdPollingBackend::isSessionCleanupPending(
            "late-close");
    });
    EXPECT_EQ(1u, results.size());
}

TEST_F(TdPollingBackendTest, PhysicalCloseBeatsReadyDeadline)
{
    std::shared_ptr<ClientControl> client(new ClientControl());
    std::shared_ptr<ManualDeadlineControl> deadline(
        new ManualDeadlineControl());
    std::unique_ptr<TdPollingBackend> backend =
        makeBackend("close-wins", client, deadline);
    std::vector<TdPollingBackend::CloseResult> results;

    ASSERT_EQ(
        TdPollingBackend::StartResult::Started,
        backend->start(TdPollingBackend::Receiver()));
    ASSERT_TRUE(client->waitForSent(1));
    backend->close([&](TdPollingBackend::CloseResult result) {
        results.push_back(result);
    });
    ASSERT_TRUE(client->waitForSent(2));
    client->push(makeClosed());
    // Do not iterate the captured context yet. The worker has physically
    // stopped, but both its finalizer and the now-ready deadline are queued.
    ASSERT_TRUE(client->waitForDestroyed());
    ASSERT_TRUE(deadline->fireNext());
    iterateUntil([&]() { return results.size() == 1; });

    EXPECT_EQ(TdPollingBackend::CloseResult::Closed, results[0]);
}

TEST_F(
    TdPollingBackendTest,
    CloseAfterPhysicalStopCompletesPendingAndNewCallbacks)
{
    std::shared_ptr<ClientControl> client(new ClientControl());
    std::shared_ptr<ManualDeadlineControl> deadline(
        new ManualDeadlineControl());
    std::unique_ptr<GSource, void (*)(GSource *)> finalizerObserver(
        nullptr, g_source_unref);
    std::vector<TdPollingBackend::CloseResult> results;
    m_clients.push_back(client);
    TdPollingBackend backend(
        "physical-close-callbacks",
        CLOSE_TIMEOUT_SECONDS,
        POLL_TIMEOUT_SECONDS,
        [client]() {
            return std::unique_ptr<TdPollingClient>(
                new GatedClient(client));
        },
        [deadline](unsigned seconds) {
            return deadline->create(seconds);
        },
        [&]() {
            GSource *source = g_idle_source_new();
            finalizerObserver.reset(g_source_ref(source));
            return source;
        });

    ASSERT_EQ(
        TdPollingBackend::StartResult::Started,
        backend.start(TdPollingBackend::Receiver()));
    ASSERT_TRUE(client->waitForSent(1));
    backend.close([&](TdPollingBackend::CloseResult result) {
        results.push_back(result);
    });
    ASSERT_TRUE(client->waitForSent(2));
    client->push(makeClosed());
    ASSERT_TRUE(client->waitForDestroyed());

    // The reaper attaches this source only after joining the polling worker,
    // which makes physicalFinished observable without dispatching either the
    // finalizer or the close deadline.
    const gint64 attachDeadline =
        g_get_monotonic_time() + 5 * G_TIME_SPAN_SECOND;
    while (finalizerObserver &&
           !g_source_get_context(finalizerObserver.get()) &&
           g_get_monotonic_time() < attachDeadline) {
        std::this_thread::yield();
    }
    ASSERT_NE(nullptr, finalizerObserver.get());
    ASSERT_NE(
        nullptr, g_source_get_context(finalizerObserver.get()));

    backend.close([&](TdPollingBackend::CloseResult result) {
        results.push_back(result);
    });
    iterateUntil([&]() { return results.size() == 2; });

    EXPECT_EQ(TdPollingBackend::CloseResult::Closed, results[0]);
    EXPECT_EQ(TdPollingBackend::CloseResult::Closed, results[1]);
}

TEST_F(TdPollingBackendTest, BlockedReceiverCannotDefeatDeadline)
{
    std::shared_ptr<ClientControl> client(new ClientControl());
    std::shared_ptr<ManualDeadlineControl> deadline(
        new ManualDeadlineControl());
    std::unique_ptr<TdPollingBackend> backend =
        makeBackend("blocked-receiver", client, deadline);
    std::vector<TdPollingBackend::CloseResult> results;
    std::mutex gateMutex;
    std::condition_variable gateCondition;
    bool closedEntered = false;
    bool releaseClosed = false;

    ASSERT_EQ(
        TdPollingBackend::StartResult::Started,
        backend->start(
            [&](std::uint64_t,
                TdTransport::ObjectPtr object,
                TdTransport::DeliveryReceipt receipt) {
                if (!object ||
                    object->get_id() !=
                        updateAuthorizationState::ID) {
                    return;
                }
                std::unique_lock<std::mutex> lock(gateMutex);
                closedEntered = true;
                gateCondition.notify_all();
                gateCondition.wait(
                    lock, [&]() { return releaseClosed; });
                receipt.settle(true);
            }));
    ASSERT_TRUE(client->waitForSent(1));
    backend->close([&](TdPollingBackend::CloseResult result) {
        results.push_back(result);
    });
    ASSERT_TRUE(client->waitForSent(2));
    client->push(makeClosed());
    {
        std::unique_lock<std::mutex> lock(gateMutex);
        ASSERT_TRUE(gateCondition.wait_for(
            lock,
            std::chrono::seconds(5),
            [&]() { return closedEntered; }));
    }

    ASSERT_TRUE(deadline->fireNext());
    iterateUntil([&]() { return results.size() == 1; });
    EXPECT_EQ(TdPollingBackend::CloseResult::TimedOut, results[0]);

    {
        std::lock_guard<std::mutex> lock(gateMutex);
        releaseClosed = true;
    }
    gateCondition.notify_all();
    ASSERT_TRUE(client->waitForDestroyed());
    iterateUntil([]() {
        return !TdPollingBackend::hasActiveWorkers();
    });
    EXPECT_EQ(1u, results.size());
}

TEST_F(
    TdPollingBackendTest,
    CloseAfterClosedIngressAvoidsRedundantRequestAndKeepsDeadline)
{
    std::shared_ptr<ClientControl> client(new ClientControl());
    std::shared_ptr<ManualDeadlineControl> deadline(
        new ManualDeadlineControl());
    std::unique_ptr<TdPollingBackend> backend =
        makeBackend("closed-before-close", client, deadline);
    std::vector<TdPollingBackend::CloseResult> results;
    std::mutex gateMutex;
    std::condition_variable gateCondition;
    bool closedEntered = false;
    bool releaseClosed = false;

    ASSERT_EQ(
        TdPollingBackend::StartResult::Started,
        backend->start(
            [&](std::uint64_t,
                TdTransport::ObjectPtr object,
                TdTransport::DeliveryReceipt receipt) {
                if (!object ||
                    object->get_id() !=
                        updateAuthorizationState::ID) {
                    return;
                }
                std::unique_lock<std::mutex> lock(gateMutex);
                closedEntered = true;
                gateCondition.notify_all();
                gateCondition.wait(
                    lock, [&]() { return releaseClosed; });
                receipt.settle(true);
            }));
    ASSERT_TRUE(client->waitForSent(1));
    client->push(makeClosed());
    {
        std::unique_lock<std::mutex> lock(gateMutex);
        ASSERT_TRUE(gateCondition.wait_for(
            lock,
            std::chrono::seconds(5),
            [&]() { return closedEntered; }));
    }

    backend->close([&](TdPollingBackend::CloseResult result) {
        results.push_back(result);
    });
    {
        std::lock_guard<std::mutex> lock(client->mutex);
        EXPECT_EQ(1u, client->sent.size());
    }
    EXPECT_EQ(
        (std::vector<unsigned>{CLOSE_TIMEOUT_SECONDS}),
        deadline->intervals());
    ASSERT_TRUE(deadline->fireNext());
    iterateUntil([&]() { return results.size() == 1; });
    EXPECT_EQ(TdPollingBackend::CloseResult::TimedOut, results[0]);

    {
        std::lock_guard<std::mutex> lock(gateMutex);
        releaseClosed = true;
    }
    gateCondition.notify_all();
    ASSERT_TRUE(client->waitForDestroyed());
    iterateUntil([]() {
        return !TdPollingBackend::hasActiveWorkers();
    });
    EXPECT_EQ(1u, results.size());
}

TEST_F(TdPollingBackendTest, BlockingClientDestructorCannotDefeatDeadline)
{
    std::shared_ptr<ClientControl> client(new ClientControl());
    std::shared_ptr<ManualDeadlineControl> deadline(
        new ManualDeadlineControl());
    std::unique_ptr<TdPollingBackend> backend =
        makeBackend("blocked-destructor", client, deadline);
    std::vector<TdPollingBackend::CloseResult> results;
    client->blockDestruction();

    ASSERT_EQ(
        TdPollingBackend::StartResult::Started,
        backend->start(TdPollingBackend::Receiver()));
    ASSERT_TRUE(client->waitForSent(1));
    backend->close([&](TdPollingBackend::CloseResult result) {
        results.push_back(result);
    });
    ASSERT_TRUE(client->waitForSent(2));
    client->push(makeClosed());
    ASSERT_TRUE(client->waitForDestructionEntered());

    ASSERT_TRUE(deadline->fireNext());
    iterateUntil([&]() { return results.size() == 1; });
    EXPECT_EQ(TdPollingBackend::CloseResult::TimedOut, results[0]);

    client->releaseDestruction();
    ASSERT_TRUE(client->waitForDestroyed());
    iterateUntil([]() {
        return !TdPollingBackend::hasActiveWorkers();
    });
    EXPECT_EQ(1u, results.size());
}

TEST_F(
    TdPollingBackendTest,
    CloseAfterUnsolicitedClosedDoesNotWaitForClientDestructor)
{
    std::shared_ptr<ClientControl> client(new ClientControl());
    std::shared_ptr<ManualDeadlineControl> deadline(
        new ManualDeadlineControl());
    std::unique_ptr<TdPollingBackend> backend =
        makeBackend("unsolicited-blocked-destructor", client, deadline);
    std::vector<TdPollingBackend::CloseResult> results;
    std::atomic<unsigned> failureCount(0);
    client->blockDestruction();

    ASSERT_EQ(
        TdPollingBackend::StartResult::Started,
        backend->start(
            TdPollingBackend::Receiver(),
            [&]() { ++failureCount; }));
    ASSERT_TRUE(client->waitForSent(1));
    client->push(makeClosed());
    ASSERT_TRUE(client->waitForDestructionEntered());

    backend->close([&](TdPollingBackend::CloseResult result) {
        results.push_back(result);
    });
    EXPECT_EQ(
        (std::vector<unsigned>{CLOSE_TIMEOUT_SECONDS}),
        deadline->intervals());
    ASSERT_TRUE(deadline->fireNext());
    iterateUntil([&]() { return results.size() == 1; });
    EXPECT_EQ(TdPollingBackend::CloseResult::TimedOut, results[0]);
    EXPECT_EQ(1u, failureCount.load());

    client->releaseDestruction();
    ASSERT_TRUE(client->waitForDestroyed());
    iterateUntil([]() {
        return !TdPollingBackend::hasActiveWorkers();
    });
    EXPECT_EQ(1u, results.size());
}

TEST_F(TdPollingBackendTest, CloseCallbackUsesCapturedContextAndIsReentrant)
{
    std::shared_ptr<ClientControl> client(new ClientControl());
    std::shared_ptr<ManualDeadlineControl> deadline(
        new ManualDeadlineControl());
    std::unique_ptr<TdPollingBackend> backend =
        makeBackend("reentrant", client, deadline);
    const std::thread::id ownerThread = std::this_thread::get_id();
    std::vector<TdPollingBackend::CloseResult> results;

    ASSERT_EQ(
        TdPollingBackend::StartResult::Started,
        backend->start(TdPollingBackend::Receiver()));
    ASSERT_TRUE(client->waitForSent(1));
    backend->close([&](TdPollingBackend::CloseResult result) {
        EXPECT_EQ(ownerThread, std::this_thread::get_id());
        results.push_back(result);
        backend->close(
            [&](TdPollingBackend::CloseResult repeated) {
                EXPECT_EQ(ownerThread, std::this_thread::get_id());
                results.push_back(repeated);
            });
    });
    ASSERT_TRUE(client->waitForSent(2));
    client->push(makeClosed());
    ASSERT_TRUE(client->waitForDestroyed());
    iterateUntil([&]() { return results.size() == 2; });

    EXPECT_EQ(TdPollingBackend::CloseResult::Closed, results[0]);
    EXPECT_EQ(TdPollingBackend::CloseResult::Closed, results[1]);
}

TEST_F(TdPollingBackendTest, CloseCallbackMayDestroyFrontend)
{
    std::shared_ptr<ClientControl> client(new ClientControl());
    std::shared_ptr<ManualDeadlineControl> deadline(
        new ManualDeadlineControl());
    std::unique_ptr<TdPollingBackend> backend =
        makeBackend("destroy-callback", client, deadline);
    std::atomic<bool> called(false);

    ASSERT_EQ(
        TdPollingBackend::StartResult::Started,
        backend->start(TdPollingBackend::Receiver()));
    ASSERT_TRUE(client->waitForSent(1));
    backend->close([&](TdPollingBackend::CloseResult result) {
        EXPECT_EQ(TdPollingBackend::CloseResult::Closed, result);
        backend.reset();
        called = true;
    });
    ASSERT_TRUE(client->waitForSent(2));
    client->push(makeClosed());
    ASSERT_TRUE(client->waitForDestroyed());
    iterateUntil([&]() { return called.load(); });

    EXPECT_EQ(nullptr, backend);
}

TEST_F(TdPollingBackendTest, StableSenderRejectsOutsideRunningState)
{
    std::shared_ptr<ClientControl> client(new ClientControl());
    std::shared_ptr<ManualDeadlineControl> deadline(
        new ManualDeadlineControl());
    std::unique_ptr<TdPollingBackend> backend =
        makeBackend("sender-state", client, deadline);
    TdPollingBackend::Sender sender = backend->sender();

    EXPECT_ANY_THROW(sender(10, make_object<getMe>()));
    ASSERT_EQ(
        TdPollingBackend::StartResult::Started,
        backend->start(TdPollingBackend::Receiver()));
    ASSERT_TRUE(client->waitForSent(1));
    EXPECT_NO_THROW(sender(10, make_object<getMe>()));
    ASSERT_TRUE(client->waitForSent(2));

    backend->close();
    ASSERT_TRUE(client->waitForSent(3));
    EXPECT_ANY_THROW(sender(11, make_object<getMe>()));
    client->push(makeClosed());
    ASSERT_TRUE(client->waitForDestroyed());
    iterateUntil([]() {
        return !TdPollingBackend::hasActiveWorkers();
    });
    backend.reset();
    EXPECT_ANY_THROW(sender(12, make_object<getMe>()));
}

TEST_F(TdPollingBackendTest, SenderIsSafeDuringConcurrentStart)
{
    std::shared_ptr<ClientControl> client(new ClientControl());
    std::shared_ptr<ManualDeadlineControl> deadline(
        new ManualDeadlineControl());
    m_clients.push_back(client);
    std::mutex gateMutex;
    std::condition_variable gateCondition;
    bool factoryEntered = false;
    bool senderLoopStarted = false;
    std::atomic<bool> stopSender(false);
    std::atomic<unsigned> attempts(0);
    std::atomic<unsigned> rejected(0);

    std::unique_ptr<TdPollingBackend> backend(
        new TdPollingBackend(
            "concurrent-start",
            CLOSE_TIMEOUT_SECONDS,
            POLL_TIMEOUT_SECONDS,
            [&]() {
                std::unique_lock<std::mutex> lock(gateMutex);
                factoryEntered = true;
                gateCondition.notify_all();
                gateCondition.wait(lock, [&]() {
                    return senderLoopStarted;
                });
                return std::unique_ptr<TdPollingClient>(
                    new GatedClient(client));
            },
            [deadline](unsigned seconds) {
                return deadline->create(seconds);
            }));
    TdPollingBackend::Sender sender = backend->sender();
    std::thread senderThread([&]() {
        {
            std::unique_lock<std::mutex> lock(gateMutex);
            gateCondition.wait(lock, [&]() {
                return factoryEntered;
            });
            senderLoopStarted = true;
            gateCondition.notify_all();
        }
        while (!stopSender.load()) {
            ++attempts;
            try {
                sender(61, make_object<getMe>());
            } catch (...) {
                ++rejected;
            }
        }
    });

    TdPollingBackend::StartResult startResult =
        backend->start(TdPollingBackend::Receiver());
    stopSender = true;
    senderThread.join();

    ASSERT_EQ(
        TdPollingBackend::StartResult::Started, startResult);
    EXPECT_GT(attempts.load(), 0u);
    EXPECT_GT(rejected.load(), 0u);
    ASSERT_TRUE(client->waitForSent(1));
    finishCleanly(*backend, client);
}

TEST_F(
    TdPollingBackendTest,
    SendFailureAfterUnsolicitedClosedPreservesPhysicalResult)
{
    std::shared_ptr<ClientControl> client(new ClientControl());
    std::shared_ptr<ManualDeadlineControl> deadline(
        new ManualDeadlineControl());
    std::unique_ptr<TdPollingBackend> backend =
        makeBackend("send-race-closed", client, deadline);
    std::mutex closedMutex;
    std::condition_variable closedCondition;
    bool closedDelivered = false;
    std::atomic<unsigned> failureCount(0);
    std::atomic<bool> senderThrew(false);
    std::vector<TdPollingBackend::CloseResult> results;

    ASSERT_EQ(
        TdPollingBackend::StartResult::Started,
        backend->start(
            [&](std::uint64_t,
                TdTransport::ObjectPtr object,
                TdTransport::DeliveryReceipt receipt) {
                if (!object ||
                    object->get_id() !=
                        updateAuthorizationState::ID) {
                    return;
                }
                {
                    std::lock_guard<std::mutex> lock(closedMutex);
                    closedDelivered = true;
                }
                closedCondition.notify_all();
                receipt.settle(true);
            },
            [&]() { ++failureCount; }));
    ASSERT_TRUE(client->waitForSent(1));
    {
        std::lock_guard<std::mutex> lock(client->mutex);
        client->throwOnSend = true;
    }
    client->blockSend();
    std::thread senderThread([&]() {
        try {
            backend->sender()(62, make_object<getMe>());
        } catch (...) {
            senderThrew = true;
        }
    });
    const bool sendEntered = client->waitForSendEntered();
    bool sawClosed = false;
    if (sendEntered) {
        client->push(makeClosed());
        std::unique_lock<std::mutex> lock(closedMutex);
        sawClosed = closedCondition.wait_for(
            lock,
            std::chrono::seconds(5),
            [&]() { return closedDelivered; });
    }
    client->releaseSend();
    senderThread.join();
    ASSERT_TRUE(sendEntered);
    ASSERT_TRUE(sawClosed);
    ASSERT_TRUE(client->waitForDestroyed());

    backend->close([&](TdPollingBackend::CloseResult result) {
        results.push_back(result);
    });
    iterateUntil([&]() {
        return results.size() == 1 &&
               failureCount.load() == 1;
    });

    EXPECT_TRUE(senderThrew.load());
    EXPECT_EQ(TdPollingBackend::CloseResult::Closed, results[0]);
    EXPECT_EQ(1u, failureCount.load());
}

TEST_F(TdPollingBackendTest, FrontendDestructionNeverWaitsForClosed)
{
    std::shared_ptr<ClientControl> client(new ClientControl());
    std::shared_ptr<ManualDeadlineControl> deadline(
        new ManualDeadlineControl());
    std::unique_ptr<TdPollingBackend> backend =
        makeBackend("nonblocking-destroy", client, deadline);

    ASSERT_EQ(
        TdPollingBackend::StartResult::Started,
        backend->start(TdPollingBackend::Receiver()));
    ASSERT_TRUE(client->waitForSent(1));
    backend.reset();

    ASSERT_TRUE(client->waitForSent(2));
    EXPECT_FALSE(client->isDestroyed());
    EXPECT_TRUE(TdPollingBackend::hasActiveWorkers());
    EXPECT_TRUE(TdPollingBackend::isSessionCleanupPending(
        "nonblocking-destroy"));

    client->push(makeClosed());
    ASSERT_TRUE(client->waitForDestroyed());
    iterateUntil([]() {
        return !TdPollingBackend::isSessionCleanupPending(
            "nonblocking-destroy");
    });
}

TEST_F(TdPollingBackendTest, RejectsSameKeyWhileFirstBackendIsRunning)
{
    std::shared_ptr<ClientControl> firstClient(new ClientControl());
    std::shared_ptr<ClientControl> secondClient(new ClientControl());
    std::shared_ptr<ManualDeadlineControl> firstDeadline(
        new ManualDeadlineControl());
    std::shared_ptr<ManualDeadlineControl> secondDeadline(
        new ManualDeadlineControl());
    std::unique_ptr<TdPollingBackend> first =
        makeBackend("same-key-running", firstClient, firstDeadline);
    std::unique_ptr<TdPollingBackend> second =
        makeBackend("same-key-running", secondClient, secondDeadline);

    ASSERT_EQ(
        TdPollingBackend::StartResult::Started,
        first->start(TdPollingBackend::Receiver()));
    ASSERT_TRUE(firstClient->waitForSent(1));
    EXPECT_EQ(
        TdPollingBackend::StartResult::SessionBusy,
        second->start(TdPollingBackend::Receiver()));
    {
        std::lock_guard<std::mutex> lock(secondClient->mutex);
        EXPECT_TRUE(secondClient->sent.empty());
    }

    finishCleanly(*first, firstClient);
}

TEST_F(TdPollingBackendTest, AllowsDifferentKeysToRunConcurrently)
{
    std::shared_ptr<ClientControl> firstClient(new ClientControl());
    std::shared_ptr<ClientControl> secondClient(new ClientControl());
    std::shared_ptr<ManualDeadlineControl> firstDeadline(
        new ManualDeadlineControl());
    std::shared_ptr<ManualDeadlineControl> secondDeadline(
        new ManualDeadlineControl());
    std::unique_ptr<TdPollingBackend> first =
        makeBackend("concurrent-key-one", firstClient, firstDeadline);
    std::unique_ptr<TdPollingBackend> second =
        makeBackend("concurrent-key-two", secondClient, secondDeadline);

    ASSERT_EQ(
        TdPollingBackend::StartResult::Started,
        first->start(TdPollingBackend::Receiver()));
    ASSERT_EQ(
        TdPollingBackend::StartResult::Started,
        second->start(TdPollingBackend::Receiver()));
    ASSERT_TRUE(firstClient->waitForSent(1));
    ASSERT_TRUE(secondClient->waitForSent(1));

    finishCleanly(*first, firstClient);
    EXPECT_TRUE(TdPollingBackend::hasActiveWorkers());
    finishCleanly(*second, secondClient);
    iterateUntil([]() {
        return !TdPollingBackend::hasActiveWorkers();
    });
}

TEST_F(
    TdPollingBackendTest,
    RepeatedStartFailsWithoutCreatingAnotherClientOrActivation)
{
    std::shared_ptr<ClientControl> client(new ClientControl());
    std::shared_ptr<ManualDeadlineControl> deadline(
        new ManualDeadlineControl());
    m_clients.push_back(client);
    unsigned factoryCalls = 0;
    std::unique_ptr<TdPollingBackend> backend(
        new TdPollingBackend(
            "repeated-start",
            CLOSE_TIMEOUT_SECONDS,
            POLL_TIMEOUT_SECONDS,
            [&]() {
                ++factoryCalls;
                return std::unique_ptr<TdPollingClient>(
                    new GatedClient(client));
            },
            [deadline](unsigned seconds) {
                return deadline->create(seconds);
            }));

    ASSERT_EQ(
        TdPollingBackend::StartResult::Started,
        backend->start(TdPollingBackend::Receiver()));
    ASSERT_TRUE(client->waitForSent(1));
    EXPECT_EQ(
        TdPollingBackend::StartResult::Failed,
        backend->start(TdPollingBackend::Receiver()));
    EXPECT_EQ(1u, factoryCalls);
    {
        std::lock_guard<std::mutex> lock(client->mutex);
        ASSERT_EQ(1u, client->sent.size());
        EXPECT_EQ(
            td_request_id::ACTIVATE,
            client->sent.front().requestId);
    }

    finishCleanly(*backend, client);
}

TEST_F(TdPollingBackendTest, SameKeyReconnectWaitsForPhysicalCleanup)
{
    std::shared_ptr<ClientControl> firstClient(new ClientControl());
    std::shared_ptr<ClientControl> secondClient(new ClientControl());
    std::shared_ptr<ManualDeadlineControl> firstDeadline(
        new ManualDeadlineControl());
    std::shared_ptr<ManualDeadlineControl> secondDeadline(
        new ManualDeadlineControl());
    std::unique_ptr<TdPollingBackend> first =
        makeBackend("same-key", firstClient, firstDeadline);
    std::unique_ptr<TdPollingBackend> second =
        makeBackend("same-key", secondClient, secondDeadline);

    ASSERT_EQ(
        TdPollingBackend::StartResult::Started,
        first->start(TdPollingBackend::Receiver()));
    ASSERT_TRUE(firstClient->waitForSent(1));
    first.reset();
    ASSERT_TRUE(firstClient->waitForSent(2));
    EXPECT_EQ(
        TdPollingBackend::StartResult::SessionBusy,
        second->start(TdPollingBackend::Receiver()));

    firstClient->push(makeClosed());
    ASSERT_TRUE(firstClient->waitForDestroyed());
    iterateUntil([]() {
        return !TdPollingBackend::isSessionCleanupPending("same-key");
    });

    EXPECT_EQ(
        TdPollingBackend::StartResult::Started,
        second->start(TdPollingBackend::Receiver()));
    ASSERT_TRUE(secondClient->waitForSent(1));
    finishCleanly(*second, secondClient);
}

TEST_F(TdPollingBackendTest, ActiveWorkerGuardTracksCleanAndTimedOutClose)
{
    std::shared_ptr<ClientControl> client(new ClientControl());
    std::shared_ptr<ManualDeadlineControl> deadline(
        new ManualDeadlineControl());
    std::unique_ptr<TdPollingBackend> backend =
        makeBackend("guard", client, deadline);
    std::atomic<bool> timedOut(false);

    EXPECT_FALSE(TdPollingBackend::hasActiveWorkers());
    ASSERT_EQ(
        TdPollingBackend::StartResult::Started,
        backend->start(TdPollingBackend::Receiver()));
    ASSERT_TRUE(client->waitForSent(1));
    EXPECT_TRUE(TdPollingBackend::hasActiveWorkers());
    EXPECT_FALSE(
        TdPollingBackend::isSessionCleanupPending("guard"));

    backend->close([&](TdPollingBackend::CloseResult result) {
        timedOut =
            result == TdPollingBackend::CloseResult::TimedOut;
    });
    ASSERT_TRUE(client->waitForSent(2));
    EXPECT_TRUE(
        TdPollingBackend::isSessionCleanupPending("guard"));
    ASSERT_TRUE(deadline->fireNext());
    iterateUntil([&]() { return timedOut.load(); });
    EXPECT_TRUE(TdPollingBackend::hasActiveWorkers());

    client->push(makeClosed());
    ASSERT_TRUE(client->waitForDestroyed());
    iterateUntil([]() {
        return !TdPollingBackend::hasActiveWorkers();
    });
}

TEST_F(TdPollingBackendTest, ActiveWorkerGuardTracksPendingCallbacks)
{
    std::shared_ptr<ClientControl> client(new ClientControl());
    std::shared_ptr<ManualDeadlineControl> deadline(
        new ManualDeadlineControl());
    std::unique_ptr<TdPollingBackend> backend =
        makeBackend("pending-callback", client, deadline);
    bool callbackCalled = false;
    bool guardedDuringCallback = false;

    backend->close([&](TdPollingBackend::CloseResult result) {
        EXPECT_EQ(TdPollingBackend::CloseResult::Closed, result);
        guardedDuringCallback =
            TdPollingBackend::hasActiveWorkers();
        callbackCalled = true;
    });
    EXPECT_TRUE(TdPollingBackend::hasActiveWorkers());
    EXPECT_EQ(
        TdPollingBackend::StartResult::Failed,
        backend->start(TdPollingBackend::Receiver()));
    iterateUntil([&]() { return callbackCalled; });

    EXPECT_TRUE(guardedDuringCallback);
    iterateUntil([]() {
        return !TdPollingBackend::hasActiveWorkers();
    });
}

TEST_F(
    TdPollingBackendTest,
    CallbackCaptureDestructionMayReenterActiveWorkerGuard)
{
    std::shared_ptr<ClientControl> client(new ClientControl());
    std::shared_ptr<ManualDeadlineControl> deadline(
        new ManualDeadlineControl());
    std::unique_ptr<TdPollingBackend> backend =
        makeBackend("reentrant-capture-cleanup", client, deadline);
    std::atomic<bool> callbackCalled(false);
    std::atomic<bool> captureDestroyed(false);
    std::atomic<bool> guardedDuringDestruction(false);
    std::shared_ptr<ReentrantCleanupProbe> capture(
        new ReentrantCleanupProbe(
            &captureDestroyed, &guardedDuringDestruction));

    backend->close(
        [capture, &callbackCalled](
            TdPollingBackend::CloseResult result) {
            EXPECT_EQ(
                TdPollingBackend::CloseResult::Closed, result);
            callbackCalled = true;
        });
    capture.reset();
    iterateUntil([&]() { return captureDestroyed.load(); });

    EXPECT_TRUE(callbackCalled.load());
    EXPECT_TRUE(guardedDuringDestruction.load());
    iterateUntil([]() {
        return !TdPollingBackend::hasActiveWorkers();
    });
}

TEST_F(TdPollingBackendTest, ReleasesReceiverCaptureAfterCleanClose)
{
    std::shared_ptr<ClientControl> client(new ClientControl());
    std::shared_ptr<ManualDeadlineControl> deadline(
        new ManualDeadlineControl());
    std::unique_ptr<TdPollingBackend> backend =
        makeBackend("capture", client, deadline);
    std::shared_ptr<int> capture(new int(1));
    std::weak_ptr<int> weakCapture(capture);

    ASSERT_EQ(
        TdPollingBackend::StartResult::Started,
        backend->start(
            [capture](
                std::uint64_t,
                TdTransport::ObjectPtr,
                TdTransport::DeliveryReceipt receipt) {
                receipt.settle(true);
            }));
    capture.reset();
    ASSERT_FALSE(weakCapture.expired());
    ASSERT_TRUE(client->waitForSent(1));
    finishCleanly(*backend, client);

    EXPECT_TRUE(weakCapture.expired());
}

TEST_F(TdPollingBackendTest, FactoryExceptionFailsStartWithoutWorker)
{
    std::shared_ptr<ManualDeadlineControl> deadline(
        new ManualDeadlineControl());
    TdPollingBackend backend(
        "factory-failure",
        CLOSE_TIMEOUT_SECONDS,
        POLL_TIMEOUT_SECONDS,
        []() -> std::unique_ptr<TdPollingClient> {
            throw std::runtime_error("synthetic factory failure");
        },
        [deadline](unsigned seconds) {
            return deadline->create(seconds);
        });

    EXPECT_EQ(
        TdPollingBackend::StartResult::Failed,
        backend.start(TdPollingBackend::Receiver()));
    EXPECT_FALSE(TdPollingBackend::isSessionCleanupPending(
        "factory-failure"));
    iterateUntil([]() {
        return !TdPollingBackend::hasActiveWorkers();
    });
}

TEST_F(
    TdPollingBackendTest,
    FinalizerFactoryFailurePrecedesClientCreationAndReleasesKey)
{
    std::shared_ptr<ClientControl> unusedClient(
        new ClientControl());
    unsigned clientFactoryCalls = 0;
    TdPollingBackend failedBackend(
        "finalizer-preparation-failure",
        CLOSE_TIMEOUT_SECONDS,
        POLL_TIMEOUT_SECONDS,
        [&]() {
            ++clientFactoryCalls;
            return std::unique_ptr<TdPollingClient>(
                new GatedClient(unusedClient));
        },
        TdPollingBackend::CloseTimeoutSourceFactory(),
        []() -> GSource * {
            throw std::runtime_error(
                "synthetic finalizer source failure");
        });

    EXPECT_EQ(
        TdPollingBackend::StartResult::Failed,
        failedBackend.start(TdPollingBackend::Receiver()));
    EXPECT_EQ(0u, clientFactoryCalls);
    EXPECT_FALSE(TdPollingBackend::isSessionCleanupPending(
        "finalizer-preparation-failure"));
    iterateUntil([]() {
        return !TdPollingBackend::hasActiveWorkers();
    });

    std::shared_ptr<ClientControl> replacementClient(
        new ClientControl());
    std::shared_ptr<ManualDeadlineControl> replacementDeadline(
        new ManualDeadlineControl());
    std::unique_ptr<TdPollingBackend> replacement =
        makeBackend(
            "finalizer-preparation-failure",
            replacementClient,
            replacementDeadline);
    ASSERT_EQ(
        TdPollingBackend::StartResult::Started,
        replacement->start(TdPollingBackend::Receiver()));
    ASSERT_TRUE(replacementClient->waitForSent(1));
    finishCleanly(*replacement, replacementClient);
}

TEST_F(
    TdPollingBackendTest,
    PreparesFinalizerBeforeConstructingClient)
{
    std::shared_ptr<ClientControl> client(new ClientControl());
    std::shared_ptr<ManualDeadlineControl> deadline(
        new ManualDeadlineControl());
    std::vector<std::string> events;
    m_clients.push_back(client);
    TdPollingBackend backend(
        "finalizer-preparation-order",
        CLOSE_TIMEOUT_SECONDS,
        POLL_TIMEOUT_SECONDS,
        [&]() {
            events.push_back("client");
            return std::unique_ptr<TdPollingClient>(
                new GatedClient(client));
        },
        [deadline](unsigned seconds) {
            return deadline->create(seconds);
        },
        [&]() {
            events.push_back("finalizer");
            return g_idle_source_new();
        });

    ASSERT_EQ(
        TdPollingBackend::StartResult::Started,
        backend.start(TdPollingBackend::Receiver()));
    EXPECT_EQ(
        (std::vector<std::string>{"finalizer", "client"}),
        events);
    ASSERT_TRUE(client->waitForSent(1));
    finishCleanly(backend, client);
}

TEST_F(TdPollingBackendTest, CloseRequestedDuringStartRunsAfterActivation)
{
    std::shared_ptr<ClientControl> client(new ClientControl());
    std::shared_ptr<ManualDeadlineControl> deadline(
        new ManualDeadlineControl());
    m_clients.push_back(client);
    TdPollingBackend *backendPointer = nullptr;
    std::vector<TdPollingBackend::CloseResult> results;
    std::unique_ptr<TdPollingBackend> backend(
        new TdPollingBackend(
            "close-during-start",
            CLOSE_TIMEOUT_SECONDS,
            POLL_TIMEOUT_SECONDS,
            [&]() {
                backendPointer->close(
                    [&](TdPollingBackend::CloseResult result) {
                        results.push_back(result);
                    });
                return std::unique_ptr<TdPollingClient>(
                    new GatedClient(client));
            },
            [deadline](unsigned seconds) {
                return deadline->create(seconds);
            }));
    backendPointer = backend.get();

    EXPECT_EQ(
        TdPollingBackend::StartResult::Started,
        backend->start(TdPollingBackend::Receiver()));
    ASSERT_TRUE(client->waitForSent(2));
    {
        std::lock_guard<std::mutex> lock(client->mutex);
        ASSERT_EQ(2u, client->sent.size());
        EXPECT_EQ(
            td_request_id::ACTIVATE,
            client->sent[0].requestId);
        EXPECT_EQ(td_request_id::CLOSE, client->sent[1].requestId);
    }

    client->push(makeClosed());
    ASSERT_TRUE(client->waitForDestroyed());
    iterateUntil([&]() { return results.size() == 1; });
    EXPECT_EQ(TdPollingBackend::CloseResult::Closed, results[0]);
}

TEST_F(TdPollingBackendTest, CloseRequestedDuringFailedStartIsResolved)
{
    std::shared_ptr<ClientControl> client(new ClientControl());
    std::shared_ptr<ManualDeadlineControl> deadline(
        new ManualDeadlineControl());
    m_clients.push_back(client);
    {
        std::lock_guard<std::mutex> lock(client->mutex);
        client->throwOnSend = true;
    }
    TdPollingBackend *backendPointer = nullptr;
    std::vector<TdPollingBackend::CloseResult> results;
    std::unique_ptr<TdPollingBackend> backend(
        new TdPollingBackend(
            "close-during-failed-start",
            CLOSE_TIMEOUT_SECONDS,
            POLL_TIMEOUT_SECONDS,
            [&]() {
                backendPointer->close(
                    [&](TdPollingBackend::CloseResult result) {
                        results.push_back(result);
                    });
                return std::unique_ptr<TdPollingClient>(
                    new GatedClient(client));
            },
            [deadline](unsigned seconds) {
                return deadline->create(seconds);
            }));
    backendPointer = backend.get();

    EXPECT_EQ(
        TdPollingBackend::StartResult::Failed,
        backend->start(TdPollingBackend::Receiver()));
    ASSERT_TRUE(client->waitForDestroyed());
    iterateUntil([&]() { return results.size() == 1; });

    EXPECT_EQ(TdPollingBackend::CloseResult::Failed, results[0]);
    iterateUntil([]() {
        return !TdPollingBackend::hasActiveWorkers();
    });
}

TEST_F(TdPollingBackendTest, ReceiveExceptionReportsFailedAndReaps)
{
    std::shared_ptr<ClientControl> client(new ClientControl());
    std::shared_ptr<ManualDeadlineControl> deadline(
        new ManualDeadlineControl());
    std::unique_ptr<TdPollingBackend> backend =
        makeBackend("receive-failure", client, deadline);
    std::vector<TdPollingBackend::CloseResult> results;
    std::atomic<unsigned> failureCount(0);
    const std::thread::id ownerThread = std::this_thread::get_id();
    client->throwOnReceive = true;

    ASSERT_EQ(
        TdPollingBackend::StartResult::Started,
        backend->start(
            TdPollingBackend::Receiver(),
            [&]() {
                EXPECT_EQ(
                    ownerThread, std::this_thread::get_id());
                ++failureCount;
            }));
    ASSERT_TRUE(client->waitForDestroyed());
    iterateUntil([&]() { return failureCount.load() == 1; });
    backend->close([&](TdPollingBackend::CloseResult result) {
        results.push_back(result);
    });
    iterateUntil([&]() { return results.size() == 1; });

    EXPECT_EQ(TdPollingBackend::CloseResult::Failed, results[0]);
    EXPECT_EQ(1u, failureCount.load());
    iterateUntil([]() {
        return !TdPollingBackend::hasActiveWorkers();
    });
}

TEST_F(
    TdPollingBackendTest,
    ReceiveFailureFallbackDoesNotWaitForClientDestruction)
{
    std::shared_ptr<ClientControl> client(new ClientControl());
    std::shared_ptr<ManualDeadlineControl> deadline(
        new ManualDeadlineControl());
    m_clients.push_back(client);
    TdPollingBackend backend(
        "receive-failure-fallback",
        CLOSE_TIMEOUT_SECONDS,
        POLL_TIMEOUT_SECONDS,
        [client]() {
            return std::unique_ptr<TdPollingClient>(
                new GatedClient(client));
        },
        [deadline](unsigned seconds) {
            return deadline->create(seconds);
        },
        TdPollingBackend::FinalizerSourceFactory(),
        []() -> GSource * {
            return nullptr;
        });
    std::atomic<unsigned> failureCount(0);
    std::vector<TdPollingBackend::CloseResult> results;
    const std::thread::id ownerThread = std::this_thread::get_id();
    client->throwOnReceive = true;
    client->blockDestruction();

    ASSERT_EQ(
        TdPollingBackend::StartResult::Started,
        backend.start(
            TdPollingBackend::Receiver(),
            [&]() {
                EXPECT_EQ(
                    ownerThread, std::this_thread::get_id());
                ++failureCount;
            }));
    ASSERT_TRUE(client->waitForDestructionEntered());
    iterateUntil([&]() { return failureCount.load() == 1; });

    backend.close([&](TdPollingBackend::CloseResult result) {
        results.push_back(result);
    });
    iterateUntil([&]() { return results.size() == 1; });
    EXPECT_EQ(TdPollingBackend::CloseResult::Failed, results[0]);
    EXPECT_EQ(1u, failureCount.load());
    EXPECT_TRUE(TdPollingBackend::hasActiveWorkers());
    EXPECT_TRUE(TdPollingBackend::isSessionCleanupPending(
        "receive-failure-fallback"));

    client->releaseDestruction();
    ASSERT_TRUE(client->waitForDestroyed());
    iterateUntil([]() {
        return !TdPollingBackend::hasActiveWorkers();
    });
}

TEST_F(
    TdPollingBackendTest,
    PendingLogicalFailureWaitsForRuntimeFailureHandoff)
{
    std::shared_ptr<ClientControl> client(new ClientControl());
    std::shared_ptr<BlockingFailureSourceFactory> failureFactory(
        new BlockingFailureSourceFactory());
    m_clients.push_back(client);
    TdPollingBackend backend(
        "logical-failure-handoff",
        CLOSE_TIMEOUT_SECONDS,
        POLL_TIMEOUT_SECONDS,
        [client]() {
            return std::unique_ptr<TdPollingClient>(
                new GatedClient(client));
        },
        [](unsigned) -> GSource * {
            return nullptr;
        },
        TdPollingBackend::FinalizerSourceFactory(),
        [failureFactory]() {
            return failureFactory->create();
        });
    std::vector<std::string> events;

    ASSERT_EQ(
        TdPollingBackend::StartResult::Started,
        backend.start(
            TdPollingBackend::Receiver(),
            [&]() { events.push_back("failure"); }));
    ASSERT_TRUE(client->waitForSent(1));
    ASSERT_TRUE(client->waitForReceiveCalls(1));
    backend.close([&](TdPollingBackend::CloseResult result) {
        EXPECT_EQ(TdPollingBackend::CloseResult::Failed, result);
        events.push_back("close");
    });
    ASSERT_TRUE(client->waitForSent(2));

    client->failNextReceive();
    ASSERT_TRUE(failureFactory->waitUntilEntered());
    EXPECT_TRUE(g_main_context_iteration(m_context, FALSE));
    EXPECT_TRUE(events.empty());

    failureFactory->release();
    ASSERT_TRUE(client->waitForDestroyed());
    iterateUntil([&]() { return events.size() == 2; });
    EXPECT_EQ(
        (std::vector<std::string>{"failure", "close"}),
        events);
    iterateUntil([]() {
        return !TdPollingBackend::hasActiveWorkers();
    });
}

TEST_F(
    TdPollingBackendTest,
    FailureFallbackPreservesPreviouslyReportedTimeout)
{
    std::shared_ptr<ClientControl> client(new ClientControl());
    std::shared_ptr<ManualDeadlineControl> deadline(
        new ManualDeadlineControl());
    m_clients.push_back(client);
    TdPollingBackend backend(
        "timeout-then-failure-fallback",
        CLOSE_TIMEOUT_SECONDS,
        POLL_TIMEOUT_SECONDS,
        [client]() {
            return std::unique_ptr<TdPollingClient>(
                new GatedClient(client));
        },
        [deadline](unsigned seconds) {
            return deadline->create(seconds);
        },
        TdPollingBackend::FinalizerSourceFactory(),
        []() -> GSource * {
            return nullptr;
        });
    std::vector<std::string> events;
    std::vector<TdPollingBackend::CloseResult> results;

    ASSERT_EQ(
        TdPollingBackend::StartResult::Started,
        backend.start(
            TdPollingBackend::Receiver(),
            [&]() { events.push_back("failure"); }));
    ASSERT_TRUE(client->waitForSent(1));
    ASSERT_TRUE(client->waitForReceiveCalls(1));
    backend.close([&](TdPollingBackend::CloseResult result) {
        results.push_back(result);
        events.push_back("first-close");
    });
    ASSERT_TRUE(client->waitForSent(2));
    ASSERT_TRUE(deadline->fireNext());
    iterateUntil([&]() { return results.size() == 1; });
    ASSERT_EQ(TdPollingBackend::CloseResult::TimedOut, results[0]);

    client->failNextReceive();
    ASSERT_TRUE(client->waitForDestroyed());
    backend.close([&](TdPollingBackend::CloseResult result) {
        results.push_back(result);
        events.push_back("second-close");
    });
    iterateUntil([&]() { return events.size() == 3; });

    ASSERT_EQ(2u, results.size());
    EXPECT_EQ(TdPollingBackend::CloseResult::TimedOut, results[1]);
    EXPECT_EQ(
        (std::vector<std::string>{
            "first-close", "failure", "second-close"}),
        events);
    iterateUntil([]() {
        return !TdPollingBackend::hasActiveWorkers();
    });
}

TEST_F(TdPollingBackendTest, SendExceptionFailsSessionWithoutEscapingWorker)
{
    std::shared_ptr<ClientControl> client(new ClientControl());
    std::shared_ptr<ManualDeadlineControl> deadline(
        new ManualDeadlineControl());
    std::unique_ptr<TdPollingBackend> backend =
        makeBackend("send-failure", client, deadline);
    std::vector<TdPollingBackend::CloseResult> results;
    std::atomic<unsigned> failureCount(0);

    ASSERT_EQ(
        TdPollingBackend::StartResult::Started,
        backend->start(
            TdPollingBackend::Receiver(),
            [&]() {
                ++failureCount;
                backend->close(
                    [&](TdPollingBackend::CloseResult result) {
                        results.push_back(result);
                    });
            }));
    ASSERT_TRUE(client->waitForSent(1));
    {
        std::lock_guard<std::mutex> lock(client->mutex);
        client->throwOnSend = true;
    }

    EXPECT_ANY_THROW(
        backend->sender()(19, make_object<getMe>()));
    client->failNextReceive();
    iterateUntil([&]() { return failureCount.load() == 1; });
    ASSERT_TRUE(client->waitForDestroyed());
    iterateUntil([&]() { return results.size() == 1; });
    EXPECT_EQ(TdPollingBackend::CloseResult::Failed, results[0]);
    EXPECT_EQ(1u, failureCount.load());
}

TEST_F(TdPollingBackendTest, ThrowingDeadlineFactoryReportsFailedButStillReaps)
{
    std::shared_ptr<ClientControl> client(new ClientControl());
    std::unique_ptr<TdPollingBackend> backend(
        new TdPollingBackend(
            "deadline-failure",
            CLOSE_TIMEOUT_SECONDS,
            POLL_TIMEOUT_SECONDS,
            [client]() {
                return std::unique_ptr<TdPollingClient>(
                    new GatedClient(client));
            },
            [](unsigned) -> GSource * {
                throw std::runtime_error(
                    "synthetic deadline failure");
            }));
    std::vector<TdPollingBackend::CloseResult> results;

    ASSERT_EQ(
        TdPollingBackend::StartResult::Started,
        backend->start(TdPollingBackend::Receiver()));
    ASSERT_TRUE(client->waitForSent(1));
    backend->close([&](TdPollingBackend::CloseResult result) {
        results.push_back(result);
    });
    iterateUntil([&]() { return results.size() == 1; });
    EXPECT_EQ(TdPollingBackend::CloseResult::Failed, results[0]);

    client->push(makeClosed());
    ASSERT_TRUE(client->waitForDestroyed());
    iterateUntil([]() {
        return !TdPollingBackend::hasActiveWorkers();
    });
    EXPECT_EQ(1u, results.size());
}

} // namespace
