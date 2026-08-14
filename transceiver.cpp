#include "transceiver.h"

#include "config.h"
#include "purple2-scheduler.h"
#include "translate.h"

#include <stdexcept>
#include <string>
#include <utility>

namespace {

constexpr unsigned CLOSE_TIMEOUT_SECONDS = 10;
constexpr double POLL_TIMEOUT_SECONDS = 0.1;

// Pushes a context for as long as it is in scope, and does nothing at all when
// there is none. A guard rather than a pair of calls because what sits between
// them can throw, and a thread default left pushed would follow this thread
// into everything it does afterwards.
class ThreadDefaultContext {
public:
    explicit ThreadDefaultContext(GMainContext *context)
        : m_context(context)
    {
        if (m_context)
            g_main_context_push_thread_default(m_context);
    }

    ~ThreadDefaultContext()
    {
        if (m_context)
            g_main_context_pop_thread_default(m_context);
    }

    ThreadDefaultContext(const ThreadDefaultContext &) = delete;
    ThreadDefaultContext &operator=(
        const ThreadDefaultContext &) = delete;

private:
    GMainContext *m_context;
};

std::string purple2SessionKey(const std::string &databasePath)
{
    gchar *digest = g_compute_checksum_for_string(
        G_CHECKSUM_SHA256,
        databasePath.c_str(),
        -1);
    if (!digest)
        throw std::runtime_error(
            "Telegram session identity is unavailable");

    std::string sessionKey("purple2:");
    sessionKey += digest;
    g_free(digest);
    return sessionKey;
}

TdTransport::UpdateCallback makeUpdateCallback(
    PurpleTdClient *owner,
    TdTransceiver::UpdateCb updateCallback)
{
    if (!owner || !updateCallback)
        return TdTransport::UpdateCallback();

    return [owner, updateCallback](TdTransport::ObjectPtr object) {
        if (object)
            (owner->*updateCallback)(*object);
    };
}

TdTransceiver::ResponseCb2 makeResponseCallback(
    PurpleTdClient *owner,
    TdTransceiver::ResponseCb responseCallback)
{
    if (!owner || !responseCallback)
        return TdTransceiver::ResponseCb2();

    return [owner, responseCallback](
               uint64_t requestId,
               TdTransport::ObjectPtr object) {
        (owner->*responseCallback)(
            requestId, std::move(object));
    };
}

} // namespace

std::string getPurple2BaseDatabasePath()
{
    const char *userDirectory = purple_user_dir();
    if (!userDirectory)
        throw std::runtime_error(
            "Telegram data directory is unavailable");

    return std::string(userDirectory) + G_DIR_SEPARATOR_S +
        config::configSubdir;
}

std::string getPurple2DatabasePath(PurpleAccount *account)
{
    if (!account)
        throw std::runtime_error("Telegram account is unavailable");

    const char *username = purple_account_get_username(account);
    if (!username || username[0] == '\0')
        throw std::runtime_error(
            "Telegram account identifier is unavailable");

    return getPurple2BaseDatabasePath() + G_DIR_SEPARATOR_S + username;
}

void ITransceiverBackend::setReceiver(
    TdTransport::ReceiveCallback receiver)
{
    std::lock_guard<std::mutex> lock(m_receiverMutex);
    m_receiver = std::move(receiver);
}

void ITransceiverBackend::receive(td::Client::Response response)
{
    TdTransport::ReceiveCallback receiver;
    {
        std::lock_guard<std::mutex> lock(m_receiverMutex);
        receiver = m_receiver;
    }
    if (receiver) {
        receiver(
            response.id, std::move(response.object));
    }
}

void ITransceiverBackend::close(
    TdPollingBackend::CloseCallback callback)
{
    if (callback)
        callback(TdPollingBackend::CloseResult::Closed);
}

TdTransceiver::TdTransceiver(
    PurpleTdClient *owner,
    PurpleAccount *account,
    UpdateCb updateCallback,
    ITransceiverBackend *testBackend)
    : m_owner(owner),
      m_testBackend(testBackend)
{
    TdTransport::UpdateCallback update =
        makeUpdateCallback(owner, updateCallback);

    if (account) {
        m_databasePath = getPurple2DatabasePath(account);
    } else if (!m_testBackend) {
        throw std::runtime_error("Telegram account is unavailable");
    }

    if (m_testBackend) {
        GMainContext *context =
            m_testBackend->transportContext();
        if (!context)
            throw std::runtime_error(
                "Test transport context is unavailable");

        m_transport.reset(new TdTransport(
            [testBackend](
                uint64_t requestId,
                TdTransport::FunctionPtr function) {
                testBackend->send(
                    {requestId, std::move(function)});
            },
            std::move(update),
            [testBackend](unsigned timeoutSeconds) {
                return testBackend->createTimeoutSource(
                    timeoutSeconds);
            },
            context));
        m_testBackend->setReceiver(
            m_transport->synchronousReceiverForTestBackend());
        return;
    }

#if !GLIB_CHECK_VERSION(2, 32, 0)
    if (!g_thread_supported())
        g_thread_init(NULL);
#endif

    /* Both of the objects below capture, at construction, the context they will
     * dispatch on, and both take the thread default when they are not told
     * otherwise. Putting the scheduler's context there is what places their work
     * somewhere that is actually driven. With no scheduler installed this is a
     * null context and nothing is pushed, so they capture the thread default
     * exactly as they did before. */
    ThreadDefaultContext dispatchContext(purple2SchedulerContext());

    m_backend.reset(new TdPollingBackend(
        purple2SessionKey(m_databasePath),
        CLOSE_TIMEOUT_SECONDS,
        POLL_TIMEOUT_SECONDS,
        TdPollingBackend::ClientFactory()));
    m_transport.reset(new TdTransport(
        m_backend->sender(), std::move(update)));
    m_acceptBackendFailures =
        std::make_shared<std::atomic<bool>>(true);
    std::weak_ptr<std::atomic<bool>> acceptBackendFailures(
        m_acceptBackendFailures);
    if (m_backend->start(
            m_transport->acknowledgedReceiver(),
            [acceptBackendFailures, account]() {
                std::shared_ptr<std::atomic<bool>> active =
                    acceptBackendFailures.lock();
                if (!active ||
                    !active->load(std::memory_order_acquire)) {
                    return;
                }

                PurpleConnection *connection =
                    purple_account_get_connection(account);
                if (connection) {
                    // TRANSLATOR: Buddy-window error shown when the TDLib
                    // polling backend stops unexpectedly.
                    purple_connection_error(
                        connection,
                        _("Telegram connection stopped unexpectedly"));
                }
            }) !=
        TdPollingBackend::StartResult::Started) {
        m_acceptBackendFailures->store(
            false, std::memory_order_release);
        m_transport->shutdown();
        m_backend.reset();
        throw std::runtime_error(
            "Telegram session could not be started");
    }
}

TdTransceiver::~TdTransceiver()
{
    shutdown();
}

uint64_t TdTransceiver::sendQuery(
    td::td_api::object_ptr<td::td_api::Function> function,
    ResponseCb2 handler)
{
    if (m_shutdown || !m_transport)
        return 0;
    return m_transport->send(
        std::move(function), std::move(handler));
}

uint64_t TdTransceiver::sendQuery(
    td::td_api::object_ptr<td::td_api::Function> function,
    ResponseCb handler)
{
    return sendQuery(
        std::move(function),
        makeResponseCallback(m_owner, handler));
}

uint64_t TdTransceiver::sendQueryWithTimeout(
    td::td_api::object_ptr<td::td_api::Function> function,
    ResponseCb2 handler,
    unsigned timeoutSeconds)
{
    if (m_shutdown || !m_transport)
        return 0;
    return m_transport->sendWithTimeout(
        std::move(function),
        std::move(handler),
        timeoutSeconds);
}

void TdTransceiver::setQueryTimer(
    uint64_t queryId,
    ResponseCb2 handler,
    unsigned timeoutSeconds,
    bool cancelNormalResponse)
{
    if (m_shutdown || !m_transport)
        return;

    if (cancelNormalResponse) {
        m_transport->setResponseTimeout(
            queryId, timeoutSeconds, std::move(handler));
    } else {
        m_transport->setNotificationTimeout(
            queryId,
            timeoutSeconds,
            [handler = std::move(handler)](
                uint64_t requestId) mutable {
                if (handler)
                    handler(requestId, nullptr);
            });
    }
}

void TdTransceiver::setQueryTimer(
    uint64_t queryId,
    ResponseCb handler,
    unsigned timeoutSeconds,
    bool cancelNormalResponse)
{
    setQueryTimer(
        queryId,
        makeResponseCallback(m_owner, handler),
        timeoutSeconds,
        cancelNormalResponse);
}

const std::string &TdTransceiver::databasePath() const
{
    return m_databasePath;
}

void TdTransceiver::shutdown(TdPollingBackend::CloseCallback callback)
{
    if (m_shutdown) {
        if (m_testBackend) {
            m_testBackend->close(std::move(callback));
        } else if (m_backend) {
            m_backend->close(std::move(callback));
        } else if (callback) {
            callback(TdPollingBackend::CloseResult::Failed);
        }
        return;
    }
    m_shutdown = true;

    if (m_acceptBackendFailures) {
        m_acceptBackendFailures->store(
            false, std::memory_order_release);
    }
    if (m_testBackend) {
        m_testBackend->setReceiver(TdTransport::ReceiveCallback());
    }
    if (m_transport)
        m_transport->shutdown();
    if (m_testBackend)
        m_testBackend->close(std::move(callback));
    else if (m_backend)
        m_backend->close(std::move(callback));
    else if (callback)
        callback(TdPollingBackend::CloseResult::Failed);
    m_owner = nullptr;
}
