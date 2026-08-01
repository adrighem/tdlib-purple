#ifndef TD_TRANSPORT_H
#define TD_TRANSPORT_H

#include <glib.h>
#include <td/telegram/td_api.hpp>

#include <cstdint>
#include <functional>
#include <memory>

class TdTransportState;
class TdTransceiver;

// Purple-neutral request dispatcher for TDLib. Send, timeout-control, and
// shutdown methods are serialized on the captured creating context.
// receive() is thread-safe while the transport is alive; receiver() remains
// safe after the transport is gone.
class TdTransport {
public:
    using ObjectPtr = td::td_api::object_ptr<td::td_api::Object>;
    using FunctionPtr = td::td_api::object_ptr<td::td_api::Function>;
    using SendCallback = std::function<void(uint64_t, FunctionPtr)>;
    using UpdateCallback = std::function<void(ObjectPtr)>;
    using ResponseCallback =
        std::function<void(uint64_t, ObjectPtr)>;
    using ReceiveCallback =
        std::function<void(uint64_t, ObjectPtr)>;
    class DeliveryReceipt {
    public:
        using Callback = std::function<void(bool)>;

        DeliveryReceipt() noexcept = default;
        explicit DeliveryReceipt(Callback callback) noexcept;
        ~DeliveryReceipt();

        DeliveryReceipt(const DeliveryReceipt &) = delete;
        DeliveryReceipt &operator=(const DeliveryReceipt &) = delete;
        DeliveryReceipt(DeliveryReceipt &&other) noexcept;
        DeliveryReceipt &operator=(
            DeliveryReceipt &&other) noexcept;

        // Settles exactly once. Destruction settles an unconsumed receipt as
        // dropped, so every receive and cancellation path is fail-safe.
        void settle(bool delivered) noexcept;

    private:
        Callback m_callback;
    };
    using AcknowledgedReceiveCallback =
        std::function<void(
            uint64_t, ObjectPtr, DeliveryReceipt)>;
    using TimeoutCallback = std::function<void(uint64_t)>;
    // The factory must return a fresh, unattached, non-destroyed GSource with
    // one reference transferred to TdTransport.
    using TimeoutSourceFactory =
        std::function<GSource *(unsigned timeoutSeconds)>;

    // dispatchContext is borrowed. A null value captures the creating
    // thread-default context.
    TdTransport(
        SendCallback sendCallback,
        UpdateCallback updateCallback,
        TimeoutSourceFactory timeoutSourceFactory =
            TimeoutSourceFactory(),
        GMainContext *dispatchContext = nullptr);
    ~TdTransport();

    TdTransport(const TdTransport &) = delete;
    TdTransport &operator=(const TdTransport &) = delete;

    uint64_t send(
        FunctionPtr function,
        ResponseCallback responseCallback = ResponseCallback());
    uint64_t sendWithTimeout(
        FunctionPtr function,
        ResponseCallback responseCallback,
        unsigned timeoutSeconds);
    // A response timeout completes the request with a null object. The
    // two-argument form reuses the pending response callback; the overload
    // uses its explicit callback. A notification timeout fires once without
    // consuming the pending response. Timer setters return false for stopped,
    // unknown, completed, duplicate, or invalid-source requests.
    bool setResponseTimeout(
        uint64_t requestId, unsigned timeoutSeconds);
    bool setResponseTimeout(
        uint64_t requestId,
        unsigned timeoutSeconds,
        ResponseCallback timeoutCallback);
    bool setNotificationTimeout(
        uint64_t requestId,
        unsigned timeoutSeconds,
        TimeoutCallback timeoutCallback);
    void receive(uint64_t requestId, ObjectPtr object);
    // Use this stable weak receiver for work that can outlive the transport.
    ReceiveCallback receiver() const;
    // The delivery callback is settled after the application callback
    // returns. This lets a backend sequence terminal cleanup without relying
    // on same-priority GLib source ordering.
    AcknowledgedReceiveCallback acknowledgedReceiver() const;
    void shutdown();

private:
    friend class TdTransceiver;

    // The Purple 2 test backend historically delivers replies immediately,
    // including replies nested inside an update callback. Keep that behavior
    // behind the facade instead of weakening normal main-context dispatch.
    ReceiveCallback synchronousReceiverForTestBackend() const;

    std::shared_ptr<TdTransportState> m_state;
};

#endif
