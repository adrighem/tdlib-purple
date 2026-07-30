#include "td-transport.h"

#include <glib.h>

#include <deque>
#include <map>
#include <mutex>
#include <utility>

namespace {

constexpr unsigned MAX_DELIVERIES_PER_DISPATCH = 32;

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

} // namespace

class TdTransportState {
public:
    struct PendingDelivery {
        uint64_t requestId;
        TdTransport::ObjectPtr object;
    };

    TdTransportState(
        TdTransport::SendCallback send,
        TdTransport::UpdateCallback update)
        : context(captureThreadDefaultContext())
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
    }

    ~TdTransportState()
    {
        g_main_context_unref(context);
    }

    GMainContext *context;
    std::mutex mutex;
    bool stopped = false;
    uint64_t lastRequestId = 0;
    GSource *dispatchSource = nullptr;
    // A retiring source can overlap its replacement. Keep the state alive
    // until every source has run its destroy notifier.
    unsigned attachedSourceCount = 0;
    std::shared_ptr<TdTransportState> sourceLifetime;
    std::deque<PendingDelivery> queue;
    std::shared_ptr<TdTransport::SendCallback> sendCallback;
    std::shared_ptr<TdTransport::UpdateCallback> updateCallback;
    std::map<uint64_t, TdTransport::ResponseCallback> responseCallbacks;
};

namespace {

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
    std::shared_ptr<TdTransport::UpdateCallback> updateCallback;
    TdTransport::ResponseCallback responseCallback;

    {
        std::lock_guard<std::mutex> lock(state->mutex);
        if (state->stopped || state->queue.empty())
            return false;

        delivery = std::move(state->queue.front());
        state->queue.pop_front();

        if (delivery.requestId == 0) {
            updateCallback = state->updateCallback;
        } else {
            auto callback = state->responseCallbacks.find(delivery.requestId);
            if (callback != state->responseCallbacks.end()) {
                responseCallback = std::move(callback->second);
                state->responseCallbacks.erase(callback);
            }
        }
    }

    if (!delivery.object)
        return true;

    try {
        if (delivery.requestId == 0) {
            if (updateCallback)
                (*updateCallback)(std::move(delivery.object));
        } else if (responseCallback) {
            responseCallback(
                delivery.requestId, std::move(delivery.object));
        }
    } catch (...) {
        // Never let an application callback unwind through GLib.
    }

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
    g_source_attach(source, state->context);
}

void enqueueDelivery(
    const std::shared_ptr<TdTransportState> &state,
    uint64_t requestId,
    TdTransport::ObjectPtr object)
{
    if (!object)
        return;

    std::lock_guard<std::mutex> lock(state->mutex);
    if (state->stopped)
        return;

    state->queue.push_back(
        TdTransportState::PendingDelivery{
            requestId, std::move(object)});
    scheduleDispatchLocked(state);
}

} // namespace

TdTransport::TdTransport(
    SendCallback sendCallback,
    UpdateCallback updateCallback)
    : m_state(std::make_shared<TdTransportState>(
          std::move(sendCallback), std::move(updateCallback)))
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
    std::shared_ptr<SendCallback> sendCallback;
    uint64_t requestId = 0;

    {
        std::lock_guard<std::mutex> lock(state->mutex);
        if (state->stopped || !state->sendCallback || !function)
            return 0;

        sendCallback = state->sendCallback;
        do {
            ++state->lastRequestId;
        } while (state->lastRequestId == 0 ||
                 state->responseCallbacks.count(state->lastRequestId) != 0);

        requestId = state->lastRequestId;
        if (responseCallback) {
            state->responseCallbacks.emplace(
                requestId, std::move(responseCallback));
        }
    }

    try {
        (*sendCallback)(requestId, std::move(function));
    } catch (...) {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->responseCallbacks.erase(requestId);
        return 0;
    }

    return requestId;
}

void TdTransport::receive(uint64_t requestId, ObjectPtr object)
{
    enqueueDelivery(m_state, requestId, std::move(object));
}

TdTransport::ReceiveCallback TdTransport::receiver() const
{
    std::weak_ptr<TdTransportState> weakState(m_state);
    return [weakState](uint64_t requestId, ObjectPtr object) {
        std::shared_ptr<TdTransportState> state = weakState.lock();
        if (!state)
            return;

        enqueueDelivery(state, requestId, std::move(object));
    };
}

void TdTransport::shutdown()
{
    std::shared_ptr<TdTransportState> state = m_state;
    GSource *source = nullptr;
    std::deque<TdTransportState::PendingDelivery> queuedDeliveries;
    std::map<uint64_t, ResponseCallback> responseCallbacks;
    std::shared_ptr<SendCallback> sendCallback;
    std::shared_ptr<UpdateCallback> updateCallback;

    {
        std::lock_guard<std::mutex> lock(state->mutex);
        if (state->stopped)
            return;

        state->stopped = true;
        source = state->dispatchSource;
        state->dispatchSource = nullptr;
        queuedDeliveries.swap(state->queue);
        responseCallbacks.swap(state->responseCallbacks);
        sendCallback.swap(state->sendCallback);
        updateCallback.swap(state->updateCallback);
    }

    if (source) {
        g_source_destroy(source);
        g_source_unref(source);
    }
}
