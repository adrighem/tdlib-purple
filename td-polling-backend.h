#ifndef TD_POLLING_BACKEND_H
#define TD_POLLING_BACKEND_H

#include "td-transport.h"

#include <glib.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

class TdPollingBackendState;

class TdPollingClient {
public:
    using ObjectPtr = TdTransport::ObjectPtr;
    using FunctionPtr = TdTransport::FunctionPtr;

    struct Response {
        std::uint64_t requestId = 0;
        ObjectPtr object;
    };

    virtual ~TdPollingClient() {}

    // May run concurrently with receive(). Must be thread-safe and perform
    // only a bounded local enqueue. In particular, it must not wait for a
    // network response. The production ClientManager adapter satisfies this
    // contract.
    virtual void send(
        std::uint64_t requestId, FunctionPtr function) = 0;
    // Runs on the polling thread and may overlap send(). Must return no later
    // than timeoutSeconds unless an object or exception becomes available
    // first.
    virtual Response receive(double timeoutSeconds) = 0;
};

// Purple-neutral owner of one TDLib client manager and its polling thread.
// Public methods are called on the captured creating context. sender() is
// stable and thread-safe while work is being admitted by the backend. close()
// never waits to acquire a concurrent sender; it starts the frontend deadline
// and lets the polling worker send the close request when that sender clears.
class TdPollingBackend {
public:
    enum class StartResult {
        Started,
        SessionBusy,
        Failed
    };

    enum class CloseResult {
        Closed,
        TimedOut,
        Failed
    };

    using Sender = TdTransport::SendCallback;
    // A receiver owns the delivery receipt until it has delivered or dropped
    // the object. Receipt destruction safely records a drop. A successful
    // Closed result is never reported before the terminal receipt settles;
    // the bounded deadline can still report TimedOut while it is held.
    using Receiver = TdTransport::AcknowledgedReceiveCallback;
    using CloseCallback = std::function<void(CloseResult)>;
    using FailureCallback = std::function<void()>;
    using ClientFactory =
        std::function<std::unique_ptr<TdPollingClient>()>;
    // The factory transfers one reference to a fresh, unattached,
    // non-destroyed source.
    using CloseTimeoutSourceFactory =
        std::function<GSource *(unsigned timeoutSeconds)>;
    // Test seam for preparing the worker's main-context finalizer before
    // TDLib or any worker thread is created. The factory transfers one
    // reference to a fresh, unattached, non-destroyed idle source.
    using FinalizerSourceFactory = std::function<GSource *()>;
    // Test seams for failure delivery and finalizer attachment. A failure
    // factory transfers one reference to a fresh, unattached, non-destroyed,
    // immediately-ready one-shot idle source. An attacher has the same
    // ownership contract and return value as g_source_attach().
    using FailureSourceFactory = std::function<GSource *()>;
    using FinalizerSourceAttacher =
        std::function<guint(GSource *, GMainContext *)>;
    // Test-only lifecycle observer, invoked once on the reaper thread after
    // it has joined the polling worker.
    using ReaperReadyCallback = std::function<void()>;

    TdPollingBackend(
        std::string sessionKey,
        unsigned closeTimeoutSeconds,
        double pollTimeoutSeconds,
        ClientFactory clientFactory,
        CloseTimeoutSourceFactory closeTimeoutSourceFactory =
            CloseTimeoutSourceFactory(),
        FinalizerSourceFactory finalizerSourceFactory =
            FinalizerSourceFactory(),
        FailureSourceFactory failureSourceFactory =
            FailureSourceFactory(),
        FinalizerSourceAttacher finalizerSourceAttacher =
            FinalizerSourceAttacher(),
        ReaperReadyCallback reaperReadyCallback =
            ReaperReadyCallback());
    ~TdPollingBackend();

    TdPollingBackend(const TdPollingBackend &) = delete;
    TdPollingBackend &operator=(const TdPollingBackend &) = delete;

    Sender sender() const;
    // Runtime failure notification completes before any Failed close
    // callback. A configured close deadline may independently win with
    // TimedOut so frontend shutdown remains bounded.
    StartResult start(
        Receiver receiver,
        FailureCallback failureCallback = FailureCallback());
    void close(CloseCallback callback = CloseCallback());

    // Also covers callbacks queued by a worker or close(), so a false
    // result means the backend no longer owns executable plugin code.
    static bool hasActiveWorkers();
    static bool isSessionCleanupPending(
        const std::string &sessionKey);

private:
    std::shared_ptr<TdPollingBackendState> m_state;
};

#endif
