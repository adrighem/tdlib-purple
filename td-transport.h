#ifndef TD_TRANSPORT_H
#define TD_TRANSPORT_H

#include <td/telegram/td_api.hpp>

#include <cstdint>
#include <functional>
#include <memory>

class TdTransportState;

// Purple-neutral request dispatcher for TDLib. send() and shutdown() are
// called from the creating context. receive() is thread-safe while the
// transport is alive; receiver() remains safe after the transport is gone.
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

    TdTransport(SendCallback sendCallback, UpdateCallback updateCallback);
    ~TdTransport();

    TdTransport(const TdTransport &) = delete;
    TdTransport &operator=(const TdTransport &) = delete;

    uint64_t send(
        FunctionPtr function,
        ResponseCallback responseCallback = ResponseCallback());
    void receive(uint64_t requestId, ObjectPtr object);
    // Use this stable weak receiver for work that can outlive the transport.
    ReceiveCallback receiver() const;
    void shutdown();

private:
    std::shared_ptr<TdTransportState> m_state;
};

#endif
