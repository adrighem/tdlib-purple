#ifndef TD_TRANSPORT_H
#define TD_TRANSPORT_H

#include <glib.h>
#include <td/telegram/td_api.hpp>

#include <cstdint>
#include <functional>
#include <memory>

class TdTransportState;

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
    using TimeoutCallback = std::function<void(uint64_t)>;
    // The factory must return a fresh, unattached, non-destroyed GSource with
    // one reference transferred to TdTransport.
    using TimeoutSourceFactory =
        std::function<GSource *(unsigned timeoutSeconds)>;

    TdTransport(
        SendCallback sendCallback,
        UpdateCallback updateCallback,
        TimeoutSourceFactory timeoutSourceFactory =
            TimeoutSourceFactory());
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
    void shutdown();

private:
    std::shared_ptr<TdTransportState> m_state;
};

#endif
