#ifndef _TRANSCEIVER_H
#define _TRANSCEIVER_H

#include "td-polling-backend.h"
#include "td-transport.h"

#include <glib.h>
#include <purple.h>
#include <td/telegram/Client.h>
#include <td/telegram/td_api.hpp>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

class PurpleTdClient;

std::string getPurple2BaseDatabasePath();
std::string getPurple2DatabasePath(PurpleAccount *account);

// Compatibility seam used by the existing Purple 2 test harness. Production
// traffic uses TdPollingBackend directly.
class ITransceiverBackend {
public:
    virtual ~ITransceiverBackend() {}

    virtual void send(td::Client::Request &&request) = 0;
    // Borrowed context owned by the test backend.
    virtual GMainContext *transportContext() = 0;
    // Transfers one reference to a fresh, unattached, non-destroyed source.
    virtual GSource *createTimeoutSource(unsigned timeoutSeconds) = 0;
    virtual void close(TdPollingBackend::CloseCallback callback);

    // Test fakes must call receive() serially on the transport's owner
    // thread. The compatibility facade deliberately preserves the legacy
    // harness's immediate, reentrant delivery semantics.
    void setReceiver(TdTransport::ReceiveCallback receiver);
    void receive(td::Client::Response response);

private:
    std::mutex m_receiverMutex;
    TdTransport::ReceiveCallback m_receiver;
};

// Thin Purple 2 compatibility facade around the Purple-neutral dispatcher and
// polling backend.
class TdTransceiver {
private:
    using TdObjectPtr =
        td::td_api::object_ptr<td::td_api::Object>;

public:
    using ResponseCb =
        void (PurpleTdClient::*)(uint64_t, TdObjectPtr);
    using ResponseCb2 =
        std::function<void(uint64_t, TdObjectPtr)>;
    using UpdateCb =
        void (PurpleTdClient::*)(td::td_api::Object &);

    TdTransceiver(
        PurpleTdClient *owner,
        PurpleAccount *account,
        UpdateCb updateCb,
        ITransceiverBackend *testBackend);
    ~TdTransceiver();

    TdTransceiver(const TdTransceiver &) = delete;
    TdTransceiver &operator=(const TdTransceiver &) = delete;

    uint64_t sendQuery(
        td::td_api::object_ptr<td::td_api::Function> function,
        ResponseCb handler);
    uint64_t sendQuery(
        td::td_api::object_ptr<td::td_api::Function> function,
        ResponseCb2 handler);
    uint64_t sendQueryWithTimeout(
        td::td_api::object_ptr<td::td_api::Function> function,
        ResponseCb2 handler,
        unsigned timeoutSeconds);
    void setQueryTimer(
        uint64_t queryId,
        ResponseCb handler,
        unsigned timeoutSeconds,
        bool cancelNormalResponse);
    void setQueryTimer(
        uint64_t queryId,
        ResponseCb2 handler,
        unsigned timeoutSeconds,
        bool cancelNormalResponse);
    const std::string &databasePath() const;
    void shutdown(
        TdPollingBackend::CloseCallback callback =
            TdPollingBackend::CloseCallback());

private:
    PurpleTdClient *m_owner = nullptr;
    std::string m_databasePath;
    std::unique_ptr<TdTransport> m_transport;
    std::unique_ptr<TdPollingBackend> m_backend;
    std::shared_ptr<std::atomic<bool>> m_acceptBackendFailures;
    ITransceiverBackend *m_testBackend = nullptr;
    bool m_shutdown = false;
};

#endif
