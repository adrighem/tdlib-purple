#include "td-transport.h"
#include "td-request-id.h"

#include <glib.h>

#include <algorithm>
#include <list>
#include <map>
#include <mutex>
#include <utility>

namespace {

constexpr unsigned MAX_DELIVERIES_PER_DISPATCH = 32;

GMainContext *captureDispatchContext(GMainContext *dispatchContext)
{
    if (dispatchContext)
        return g_main_context_ref(dispatchContext);

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

struct MainContextUnref {
    void operator()(GMainContext *context) const noexcept
    {
        if (context)
            g_main_context_unref(context);
    }
};

using MainContextPtr =
    std::unique_ptr<GMainContext, MainContextUnref>;

// Attaching a source is not enough on its own: something has to notice that it
// is there. GLib signals a context's wakeup when the attach comes from a thread
// other than the one owning the context, and not at all when it comes from the
// owner, because a GLib main loop rechecks on its way round anyway. A context
// driven from a foreign event loop has to be told in both cases: nothing is
// sitting in poll() to notice by itself. g_main_context_wakeup does not consult
// the owner, and under a GLib main loop it costs one write to a descriptor that
// is usually signalled already.
guint attachAndWake(GSource *source, GMainContext *context)
{
    const guint sourceId = g_source_attach(source, context);
    g_main_context_wakeup(context);
    return sourceId;
}


} // namespace

TdTransport::DeliveryReceipt::DeliveryReceipt(
    Callback callback) noexcept
    : m_callback(std::move(callback))
{
}

TdTransport::DeliveryReceipt::~DeliveryReceipt()
{
    settle(false);
}

TdTransport::DeliveryReceipt::DeliveryReceipt(
    DeliveryReceipt &&other) noexcept
{
    m_callback.swap(other.m_callback);
}

TdTransport::DeliveryReceipt &
TdTransport::DeliveryReceipt::operator=(
    DeliveryReceipt &&other) noexcept
{
    if (this == &other)
        return *this;

    settle(false);
    m_callback.swap(other.m_callback);
    return *this;
}

void TdTransport::DeliveryReceipt::settle(
    bool delivered) noexcept
{
    Callback callback;
    callback.swap(m_callback);
    try {
        if (callback)
            callback(delivered);
    } catch (...) {
    }
}

struct TimeoutToken;

enum class TimeoutKind {
    Terminal,
    Notification
};

enum class TimeoutAttachment {
    Immediate,
    AfterSend
};

enum class TerminalCallbackMode {
    PendingResponse,
    Explicit
};

class TdTransportState {
public:
    struct TimeoutInfo {
        GSource *source = nullptr;
        TimeoutToken *token = nullptr;
        TdTransport::TimeoutCallback notificationCallback;
        TdTransport::ResponseCallback terminalCallback;
        TimeoutKind kind = TimeoutKind::Notification;
        TerminalCallbackMode terminalCallbackMode =
            TerminalCallbackMode::PendingResponse;

        TimeoutInfo() = default;
        TimeoutInfo(const TimeoutInfo &) = delete;
        TimeoutInfo &operator=(const TimeoutInfo &) = delete;

        TimeoutInfo(TimeoutInfo &&other) noexcept
            : source(other.source),
              token(other.token),
              notificationCallback(
                  std::move(other.notificationCallback)),
              terminalCallback(
                  std::move(other.terminalCallback)),
              kind(other.kind),
              terminalCallbackMode(other.terminalCallbackMode)
        {
            other.source = nullptr;
            other.token = nullptr;
            other.kind = TimeoutKind::Notification;
            other.terminalCallbackMode =
                TerminalCallbackMode::PendingResponse;
        }

        TimeoutInfo &operator=(TimeoutInfo &&) = delete;

        void swap(TimeoutInfo &other) noexcept
        {
            std::swap(source, other.source);
            std::swap(token, other.token);
            notificationCallback.swap(
                other.notificationCallback);
            terminalCallback.swap(other.terminalCallback);
            std::swap(kind, other.kind);
            std::swap(
                terminalCallbackMode,
                other.terminalCallbackMode);
        }
    };

    struct PendingRequest {
        TdTransport::ResponseCallback responseCallback;
        TimeoutInfo timeout;
    };

    struct PendingDelivery {
        uint64_t requestId;
        TdTransport::ObjectPtr object;
        TdTransport::ResponseCallback responseCallback;
        TdTransport::DeliveryReceipt deliveryReceipt;
        TimeoutInfo canceledTimeout;
    };

    TdTransportState(
        TdTransport::SendCallback send,
        TdTransport::UpdateCallback update,
        TdTransport::TimeoutSourceFactory timeoutFactory,
        GMainContext *dispatchContext)
        : contextOwner(captureDispatchContext(dispatchContext)),
          context(contextOwner.get())
    {
        if (send) {
            sendCallback =
                std::make_shared<TdTransport::SendCallback>(
                    std::move(send));
        }
        if (update) {
            updateCallback =
                std::make_shared<TdTransport::UpdateCallback>(
                    std::move(update));
        }
        if (timeoutFactory) {
            timeoutSourceFactory =
                std::make_shared<TdTransport::TimeoutSourceFactory>(
                    std::move(timeoutFactory));
        }
    }

    MainContextPtr contextOwner;
    GMainContext *context;
    std::mutex mutex;
    bool stopped = false;
    uint64_t lastRequestId = 0;
    GSource *dispatchSource = nullptr;
    // A retiring source can overlap its replacement. Keep the state alive
    // until every source has run its destroy notifier.
    unsigned attachedSourceCount = 0;
    std::shared_ptr<TdTransportState> sourceLifetime;
    std::list<PendingDelivery> queue;
    std::shared_ptr<TdTransport::SendCallback> sendCallback;
    std::shared_ptr<TdTransport::UpdateCallback> updateCallback;
    std::shared_ptr<TdTransport::TimeoutSourceFactory>
        timeoutSourceFactory;
    std::map<uint64_t, PendingRequest> requests;
};

struct TimeoutToken {
    std::weak_ptr<TdTransportState> state;
    uint64_t requestId;
};

namespace {

void destroyTimeoutSource(GSource *source)
{
    if (!source)
        return;

    g_source_destroy(source);
    g_source_unref(source);
}

class OwnedTimeoutSource {
public:
    explicit OwnedTimeoutSource(GSource *source = nullptr)
        : m_source(source)
    {
    }

    ~OwnedTimeoutSource()
    {
        clear();
    }

    OwnedTimeoutSource(const OwnedTimeoutSource &) = delete;
    OwnedTimeoutSource &operator=(const OwnedTimeoutSource &) = delete;

    GSource *get() const
    {
        return m_source;
    }

    void adopt(GSource *source)
    {
        m_source = source;
    }

    GSource *release()
    {
        GSource *source = m_source;
        m_source = nullptr;
        return source;
    }

    void clear()
    {
        destroyTimeoutSource(m_source);
        m_source = nullptr;
    }

private:
    GSource *m_source;
};

void releaseSourceLifetime(gpointer userData)
{
    TdTransportState *state =
        static_cast<TdTransportState *>(userData);
    std::shared_ptr<TdTransportState> lifetime;

    try {
        std::lock_guard<std::mutex> lock(state->mutex);
        if (state->attachedSourceCount > 0)
            --state->attachedSourceCount;
        if (state->attachedSourceCount == 0)
            lifetime.swap(state->sourceLifetime);
    } catch (...) {
    }
}

bool dispatchOne(const std::shared_ptr<TdTransportState> &state)
{
    TdTransportState::PendingDelivery delivery;
    OwnedTimeoutSource timeoutSource;
    std::shared_ptr<TdTransport::UpdateCallback> updateCallback;

    {
        std::lock_guard<std::mutex> lock(state->mutex);
        if (state->stopped || state->queue.empty())
            return false;

        delivery.requestId = state->queue.front().requestId;
        delivery.object = std::move(state->queue.front().object);
        delivery.responseCallback.swap(
            state->queue.front().responseCallback);
        delivery.deliveryReceipt =
            std::move(state->queue.front().deliveryReceipt);
        delivery.canceledTimeout.swap(
            state->queue.front().canceledTimeout);
        state->queue.pop_front();

        if (delivery.requestId == 0) {
            updateCallback = state->updateCallback;
        }
    }

    // Source destruction and its destroy notifier must run on the captured
    // context, never concurrently with a timeout callback.
    timeoutSource.adopt(delivery.canceledTimeout.source);
    delivery.canceledTimeout.source = nullptr;
    delivery.canceledTimeout.token = nullptr;
    timeoutSource.clear();

    bool delivered = false;
    if (delivery.object) {
        try {
            if (delivery.requestId == 0) {
                if (updateCallback) {
                    delivered = true;
                    (*updateCallback)(std::move(delivery.object));
                }
            } else if (delivery.responseCallback) {
                delivered = true;
                delivery.responseCallback(
                    delivery.requestId, std::move(delivery.object));
            }
        } catch (...) {
            // Never let an application callback unwind through GLib.
        }
    }

    delivery.deliveryReceipt.settle(delivered);

    return true;
}

gboolean dispatchPending(gpointer userData)
{
    TdTransportState *rawState =
        static_cast<TdTransportState *>(userData);
    GSource *const dispatchedSource = g_main_current_source();
    std::shared_ptr<TdTransportState> state;
    {
        std::lock_guard<std::mutex> lock(rawState->mutex);
        state = rawState->sourceLifetime;
    }
    if (!state)
        return FALSE;

    try {
        unsigned delivered = 0;
        while (delivered < MAX_DELIVERIES_PER_DISPATCH &&
               dispatchOne(state)) {
            ++delivered;
        }

        GSource *sourceToRelease = nullptr;
        bool keepSource = false;
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            keepSource = !state->stopped && !state->queue.empty();
            if (!keepSource &&
                state->dispatchSource == dispatchedSource) {
                sourceToRelease = state->dispatchSource;
                state->dispatchSource = nullptr;
            }
        }

        if (sourceToRelease)
            g_source_unref(sourceToRelease);

        return keepSource ? TRUE : FALSE;
    } catch (...) {
        GSource *sourceToRelease = nullptr;
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            if (state->dispatchSource == dispatchedSource) {
                sourceToRelease = state->dispatchSource;
                state->dispatchSource = nullptr;
            }
        }
        if (sourceToRelease)
            g_source_unref(sourceToRelease);
        return FALSE;
    }
}

void scheduleDispatchLocked(
    const std::shared_ptr<TdTransportState> &state)
{
    if (state->dispatchSource)
        return;

    GSource *source = g_idle_source_new();

    // Match normal event priority so a ready timeout does not
    // systematically overtake an already queued TDLib response. The bounded
    // dispatch batch below prevents this source from monopolizing the loop.
    g_source_set_priority(source, G_PRIORITY_DEFAULT);
    g_source_set_callback(
        source, dispatchPending, state.get(), releaseSourceLifetime);
    state->dispatchSource = source;
    if (state->attachedSourceCount++ == 0)
        state->sourceLifetime = state;
    attachAndWake(source, state->context);
}

void deleteTimeoutToken(gpointer userData)
{
    delete static_cast<TimeoutToken *>(userData);
}

gboolean timeoutExpired(gpointer userData)
{
    TimeoutToken *token = static_cast<TimeoutToken *>(userData);
    const uint64_t requestId = token->requestId;
    std::shared_ptr<TdTransportState> state = token->state.lock();
    if (!state)
        return FALSE;

    GSource *sourceToRelease = nullptr;
    TimeoutKind kind = TimeoutKind::Notification;
    TdTransport::ResponseCallback responseCallback;
    TdTransport::ResponseCallback discardedResponseCallback;
    TdTransport::TimeoutCallback notificationCallback;

    try {
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            auto request = state->requests.find(requestId);
            if (state->stopped ||
                request == state->requests.end() ||
                request->second.timeout.token != token) {
                return FALSE;
            }

            kind = request->second.timeout.kind;
            sourceToRelease = request->second.timeout.source;
            request->second.timeout.source = nullptr;
            request->second.timeout.token = nullptr;

            if (kind == TimeoutKind::Terminal) {
                if (request->second.timeout.terminalCallbackMode ==
                    TerminalCallbackMode::Explicit) {
                    responseCallback.swap(
                        request->second.timeout.terminalCallback);
                    discardedResponseCallback.swap(
                        request->second.responseCallback);
                } else {
                    responseCallback.swap(
                        request->second.responseCallback);
                }
                state->requests.erase(request);
            } else {
                notificationCallback.swap(
                    request->second.timeout.notificationCallback);
                request->second.timeout.kind =
                    TimeoutKind::Notification;
            }
        }

        // Drop the creator's source reference. The context retains the source
        // for the duration of this dispatch and destroys it after FALSE is
        // returned.
        if (sourceToRelease)
            g_source_unref(sourceToRelease);

        try {
            if (kind == TimeoutKind::Terminal) {
                if (responseCallback)
                    responseCallback(requestId, nullptr);
            } else if (notificationCallback) {
                notificationCallback(requestId);
            }
        } catch (...) {
            // Never let an application callback unwind through GLib.
        }
    } catch (...) {
        // GLib callbacks must not throw. If internal bookkeeping ever fails,
        // shutdown or a later response will release the creator reference.
    }

    return FALSE;
}

GSource *createTimeoutSource(
    const std::shared_ptr<TdTransportState> &state,
    unsigned timeoutSeconds)
{
    std::shared_ptr<TdTransport::TimeoutSourceFactory> factory;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        if (state->stopped)
            return nullptr;
        factory = state->timeoutSourceFactory;
    }

    GSource *source = nullptr;
    try {
        if (factory) {
            source = (*factory)(timeoutSeconds);
        } else if (timeoutSeconds == 0) {
            source = g_idle_source_new();
        } else {
            source = g_timeout_source_new_seconds(timeoutSeconds);
        }
    } catch (...) {
        return nullptr;
    }

    if (!source)
        return nullptr;

    // A source factory transfers one reference and must return a fresh source.
    // Do not mutate an invalid source that may already belong to another
    // context.
    if (g_source_is_destroyed(source) ||
        g_source_get_context(source) != nullptr) {
        g_source_unref(source);
        return nullptr;
    }

    return source;
}

bool installTimeout(
    const std::shared_ptr<TdTransportState> &state,
    uint64_t requestId,
    unsigned timeoutSeconds,
    TdTransportState::TimeoutInfo timeout,
    TimeoutAttachment attachment)
{
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        auto request = state->requests.find(requestId);
        if (state->stopped ||
            request == state->requests.end() ||
            request->second.timeout.source) {
            return false;
        }
    }

    OwnedTimeoutSource source(
        createTimeoutSource(state, timeoutSeconds));
    if (!source.get())
        return false;

    TimeoutToken *token = nullptr;
    try {
        token = new TimeoutToken{
            std::weak_ptr<TdTransportState>(state), requestId};
    } catch (...) {
        return false;
    }

    g_source_set_priority(source.get(), G_PRIORITY_DEFAULT);
    g_source_set_callback(
        source.get(), timeoutExpired, token, deleteTimeoutToken);
    timeout.source = source.get();
    timeout.token = token;

    bool installed = false;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        auto request = state->requests.find(requestId);
        if (!state->stopped &&
            request != state->requests.end() &&
            !request->second.timeout.source) {
            request->second.timeout.swap(timeout);

            if (attachment == TimeoutAttachment::AfterSend ||
                attachAndWake(source.get(), state->context) != 0) {
                installed = true;
            } else {
                request->second.timeout.swap(timeout);
            }
        }
    }

    if (installed)
        source.release();

    return installed;
}

bool installTerminalTimeout(
    const std::shared_ptr<TdTransportState> &state,
    uint64_t requestId,
    unsigned timeoutSeconds,
    TimeoutAttachment attachment,
    TerminalCallbackMode callbackMode,
    TdTransport::ResponseCallback timeoutCallback =
        TdTransport::ResponseCallback())
{
    TdTransportState::TimeoutInfo timeout;
    timeout.kind = TimeoutKind::Terminal;
    timeout.terminalCallbackMode = callbackMode;
    timeout.terminalCallback = std::move(timeoutCallback);
    return installTimeout(
        state,
        requestId,
        timeoutSeconds,
        std::move(timeout),
        attachment);
}

bool installNotificationTimeout(
    const std::shared_ptr<TdTransportState> &state,
    uint64_t requestId,
    unsigned timeoutSeconds,
    TdTransport::TimeoutCallback timeoutCallback)
{
    TdTransportState::TimeoutInfo timeout;
    timeout.kind = TimeoutKind::Notification;
    timeout.notificationCallback = std::move(timeoutCallback);
    return installTimeout(
        state,
        requestId,
        timeoutSeconds,
        std::move(timeout),
        TimeoutAttachment::Immediate);
}

bool attachPreparedTimeout(
    const std::shared_ptr<TdTransportState> &state,
    uint64_t requestId)
{
    std::lock_guard<std::mutex> lock(state->mutex);
    if (state->stopped)
        return false;

    auto request = state->requests.find(requestId);
    if (request == state->requests.end()) {
        // A synchronous response already claimed and canceled the
        // still-unattached source.
        return true;
    }

    GSource *source = request->second.timeout.source;
    if (!source || g_source_is_destroyed(source))
        return false;

    GMainContext *attachedContext = g_source_get_context(source);
    if (attachedContext)
        return attachedContext == state->context;

    return attachAndWake(source, state->context) != 0;
}

bool enqueueUpdate(
    const std::shared_ptr<TdTransportState> &state,
    TdTransport::ObjectPtr object,
    TdTransport::DeliveryReceipt *deliveryReceipt)
{
    if (!object)
        return false;

    std::lock_guard<std::mutex> lock(state->mutex);
    if (state->stopped)
        return false;

    state->queue.emplace_back();
    state->queue.back().requestId = 0;
    state->queue.back().object = std::move(object);
    if (deliveryReceipt)
        state->queue.back().deliveryReceipt =
            std::move(*deliveryReceipt);
    scheduleDispatchLocked(state);
    return true;
}

bool receiveObject(
    const std::shared_ptr<TdTransportState> &state,
    uint64_t requestId,
    TdTransport::ObjectPtr object,
    TdTransport::DeliveryReceipt *deliveryReceipt = nullptr)
{
    if (!object)
        return false;

    if (requestId == 0) {
        return enqueueUpdate(
            state, std::move(object), deliveryReceipt);
    }

    {
        std::lock_guard<std::mutex> lock(state->mutex);
        if (state->stopped)
            return false;

        auto request = state->requests.find(requestId);
        if (request == state->requests.end())
            return false;

        state->queue.emplace_back();
        state->queue.back().requestId = requestId;
        state->queue.back().object = std::move(object);
        state->queue.back().responseCallback.swap(
            request->second.responseCallback);
        if (deliveryReceipt)
            state->queue.back().deliveryReceipt =
                std::move(*deliveryReceipt);
        state->queue.back().canceledTimeout.swap(
            request->second.timeout);
        state->requests.erase(request);
        scheduleDispatchLocked(state);
    }

    // Claiming the request at ingress makes a received response win over a
    // ready timeout even when its main-context delivery is still queued. The
    // queued delivery cancels it on that context, or an already-selected
    // timeout callback observes the missing request and becomes a no-op.
    return true;
}

void dispatchSynchronouslyForTestBackend(
    const std::shared_ptr<TdTransportState> &state)
{
    while (true) {
        GSource *source = nullptr;
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            source = state->dispatchSource;
            state->dispatchSource = nullptr;
        }

        if (source) {
            g_source_destroy(source);
            g_source_unref(source);
        }

        while (dispatchOne(state)) {
        }

        std::lock_guard<std::mutex> lock(state->mutex);
        if (state->stopped ||
            (state->queue.empty() && !state->dispatchSource)) {
            return;
        }
    }
}

uint64_t reserveRequest(
    const std::shared_ptr<TdTransportState> &state,
    TdTransport::ResponseCallback responseCallback,
    std::shared_ptr<TdTransport::SendCallback> &sender)
{
    std::lock_guard<std::mutex> lock(state->mutex);
    if (state->stopped || !state->sendCallback)
        return 0;

    sender = state->sendCallback;
    do {
        state->lastRequestId =
            td_request_id::nextPublic(state->lastRequestId);
    } while (
        state->requests.count(state->lastRequestId) != 0);

    const uint64_t requestId = state->lastRequestId;
    auto inserted = state->requests.emplace(
        requestId, TdTransportState::PendingRequest());
    inserted.first->second.responseCallback.swap(responseCallback);
    return requestId;
}

void cancelRequest(
    const std::shared_ptr<TdTransportState> &state,
    uint64_t requestId)
{
    TdTransportState::PendingRequest canceledRequest;
    TdTransportState::PendingDelivery canceledDelivery;
    OwnedTimeoutSource timeoutSource;

    {
        std::lock_guard<std::mutex> lock(state->mutex);
        auto request = state->requests.find(requestId);
        if (request != state->requests.end()) {
            canceledRequest.responseCallback.swap(
                request->second.responseCallback);
            canceledRequest.timeout.swap(request->second.timeout);
            state->requests.erase(request);
            timeoutSource.adopt(canceledRequest.timeout.source);
            canceledRequest.timeout.source = nullptr;
            canceledRequest.timeout.token = nullptr;
        } else {
            auto delivery = std::find_if(
                state->queue.begin(),
                state->queue.end(),
                [requestId](
                    const TdTransportState::PendingDelivery &candidate) {
                    return candidate.requestId == requestId;
                });
            if (delivery != state->queue.end()) {
                canceledDelivery.requestId = delivery->requestId;
                canceledDelivery.object = std::move(delivery->object);
                canceledDelivery.responseCallback.swap(
                    delivery->responseCallback);
                canceledDelivery.deliveryReceipt =
                    std::move(delivery->deliveryReceipt);
                canceledDelivery.canceledTimeout.swap(
                    delivery->canceledTimeout);
                timeoutSource.adopt(
                    canceledDelivery.canceledTimeout.source);
                canceledDelivery.canceledTimeout.source = nullptr;
                canceledDelivery.canceledTimeout.token = nullptr;
                state->queue.erase(delivery);
            }
        }
    }
    // Callback captures, delivery receipts, and the timeout source are
    // released after unlocking.
}

} // namespace

TdTransport::TdTransport(
    SendCallback sendCallback,
    UpdateCallback updateCallback,
    TimeoutSourceFactory timeoutSourceFactory,
    GMainContext *dispatchContext)
    : m_state(std::make_shared<TdTransportState>(
          std::move(sendCallback),
          std::move(updateCallback),
          std::move(timeoutSourceFactory),
          dispatchContext))
{
}

TdTransport::~TdTransport()
{
    shutdown();
}

uint64_t TdTransport::send(
    FunctionPtr function,
    ResponseCallback responseCallback)
{
    std::shared_ptr<TdTransportState> state = m_state;
    if (!function)
        return 0;

    std::shared_ptr<SendCallback> sendCallback;
    const uint64_t requestId = reserveRequest(
        state, std::move(responseCallback), sendCallback);
    if (requestId == 0)
        return 0;

    try {
        (*sendCallback)(requestId, std::move(function));
    } catch (...) {
        cancelRequest(state, requestId);
        return 0;
    }

    return requestId;
}

uint64_t TdTransport::sendWithTimeout(
    FunctionPtr function,
    ResponseCallback responseCallback,
    unsigned timeoutSeconds)
{
    std::shared_ptr<TdTransportState> state = m_state;
    if (!function)
        return 0;

    std::shared_ptr<SendCallback> sendCallback;
    const uint64_t requestId = reserveRequest(
        state, std::move(responseCallback), sendCallback);
    if (requestId == 0)
        return 0;

    // Publish the timer before the backend can synchronously respond, but do
    // not attach it until send returns. This prevents a nested or concurrent
    // context dispatch from timing out a request that has not been sent yet.
    if (!installTerminalTimeout(
            state,
            requestId,
            timeoutSeconds,
            TimeoutAttachment::AfterSend,
            TerminalCallbackMode::PendingResponse)) {
        cancelRequest(state, requestId);
        return 0;
    }

    try {
        (*sendCallback)(requestId, std::move(function));
    } catch (...) {
        cancelRequest(state, requestId);
        return 0;
    }

    if (!attachPreparedTimeout(state, requestId)) {
        cancelRequest(state, requestId);
        return 0;
    }

    return requestId;
}

bool TdTransport::setResponseTimeout(
    uint64_t requestId,
    unsigned timeoutSeconds)
{
    return installTerminalTimeout(
        m_state,
        requestId,
        timeoutSeconds,
        TimeoutAttachment::Immediate,
        TerminalCallbackMode::PendingResponse);
}

bool TdTransport::setResponseTimeout(
    uint64_t requestId,
    unsigned timeoutSeconds,
    ResponseCallback timeoutCallback)
{
    return installTerminalTimeout(
        m_state,
        requestId,
        timeoutSeconds,
        TimeoutAttachment::Immediate,
        TerminalCallbackMode::Explicit,
        std::move(timeoutCallback));
}

bool TdTransport::setNotificationTimeout(
    uint64_t requestId,
    unsigned timeoutSeconds,
    TimeoutCallback timeoutCallback)
{
    return installNotificationTimeout(
        m_state,
        requestId,
        timeoutSeconds,
        std::move(timeoutCallback));
}

void TdTransport::receive(uint64_t requestId, ObjectPtr object)
{
    try {
        receiveObject(m_state, requestId, std::move(object));
    } catch (...) {
        // Never let allocation or callback-bookkeeping failures escape a
        // backend receive boundary.
    }
}

TdTransport::ReceiveCallback TdTransport::receiver() const
{
    std::weak_ptr<TdTransportState> weakState(m_state);
    return [weakState](uint64_t requestId, ObjectPtr object) {
        std::shared_ptr<TdTransportState> state = weakState.lock();
        if (!state)
            return;

        try {
            receiveObject(state, requestId, std::move(object));
        } catch (...) {
            // Stable backend receivers must remain exception-safe.
        }
    };
}

TdTransport::AcknowledgedReceiveCallback
TdTransport::acknowledgedReceiver() const
{
    std::weak_ptr<TdTransportState> weakState(m_state);
    return [weakState](
               uint64_t requestId,
               ObjectPtr object,
               DeliveryReceipt receipt) {
        std::shared_ptr<TdTransportState> state = weakState.lock();
        if (!state)
            return;

        try {
            receiveObject(
                state,
                requestId,
                std::move(object),
                &receipt);
        } catch (...) {
            // The receipt settles as dropped after an ingress failure.
        }
    };
}

TdTransport::ReceiveCallback
TdTransport::synchronousReceiverForTestBackend() const
{
    std::weak_ptr<TdTransportState> weakState(m_state);
    return [weakState](uint64_t requestId, ObjectPtr object) {
        std::shared_ptr<TdTransportState> state = weakState.lock();
        if (!state)
            return;

        try {
            receiveObject(state, requestId, std::move(object));
            dispatchSynchronouslyForTestBackend(state);
        } catch (...) {
            // Test backend receivers obey the same exception boundary as
            // production receivers.
        }
    };
}

void TdTransport::shutdown()
{
    std::shared_ptr<TdTransportState> state = m_state;
    GSource *source = nullptr;
    std::list<TdTransportState::PendingDelivery> queuedDeliveries;
    std::map<uint64_t, TdTransportState::PendingRequest> requests;
    std::shared_ptr<SendCallback> sendCallback;
    std::shared_ptr<UpdateCallback> updateCallback;
    std::shared_ptr<TimeoutSourceFactory> timeoutSourceFactory;

    {
        std::lock_guard<std::mutex> lock(state->mutex);
        if (state->stopped)
            return;

        state->stopped = true;
        source = state->dispatchSource;
        state->dispatchSource = nullptr;
        queuedDeliveries.swap(state->queue);
        requests.swap(state->requests);
        sendCallback.swap(state->sendCallback);
        updateCallback.swap(state->updateCallback);
        timeoutSourceFactory.swap(state->timeoutSourceFactory);
    }

    if (source) {
        g_source_destroy(source);
        g_source_unref(source);
    }

    for (auto &delivery: queuedDeliveries) {
        destroyTimeoutSource(delivery.canceledTimeout.source);
        delivery.canceledTimeout.source = nullptr;
        delivery.canceledTimeout.token = nullptr;
        delivery.deliveryReceipt.settle(false);
    }

    for (auto &request: requests) {
        GSource *timeoutSource = request.second.timeout.source;
        request.second.timeout.source = nullptr;
        request.second.timeout.token = nullptr;
        destroyTimeoutSource(timeoutSource);
    }
}
