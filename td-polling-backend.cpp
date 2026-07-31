#include "td-polling-backend.h"
#include "td-request-id.h"

#include <td/telegram/Client.h>

#include <atomic>
#include <cmath>
#include <condition_variable>
#include <map>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace {

GMainContext *captureThreadDefaultContext()
{
#if GLIB_CHECK_VERSION(2, 32, 0)
    return g_main_context_ref_thread_default();
#elif GLIB_CHECK_VERSION(2, 22, 0)
    GMainContext *context = g_main_context_get_thread_default();
    if (!context)
        context = g_main_context_default();
    return g_main_context_ref(context);
#else
    return g_main_context_ref(g_main_context_default());
#endif
}

class ClientManagerPollingClient final : public TdPollingClient {
public:
    ClientManagerPollingClient()
        : m_clientId(m_manager.create_client_id())
    {
    }

    void send(
        std::uint64_t requestId, FunctionPtr function) override
    {
        m_manager.send(
            m_clientId, requestId, std::move(function));
    }

    Response receive(double timeoutSeconds) override
    {
        td::ClientManager::Response response =
            m_manager.receive(timeoutSeconds);
        if (response.object &&
            response.client_id != m_clientId) {
            throw std::runtime_error(
                "TDLib response belongs to another client");
        }

        Response result;
        result.requestId = response.request_id;
        result.object = std::move(response.object);
        return result;
    }

private:
    td::ClientManager m_manager;
    td::ClientManager::ClientId m_clientId;
};

} // namespace

struct TdPollingWorkerRecord;

namespace {

std::atomic<std::size_t> &backendCallbackActivityCount()
{
    // A receipt may be settled on a helper thread while the captured context
    // tears down its worker source. Keep the unload guard process-lived until
    // the callback object has finished unwinding on that helper.
    static std::atomic<std::size_t> *count =
        new std::atomic<std::size_t>(0);
    return *count;
}

} // namespace

class BackendCallbackActivity {
public:
    BackendCallbackActivity()
    {
        backendCallbackActivityCount().fetch_add(
            1, std::memory_order_acq_rel);
    }

    ~BackendCallbackActivity()
    {
        backendCallbackActivityCount().fetch_sub(
            1, std::memory_order_acq_rel);
    }

    BackendCallbackActivity(const BackendCallbackActivity &) = delete;
    BackendCallbackActivity &operator=(
        const BackendCallbackActivity &) = delete;
};

class TdPollingBackendState {
public:
    TdPollingBackendState(
        std::string key,
        unsigned closeSeconds,
        double pollSeconds,
        TdPollingBackend::ClientFactory client,
        TdPollingBackend::CloseTimeoutSourceFactory timeout,
        TdPollingBackend::FinalizerSourceFactory finalizer,
        TdPollingBackend::FailureSourceFactory failure,
        TdPollingBackend::FinalizerSourceAttacher attacher,
        TdPollingBackend::ReaperReadyCallback reaperReady)
        : sessionKey(std::move(key)),
          closeTimeoutSeconds(closeSeconds),
          pollTimeoutSeconds(pollSeconds),
          clientFactory(std::move(client)),
          timeoutSourceFactory(
              std::make_shared<
                  TdPollingBackend::CloseTimeoutSourceFactory>(
                  std::move(timeout))),
          finalizerSourceFactory(std::move(finalizer)),
          failureSourceFactory(
              std::make_shared<
                  TdPollingBackend::FailureSourceFactory>(
                  std::move(failure))),
          finalizerSourceAttacher(
              std::make_shared<
                  TdPollingBackend::FinalizerSourceAttacher>(
                  std::move(attacher))),
          reaperReadyCallback(std::move(reaperReady)),
          context(captureThreadDefaultContext())
    {
    }

    ~TdPollingBackendState()
    {
        g_main_context_unref(context);
    }

    enum class Phase {
        New,
        Starting,
        Running,
        CloseRequested,
        Failed,
        PhysicallyStopped
    };

    const std::string sessionKey;
    const unsigned closeTimeoutSeconds;
    const double pollTimeoutSeconds;
    std::mutex mutex;
    Phase phase = Phase::New;
    bool closeSent = false;
    bool closeQueued = false;
    bool abortWorker = false;
    bool closedReceived = false;
    bool physicalFinished = false;
    bool closeRequestedDuringStart = false;
    bool runtimeFailureReported = false;
    bool logicalResultReported = false;
    bool logicalFailureDeliveryPending = false;
    bool logicalFailureSourceScheduled = false;
    bool finalizerUnavailable = false;
    bool closedDeliveryExpected = false;
    bool closedDeliverySettled = false;
    bool unexpectedClosedAwaitingReport = false;
    bool failureHandoffPending = false;
    bool failureFallbackPending = false;
    bool reaperReady = false;
    bool finalizationScheduled = false;
    TdPollingBackend::CloseResult logicalResult =
        TdPollingBackend::CloseResult::Failed;
    TdPollingBackend::CloseResult physicalResult =
        TdPollingBackend::CloseResult::Failed;
    std::shared_ptr<TdPollingBackend::Receiver> receiver;
    TdPollingBackend::FailureCallback runtimeFailureCallback;
    std::vector<TdPollingBackend::CloseCallback> closeCallbacks;
    TdPollingBackend::ClientFactory clientFactory;
    std::shared_ptr<
        TdPollingBackend::CloseTimeoutSourceFactory>
        timeoutSourceFactory;
    TdPollingBackend::FinalizerSourceFactory
        finalizerSourceFactory;
    std::shared_ptr<TdPollingBackend::FailureSourceFactory>
        failureSourceFactory;
    std::shared_ptr<TdPollingBackend::FinalizerSourceAttacher>
        finalizerSourceAttacher;
    TdPollingBackend::ReaperReadyCallback
        reaperReadyCallback;
    GMainContext *context;
    GSource *deadlineSource = nullptr;
    std::weak_ptr<TdPollingWorkerRecord> worker;
};

struct TdPollingWorkerRecord {
    explicit TdPollingWorkerRecord(
        std::shared_ptr<TdPollingBackendState> backendState)
        : state(std::move(backendState))
    {
    }

    std::shared_ptr<TdPollingBackendState> state;
    std::mutex sendMutex;
    std::unique_ptr<TdPollingClient> client;
    std::mutex gateMutex;
    std::condition_variable gateCondition;
    bool gateOpen = false;
    std::thread thread;
    std::thread reaperThread;
    std::mutex reaperExitMutex;
    std::condition_variable reaperExitCondition;
    std::mutex closedDeliveryExitMutex;
    GSource *finalizerSource = nullptr;
    std::shared_ptr<BackendCallbackActivity>
        closedDeliveryActivity;
    TdTransport::DeliveryReceipt::Callback
        closedDeliveryCallback;
};

namespace {

bool terminalDeliveryReady(
    const TdPollingBackendState &state)
{
    const bool receiptSettled =
        !state.closedDeliveryExpected ||
        state.closedDeliverySettled;
    return receiptSettled &&
           !state.failureHandoffPending &&
           !state.failureFallbackPending &&
           !state.unexpectedClosedAwaitingReport;
}

bool finalizerCanRun(
    const TdPollingBackendState &state)
{
    const bool receiptSettled =
        !state.closedDeliveryExpected ||
        state.closedDeliverySettled;
    return receiptSettled &&
           !state.failureHandoffPending;
}

class WorkerRegistry {
public:
    bool reserve(
        const std::string &key,
        const std::shared_ptr<TdPollingWorkerRecord> &record)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_records.emplace(key, record).second;
    }

    void removeIf(
        const std::string &key,
        const TdPollingWorkerRecord *expected)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto record = m_records.find(key);
        if (record != m_records.end() &&
            record->second.get() == expected) {
            m_records.erase(record);
        }
    }

    bool hasWorkers() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return !m_records.empty();
    }

    std::shared_ptr<TdPollingWorkerRecord> find(
        const std::string &key) const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto record = m_records.find(key);
        return record == m_records.end()
                   ? std::shared_ptr<TdPollingWorkerRecord>()
                   : record->second;
    }

private:
    mutable std::mutex m_mutex;
    std::map<
        std::string,
        std::shared_ptr<TdPollingWorkerRecord>> m_records;
};

WorkerRegistry &workerRegistry()
{
    // The registry is intentionally process-lived. A TDLib destructor can
    // wait indefinitely, so static destruction must never destroy a joinable
    // worker or a live client on the application thread.
    static WorkerRegistry *registry = new WorkerRegistry();
    return *registry;
}

bool isClosedUpdate(
    std::uint64_t requestId,
    const TdPollingClient::ObjectPtr &object)
{
    if (requestId != 0 || !object ||
        object->get_id() !=
            td::td_api::updateAuthorizationState::ID) {
        return false;
    }

    const auto &update =
        static_cast<
            const td::td_api::updateAuthorizationState &>(
            *object);
    return update.authorization_state_ &&
           update.authorization_state_->get_id() ==
               td::td_api::authorizationStateClosed::ID;
}

void destroyOwnedSource(GSource *source)
{
    if (!source)
        return;
    g_source_destroy(source);
    g_source_unref(source);
}

struct SourcePayload {
    std::shared_ptr<TdPollingBackendState> state;
    TdPollingBackend::CloseCallback closeCallback;
    TdPollingBackend::FailureCallback failureCallback;
    bool unexpectedClose = false;
    TdPollingBackend::CloseResult result =
        TdPollingBackend::CloseResult::Failed;
};

class SourcePayloadRegistry {
public:
    SourcePayload *publish(
        const std::shared_ptr<SourcePayload> &payload)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_payloads.emplace(payload.get(), payload);
        return payload.get();
    }

    std::shared_ptr<SourcePayload> acquire(
        SourcePayload *payload) const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto entry = m_payloads.find(payload);
        if (entry == m_payloads.end())
            return std::shared_ptr<SourcePayload>();
        return entry->second;
    }

    void discard(SourcePayload *payload)
    {
        std::shared_ptr<SourcePayload> retiredPayload;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            auto entry = m_payloads.find(payload);
            if (entry == m_payloads.end())
                return;
            retiredPayload.swap(entry->second);
            m_payloads.erase(entry);
            ++m_retiringPayloads;
        }

        // Callable and backend-state destructors are user-controlled and
        // may re-enter hasActiveWorkers() or destroy another source.
        retiredPayload.reset();

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            --m_retiringPayloads;
        }
    }

    bool hasPending() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return !m_payloads.empty() ||
               m_retiringPayloads != 0;
    }

private:
    mutable std::mutex m_mutex;
    std::map<
        SourcePayload *,
        std::shared_ptr<SourcePayload>> m_payloads;
    std::size_t m_retiringPayloads = 0;
};

SourcePayloadRegistry &sourcePayloadRegistry()
{
    static SourcePayloadRegistry *registry =
        new SourcePayloadRegistry();
    return *registry;
}

void discardSourcePayload(gpointer userData)
{
    sourcePayloadRegistry().discard(
        static_cast<SourcePayload *>(userData));
}

void invokeFailureCallback(
    TdPollingBackend::FailureCallback callback);

void completeRuntimeFailureDelivery(
    const std::shared_ptr<TdPollingBackendState> &state,
    bool unexpectedClose);

gboolean runResultCallback(gpointer userData)
{
    std::shared_ptr<SourcePayload> payload =
        sourcePayloadRegistry().acquire(
            static_cast<SourcePayload *>(userData));
    if (!payload)
        return FALSE;
    try {
        if (payload->closeCallback)
            payload->closeCallback(payload->result);
    } catch (...) {
    }
    return FALSE;
}

gboolean runFailureCallback(gpointer userData)
{
    std::shared_ptr<SourcePayload> payload =
        sourcePayloadRegistry().acquire(
            static_cast<SourcePayload *>(userData));
    if (!payload)
        return FALSE;

    TdPollingBackend::FailureCallback callback;
    callback.swap(payload->failureCallback);
    std::shared_ptr<TdPollingBackendState> state = payload->state;
    const bool unexpectedClose = payload->unexpectedClose;
    invokeFailureCallback(std::move(callback));
    if (state) {
        completeRuntimeFailureDelivery(
            state, unexpectedClose);
    }
    return FALSE;
}

bool scheduleFailureCallback(
    GMainContext *context,
    const std::shared_ptr<
        TdPollingBackend::FailureSourceFactory> &factory,
    const std::shared_ptr<TdPollingBackendState> &state,
    bool unexpectedClose,
    TdPollingBackend::FailureCallback &callback,
    gint priority = G_PRIORITY_DEFAULT)
{
    if (!callback)
        return true;

    GSource *source = nullptr;
    try {
        source = factory && *factory
                     ? (*factory)()
                     : g_idle_source_new();
    } catch (...) {
        return false;
    }
    if (!source)
        return false;
    if (g_source_is_destroyed(source) ||
        g_source_get_context(source) != nullptr) {
        g_source_unref(source);
        return false;
    }

    SourcePayload *token = nullptr;
    std::shared_ptr<SourcePayload> payload;
    try {
        payload = std::make_shared<SourcePayload>();
        payload->state = state;
        payload->failureCallback = std::move(callback);
        payload->unexpectedClose = unexpectedClose;
        token = sourcePayloadRegistry().publish(payload);
    } catch (...) {
        if (payload)
            payload->failureCallback.swap(callback);
        g_source_unref(source);
        return false;
    }

    g_source_set_priority(source, priority);
    g_source_set_callback(
        source,
        runFailureCallback,
        token,
        discardSourcePayload);
    if (g_source_attach(source, context) == 0) {
        payload->failureCallback.swap(callback);
        g_source_destroy(source);
        g_source_unref(source);
        return false;
    }
    g_source_unref(source);
    return true;
}

bool scheduleResultCallback(
    GMainContext *context,
    TdPollingBackend::CloseCallback callback,
    TdPollingBackend::CloseResult result)
{
    if (!callback)
        return true;

    SourcePayload *token = nullptr;
    try {
        std::shared_ptr<SourcePayload> payload =
            std::make_shared<SourcePayload>();
        payload->closeCallback = std::move(callback);
        payload->result = result;
        token = sourcePayloadRegistry().publish(payload);
    } catch (...) {
        return false;
    }

    GSource *source = g_idle_source_new();
    g_source_set_priority(source, G_PRIORITY_DEFAULT);
    g_source_set_callback(
        source,
        runResultCallback,
        token,
        discardSourcePayload);
    if (g_source_attach(source, context) == 0) {
        g_source_destroy(source);
        g_source_unref(source);
        return false;
    }
    g_source_unref(source);
    return true;
}

SourcePayload *publishStatePayload(
    const std::shared_ptr<TdPollingBackendState> &state)
{
    std::shared_ptr<SourcePayload> payload =
        std::make_shared<SourcePayload>();
    payload->state = state;
    return sourcePayloadRegistry().publish(payload);
}

void invokeCallbacks(
    std::vector<TdPollingBackend::CloseCallback> callbacks,
    TdPollingBackend::CloseResult result)
{
    for (auto &callback: callbacks) {
        try {
            if (callback)
                callback(result);
        } catch (...) {
        }
    }
}

TdPollingBackend::FailureCallback takeFailureFallbackLocked(
    TdPollingBackendState &state)
{
    TdPollingBackend::FailureCallback callback;
    if (!state.failureFallbackPending)
        return callback;

    state.failureFallbackPending = false;
    state.failureHandoffPending = false;
    state.unexpectedClosedAwaitingReport = false;
    state.runtimeFailureReported = true;
    callback.swap(state.runtimeFailureCallback);
    return callback;
}

void invokeFailureCallback(
    TdPollingBackend::FailureCallback callback)
{
    try {
        if (callback)
            callback();
    } catch (...) {
    }
}

gboolean finalizeWorker(gpointer userData)
{
    std::shared_ptr<SourcePayload> payload =
        sourcePayloadRegistry().acquire(
            static_cast<SourcePayload *>(userData));
    if (!payload || !payload->state)
        return FALSE;
    std::shared_ptr<TdPollingBackendState> state =
        payload->state;
    std::shared_ptr<TdPollingWorkerRecord> record =
        state->worker.lock();
    if (!record)
        return FALSE;

    // notify_all_at_thread_exit releases this latch only after the reaper
    // has completely exited. Never wait for thread scheduling on the
    // captured context.
    std::unique_lock<std::mutex> reaperExitLock(
        record->reaperExitMutex, std::try_to_lock);
    if (!reaperExitLock.owns_lock())
        return TRUE;

    // A receipt may attach this source from a helper thread. Do not retire
    // the worker or its source payload until that callback has returned from
    // all backend settlement and attachment code.
    std::unique_lock<std::mutex> deliveryExitLock(
        record->closedDeliveryExitMutex, std::try_to_lock);
    if (!deliveryExitLock.owns_lock())
        return TRUE;

    if (record->reaperThread.joinable())
        record->reaperThread.join();

    GSource *deadline = nullptr;
    TdPollingBackend::CloseResult result =
        TdPollingBackend::CloseResult::Failed;
    std::vector<TdPollingBackend::CloseCallback> callbacks;
    std::shared_ptr<TdPollingBackend::Receiver> receiver;
    TdPollingBackend::FailureCallback failureCallback;
    TdPollingBackend::FailureCallback fallbackFailure;
    TdPollingBackend::ClientFactory clientFactory;
    std::shared_ptr<
        TdPollingBackend::CloseTimeoutSourceFactory>
        timeoutSourceFactory;
    std::shared_ptr<TdPollingBackend::FailureSourceFactory>
        failureSourceFactory;
    std::shared_ptr<TdPollingBackend::FinalizerSourceAttacher>
        finalizerSourceAttacher;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        if (state->failureHandoffPending)
            return TRUE;

        deadline = state->deadlineSource;
        state->deadlineSource = nullptr;
        result = state->physicalResult;
        state->phase = TdPollingBackendState::Phase::PhysicallyStopped;
        if (state->logicalFailureDeliveryPending) {
            state->logicalFailureDeliveryPending = false;
            state->logicalFailureSourceScheduled = false;
            result = state->logicalResult;
            callbacks.swap(state->closeCallbacks);
        } else if (!state->logicalResultReported) {
            state->logicalResultReported = true;
            state->logicalResult = result;
            callbacks.swap(state->closeCallbacks);
        }
        receiver.swap(state->receiver);
        fallbackFailure =
            takeFailureFallbackLocked(*state);
        failureCallback.swap(state->runtimeFailureCallback);
        clientFactory.swap(state->clientFactory);
        timeoutSourceFactory.swap(
            state->timeoutSourceFactory);
        failureSourceFactory.swap(
            state->failureSourceFactory);
        finalizerSourceAttacher.swap(
            state->finalizerSourceAttacher);
    }

    destroyOwnedSource(deadline);
    workerRegistry().removeIf(state->sessionKey, record.get());
    invokeFailureCallback(std::move(fallbackFailure));
    invokeCallbacks(std::move(callbacks), result);
    return FALSE;
}

bool scheduleWorkerFinalization(
    const std::shared_ptr<TdPollingWorkerRecord> &record)
{
    GSource *source = record->finalizerSource;
    if (!source)
        return false;

    std::shared_ptr<TdPollingBackend::FinalizerSourceAttacher>
        attacher;
    {
        std::lock_guard<std::mutex> lock(record->state->mutex);
        attacher = record->state->finalizerSourceAttacher;
    }
    guint sourceId = 0;
    try {
        sourceId =
            attacher && *attacher
                ? (*attacher)(
                      source, record->state->context)
                : g_source_attach(
                      source, record->state->context);
    } catch (...) {
        return false;
    }
    if (sourceId == 0)
        return false;

    record->finalizerSource = nullptr;
    g_source_unref(source);
    return true;
}

GSource *prepareWorkerFinalization(
    const std::shared_ptr<TdPollingBackendState> &state)
{
    TdPollingBackend::FinalizerSourceFactory factory;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        factory.swap(state->finalizerSourceFactory);
    }

    GSource *source = nullptr;
    try {
        source = factory ? factory() : g_idle_source_new();
    } catch (...) {
        return nullptr;
    }
    if (!source)
        return nullptr;
    if (g_source_is_destroyed(source) ||
        g_source_get_context(source) != nullptr) {
        g_source_unref(source);
        return nullptr;
    }

    SourcePayload *token = nullptr;
    try {
        token = publishStatePayload(state);
    } catch (...) {
        g_source_unref(source);
        return nullptr;
    }

    // The terminal delivery receipt prevents this source from being attached
    // until that delivery is settled. Normal priority then avoids starving
    // cleanup behind a continuously busy context.
    g_source_set_priority(source, G_PRIORITY_DEFAULT);
    g_source_set_callback(
        source,
        finalizeWorker,
        token,
        discardSourcePayload);
    return source;
}

gboolean reportDeadline(gpointer userData)
{
    std::shared_ptr<SourcePayload> payload =
        sourcePayloadRegistry().acquire(
            static_cast<SourcePayload *>(userData));
    if (!payload || !payload->state)
        return FALSE;
    std::shared_ptr<TdPollingBackendState> state =
        payload->state;
    GSource *sourceToRelease = nullptr;
    std::vector<TdPollingBackend::CloseCallback> callbacks;
    TdPollingBackend::FailureCallback fallbackFailure;
    bool reportResult = false;
    TdPollingBackend::CloseResult result =
        TdPollingBackend::CloseResult::TimedOut;

    {
        std::lock_guard<std::mutex> lock(state->mutex);
        if (state->deadlineSource != g_main_current_source())
            return FALSE;

        sourceToRelease = state->deadlineSource;
        state->deadlineSource = nullptr;
        if (state->logicalFailureDeliveryPending) {
            if (state->failureHandoffPending) {
                // Runtime failure notification is still being prepared,
                // queued, or invoked. Settle the current close callbacks
                // independently so that notification cannot defeat the
                // frontend deadline. A failed source handoff arranges its
                // own fallback delivery.
                state->logicalFailureDeliveryPending = false;
                state->logicalFailureSourceScheduled = false;
                state->logicalResult =
                    TdPollingBackend::CloseResult::TimedOut;
                result = state->logicalResult;
                callbacks.swap(state->closeCallbacks);
                reportResult = true;
            } else {
                state->logicalFailureDeliveryPending = false;
                state->logicalFailureSourceScheduled = false;
                fallbackFailure =
                    takeFailureFallbackLocked(*state);
                result = state->logicalResult;
                callbacks.swap(state->closeCallbacks);
                reportResult = true;
            }
        } else if (!state->logicalResultReported) {
            fallbackFailure =
                takeFailureFallbackLocked(*state);
            const bool deliverySettled =
                terminalDeliveryReady(*state);
            result =
                state->physicalFinished && deliverySettled
                         ? state->physicalResult
                         : TdPollingBackend::CloseResult::TimedOut;
            state->logicalResultReported = true;
            state->logicalResult = result;
            callbacks.swap(state->closeCallbacks);
            reportResult = true;
        }
    }

    if (sourceToRelease)
        g_source_unref(sourceToRelease);
    invokeFailureCallback(std::move(fallbackFailure));
    if (reportResult)
        invokeCallbacks(std::move(callbacks), result);
    return FALSE;
}

GSource *createDeadlineSource(
    const std::shared_ptr<TdPollingBackendState> &state)
{
    std::shared_ptr<
        TdPollingBackend::CloseTimeoutSourceFactory> factory;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        factory = state->timeoutSourceFactory;
    }

    GSource *source = nullptr;
    try {
        source = factory && *factory
                     ? (*factory)(state->closeTimeoutSeconds)
                     : g_timeout_source_new_seconds(
                           state->closeTimeoutSeconds);
    } catch (...) {
        return nullptr;
    }
    if (!source)
        return nullptr;
    if (g_source_is_destroyed(source) ||
        g_source_get_context(source) != nullptr) {
        g_source_unref(source);
        return nullptr;
    }
    return source;
}

enum class DeadlineInstallResult {
    Installed,
    NotNeeded,
    Failed
};

GSource *prepareDeadline(
    const std::shared_ptr<TdPollingBackendState> &state)
{
    GSource *source = createDeadlineSource(state);
    if (!source)
        return nullptr;

    SourcePayload *token = nullptr;
    try {
        token = publishStatePayload(state);
    } catch (...) {
        g_source_unref(source);
        return nullptr;
    }

    g_source_set_priority(source, G_PRIORITY_DEFAULT);
    g_source_set_callback(
        source,
        reportDeadline,
        token,
        discardSourcePayload);
    return source;
}

DeadlineInstallResult installPreparedDeadline(
    const std::shared_ptr<TdPollingBackendState> &state,
    GSource *source)
{
    bool installed = false;
    bool needed = true;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        const bool physicalResultReady =
            state->physicalFinished &&
            terminalDeliveryReady(*state);
        needed =
            !physicalResultReady &&
            !state->logicalResultReported;
        if (needed && !state->deadlineSource) {
            state->deadlineSource = source;
            if (g_source_attach(source, state->context) != 0) {
                installed = true;
            } else {
                state->deadlineSource = nullptr;
            }
        }
    }

    if (installed)
        return DeadlineInstallResult::Installed;

    g_source_destroy(source);
    g_source_unref(source);
    return needed
               ? DeadlineInstallResult::Failed
               : DeadlineInstallResult::NotNeeded;
}

bool deliverLogicalFailure(
    const std::shared_ptr<TdPollingBackendState> &state)
{
    std::vector<TdPollingBackend::CloseCallback> callbacks;
    TdPollingBackend::FailureCallback fallbackFailure;
    TdPollingBackend::CloseResult result =
        TdPollingBackend::CloseResult::Failed;
    bool reportResult = false;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        if (state->failureHandoffPending) {
            state->logicalFailureSourceScheduled = false;
            return false;
        }
        if (!state->logicalFailureDeliveryPending)
            return true;
        state->logicalFailureDeliveryPending = false;
        state->logicalFailureSourceScheduled = false;
        fallbackFailure =
            takeFailureFallbackLocked(*state);
        result = state->logicalResult;
        callbacks.swap(state->closeCallbacks);
        reportResult = true;
    }

    invokeFailureCallback(std::move(fallbackFailure));
    if (reportResult) {
        invokeCallbacks(
            std::move(callbacks),
            result);
    }
    return true;
}

gboolean reportLogicalFailureOnContext(gpointer userData)
{
    std::shared_ptr<SourcePayload> payload =
        sourcePayloadRegistry().acquire(
            static_cast<SourcePayload *>(userData));
    if (!payload || !payload->state)
        return FALSE;

    deliverLogicalFailure(payload->state);
    return FALSE;
}

void scheduleLogicalFailureDelivery(
    const std::shared_ptr<TdPollingBackendState> &state)
{
    GSource *source = g_idle_source_new();
    if (!source) {
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            state->logicalFailureSourceScheduled = false;
        }
        if (g_main_context_is_owner(state->context))
            deliverLogicalFailure(state);
        return;
    }

    SourcePayload *token = nullptr;
    try {
        token = publishStatePayload(state);
    } catch (...) {
    }
    if (!token) {
        g_source_unref(source);
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            state->logicalFailureSourceScheduled = false;
        }
        if (g_main_context_is_owner(state->context))
            deliverLogicalFailure(state);
        return;
    }

    g_source_set_priority(source, G_PRIORITY_DEFAULT);
    g_source_set_callback(
        source,
        reportLogicalFailureOnContext,
        token,
        discardSourcePayload);
    if (g_source_attach(source, state->context) == 0) {
        g_source_destroy(source);
        g_source_unref(source);
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            state->logicalFailureSourceScheduled = false;
        }
        if (g_main_context_is_owner(state->context))
            deliverLogicalFailure(state);
        return;
    }
    g_source_unref(source);
}

void reportLogicalFailure(
    const std::shared_ptr<TdPollingBackendState> &state)
{
    bool scheduleDelivery = false;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        const bool failureDeliveryNeeded =
            state->failureFallbackPending;
        if (state->logicalResultReported &&
            !state->logicalFailureDeliveryPending &&
            !failureDeliveryNeeded) {
            return;
        }
        if (!state->logicalResultReported) {
            state->logicalResultReported = true;
            state->logicalResult =
                TdPollingBackend::CloseResult::Failed;
        }
        state->logicalFailureDeliveryPending = true;
        if (!state->failureHandoffPending &&
            !state->logicalFailureSourceScheduled) {
            state->logicalFailureSourceScheduled = true;
            scheduleDelivery = true;
        }
    }

    if (scheduleDelivery)
        scheduleLogicalFailureDelivery(state);
}

bool beginRuntimeFailureHandoffLocked(
    TdPollingBackendState &state)
{
    if (state.runtimeFailureReported ||
        state.failureHandoffPending ||
        state.failureFallbackPending) {
        return false;
    }

    state.failureHandoffPending = true;
    return true;
}

void attachPreparedFinalizer(
    const std::shared_ptr<TdPollingWorkerRecord> &record);

bool claimReadyFinalizerLocked(
    TdPollingBackendState &state)
{
    if (!state.reaperReady ||
        !finalizerCanRun(state) ||
        state.finalizationScheduled) {
        return false;
    }

    state.finalizationScheduled = true;
    return true;
}

void completeRuntimeFailureDelivery(
    const std::shared_ptr<TdPollingBackendState> &state,
    bool unexpectedClose)
{
    bool attachFinalizer = false;
    bool scheduleLogicalFailure = false;
    std::shared_ptr<TdPollingWorkerRecord> record;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        if (!state->failureHandoffPending)
            return;

        state->runtimeFailureReported = true;
        if (unexpectedClose)
            state->unexpectedClosedAwaitingReport = false;
        state->failureHandoffPending = false;
        if (state->logicalFailureDeliveryPending &&
            !state->logicalFailureSourceScheduled) {
            state->logicalFailureSourceScheduled = true;
            scheduleLogicalFailure = true;
        }
        attachFinalizer = claimReadyFinalizerLocked(*state);
        if (attachFinalizer)
            record = state->worker.lock();
    }

    if (scheduleLogicalFailure)
        scheduleLogicalFailureDelivery(state);
    if (attachFinalizer && record)
        attachPreparedFinalizer(record);
}

void finishRuntimeFailureHandoff(
    const std::shared_ptr<TdPollingBackendState> &state,
    bool unexpectedClose,
    bool invokeDirectlyOnOwner)
{
    TdPollingBackend::FailureCallback callback;
    std::shared_ptr<TdPollingBackend::FailureSourceFactory>
        sourceFactory;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        if (!state->failureHandoffPending)
            return;
        callback.swap(state->runtimeFailureCallback);
        sourceFactory = state->failureSourceFactory;
    }

    if (invokeDirectlyOnOwner &&
        g_main_context_is_owner(state->context)) {
        invokeFailureCallback(std::move(callback));
        completeRuntimeFailureDelivery(
            state, unexpectedClose);
        return;
    }
    if (!callback) {
        completeRuntimeFailureDelivery(
            state, unexpectedClose);
        return;
    }

    // High priority minimizes notification latency. Correct ordering does
    // not depend on priority: failureHandoffPending remains set until the
    // source actually invokes the application callback.
    const bool handedOff = scheduleFailureCallback(
        state->context,
        sourceFactory,
        state,
        unexpectedClose,
        callback,
        G_PRIORITY_HIGH);
    if (handedOff)
        return;

    bool attachFinalizer = false;
    bool scheduleLogicalFailure = false;
    std::shared_ptr<TdPollingWorkerRecord> record;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        if (!state->runtimeFailureCallback)
            state->runtimeFailureCallback.swap(callback);
        state->failureFallbackPending = true;
        if (state->logicalResultReported)
            state->logicalFailureDeliveryPending = true;
        state->failureHandoffPending = false;
        if (state->logicalFailureDeliveryPending &&
            !state->logicalFailureSourceScheduled) {
            state->logicalFailureSourceScheduled = true;
            scheduleLogicalFailure = true;
        }
        attachFinalizer = claimReadyFinalizerLocked(*state);
        if (attachFinalizer)
            record = state->worker.lock();
    }

    if (scheduleLogicalFailure)
        scheduleLogicalFailureDelivery(state);
    if (attachFinalizer && record)
        attachPreparedFinalizer(record);
}

void markFinalizerUnavailable(
    const std::shared_ptr<TdPollingWorkerRecord> &record)
{
    std::lock_guard<std::mutex> lock(record->state->mutex);
    // Retain the record and source process-wide so unload and same-key reuse
    // remain blocked if the validated source cannot be attached.
    record->state->finalizerUnavailable = true;
}

void attachPreparedFinalizer(
    const std::shared_ptr<TdPollingWorkerRecord> &record)
{
    if (!scheduleWorkerFinalization(record)) {
        markFinalizerUnavailable(record);
        reportLogicalFailure(record->state);
    }
}

void settleClosedDelivery(
    const std::shared_ptr<TdPollingWorkerRecord> &record,
    bool)
{
    bool attachFinalizer = false;
    bool reportUnexpectedClose = false;
    {
        std::lock_guard<std::mutex> lock(record->state->mutex);
        TdPollingBackendState &state = *record->state;
        if (!state.closedDeliveryExpected ||
            state.closedDeliverySettled) {
            return;
        }

        state.closedDeliverySettled = true;
        reportUnexpectedClose =
            state.unexpectedClosedAwaitingReport &&
            beginRuntimeFailureHandoffLocked(state);
        if (!reportUnexpectedClose)
            attachFinalizer =
                claimReadyFinalizerLocked(state);
    }

    if (reportUnexpectedClose) {
        finishRuntimeFailureHandoff(
            record->state, true, true);
        return;
    }
    if (attachFinalizer)
        attachPreparedFinalizer(record);
}

void markReaperReady(
    const std::shared_ptr<TdPollingWorkerRecord> &record)
{
    bool attachFinalizer = false;
    TdPollingBackend::ReaperReadyCallback readyCallback;
    {
        std::lock_guard<std::mutex> lock(record->state->mutex);
        TdPollingBackendState &state = *record->state;
        state.reaperReady = true;
        readyCallback.swap(state.reaperReadyCallback);
        attachFinalizer =
            claimReadyFinalizerLocked(state);
    }

    try {
        if (readyCallback)
            readyCallback();
    } catch (...) {
    }
    if (attachFinalizer)
        attachPreparedFinalizer(record);
}

bool markSendFailure(
    const std::shared_ptr<TdPollingBackendState> &state);

bool sendQueuedClose(
    const std::shared_ptr<TdPollingWorkerRecord> &record)
{
    std::shared_ptr<TdPollingBackendState> state = record->state;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        if (!state->closeQueued)
            return !state->abortWorker;
    }

    std::lock_guard<std::mutex> sendLock(record->sendMutex);
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        if (!state->closeQueued)
            return !state->abortWorker;

        state->closeQueued = false;
        if (state->abortWorker ||
            state->closedReceived ||
            state->physicalFinished ||
            !record->client) {
            return !state->abortWorker;
        }
        state->closeSent = true;
    }

    try {
        record->client->send(
            td_request_id::CLOSE,
            td::td_api::make_object<td::td_api::close>());
    } catch (...) {
        markSendFailure(state);
        return false;
    }
    return true;
}

void runPollingWorker(
    const std::shared_ptr<TdPollingWorkerRecord> &record)
{
    std::shared_ptr<TdPollingBackendState> state = record->state;
    {
        std::unique_lock<std::mutex> lock(record->gateMutex);
        record->gateCondition.wait(
            lock, [&]() { return record->gateOpen; });
    }

    TdPollingBackend::CloseResult physicalResult =
        TdPollingBackend::CloseResult::Failed;
    bool shouldPoll = false;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        shouldPoll = !state->abortWorker;
    }

    while (shouldPoll) {
        if (!sendQueuedClose(record))
            break;

        TdPollingClient::Response response;
        try {
            response =
                record->client->receive(state->pollTimeoutSeconds);
        } catch (...) {
            bool reportFailure = false;
            {
                std::lock_guard<std::mutex> lock(state->mutex);
                state->abortWorker = true;
                state->phase = TdPollingBackendState::Phase::Failed;
                reportFailure =
                    beginRuntimeFailureHandoffLocked(*state);
            }
            if (reportFailure) {
                finishRuntimeFailureHandoff(
                    state, false, false);
            }
            // Failure notification and logical close settlement must not
            // depend on the TDLib client destructor or reaper completing.
            // The destructor may block indefinitely, while the process-lived
            // worker record continues to guard unload and storage reuse.
            reportLogicalFailure(state);
            break;
        }

        {
            std::lock_guard<std::mutex> lock(state->mutex);
            if (state->abortWorker)
                break;
        }

        if (!response.object)
            continue;

        if (td_request_id::isControl(response.requestId))
            continue;

        const bool closed =
            isClosedUpdate(response.requestId, response.object);
        std::shared_ptr<TdPollingBackend::Receiver> receiver;
        TdTransport::DeliveryReceipt deliveryReceipt;
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            if (closed) {
                state->unexpectedClosedAwaitingReport =
                    state->phase ==
                    TdPollingBackendState::Phase::Running;
                state->closedReceived = true;
                state->closedDeliveryExpected = true;
                deliveryReceipt =
                    TdTransport::DeliveryReceipt(
                        std::move(
                            record->closedDeliveryCallback));
            }
            receiver = state->receiver;
        }
        try {
            if (receiver && *receiver) {
                (*receiver)(
                    response.requestId,
                    std::move(response.object),
                    std::move(deliveryReceipt));
            }
        } catch (...) {
        }

        if (closed) {
            physicalResult =
                TdPollingBackend::CloseResult::Closed;
            break;
        }
    }

    std::unique_ptr<TdPollingClient> client;
    {
        std::lock_guard<std::mutex> lock(record->sendMutex);
        client = std::move(record->client);
    }
    client.reset();

    {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->physicalResult = physicalResult;
        state->physicalFinished = true;
        state->phase = TdPollingBackendState::Phase::PhysicallyStopped;
    }
}

bool markSendFailure(
    const std::shared_ptr<TdPollingBackendState> &state)
{
    bool reportFailure = false;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        if (state->closedReceived ||
            state->physicalFinished) {
            return false;
        }
        state->abortWorker = true;
        state->phase = TdPollingBackendState::Phase::Failed;
        reportFailure =
            beginRuntimeFailureHandoffLocked(*state);
    }
    if (reportFailure)
        finishRuntimeFailureHandoff(
            state, false, false);
    reportLogicalFailure(state);
    return true;
}

void runPollingReaper(
    const std::shared_ptr<TdPollingWorkerRecord> &record)
{
    std::unique_lock<std::mutex> exitLock(
        record->reaperExitMutex);
    if (record->thread.joinable())
        record->thread.join();
    markReaperReady(record);
    std::notify_all_at_thread_exit(
        record->reaperExitCondition, std::move(exitLock));
}

} // namespace

TdPollingBackend::TdPollingBackend(
    std::string sessionKey,
    unsigned closeTimeoutSeconds,
    double pollTimeoutSeconds,
    ClientFactory clientFactory,
    CloseTimeoutSourceFactory closeTimeoutSourceFactory,
    FinalizerSourceFactory finalizerSourceFactory,
    FailureSourceFactory failureSourceFactory,
    FinalizerSourceAttacher finalizerSourceAttacher,
    ReaperReadyCallback reaperReadyCallback)
{
    if (sessionKey.empty())
        throw std::invalid_argument(
            "TDLib polling session key must not be empty");
    if (!std::isfinite(pollTimeoutSeconds) ||
        pollTimeoutSeconds <= 0.0) {
        throw std::invalid_argument(
            "TDLib polling timeout must be finite and positive");
    }

    if (!clientFactory) {
        clientFactory = []() {
            return std::unique_ptr<TdPollingClient>(
                new ClientManagerPollingClient());
        };
    }

    m_state = std::make_shared<TdPollingBackendState>(
        std::move(sessionKey),
        closeTimeoutSeconds,
        pollTimeoutSeconds,
        std::move(clientFactory),
        std::move(closeTimeoutSourceFactory),
        std::move(finalizerSourceFactory),
        std::move(failureSourceFactory),
        std::move(finalizerSourceAttacher),
        std::move(reaperReadyCallback));
}

TdPollingBackend::~TdPollingBackend()
{
    close();
    m_state.reset();
}

TdPollingBackend::Sender TdPollingBackend::sender() const
{
    std::weak_ptr<TdPollingBackendState> weakState(m_state);
    return [weakState](
               std::uint64_t requestId,
               TdTransport::FunctionPtr function) {
        std::shared_ptr<TdPollingBackendState> state =
            weakState.lock();
        if (!state || !function)
            throw std::runtime_error(
                "TDLib polling session is unavailable");
        if (!td_request_id::isPublic(requestId))
            throw std::runtime_error(
                "TDLib request identifier is reserved");

        std::shared_ptr<TdPollingWorkerRecord> record;
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            if (state->phase !=
                    TdPollingBackendState::Phase::Running ||
                state->abortWorker ||
                state->closedReceived ||
                state->physicalFinished) {
                throw std::runtime_error(
                    "TDLib polling session is not running");
            }
            record = state->worker.lock();
        }
        if (!record)
            throw std::runtime_error(
                "TDLib polling session is unavailable");

        std::lock_guard<std::mutex> sendLock(record->sendMutex);
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            if (state->phase !=
                    TdPollingBackendState::Phase::Running ||
                state->abortWorker ||
                state->closedReceived ||
                state->physicalFinished ||
                !record->client) {
                throw std::runtime_error(
                    "TDLib polling session is not running");
            }
        }

        try {
            record->client->send(
                requestId, std::move(function));
        } catch (...) {
            markSendFailure(state);
            throw;
        }
    };
}

TdPollingBackend::StartResult TdPollingBackend::start(
    Receiver receiver,
    FailureCallback failureCallback)
{
    std::shared_ptr<TdPollingBackendState> state = m_state;
    std::shared_ptr<Receiver> receiverHolder;
    try {
        receiverHolder =
            std::make_shared<Receiver>(std::move(receiver));
    } catch (...) {
        return StartResult::Failed;
    }

    {
        std::lock_guard<std::mutex> lock(state->mutex);
        if (state->phase != TdPollingBackendState::Phase::New)
            return StartResult::Failed;
        state->phase = TdPollingBackendState::Phase::Starting;
    }

    std::shared_ptr<TdPollingWorkerRecord> record;
    try {
        record =
            std::make_shared<TdPollingWorkerRecord>(state);
        record->closedDeliveryActivity =
            std::make_shared<BackendCallbackActivity>();
        std::weak_ptr<TdPollingWorkerRecord> weakRecord(record);
        std::shared_ptr<BackendCallbackActivity> callbackActivity =
            record->closedDeliveryActivity;
        record->closedDeliveryCallback =
            [weakRecord, callbackActivity](bool delivered) {
                (void)callbackActivity;
                std::shared_ptr<TdPollingWorkerRecord> lockedRecord =
                    weakRecord.lock();
                if (lockedRecord) {
                    std::lock_guard<std::mutex> exitLock(
                        lockedRecord->closedDeliveryExitMutex);
                    settleClosedDelivery(
                        lockedRecord, delivered);
                }
            };
    } catch (...) {
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            state->phase = TdPollingBackendState::Phase::Failed;
            state->closeRequestedDuringStart = false;
        }
        reportLogicalFailure(state);
        return StartResult::Failed;
    }
    if (!workerRegistry().reserve(state->sessionKey, record)) {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->phase = TdPollingBackendState::Phase::New;
        return StartResult::SessionBusy;
    }

    record->finalizerSource =
        prepareWorkerFinalization(state);
    if (!record->finalizerSource) {
        workerRegistry().removeIf(
            state->sessionKey, record.get());
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            state->phase = TdPollingBackendState::Phase::Failed;
            state->closeRequestedDuringStart = false;
        }
        reportLogicalFailure(state);
        return StartResult::Failed;
    }

    try {
        record->client = state->clientFactory();
    } catch (...) {
    }
    if (!record->client) {
        destroyOwnedSource(record->finalizerSource);
        record->finalizerSource = nullptr;
        workerRegistry().removeIf(
            state->sessionKey, record.get());
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            state->phase = TdPollingBackendState::Phase::Failed;
            state->closeRequestedDuringStart = false;
        }
        reportLogicalFailure(state);
        return StartResult::Failed;
    }

    {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->receiver = std::move(receiverHolder);
        state->runtimeFailureCallback =
            std::move(failureCallback);
        state->worker = record;
    }

    try {
        record->thread =
            std::thread([record]() { runPollingWorker(record); });
    } catch (...) {
        std::shared_ptr<Receiver> retiredReceiver;
        FailureCallback retiredFailureCallback;
        ClientFactory retiredClientFactory;
        std::shared_ptr<CloseTimeoutSourceFactory>
            retiredTimeoutSourceFactory;
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            state->abortWorker = true;
            state->phase = TdPollingBackendState::Phase::Failed;
            state->closeRequestedDuringStart = false;
            retiredReceiver.swap(state->receiver);
            retiredFailureCallback.swap(
                state->runtimeFailureCallback);
            retiredClientFactory.swap(state->clientFactory);
            retiredTimeoutSourceFactory.swap(
                state->timeoutSourceFactory);
        }
        // A TdPollingClient destructor has no bounded-time guarantee. Keep
        // the reserved record process-lived instead of destroying the client
        // on the frontend thread. This also keeps unload and same-key starts
        // blocked after thread-resource exhaustion.
        reportLogicalFailure(state);
        return StartResult::Failed;
    }

    try {
        record->reaperThread =
            std::thread([record]() {
                runPollingReaper(record);
            });
    } catch (...) {
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            state->abortWorker = true;
            state->phase = TdPollingBackendState::Phase::Failed;
        }
        {
            std::lock_guard<std::mutex> lock(record->gateMutex);
            record->gateOpen = true;
        }
        record->gateCondition.notify_one();
        std::shared_ptr<Receiver> retiredReceiver;
        FailureCallback retiredFailureCallback;
        ClientFactory retiredClientFactory;
        std::shared_ptr<CloseTimeoutSourceFactory>
            retiredTimeoutSourceFactory;
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            state->closeRequestedDuringStart = false;
            retiredReceiver.swap(state->receiver);
            retiredFailureCallback.swap(
                state->runtimeFailureCallback);
            retiredClientFactory.swap(state->clientFactory);
            retiredTimeoutSourceFactory.swap(
                state->timeoutSourceFactory);
        }
        // Without a reaper, never join the polling thread on the frontend.
        // The process-lived registry safely quarantines the joinable thread
        // after it exits and prevents module unload or storage reuse.
        reportLogicalFailure(state);
        return StartResult::Failed;
    }

    bool activationSucceeded = true;
    {
        std::lock_guard<std::mutex> sendLock(record->sendMutex);
        try {
            record->client->send(
                td_request_id::ACTIVATE,
                td::td_api::make_object<td::td_api::getOption>(
                    "version"));
        } catch (...) {
            activationSucceeded = false;
            std::lock_guard<std::mutex> lock(state->mutex);
            state->abortWorker = true;
            state->phase = TdPollingBackendState::Phase::Failed;
        }
    }

    bool closeAfterStart = false;
    if (activationSucceeded) {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->phase = TdPollingBackendState::Phase::Running;
        closeAfterStart = state->closeRequestedDuringStart;
        state->closeRequestedDuringStart = false;
    }
    if (closeAfterStart)
        close();

    {
        std::lock_guard<std::mutex> lock(record->gateMutex);
        record->gateOpen = true;
    }
    record->gateCondition.notify_one();

    if (!activationSucceeded) {
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            state->closeRequestedDuringStart = false;
        }
        reportLogicalFailure(state);
        return StartResult::Failed;
    }
    return StartResult::Started;
}

void TdPollingBackend::close(CloseCallback callback)
{
    std::shared_ptr<TdPollingBackendState> state = m_state;
    if (!state)
        return;

    bool scheduleKnownResult = false;
    CloseResult knownResult = CloseResult::Failed;
    std::vector<CloseCallback> knownCallbacks;
    bool startClose = false;
    bool alreadyFailed = false;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        const bool orderedFailureDelivery =
            state->logicalResultReported &&
            state->logicalResult == CloseResult::Failed &&
            (state->logicalFailureDeliveryPending ||
             state->failureHandoffPending ||
             state->failureFallbackPending);
        if (orderedFailureDelivery) {
            state->logicalFailureDeliveryPending = true;
            if (callback) {
                state->closeCallbacks.push_back(
                    std::move(callback));
            }
        } else if (state->logicalResultReported) {
            scheduleKnownResult = true;
            knownResult = state->logicalResult;
            if (callback)
                knownCallbacks.push_back(std::move(callback));
        } else if (state->phase ==
                       TdPollingBackendState::Phase::New) {
            state->phase =
                TdPollingBackendState::Phase::PhysicallyStopped;
            state->logicalResultReported = true;
            state->logicalResult = CloseResult::Closed;
            scheduleKnownResult = true;
            knownResult = CloseResult::Closed;
            if (callback)
                knownCallbacks.push_back(std::move(callback));
        } else if (
            state->physicalFinished &&
            terminalDeliveryReady(*state)) {
            state->logicalResultReported = true;
            state->logicalResult = state->physicalResult;
            scheduleKnownResult = true;
            knownResult = state->physicalResult;
            knownCallbacks.swap(state->closeCallbacks);
            if (callback)
                knownCallbacks.push_back(std::move(callback));
        } else {
            if (callback)
                state->closeCallbacks.push_back(
                    std::move(callback));
            if (state->physicalFinished) {
                // Physical TDLib cleanup can finish before the terminal
                // update is delivered on the captured context. Keep the
                // frontend deadline active until its receipt settles.
                startClose = state->deadlineSource == nullptr;
            } else if (state->phase ==
                    TdPollingBackendState::Phase::Running) {
                state->phase =
                    TdPollingBackendState::Phase::CloseRequested;
                startClose = true;
            } else if (
                state->phase ==
                    TdPollingBackendState::Phase::Starting) {
                state->closeRequestedDuringStart = true;
            } else if (
                state->phase ==
                    TdPollingBackendState::Phase::Failed ||
                state->abortWorker) {
                alreadyFailed = true;
            }
        }
    }

    if (scheduleKnownResult) {
        for (auto &knownCallback: knownCallbacks) {
            scheduleResultCallback(
                state->context,
                std::move(knownCallback),
                knownResult);
        }
        return;
    }
    if (alreadyFailed) {
        reportLogicalFailure(state);
        return;
    }
    if (!startClose)
        return;

    GSource *preparedDeadline = prepareDeadline(state);
    std::shared_ptr<TdPollingWorkerRecord> record =
        state->worker.lock();
    bool closeSent = false;
    bool closeQueued = false;
    bool closeAlreadyInProgress = false;
    if (record) {
        std::unique_lock<std::mutex> sendLock(
            record->sendMutex, std::try_to_lock);
        if (sendLock.owns_lock()) {
            {
                std::lock_guard<std::mutex> lock(state->mutex);
                closeAlreadyInProgress =
                    state->closedReceived ||
                    state->physicalFinished;
                closeSent =
                    !state->closeSent &&
                    !state->closeQueued &&
                    !state->abortWorker &&
                    !closeAlreadyInProgress &&
                    record->client != nullptr;
                if (closeSent)
                    state->closeSent = true;
            }
            if (closeSent) {
                try {
                    record->client->send(
                        td_request_id::CLOSE,
                        td::td_api::make_object<td::td_api::close>());
                } catch (...) {
                    closeSent = false;
                    closeAlreadyInProgress =
                        !markSendFailure(state);
                }
            }
        } else {
            std::lock_guard<std::mutex> lock(state->mutex);
            closeAlreadyInProgress =
                state->closedReceived ||
                state->physicalFinished;
            closeQueued =
                !state->closeSent &&
                !state->closeQueued &&
                !state->abortWorker &&
                !closeAlreadyInProgress;
            if (closeQueued) {
                state->closeQueued = true;
            }
        }
    } else {
        std::lock_guard<std::mutex> lock(state->mutex);
        closeAlreadyInProgress =
            state->closedReceived ||
            state->physicalFinished;
    }

    DeadlineInstallResult deadlineResult =
        DeadlineInstallResult::Failed;
    const bool closeEstablished =
        closeSent || closeQueued || closeAlreadyInProgress;
    if (closeEstablished && preparedDeadline) {
        deadlineResult =
            installPreparedDeadline(state, preparedDeadline);
        preparedDeadline = nullptr;
    } else if (closeEstablished) {
        std::lock_guard<std::mutex> lock(state->mutex);
        if (state->physicalFinished &&
            terminalDeliveryReady(*state)) {
            deadlineResult =
                DeadlineInstallResult::NotNeeded;
        }
    }
    if (preparedDeadline)
        destroyOwnedSource(preparedDeadline);

    if (!closeEstablished ||
        deadlineResult == DeadlineInstallResult::Failed) {
        reportLogicalFailure(state);
    }
}

bool TdPollingBackend::hasActiveWorkers()
{
    return workerRegistry().hasWorkers() ||
           backendCallbackActivityCount().load(
               std::memory_order_acquire) != 0 ||
           sourcePayloadRegistry().hasPending();
}

bool TdPollingBackend::isSessionCleanupPending(
    const std::string &sessionKey)
{
    std::shared_ptr<TdPollingWorkerRecord> record =
        workerRegistry().find(sessionKey);
    if (!record)
        return false;

    std::lock_guard<std::mutex> lock(record->state->mutex);
    return record->state->phase !=
               TdPollingBackendState::Phase::Running ||
           record->state->closeSent ||
           record->state->closedReceived ||
           record->state->abortWorker;
}
