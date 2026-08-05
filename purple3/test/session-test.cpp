/*
 * tdlib-purple - Telegram client for libpurple using TDLib
 * Copyright (C) tdlib-purple contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 3 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, see <https://www.gnu.org/licenses/>.
 */

#include "../telegram-purple3-connection-private.h"
#include "../telegram-purple3-session-private.h"

#include "application-credentials-test-backend.h"

#include "../../module-activity.h"
#include "../../td-request-id.h"

#include <gio/gio.h>
#include <glib.h>
#include <glib/gstdio.h>

#include <td/telegram/td_api.hpp>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace td::td_api;

namespace {

constexpr double pollTimeoutSeconds = 0.05;
constexpr unsigned closeTimeoutSeconds = 7;
constexpr unsigned reauthorizationCleanupTimeoutSeconds = 30;
constexpr char upperAccountId[] =
    "123E4567-E89B-12D3-A456-426614174000";
constexpr char lowerAccountId[] =
    "123e4567-e89b-12d3-a456-426614174000";
constexpr char syntheticQrFirst[] =
    "tg://login?token=c3ludGhldGljLWZpcnN0";
constexpr char syntheticQrSecond[] =
    "tg://login?token=c3ludGhldGljLXNlY29uZA";
constexpr char syntheticQrCancel[] =
    "tg://login?token=c3ludGhldGljLWNhbmNlbA";
constexpr char syntheticQrUnload[] =
    "tg://login?token=c3ludGhldGljLXVubG9hZA";

typedef struct _SessionTestModule SessionTestModule;
typedef struct _SessionTestModuleClass SessionTestModuleClass;

struct _SessionTestModule {
    GTypeModule parent;
};

struct _SessionTestModuleClass {
    GTypeModuleClass parentClass;
};

#define SESSION_TEST_TYPE_MODULE (session_test_module_get_type())

GType session_test_module_get_type(void);
G_DEFINE_TYPE(SessionTestModule, session_test_module, G_TYPE_TYPE_MODULE)

static gboolean sessionTestModuleLoad(
    G_GNUC_UNUSED GTypeModule *module)
{
    return TRUE;
}

static void sessionTestModuleUnload(
    G_GNUC_UNUSED GTypeModule *module)
{
}

static void session_test_module_init(
    G_GNUC_UNUSED SessionTestModule *module)
{
}

static void session_test_module_class_init(SessionTestModuleClass *klass)
{
    GTypeModuleClass *moduleClass = G_TYPE_MODULE_CLASS(klass);

    moduleClass->load = sessionTestModuleLoad;
    moduleClass->unload = sessionTestModuleUnload;
}

class ClientControl final {
public:
    struct SentRequest {
        std::uint64_t requestId = 0;
        TdPollingClient::FunctionPtr function;
    };

    void recordSend(
        std::uint64_t requestId,
        TdPollingClient::FunctionPtr function)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_sent.push_back({requestId, std::move(function)});
        m_condition.notify_all();
    }

    TdPollingClient::Response receive(double timeoutSeconds)
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        ++m_receiveCalls;
        m_condition.notify_all();

        const bool ready = m_condition.wait_for(
            lock,
            std::chrono::duration<double>(timeoutSeconds),
            [this]() { return m_failReceive || !m_responses.empty(); });
        if (!ready)
            return TdPollingClient::Response();

        if (m_failReceive) {
            m_failReceive = false;
            throw std::runtime_error("synthetic receive failure");
        }

        TdPollingClient::Response response =
            std::move(m_responses.front());
        m_responses.pop_front();
        ++m_responsesTaken;
        m_condition.notify_all();
        return response;
    }

    void push(TdPollingClient::Response response)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_responses.push_back(std::move(response));
        m_condition.notify_all();
    }

    void failNextReceive()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_failReceive = true;
        m_condition.notify_all();
    }

    bool waitForFunction(std::int32_t functionId, std::size_t count = 1)
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        return m_condition.wait_for(
            lock,
            std::chrono::seconds(5),
            [this, functionId, count]() {
                return countFunctionLocked(functionId) >= count;
            });
    }

    bool waitForResponsesTaken(std::size_t count)
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        return m_condition.wait_for(
            lock,
            std::chrono::seconds(5),
            [this, count]() { return m_responsesTaken >= count; });
    }

    bool waitForDestroyed()
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        return m_condition.wait_for(
            lock,
            std::chrono::seconds(5),
            [this]() { return m_destroyed; });
    }

    bool waitForReceiveCalls(unsigned count)
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        return m_condition.wait_for(
            lock,
            std::chrono::seconds(5),
            [this, count]() { return m_receiveCalls >= count; });
    }

    std::size_t countFunction(std::int32_t functionId) const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return countFunctionLocked(functionId);
    }

    std::size_t indexOfFunction(std::int32_t functionId) const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (std::size_t index = 0; index < m_sent.size(); ++index) {
            if (m_sent[index].function &&
                m_sent[index].function->get_id() == functionId) {
                return index;
            }
        }
        return m_sent.size();
    }

    std::uint64_t requestIdForFunction(std::int32_t functionId) const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (const SentRequest &request : m_sent) {
            if (request.function &&
                request.function->get_id() == functionId) {
                return request.requestId;
            }
        }
        return 0;
    }

    std::size_t responseCount() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_responsesTaken;
    }

    unsigned receiveCalls() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_receiveCalls;
    }

    bool parametersUseStorage(
        const std::string &expectedDirectory) const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (const SentRequest &request : m_sent) {
            if (!request.function ||
                request.function->get_id() != setTdlibParameters::ID) {
                continue;
            }
            const setTdlibParameters &parameters =
                static_cast<const setTdlibParameters &>(
                    *request.function);
            return parameters.database_directory_ == expectedDirectory &&
                   parameters.use_chat_info_database_ &&
                   parameters.use_message_database_ &&
                   parameters.use_secret_chats_;
        }
        return false;
    }

    void markDestroyed()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_destroyed = true;
        m_condition.notify_all();
    }

private:
    std::size_t countFunctionLocked(std::int32_t functionId) const
    {
        std::size_t count = 0;
        for (const SentRequest &request : m_sent) {
            if (request.function &&
                request.function->get_id() == functionId) {
                ++count;
            }
        }
        return count;
    }

    mutable std::mutex m_mutex;
    std::condition_variable m_condition;
    std::vector<SentRequest> m_sent;
    std::deque<TdPollingClient::Response> m_responses;
    std::size_t m_responsesTaken = 0;
    unsigned m_receiveCalls = 0;
    bool m_failReceive = false;
    bool m_destroyed = false;
};

class ControlledClient final : public TdPollingClient {
public:
    explicit ControlledClient(std::shared_ptr<ClientControl> control)
        : m_control(std::move(control))
    {
    }

    ~ControlledClient() override
    {
        m_control->markDestroyed();
    }

    void send(
        std::uint64_t requestId,
        FunctionPtr function) override
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

class ManualDeadlineControl final {
public:
    ~ManualDeadlineControl()
    {
        std::vector<GSource *> sources;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            sources.swap(m_sources);
        }
        for (GSource *source : sources)
            g_source_unref(source);
    }

    GSource *create(unsigned seconds)
    {
        GSource *source = g_source_new(
            &sourceFunctions(), sizeof(ManualSource));
        if (!source)
            return nullptr;

        reinterpret_cast<ManualSource *>(source)->ready = FALSE;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_intervals.push_back(seconds);
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

    bool hasInterval(unsigned seconds) const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (unsigned interval : m_intervals) {
            if (interval == seconds)
                return true;
        }
        return false;
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
            nullptr,
        };
        return functions;
    }

    mutable std::mutex m_mutex;
    std::vector<GSource *> m_sources;
    std::vector<unsigned> m_intervals;
};

typedef struct _SessionTestUi SessionTestUi;
typedef struct _SessionTestUiClass SessionTestUiClass;

struct _SessionTestUi {
    PurpleUi parent;

    PurpleQrCode *qr;
    GCancellable *qrCancellable;
    PurpleRequestPage *page;
    GTask *pageTask;
    guint qrPresentationCount;
    guint qrTextNotifyCount;
    guint pageRequestCount;
};

struct _SessionTestUiClass {
    PurpleUiClass parentClass;
};

#define SESSION_TEST_TYPE_UI (session_test_ui_get_type())
#define SESSION_TEST_UI(object) \
    (G_TYPE_CHECK_INSTANCE_CAST( \
        (object), SESSION_TEST_TYPE_UI, SessionTestUi))

GType session_test_ui_get_type(void);
G_DEFINE_TYPE(SessionTestUi, session_test_ui, PURPLE_TYPE_UI)

static void sessionTestUiQrCancelled(
    G_GNUC_UNUSED GCancellable *cancellable,
    gpointer data)
{
    SessionTestUi *ui = SESSION_TEST_UI(data);
    g_clear_object(&ui->qr);
    g_clear_object(&ui->qrCancellable);
}

static void sessionTestUiQrTextChanged(
    G_GNUC_UNUSED GObject *qr,
    G_GNUC_UNUSED GParamSpec *spec,
    gpointer data)
{
    SessionTestUi *ui = SESSION_TEST_UI(data);
    ++ui->qrTextNotifyCount;
}

static gboolean sessionTestUiPresentQr(
    PurpleUi *purpleUi,
    PurpleQrCode *qr,
    GCancellable *cancellable,
    G_GNUC_UNUSED GError **error)
{
    SessionTestUi *ui = SESSION_TEST_UI(purpleUi);
    ++ui->qrPresentationCount;
    g_set_object(&ui->qr, qr);
    g_set_object(&ui->qrCancellable, cancellable);
    g_signal_connect_object(
        qr,
        "notify::text",
        G_CALLBACK(sessionTestUiQrTextChanged),
        ui,
        G_CONNECT_DEFAULT);
    g_signal_connect_object(
        cancellable,
        "cancelled",
        G_CALLBACK(sessionTestUiQrCancelled),
        ui,
        G_CONNECT_DEFAULT);
    return TRUE;
}

static void sessionTestUiCompletePageError(SessionTestUi *ui)
{
    if (!ui->pageTask)
        return;

    GTask *task = ui->pageTask;
    ui->pageTask = nullptr;
    g_task_return_new_error_literal(
        task,
        G_IO_ERROR,
        G_IO_ERROR_CANCELLED,
        "synthetic page close");
    g_object_unref(task);
    g_clear_object(&ui->page);
}

static void sessionTestUiPageClosed(
    G_GNUC_UNUSED PurpleRequestPage *page,
    gpointer data)
{
    sessionTestUiCompletePageError(SESSION_TEST_UI(data));
}

static void sessionTestUiRequestPage(
    PurpleUi *purpleUi,
    PurpleRequestPage *page,
    GCancellable *cancellable,
    GAsyncReadyCallback callback,
    gpointer data)
{
    SessionTestUi *ui = SESSION_TEST_UI(purpleUi);
    g_assert_null(ui->pageTask);
    ++ui->pageRequestCount;
    ui->page = PURPLE_REQUEST_PAGE(g_object_ref(page));
    ui->pageTask = g_task_new(purpleUi, cancellable, callback, data);
    g_signal_connect_object(
        page,
        "close",
        G_CALLBACK(sessionTestUiPageClosed),
        ui,
        G_CONNECT_DEFAULT);
}

static PurpleRequestPage *sessionTestUiFinishPage(
    G_GNUC_UNUSED PurpleUi *ui,
    GAsyncResult *result,
    GError **error)
{
    return PURPLE_REQUEST_PAGE(
        g_task_propagate_pointer(G_TASK(result), error));
}

static void sessionTestUiFinalize(GObject *object)
{
    SessionTestUi *ui = SESSION_TEST_UI(object);
    if (ui->pageTask)
        sessionTestUiCompletePageError(ui);
    g_clear_object(&ui->page);
    g_clear_object(&ui->qr);
    g_clear_object(&ui->qrCancellable);
    G_OBJECT_CLASS(session_test_ui_parent_class)->finalize(object);
}

static void session_test_ui_init(
    G_GNUC_UNUSED SessionTestUi *ui)
{
}

static void session_test_ui_class_init(SessionTestUiClass *klass)
{
    GObjectClass *objectClass = G_OBJECT_CLASS(klass);
    PurpleUiClass *uiClass = PURPLE_UI_CLASS(klass);

    objectClass->finalize = sessionTestUiFinalize;
    uiClass->present_qr_code = sessionTestUiPresentQr;
    uiClass->request_page_async = sessionTestUiRequestPage;
    uiClass->request_page_finish = sessionTestUiFinishPage;
}

static void sessionTestUiCancelQr(SessionTestUi *ui)
{
    g_assert_nonnull(ui->qrCancellable);
    GCancellable *cancellable = G_CANCELLABLE(
        g_object_ref(ui->qrCancellable));
    g_cancellable_cancel(cancellable);
    g_object_unref(cancellable);
}

static void sessionTestUiAcceptPassword(SessionTestUi *ui)
{
    g_assert_nonnull(ui->pageTask);
    g_assert_nonnull(ui->page);

    PurpleRequestField *field =
        purple_request_page_get_field(ui->page, "password");
    g_assert_true(PURPLE_IS_REQUEST_FIELD_STRING(field));
    purple_request_field_string_set_value(
        PURPLE_REQUEST_FIELD_STRING(field),
        "synthetic-password-input");

    GTask *task = ui->pageTask;
    PurpleRequestPage *page = ui->page;
    ui->pageTask = nullptr;
    ui->page = nullptr;
    g_task_return_pointer(task, g_object_ref(page), g_object_unref);
    g_object_unref(task);
    g_object_unref(page);
}

struct CallbackRecord {
    guint ready = 0;
    guint connectFailed = 0;
    guint runtimeFailed = 0;
    guint reauthorizationRequired = 0;
    guint closed = 0;
    TelegramTdlibSessionFailure failure =
        TELEGRAM_TDLIB_SESSION_FAILURE_BACKEND;
    TelegramTdlibSessionCloseResult closeResult =
        TELEGRAM_TDLIB_SESSION_CLOSE_FAILED;
    TelegramTdlibSession *session = nullptr;
    bool closeOnRuntimeFailure = false;
};

constexpr char callbackRecordKey[] =
    "tdlib-purple-session-test-callback-record";

static CallbackRecord *callbackRecord(PurpleConnection *connection)
{
    return static_cast<CallbackRecord *>(
        g_object_get_data(G_OBJECT(connection), callbackRecordKey));
}

static void sessionReady(PurpleConnection *connection)
{
    CallbackRecord *record = callbackRecord(connection);
    if (record)
        ++record->ready;
}

static void sessionConnectFailed(
    PurpleConnection *connection,
    TelegramTdlibSessionFailure failure)
{
    CallbackRecord *record = callbackRecord(connection);
    if (record) {
        ++record->connectFailed;
        record->failure = failure;
    }
}

static void sessionRuntimeFailed(PurpleConnection *connection)
{
    CallbackRecord *record = callbackRecord(connection);
    if (record) {
        ++record->runtimeFailed;
        if (record->closeOnRuntimeFailure && record->session)
            telegram_tdlib_session_close(record->session);
    }
}

static void sessionReauthorizationRequired(PurpleConnection *connection)
{
    CallbackRecord *record = callbackRecord(connection);
    if (record)
        ++record->reauthorizationRequired;
}

static void sessionClosed(
    PurpleConnection *connection,
    TelegramTdlibSessionCloseResult result)
{
    CallbackRecord *record = callbackRecord(connection);
    if (record) {
        ++record->closed;
        record->closeResult = result;
    }
}

static TelegramTdlibSessionCallbacks sessionCallbacks()
{
    TelegramTdlibSessionCallbacks callbacks = {};
    callbacks.ready = sessionReady;
    callbacks.connect_failed = sessionConnectFailed;
    callbacks.runtime_failed = sessionRuntimeFailed;
    callbacks.reauthorization_required =
        sessionReauthorizationRequired;
    callbacks.closed = sessionClosed;
    return callbacks;
}

static bool removeTree(const char *path)
{
    if (!path)
        return true;

    GError *error = nullptr;
    GDir *directory = g_dir_open(path, 0, &error);
    if (!directory) {
        g_clear_error(&error);
        return g_remove(path) == 0;
    }

    const char *name = nullptr;
    bool removed = true;
    while ((name = g_dir_read_name(directory)) != nullptr) {
        gchar *child = g_build_filename(path, name, nullptr);
        const bool isDirectory =
            g_file_test(child, G_FILE_TEST_IS_DIR) &&
            !g_file_test(child, G_FILE_TEST_IS_SYMLINK);
        removed = (isDirectory ? removeTree(child)
                               : g_remove(child) == 0) &&
                  removed;
        g_free(child);
    }
    g_dir_close(directory);
    return g_rmdir(path) == 0 && removed;
}

class SessionEnvironment final {
public:
    SessionEnvironment()
    {
        m_context = g_main_context_new();
        g_assert_nonnull(m_context);
        g_main_context_push_thread_default(m_context);

        m_ui = SESSION_TEST_UI(
            g_object_new(SESSION_TEST_TYPE_UI, nullptr));
        g_assert_nonnull(m_ui);

        GError *error = nullptr;
        m_dataRoot = g_dir_make_tmp(
            "tdlib-purple-session-test-XXXXXX", &error);
        g_assert_no_error(error);
        g_assert_nonnull(m_dataRoot);
    }

    ~SessionEnvironment()
    {
        drain();
        g_assert_true(iterateUntil([]() {
            return !TdPollingBackend::hasActiveWorkers();
        }));
        g_assert_true(iterateUntil([]() {
            return !moduleActivityPending();
        }));
        g_clear_object(&m_ui);

        g_main_context_pop_thread_default(m_context);
        g_main_context_unref(m_context);
        g_assert_true(removeTree(m_dataRoot));
        g_free(m_dataRoot);
    }

    SessionEnvironment(const SessionEnvironment &) = delete;
    SessionEnvironment &operator=(const SessionEnvironment &) = delete;

    void drain()
    {
        while (g_main_context_iteration(m_context, FALSE)) {
        }
    }

    bool iterateUntil(const std::function<bool()> &predicate)
    {
        const gint64 deadline =
            g_get_monotonic_time() + 5 * G_TIME_SPAN_SECOND;
        while (!predicate() && g_get_monotonic_time() < deadline) {
            while (g_main_context_iteration(m_context, FALSE)) {
            }
            std::this_thread::yield();
        }
        return predicate();
    }

    GMainContext *context() const
    {
        return m_context;
    }

    SessionTestUi *ui() const
    {
        return m_ui;
    }

    const char *dataRoot() const
    {
        return m_dataRoot;
    }

private:
    GMainContext *m_context = nullptr;
    SessionTestUi *m_ui = nullptr;
    gchar *m_dataRoot = nullptr;
};

static TdlibPurpleApplicationCredentials syntheticCredentials()
{
    TdlibPurpleApplicationCredentials credentials = {};
    credentials.api_id = 1234567;
    constexpr char hash[] =
        "0123456789abcdef0123456789abcdef";
    static_assert(
        sizeof(hash) == TDLIB_PURPLE_API_HASH_LENGTH + 1,
        "synthetic API hash must have the production length");
    std::memcpy(credentials.api_hash, hash, sizeof(hash));
    return credentials;
}

class AsyncWait final {
public:
    ~AsyncWait()
    {
        clear();
    }

    AsyncWait(const AsyncWait &) = delete;
    AsyncWait &operator=(const AsyncWait &) = delete;

    AsyncWait() = default;

    static void completed(
        GObject *sourceObject,
        GAsyncResult *asyncResult,
        gpointer data)
    {
        AsyncWait *wait = static_cast<AsyncWait *>(data);
        ++wait->callbackCount;
        if (!wait->result) {
            wait->source = G_OBJECT(g_object_ref(sourceObject));
            wait->result = G_ASYNC_RESULT(g_object_ref(asyncResult));
        }
    }

    bool wait(SessionEnvironment &environment)
    {
        return environment.iterateUntil([this]() {
            return result != nullptr;
        });
    }

    void assertSingle(SessionEnvironment &environment)
    {
        environment.drain();
        g_assert_cmpuint(callbackCount, ==, 1);
    }

    void clear()
    {
        if (result)
            g_object_unref(result);
        result = nullptr;
        g_clear_object(&source);
    }

    GObject *source = nullptr;
    GAsyncResult *result = nullptr;
    guint callbackCount = 0;
};

struct AccountErrorObservation {
    std::vector<std::string> messages;
};

static void accountErrorChanged(
    PurpleAccount *account,
    G_GNUC_UNUSED GParamSpec *spec,
    gpointer data)
{
    AccountErrorObservation *observation =
        static_cast<AccountErrorObservation *>(data);
    GError *error = purple_account_get_error(account);
    if (error)
        observation->messages.emplace_back(error->message);
}

struct ConnectionFactoryObservation {
    guint calls = 0;
};

struct ReauthorizationConnectObservation {
    guint calls = 0;
    guint readyChecks = 0;
    guint readyAfterChecks = 0;
    PurpleAccount *account = nullptr;
};

static gboolean reauthorizationReady(
    PurpleAccount *account,
    gpointer data)
{
    std::shared_ptr<ReauthorizationConnectObservation> *observation =
        static_cast<
            std::shared_ptr<ReauthorizationConnectObservation> *>(data);
    g_assert_nonnull(observation);
    ++(*observation)->readyChecks;
    return (*observation)->readyChecks >
               (*observation)->readyAfterChecks &&
           purple_account_get_disconnected(account) &&
           purple_account_get_connection(account) == nullptr;
}

static void recordReauthorizationConnect(
    PurpleAccount *account,
    gpointer data)
{
    std::shared_ptr<ReauthorizationConnectObservation> *observation =
        static_cast<
            std::shared_ptr<ReauthorizationConnectObservation> *>(data);
    g_assert_nonnull(observation);
    ++(*observation)->calls;
    (*observation)->account = account;
}

static void destroyReauthorizationConnectObservation(gpointer data)
{
    delete static_cast<
        std::shared_ptr<ReauthorizationConnectObservation> *>(data);
}

struct ConnectionFactoryData {
    SessionEnvironment *environment = nullptr;
    std::shared_ptr<ClientControl> control;
    std::shared_ptr<ManualDeadlineControl> deadline;
    std::shared_ptr<ConnectionFactoryObservation> observation;
};

static void destroyConnectionFactoryData(gpointer data)
{
    delete static_cast<ConnectionFactoryData *>(data);
}

static TelegramTdlibSession *connectionSessionFactory(
    PurpleConnection *connection,
    const TdlibPurpleApplicationCredentials *credentials,
    GCancellable *connectionCancellable,
    const TelegramTdlibSessionCallbacks *callbacks,
    gpointer data,
    GError **error)
{
    ConnectionFactoryData *factory =
        static_cast<ConnectionFactoryData *>(data);
    g_assert_nonnull(factory);
    g_assert_nonnull(factory->environment);
    g_assert_nonnull(credentials);
    g_assert_nonnull(callbacks);
    ++factory->observation->calls;

    TelegramTdlibSessionDependencies dependencies;
    dependencies.clientFactory = [control = factory->control]() {
        return std::unique_ptr<TdPollingClient>(
            new ControlledClient(control));
    };
    dependencies.closeTimeoutSourceFactory =
        [deadline = factory->deadline](unsigned seconds) {
            return deadline->create(seconds);
        };
    dependencies.closeTimeoutSeconds = closeTimeoutSeconds;
    dependencies.pollTimeoutSeconds = pollTimeoutSeconds;
    dependencies.dataRoot = factory->environment->dataRoot();
    dependencies.ui = PURPLE_UI(factory->environment->ui());

    return telegramTdlibSessionCreate(
        connection,
        *credentials,
        connectionCancellable,
        *callbacks,
        std::move(dependencies),
        error);
}

class ConnectionHarness final {
public:
    ConnectionHarness(
        SessionEnvironment &environment,
        const char *accountId)
        : control(std::make_shared<ClientControl>()),
          deadline(std::make_shared<ManualDeadlineControl>()),
          observation(std::make_shared<ConnectionFactoryObservation>()),
          reauthorizationConnect(
              std::make_shared<ReauthorizationConnectObservation>())
    {
        TdlibPurpleApplicationCredentials credentials =
            syntheticCredentials();
        purple3_test_application_credentials_set(
            credentials.api_id,
            credentials.api_hash,
            TDLIB_PURPLE_API_HASH_LENGTH);
        std::memset(&credentials, 0, sizeof(credentials));

        account = PURPLE_ACCOUNT(g_object_new(
            PURPLE_TYPE_ACCOUNT,
            "id",
            accountId,
            "name",
            "Synthetic Telegram account",
            "protocol-id",
            "telegram-tdlib",
            nullptr));
        connection = PURPLE_CONNECTION(g_object_new(
            TELEGRAM_TDLIB_TYPE_CONNECTION,
            "account",
            account,
            nullptr));
        g_assert_nonnull(account);
        g_assert_true(TELEGRAM_TDLIB_IS_CONNECTION(connection));

        ConnectionFactoryData *factory = new ConnectionFactoryData();
        factory->environment = &environment;
        factory->control = control;
        factory->deadline = deadline;
        factory->observation = observation;
        g_assert_true(
            telegram_tdlib_connection_set_session_factory_for_test(
                TELEGRAM_TDLIB_CONNECTION(connection),
                connectionSessionFactory,
                factory,
                destroyConnectionFactoryData));
        g_assert_true(
            telegram_tdlib_connection_set_reauthorization_connect_for_test(
                TELEGRAM_TDLIB_CONNECTION(connection),
                recordReauthorizationConnect,
                new std::shared_ptr<ReauthorizationConnectObservation>(
                    reauthorizationConnect),
                destroyReauthorizationConnectObservation));
        g_assert_true(
            telegram_tdlib_connection_set_reauthorization_ready_for_test(
                TELEGRAM_TDLIB_CONNECTION(connection),
                reauthorizationReady));
        purple_account_set_connection(account, connection);
        purple_account_set_enabled(account, TRUE);
    }

    ~ConnectionHarness()
    {
        releaseConnection();
        g_clear_object(&account);
    }

    ConnectionHarness(const ConnectionHarness &) = delete;
    ConnectionHarness &operator=(const ConnectionHarness &) = delete;

    void connect(AsyncWait &wait)
    {
        purple_connection_connect_async(
            connection,
            purple_connection_get_cancellable(connection),
            AsyncWait::completed,
            &wait);
    }

    void disconnect(AsyncWait &wait)
    {
        purple_connection_disconnect_async(
            connection,
            nullptr,
            purple_connection_get_cancellable(connection),
            AsyncWait::completed,
            &wait);
    }

    void releaseConnection()
    {
        if (!connection)
            return;
        if (purple_account_get_connection(account) == connection)
            purple_account_set_connection(account, nullptr);
        g_clear_object(&connection);
    }

    std::shared_ptr<ClientControl> control;
    std::shared_ptr<ManualDeadlineControl> deadline;
    std::shared_ptr<ConnectionFactoryObservation> observation;
    std::shared_ptr<ReauthorizationConnectObservation>
        reauthorizationConnect;
    PurpleAccount *account = nullptr;
    PurpleConnection *connection = nullptr;
};

class SessionHarness final {
public:
    SessionHarness(
        SessionEnvironment &environment,
        const char *accountId)
        : m_environment(environment),
          control(std::make_shared<ClientControl>()),
          deadline(std::make_shared<ManualDeadlineControl>()),
          clientFactoryCalls(std::make_shared<guint>(0))
    {
        account = PURPLE_ACCOUNT(g_object_new(
            PURPLE_TYPE_ACCOUNT,
            "id",
            accountId,
            "name",
            "Synthetic Telegram account",
            "protocol-id",
            "telegram-tdlib",
            nullptr));
        connection = PURPLE_CONNECTION(g_object_new(
            PURPLE_TYPE_CONNECTION,
            "account",
            account,
            nullptr));
        cancellable = g_cancellable_new();
        g_assert_nonnull(account);
        g_assert_nonnull(connection);
        g_assert_nonnull(cancellable);
        g_object_set_data(
            G_OBJECT(connection), callbackRecordKey, &callbacks);

        TelegramTdlibSessionDependencies dependencies;
        dependencies.clientFactory = [
            control = control,
            calls = clientFactoryCalls]() {
            ++*calls;
            return std::unique_ptr<TdPollingClient>(
                new ControlledClient(control));
        };
        dependencies.closeTimeoutSourceFactory =
            [deadline = deadline](unsigned seconds) {
                return deadline->create(seconds);
            };
        dependencies.closeTimeoutSeconds = closeTimeoutSeconds;
        dependencies.pollTimeoutSeconds = pollTimeoutSeconds;
        dependencies.dataRoot = environment.dataRoot();
        dependencies.ui = PURPLE_UI(environment.ui());

        const TdlibPurpleApplicationCredentials credentials =
            syntheticCredentials();
        const TelegramTdlibSessionCallbacks callbackFunctions =
            sessionCallbacks();
        GError *error = nullptr;
        session = telegramTdlibSessionCreate(
            connection,
            credentials,
            cancellable,
            callbackFunctions,
            std::move(dependencies),
            &error);
        g_assert_no_error(error);
        g_assert_nonnull(session);
        callbacks.session = session;
    }

    ~SessionHarness()
    {
        if (session)
            telegram_tdlib_session_free(session);
        session = nullptr;
        callbacks.session = nullptr;
        g_object_set_data(G_OBJECT(connection), callbackRecordKey, nullptr);
        g_clear_object(&cancellable);
        g_clear_object(&connection);
        g_clear_object(&account);
    }

    SessionHarness(const SessionHarness &) = delete;
    SessionHarness &operator=(const SessionHarness &) = delete;

    bool start(GError **error = nullptr)
    {
        return telegram_tdlib_session_start(session, error);
    }

    void freeSession()
    {
        if (session)
            telegram_tdlib_session_free(session);
        session = nullptr;
        callbacks.session = nullptr;
    }

    std::string expectedDatabaseDirectory() const
    {
        gchar *normalized = g_ascii_strdown(
            purple_account_get_id(account), -1);
        gchar *path = g_build_filename(
            m_environment.dataRoot(),
            "telegram-tdlib",
            "accounts",
            normalized,
            nullptr);
        gchar *canonical = g_canonicalize_filename(path, nullptr);
        const std::string result = canonical ? canonical : "";
        g_free(canonical);
        g_free(path);
        g_free(normalized);
        return result;
    }

    SessionEnvironment &m_environment;
    std::shared_ptr<ClientControl> control;
    std::shared_ptr<ManualDeadlineControl> deadline;
    std::shared_ptr<guint> clientFactoryCalls;
    CallbackRecord callbacks;
    PurpleAccount *account = nullptr;
    PurpleConnection *connection = nullptr;
    GCancellable *cancellable = nullptr;
    TelegramTdlibSession *session = nullptr;
};

template <typename State>
static TdPollingClient::Response authorizationUpdate(
    object_ptr<State> state)
{
    object_ptr<AuthorizationState> authorizationState =
        td::move_tl_object_as<AuthorizationState>(std::move(state));
    return {
        0,
        make_object<updateAuthorizationState>(
            std::move(authorizationState)),
    };
}

static TdPollingClient::Response closedUpdate()
{
    return authorizationUpdate(
        make_object<authorizationStateClosed>());
}

static void startAndWaitForActivation(SessionHarness &harness)
{
    GError *error = nullptr;
    g_assert_true(harness.start(&error));
    g_assert_no_error(error);
    g_assert_true(harness.control->waitForFunction(getOption::ID));
    g_assert_cmpuint(
        harness.control->requestIdForFunction(getOption::ID),
        ==,
        td_request_id::ACTIVATE);
}

static void pushAndWait(
    SessionEnvironment &environment,
    const std::shared_ptr<ClientControl> &control,
    TdPollingClient::Response response)
{
    const std::size_t expected = control->responseCount() + 1;
    control->push(std::move(response));
    g_assert_true(control->waitForResponsesTaken(expected));
    const unsigned callsAfterDelivery = control->receiveCalls();
    g_assert_true(control->waitForReceiveCalls(callsAfterDelivery + 1));
    environment.drain();
}

static void connectAndReachReady(
    SessionEnvironment &environment,
    ConnectionHarness &harness,
    AsyncWait &wait)
{
    harness.connect(wait);
    g_assert_true(harness.control->waitForFunction(getOption::ID));
    g_assert_null(wait.result);
    g_assert_cmpuint(harness.observation->calls, ==, 1);

    pushAndWait(
        environment,
        harness.control,
        authorizationUpdate(make_object<authorizationStateReady>()));
    g_assert_true(wait.wait(environment));
    wait.assertSingle(environment);
    g_assert_true(wait.source == G_OBJECT(harness.connection));

    GError *error = nullptr;
    g_assert_true(purple_connection_connect_finish(
        harness.connection, wait.result, &error));
    g_assert_no_error(error);
    g_assert_true(purple_account_get_connected(harness.account));
}

static void finishClosed(
    SessionEnvironment &environment,
    SessionHarness &harness)
{
    const guint closedBefore = harness.callbacks.closed;
    telegram_tdlib_session_close(harness.session);
    telegram_tdlib_session_close(harness.session);
    g_assert_true(harness.control->waitForFunction(close::ID));
    g_assert_cmpuint(
        harness.control->countFunction(close::ID), ==, 1);

    const std::uint64_t closeRequest =
        harness.control->requestIdForFunction(close::ID);
    g_assert_cmpuint(closeRequest, ==, td_request_id::CLOSE);
    pushAndWait(
        environment,
        harness.control,
        {closeRequest, make_object<ok>()});
    g_assert_cmpuint(harness.callbacks.closed, ==, closedBefore);

    harness.control->push(closedUpdate());
    g_assert_true(harness.control->waitForDestroyed());
    g_assert_true(environment.iterateUntil([&harness, closedBefore]() {
        return harness.callbacks.closed == closedBefore + 1;
    }));
    g_assert_cmpuint(harness.callbacks.closed, ==, closedBefore + 1);
    g_assert_cmpint(
        harness.callbacks.closeResult,
        ==,
        TELEGRAM_TDLIB_SESSION_CLOSE_CLOSED);

    TelegramTdlibSessionCloseResult storedResult =
        TELEGRAM_TDLIB_SESSION_CLOSE_FAILED;
    g_assert_true(telegram_tdlib_session_get_close_result(
        harness.session, &storedResult));
    g_assert_cmpint(
        storedResult, ==, TELEGRAM_TDLIB_SESSION_CLOSE_CLOSED);
    telegram_tdlib_session_close(harness.session);
    telegram_tdlib_session_close(harness.session);
    environment.drain();
    g_assert_cmpuint(harness.callbacks.closed, ==, closedBefore + 1);
}

static void testQrOrderingRotationAndReadyExactOnce()
{
    SessionEnvironment environment;
    SessionHarness harness(environment, upperAccountId);
    startAndWaitForActivation(harness);

    pushAndWait(
        environment,
        harness.control,
        authorizationUpdate(
            make_object<authorizationStateWaitTdlibParameters>()));
    g_assert_true(
        harness.control->waitForFunction(setTdlibParameters::ID));
    g_assert_true(harness.control->parametersUseStorage(
        harness.expectedDatabaseDirectory()));

    pushAndWait(
        environment,
        harness.control,
        authorizationUpdate(
            make_object<authorizationStateWaitPhoneNumber>()));
    g_assert_true(harness.control->waitForFunction(
        requestQrCodeAuthentication::ID));

    const std::size_t activationIndex =
        harness.control->indexOfFunction(getOption::ID);
    const std::size_t parametersIndex =
        harness.control->indexOfFunction(setTdlibParameters::ID);
    const std::size_t qrRequestIndex =
        harness.control->indexOfFunction(
            requestQrCodeAuthentication::ID);
    g_assert_true(activationIndex < parametersIndex);
    g_assert_true(parametersIndex < qrRequestIndex);

    pushAndWait(
        environment,
        harness.control,
        authorizationUpdate(
            make_object<authorizationStateWaitPhoneNumber>()));
    g_assert_cmpuint(
        harness.control->countFunction(
            requestQrCodeAuthentication::ID),
        ==,
        1);

    pushAndWait(
        environment,
        harness.control,
        authorizationUpdate(make_object<
            authorizationStateWaitOtherDeviceConfirmation>(
            syntheticQrFirst)));
    g_assert_cmpuint(
        environment.ui()->qrPresentationCount, ==, 1);
    g_assert_nonnull(environment.ui()->qr);
    PurpleQrCode *originalQr = environment.ui()->qr;

    const std::uint64_t qrRequest =
        harness.control->requestIdForFunction(
            requestQrCodeAuthentication::ID);
    pushAndWait(
        environment,
        harness.control,
        {qrRequest, make_object<error>(400, "synthetic rejection")});
    g_assert_true(environment.ui()->qr == originalQr);
    g_assert_cmpuint(harness.callbacks.connectFailed, ==, 0);

    pushAndWait(
        environment,
        harness.control,
        authorizationUpdate(make_object<
            authorizationStateWaitOtherDeviceConfirmation>(
            syntheticQrFirst)));
    g_assert_cmpuint(environment.ui()->qrTextNotifyCount, ==, 0);

    pushAndWait(
        environment,
        harness.control,
        authorizationUpdate(make_object<
            authorizationStateWaitOtherDeviceConfirmation>(
            syntheticQrSecond)));
    g_assert_true(environment.ui()->qr == originalQr);
    g_assert_cmpuint(
        environment.ui()->qrPresentationCount, ==, 1);
    g_assert_cmpuint(environment.ui()->qrTextNotifyCount, ==, 1);

    pushAndWait(
        environment,
        harness.control,
        authorizationUpdate(make_object<authorizationStateReady>()));
    g_assert_true(environment.iterateUntil([&harness]() {
        return harness.callbacks.ready == 1;
    }));
    g_assert_cmpuint(harness.callbacks.ready, ==, 1);
    g_assert_cmpuint(harness.callbacks.connectFailed, ==, 0);
    g_assert_null(environment.ui()->qr);
    g_assert_null(environment.ui()->qrCancellable);

    pushAndWait(
        environment,
        harness.control,
        authorizationUpdate(make_object<authorizationStateReady>()));
    g_assert_cmpuint(harness.callbacks.ready, ==, 1);

    finishClosed(environment, harness);
}

static void testPasswordSubmissionReachesReady()
{
    SessionEnvironment environment;
    SessionHarness harness(environment, lowerAccountId);
    startAndWaitForActivation(harness);

    pushAndWait(
        environment,
        harness.control,
        authorizationUpdate(make_object<authorizationStateWaitPassword>(
            "synthetic hint", false, false, "")));
    g_assert_cmpuint(environment.ui()->pageRequestCount, ==, 1);
    g_assert_nonnull(environment.ui()->page);
    PurpleRequestField *field = purple_request_page_get_field(
        environment.ui()->page, "password");
    g_assert_true(PURPLE_IS_REQUEST_FIELD_STRING(field));
    g_assert_true(purple_request_field_is_required(field));
    g_assert_true(purple_request_field_string_is_masked(
        PURPLE_REQUEST_FIELD_STRING(field)));
    g_assert_false(purple_request_page_exists(
        environment.ui()->page, "remember"));

    sessionTestUiAcceptPassword(environment.ui());
    g_assert_true(environment.iterateUntil([&harness]() {
        return harness.control->countFunction(
                   checkAuthenticationPassword::ID) == 1;
    }));
    g_assert_cmpuint(
        harness.control->countFunction(
            checkAuthenticationPassword::ID),
        ==,
        1);

    pushAndWait(
        environment,
        harness.control,
        authorizationUpdate(make_object<authorizationStateReady>()));
    g_assert_cmpuint(harness.callbacks.ready, ==, 1);
    g_assert_null(environment.ui()->page);
    finishClosed(environment, harness);
}

static void testExplicitCloseWaitsForClosedUpdate()
{
    SessionEnvironment environment;
    SessionHarness harness(environment, lowerAccountId);
    startAndWaitForActivation(harness);

    telegram_tdlib_session_close(harness.session);
    telegram_tdlib_session_close(harness.session);
    g_assert_cmpuint(harness.callbacks.connectFailed, ==, 1);
    g_assert_cmpint(
        harness.callbacks.failure,
        ==,
        TELEGRAM_TDLIB_SESSION_FAILURE_CANCELLED);
    g_assert_true(harness.control->waitForFunction(close::ID));
    g_assert_cmpuint(harness.control->countFunction(close::ID), ==, 1);

    const std::uint64_t closeRequest =
        harness.control->requestIdForFunction(close::ID);
    pushAndWait(
        environment,
        harness.control,
        {closeRequest, make_object<ok>()});
    g_assert_cmpuint(harness.callbacks.closed, ==, 0);

    harness.control->push(closedUpdate());
    g_assert_true(harness.control->waitForDestroyed());
    g_assert_true(environment.iterateUntil([&harness]() {
        return harness.callbacks.closed == 1;
    }));
    g_assert_cmpuint(harness.callbacks.closed, ==, 1);
    g_assert_cmpint(
        harness.callbacks.closeResult,
        ==,
        TELEGRAM_TDLIB_SESSION_CLOSE_CLOSED);
}

static void testLoggingOutRequestsReauthorizationAfterPhysicalClose()
{
    SessionEnvironment environment;
    SessionHarness harness(environment, lowerAccountId);
    startAndWaitForActivation(harness);

    pushAndWait(
        environment,
        harness.control,
        authorizationUpdate(make_object<authorizationStateLoggingOut>()));
    g_assert_cmpuint(harness.control->countFunction(close::ID), ==, 0);
    g_assert_true(harness.deadline->hasInterval(
        reauthorizationCleanupTimeoutSeconds));
    g_assert_cmpuint(harness.callbacks.connectFailed, ==, 0);
    g_assert_cmpuint(harness.callbacks.runtimeFailed, ==, 0);
    g_assert_cmpuint(harness.callbacks.reauthorizationRequired, ==, 0);

    pushAndWait(
        environment,
        harness.control,
        authorizationUpdate(make_object<authorizationStateClosing>()));
    harness.control->push(closedUpdate());
    g_assert_true(harness.control->waitForDestroyed());
    g_assert_true(environment.iterateUntil([&harness]() {
        return harness.callbacks.reauthorizationRequired == 1;
    }));

    g_assert_cmpuint(harness.callbacks.reauthorizationRequired, ==, 1);
    g_assert_cmpuint(harness.callbacks.connectFailed, ==, 0);
    g_assert_cmpuint(harness.callbacks.runtimeFailed, ==, 0);
    g_assert_cmpuint(harness.callbacks.closed, ==, 0);
    g_assert_cmpuint(harness.control->countFunction(close::ID), ==, 0);
}

static void testLoggingOutCleanupTimeoutFailsClosed()
{
    SessionEnvironment environment;
    SessionHarness harness(environment, lowerAccountId);
    startAndWaitForActivation(harness);

    pushAndWait(
        environment,
        harness.control,
        authorizationUpdate(make_object<authorizationStateLoggingOut>()));
    g_assert_cmpuint(harness.control->countFunction(close::ID), ==, 0);
    g_assert_true(harness.deadline->hasInterval(
        reauthorizationCleanupTimeoutSeconds));
    g_assert_true(harness.deadline->fireNext());
    g_assert_true(environment.iterateUntil([&harness]() {
        return harness.callbacks.connectFailed == 1;
    }));
    g_assert_cmpint(
        harness.callbacks.failure,
        ==,
        TELEGRAM_TDLIB_SESSION_FAILURE_AUTHORIZATION);
    g_assert_cmpuint(harness.callbacks.reauthorizationRequired, ==, 0);
    g_assert_true(harness.control->waitForFunction(close::ID));

    const std::uint64_t closeRequest =
        harness.control->requestIdForFunction(close::ID);
    pushAndWait(
        environment,
        harness.control,
        {closeRequest, make_object<ok>()});
    harness.control->push(closedUpdate());
    g_assert_true(harness.control->waitForDestroyed());
    environment.drain();
    g_assert_cmpuint(harness.callbacks.reauthorizationRequired, ==, 0);
}

static void testQrUserCancellationIsPreReadyFailure()
{
    SessionEnvironment environment;
    SessionHarness harness(environment, lowerAccountId);
    startAndWaitForActivation(harness);

    pushAndWait(
        environment,
        harness.control,
        authorizationUpdate(make_object<
            authorizationStateWaitOtherDeviceConfirmation>(
            syntheticQrCancel)));
    g_assert_nonnull(environment.ui()->qrCancellable);
    sessionTestUiCancelQr(environment.ui());
    g_assert_true(environment.iterateUntil([&harness]() {
        return harness.callbacks.connectFailed == 1;
    }));
    g_assert_cmpint(
        harness.callbacks.failure,
        ==,
        TELEGRAM_TDLIB_SESSION_FAILURE_CANCELLED);
    g_assert_cmpuint(harness.callbacks.ready, ==, 0);
    g_assert_true(harness.control->waitForFunction(close::ID));

    harness.control->push(authorizationUpdate(
        make_object<authorizationStateReady>()));
    harness.control->push(closedUpdate());
    g_assert_true(harness.control->waitForDestroyed());
    environment.drain();
    g_assert_cmpuint(harness.callbacks.connectFailed, ==, 1);
    g_assert_cmpuint(harness.callbacks.ready, ==, 0);
    g_assert_cmpuint(harness.callbacks.closed, ==, 0);
}

static void testBackendFailureIsReportedOnceBeforeReady()
{
    SessionEnvironment environment;
    SessionHarness harness(environment, lowerAccountId);
    startAndWaitForActivation(harness);
    g_assert_true(harness.control->waitForReceiveCalls(1));

    harness.control->failNextReceive();
    g_assert_true(environment.iterateUntil([&harness]() {
        return harness.callbacks.connectFailed == 1;
    }));
    g_assert_cmpint(
        harness.callbacks.failure,
        ==,
        TELEGRAM_TDLIB_SESSION_FAILURE_BACKEND);
    g_assert_cmpuint(harness.callbacks.ready, ==, 0);
    g_assert_cmpuint(harness.callbacks.runtimeFailed, ==, 0);
    g_assert_true(harness.control->waitForDestroyed());
    g_assert_true(environment.iterateUntil([]() {
        return !TdPollingBackend::hasActiveWorkers();
    }));

    telegram_tdlib_session_cancel(harness.session);
    environment.drain();
    g_assert_cmpuint(harness.callbacks.connectFailed, ==, 1);
}

static void testBackendFailureAfterReadyIsRuntimeFailure()
{
    SessionEnvironment environment;
    SessionHarness harness(environment, lowerAccountId);
    startAndWaitForActivation(harness);

    pushAndWait(
        environment,
        harness.control,
        authorizationUpdate(make_object<authorizationStateReady>()));
    g_assert_cmpuint(harness.callbacks.ready, ==, 1);

    harness.callbacks.closeOnRuntimeFailure = true;
    harness.control->failNextReceive();
    g_assert_true(environment.iterateUntil([&harness]() {
        return harness.callbacks.runtimeFailed == 1;
    }));
    g_assert_cmpuint(harness.callbacks.connectFailed, ==, 0);
    g_assert_cmpuint(harness.callbacks.runtimeFailed, ==, 1);

    g_assert_true(harness.control->waitForDestroyed());
    g_assert_true(environment.iterateUntil([&harness]() {
        return harness.callbacks.closed == 1;
    }));
    g_assert_cmpint(
        harness.callbacks.closeResult,
        ==,
        TELEGRAM_TDLIB_SESSION_CLOSE_FAILED);

    telegram_tdlib_session_cancel(harness.session);
    environment.drain();
    g_assert_cmpuint(harness.callbacks.runtimeFailed, ==, 1);
    g_assert_cmpuint(harness.callbacks.closed, ==, 1);
}

static void testTimeoutKeepsNormalizedSessionKeyBusyUntilLateClose()
{
    SessionEnvironment environment;
    std::unique_ptr<SessionHarness> first(
        new SessionHarness(environment, upperAccountId));
    startAndWaitForActivation(*first);

    telegram_tdlib_session_close(first->session);
    g_assert_true(first->control->waitForFunction(close::ID));
    g_assert_true(first->deadline->hasInterval(closeTimeoutSeconds));
    g_assert_true(first->deadline->fireNext());
    g_assert_true(environment.iterateUntil([&first]() {
        return first->callbacks.closed == 1;
    }));
    g_assert_cmpint(
        first->callbacks.closeResult,
        ==,
        TELEGRAM_TDLIB_SESSION_CLOSE_TIMED_OUT);
    g_assert_true(TdPollingBackend::hasActiveWorkers());
    g_assert_true(telegram_tdlib_session_module_busy());

    {
        SessionHarness competing(environment, lowerAccountId);
        GError *error = nullptr;
        g_assert_false(competing.start(&error));
        g_assert_error(error, G_IO_ERROR, G_IO_ERROR_BUSY);
        g_clear_error(&error);
        g_assert_cmpuint(competing.callbacks.ready, ==, 0);
        g_assert_cmpuint(competing.callbacks.connectFailed, ==, 0);
    }

    first->control->push(closedUpdate());
    g_assert_true(first->control->waitForDestroyed());
    g_assert_true(environment.iterateUntil([]() {
        return !TdPollingBackend::hasActiveWorkers();
    }));
    environment.drain();
    g_assert_cmpuint(first->callbacks.closed, ==, 1);
    first.reset();
    environment.drain();
    g_assert_false(telegram_tdlib_session_module_busy());

    SessionHarness replacement(environment, lowerAccountId);
    startAndWaitForActivation(replacement);
    finishClosed(environment, replacement);
}

static void testCancelBeforeStartSettlesWithoutWorker()
{
    SessionEnvironment environment;
    SessionHarness harness(environment, lowerAccountId);

    telegram_tdlib_session_cancel(harness.session);
    telegram_tdlib_session_cancel(harness.session);
    g_assert_cmpuint(harness.callbacks.connectFailed, ==, 1);
    g_assert_cmpint(
        harness.callbacks.failure,
        ==,
        TELEGRAM_TDLIB_SESSION_FAILURE_CANCELLED);
    g_assert_cmpuint(harness.callbacks.ready, ==, 0);
    g_assert_cmpuint(harness.callbacks.closed, ==, 0);
    g_assert_cmpuint(*harness.clientFactoryCalls, ==, 0);
    g_assert_cmpuint(harness.control->receiveCalls(), ==, 0);
    g_assert_cmpuint(harness.control->countFunction(getOption::ID), ==, 0);

    TelegramTdlibSessionCloseResult result =
        TELEGRAM_TDLIB_SESSION_CLOSE_FAILED;
    g_assert_true(environment.iterateUntil([&harness, &result]() {
        return telegram_tdlib_session_get_close_result(
            harness.session, &result);
    }));
    g_assert_cmpint(
        result, ==, TELEGRAM_TDLIB_SESSION_CLOSE_CLOSED);
    g_assert_true(environment.iterateUntil([]() {
        return !TdPollingBackend::hasActiveWorkers();
    }));
    g_assert_true(telegram_tdlib_session_module_busy());

    harness.freeSession();
    g_assert_true(environment.iterateUntil([]() {
        return !telegram_tdlib_session_module_busy();
    }));
}

static void testPrepareUnloadCancelsOpenQrPrompt()
{
    SessionEnvironment environment;
    SessionHarness harness(environment, lowerAccountId);
    startAndWaitForActivation(harness);

    pushAndWait(
        environment,
        harness.control,
        authorizationUpdate(make_object<
            authorizationStateWaitOtherDeviceConfirmation>(
            syntheticQrUnload)));
    g_assert_nonnull(environment.ui()->qr);
    g_assert_nonnull(environment.ui()->qrCancellable);
    g_assert_true(telegram_tdlib_session_module_busy());

    telegram_tdlib_session_prepare_unload();
    g_assert_true(environment.iterateUntil([&harness]() {
        return harness.callbacks.connectFailed == 1;
    }));
    g_assert_cmpint(
        harness.callbacks.failure,
        ==,
        TELEGRAM_TDLIB_SESSION_FAILURE_CANCELLED);
    g_assert_cmpuint(harness.callbacks.ready, ==, 0);
    g_assert_null(environment.ui()->qr);
    g_assert_null(environment.ui()->qrCancellable);
    g_assert_true(harness.control->waitForFunction(close::ID));
    g_assert_cmpuint(harness.control->countFunction(close::ID), ==, 1);
    g_assert_true(telegram_tdlib_session_module_busy());

    harness.control->push(closedUpdate());
    g_assert_true(harness.control->waitForDestroyed());
    environment.drain();
    g_assert_cmpuint(harness.callbacks.closed, ==, 0);
    g_assert_true(telegram_tdlib_session_module_busy());

    harness.freeSession();
    g_assert_true(environment.iterateUntil([]() {
        return !telegram_tdlib_session_module_busy();
    }));
}

static void testConnectionPreCancelledSkipsSessionFactory()
{
    SessionEnvironment environment;
    ConnectionHarness harness(environment, lowerAccountId);
    AsyncWait connectWait;

    g_cancellable_cancel(
        purple_connection_get_cancellable(harness.connection));
    harness.connect(connectWait);
    g_assert_true(connectWait.wait(environment));
    connectWait.assertSingle(environment);

    GError *error = nullptr;
    g_assert_false(purple_connection_connect_finish(
        harness.connection, connectWait.result, &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
    g_clear_error(&error);
    g_assert_cmpuint(harness.observation->calls, ==, 0);
    g_assert_cmpuint(harness.control->receiveCalls(), ==, 0);
    g_assert_cmpuint(harness.control->countFunction(getOption::ID), ==, 0);
    g_assert_false(TdPollingBackend::hasActiveWorkers());
    g_assert_true(
        purple_account_get_connection(harness.account) == nullptr);

    AsyncWait disconnectWait;
    harness.disconnect(disconnectWait);
    g_assert_true(disconnectWait.wait(environment));
    disconnectWait.assertSingle(environment);
    g_assert_true(purple_connection_disconnect_finish(
        harness.connection, disconnectWait.result, &error));
    g_assert_no_error(error);

    connectWait.clear();
    disconnectWait.clear();
    gpointer weakConnection = harness.connection;
    g_object_add_weak_pointer(
        G_OBJECT(harness.connection), &weakConnection);
    harness.releaseConnection();
    g_assert_null(weakConnection);
}

static void testConnectionInvalidAccountStateSkipsSessionFactory()
{
    SessionEnvironment environment;
    ConnectionHarness harness(environment, lowerAccountId);
    AsyncWait disabledWait;

    purple_account_set_enabled(harness.account, FALSE);
    harness.connect(disabledWait);
    g_assert_true(disabledWait.wait(environment));
    disabledWait.assertSingle(environment);

    GError *error = nullptr;
    g_assert_false(purple_connection_connect_finish(
        harness.connection, disabledWait.result, &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
    g_clear_error(&error);
    g_assert_cmpuint(harness.observation->calls, ==, 0);
    g_assert_null(purple_account_get_connection(harness.account));
    disabledWait.clear();

    PurpleConnection *replacement = PURPLE_CONNECTION(g_object_new(
        TELEGRAM_TDLIB_TYPE_CONNECTION,
        "account",
        harness.account,
        nullptr));
    g_assert_nonnull(replacement);
    purple_account_set_enabled(harness.account, TRUE);
    purple_account_set_connection(harness.account, replacement);

    AsyncWait staleWait;
    harness.connect(staleWait);
    g_assert_true(staleWait.wait(environment));
    staleWait.assertSingle(environment);
    g_assert_false(purple_connection_connect_finish(
        harness.connection, staleWait.result, &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
    g_clear_error(&error);
    g_assert_cmpuint(harness.observation->calls, ==, 0);
    g_assert_true(
        purple_account_get_connection(harness.account) == replacement);

    staleWait.clear();
    purple_account_set_connection(harness.account, nullptr);
    g_object_unref(replacement);

    gpointer weakConnection = harness.connection;
    g_object_add_weak_pointer(
        G_OBJECT(harness.connection), &weakConnection);
    harness.releaseConnection();
    g_assert_null(weakConnection);
}

static void testConnectionReadyAndJoinedDisconnectTasks()
{
    SessionEnvironment environment;
    ConnectionHarness harness(environment, lowerAccountId);
    AsyncWait connectWait;
    connectAndReachReady(environment, harness, connectWait);
    connectWait.clear();

    AsyncWait firstDisconnect;
    AsyncWait secondDisconnect;
    harness.disconnect(firstDisconnect);
    harness.disconnect(secondDisconnect);
    g_assert_true(harness.control->waitForFunction(close::ID));
    g_assert_cmpuint(harness.control->countFunction(close::ID), ==, 1);

    const std::uint64_t closeRequest =
        harness.control->requestIdForFunction(close::ID);
    pushAndWait(
        environment,
        harness.control,
        {closeRequest, make_object<ok>()});
    g_assert_null(firstDisconnect.result);
    g_assert_null(secondDisconnect.result);

    harness.control->push(closedUpdate());
    g_assert_true(harness.control->waitForDestroyed());
    g_assert_true(environment.iterateUntil(
        [&firstDisconnect, &secondDisconnect]() {
            return firstDisconnect.result != nullptr &&
                   secondDisconnect.result != nullptr;
        }));
    firstDisconnect.assertSingle(environment);
    secondDisconnect.assertSingle(environment);

    GError *error = nullptr;
    g_assert_true(purple_connection_disconnect_finish(
        harness.connection, firstDisconnect.result, &error));
    g_assert_no_error(error);
    g_assert_true(purple_connection_disconnect_finish(
        harness.connection, secondDisconnect.result, &error));
    g_assert_no_error(error);

    AsyncWait cachedDisconnect;
    harness.disconnect(cachedDisconnect);
    g_assert_true(cachedDisconnect.wait(environment));
    cachedDisconnect.assertSingle(environment);
    g_assert_true(purple_connection_disconnect_finish(
        harness.connection, cachedDisconnect.result, &error));
    g_assert_no_error(error);
    g_assert_cmpuint(harness.control->countFunction(close::ID), ==, 1);

    firstDisconnect.clear();
    secondDisconnect.clear();
    cachedDisconnect.clear();
    gpointer weakConnection = harness.connection;
    g_object_add_weak_pointer(
        G_OBJECT(harness.connection), &weakConnection);
    harness.releaseConnection();
    g_assert_null(weakConnection);
    g_assert_true(environment.iterateUntil([]() {
        return !telegram_tdlib_session_module_busy();
    }));
}

static void testConnectionReceiveFailureDisconnectsAccount()
{
    SessionEnvironment environment;
    ConnectionHarness harness(environment, lowerAccountId);
    AsyncWait connectWait;
    connectAndReachReady(environment, harness, connectWait);
    connectWait.clear();

    AccountErrorObservation errors;
    const gulong errorHandler = g_signal_connect(
        harness.account,
        "notify::error",
        G_CALLBACK(accountErrorChanged),
        &errors);

    harness.control->failNextReceive();
    g_assert_true(harness.control->waitForDestroyed());
    g_assert_true(environment.iterateUntil([&harness]() {
        return purple_account_get_disconnected(harness.account) &&
               purple_account_get_connection(harness.account) == nullptr;
    }));
    g_assert_cmpuint(harness.control->countFunction(close::ID), ==, 0);

    GError *accountError = purple_account_get_error(harness.account);
    g_assert_error(accountError, G_IO_ERROR, G_IO_ERROR_FAILED);
    g_assert_cmpuint(errors.messages.size(), ==, 1);
    g_assert_cmpstr(
        errors.messages[0].c_str(),
        ==,
        "The Telegram connection stopped unexpectedly.");
    g_assert_cmpstr(
        accountError->message, ==, errors.messages[0].c_str());
    g_signal_handler_disconnect(harness.account, errorHandler);

    gpointer weakConnection = harness.connection;
    g_object_add_weak_pointer(
        G_OBJECT(harness.connection), &weakConnection);
    harness.releaseConnection();
    g_assert_null(weakConnection);
    g_assert_true(environment.iterateUntil([]() {
        return !telegram_tdlib_session_module_busy();
    }));
}

static void testConnectionAuthorizationFailureDisconnectsAccount()
{
    SessionEnvironment environment;
    ConnectionHarness harness(environment, lowerAccountId);
    AsyncWait connectWait;
    connectAndReachReady(environment, harness, connectWait);
    connectWait.clear();

    AccountErrorObservation errors;
    const gulong errorHandler = g_signal_connect(
        harness.account,
        "notify::error",
        G_CALLBACK(accountErrorChanged),
        &errors);

    pushAndWait(
        environment,
        harness.control,
        authorizationUpdate(make_object<authorizationStateClosing>()));
    g_assert_true(purple_account_get_disconnecting(harness.account));
    g_assert_true(harness.control->waitForFunction(close::ID));
    g_assert_cmpuint(harness.control->countFunction(close::ID), ==, 1);

    harness.control->push(closedUpdate());
    g_assert_true(harness.control->waitForDestroyed());
    g_assert_true(environment.iterateUntil([&harness]() {
        return purple_account_get_disconnected(harness.account) &&
               purple_account_get_connection(harness.account) == nullptr;
    }));
    g_assert_cmpuint(harness.control->countFunction(close::ID), ==, 1);

    GError *accountError = purple_account_get_error(harness.account);
    g_assert_error(accountError, G_IO_ERROR, G_IO_ERROR_FAILED);
    g_assert_cmpuint(errors.messages.size(), ==, 1);
    g_assert_cmpstr(
        errors.messages[0].c_str(),
        ==,
        "The Telegram connection stopped unexpectedly.");
    g_assert_cmpstr(
        accountError->message, ==, errors.messages[0].c_str());
    g_signal_handler_disconnect(harness.account, errorHandler);

    gpointer weakConnection = harness.connection;
    g_object_add_weak_pointer(
        G_OBJECT(harness.connection), &weakConnection);
    harness.releaseConnection();
    g_assert_null(weakConnection);
    g_assert_true(environment.iterateUntil([]() {
        return !telegram_tdlib_session_module_busy();
    }));
}

static void testConnectionLoggingOutBeforeReadyReconnectsOnce()
{
    SessionEnvironment environment;
    ConnectionHarness harness(environment, lowerAccountId);
    purple_account_set_enabled(harness.account, TRUE);
    purple_account_ready(harness.account);
    AsyncWait connectWait;

    harness.connect(connectWait);
    g_assert_true(harness.control->waitForFunction(getOption::ID));
    g_assert_null(connectWait.result);
    pushAndWait(
        environment,
        harness.control,
        authorizationUpdate(make_object<authorizationStateLoggingOut>()));
    g_assert_cmpuint(harness.control->countFunction(close::ID), ==, 0);
    g_assert_cmpuint(harness.reauthorizationConnect->calls, ==, 0);

    pushAndWait(
        environment,
        harness.control,
        authorizationUpdate(make_object<authorizationStateClosing>()));
    harness.control->push(closedUpdate());
    g_assert_true(harness.control->waitForDestroyed());
    g_assert_true(environment.iterateUntil([&harness]() {
        return harness.reauthorizationConnect->calls == 1;
    }));

    g_assert_true(connectWait.wait(environment));
    connectWait.assertSingle(environment);
    GError *error = nullptr;
    g_assert_false(purple_connection_connect_finish(
        harness.connection, connectWait.result, &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
    g_clear_error(&error);
    g_assert_cmpuint(harness.reauthorizationConnect->calls, ==, 1);
    g_assert_true(
        harness.reauthorizationConnect->account == harness.account);
    g_assert_true(purple_account_get_enabled(harness.account));
    g_assert_true(purple_account_get_disconnected(harness.account));
    g_assert_null(purple_account_get_connection(harness.account));
    g_assert_null(purple_account_get_error(harness.account));

    connectWait.clear();
    gpointer weakConnection = harness.connection;
    g_object_add_weak_pointer(
        G_OBJECT(harness.connection), &weakConnection);
    harness.releaseConnection();
    g_assert_null(weakConnection);
    g_assert_true(environment.iterateUntil([]() {
        return !telegram_tdlib_session_module_busy();
    }));
}

static void testConnectionPreReadyRecoveryDisableCompletesConnect()
{
    SessionEnvironment environment;
    ConnectionHarness harness(environment, lowerAccountId);
    harness.reauthorizationConnect->readyAfterChecks = 1000;
    purple_account_set_enabled(harness.account, TRUE);
    purple_account_ready(harness.account);
    AsyncWait connectWait;

    harness.connect(connectWait);
    g_assert_true(harness.control->waitForFunction(getOption::ID));
    pushAndWait(
        environment,
        harness.control,
        authorizationUpdate(make_object<authorizationStateLoggingOut>()));
    g_assert_cmpuint(harness.control->countFunction(close::ID), ==, 0);

    pushAndWait(
        environment,
        harness.control,
        authorizationUpdate(make_object<authorizationStateClosing>()));
    harness.control->push(closedUpdate());
    g_assert_true(harness.control->waitForDestroyed());
    environment.drain();
    purple_account_set_enabled(harness.account, FALSE);

    g_assert_true(connectWait.wait(environment));
    connectWait.assertSingle(environment);
    GError *error = nullptr;
    g_assert_false(purple_connection_connect_finish(
        harness.connection, connectWait.result, &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
    g_clear_error(&error);
    g_assert_cmpuint(harness.reauthorizationConnect->calls, ==, 0);
    g_assert_false(purple_account_get_enabled(harness.account));
    g_assert_true(environment.iterateUntil([&harness]() {
        return purple_account_get_disconnected(harness.account) &&
               purple_account_get_connection(harness.account) == nullptr;
    }));

    connectWait.clear();
    gpointer weakConnection = harness.connection;
    g_object_add_weak_pointer(
        G_OBJECT(harness.connection), &weakConnection);
    harness.releaseConnection();
    g_assert_null(weakConnection);
    g_assert_true(environment.iterateUntil([]() {
        return !telegram_tdlib_session_module_busy();
    }));
}

static void testConnectionLoggingOutReconnectsAfterPhysicalClose()
{
    SessionEnvironment environment;
    ConnectionHarness harness(environment, lowerAccountId);
    harness.reauthorizationConnect->readyAfterChecks = 160;
    purple_account_set_enabled(harness.account, TRUE);
    AsyncWait connectWait;
    connectAndReachReady(environment, harness, connectWait);
    connectWait.clear();

    pushAndWait(
        environment,
        harness.control,
        authorizationUpdate(make_object<authorizationStateLoggingOut>()));
    g_assert_cmpuint(harness.control->countFunction(close::ID), ==, 0);
    g_assert_cmpuint(harness.reauthorizationConnect->calls, ==, 0);
    g_assert_true(purple_account_get_connected(harness.account));

    pushAndWait(
        environment,
        harness.control,
        authorizationUpdate(make_object<authorizationStateClosing>()));
    harness.control->push(closedUpdate());
    g_assert_true(harness.control->waitForDestroyed());
    g_assert_true(environment.iterateUntil([&harness]() {
        return harness.reauthorizationConnect->calls == 1;
    }));

    g_assert_cmpuint(harness.reauthorizationConnect->calls, ==, 1);
    g_assert_cmpuint(
        harness.reauthorizationConnect->readyChecks, >, 128);
    g_assert_true(
        harness.reauthorizationConnect->account == harness.account);
    g_assert_true(purple_account_get_disconnected(harness.account));
    g_assert_null(purple_account_get_connection(harness.account));
    g_assert_null(purple_account_get_error(harness.account));
    g_assert_cmpuint(harness.control->countFunction(close::ID), ==, 0);

    gpointer weakConnection = harness.connection;
    g_object_add_weak_pointer(
        G_OBJECT(harness.connection), &weakConnection);
    harness.releaseConnection();
    g_assert_null(weakConnection);
    g_assert_true(environment.iterateUntil([]() {
        return !telegram_tdlib_session_module_busy();
    }));
}

static void testConnectionDisconnectTimeoutAndLateReap()
{
    SessionEnvironment environment;
    ConnectionHarness harness(environment, upperAccountId);
    AsyncWait connectWait;
    connectAndReachReady(environment, harness, connectWait);
    connectWait.clear();

    AsyncWait disconnectWait;
    harness.disconnect(disconnectWait);
    g_assert_true(harness.control->waitForFunction(close::ID));
    g_assert_true(harness.deadline->hasInterval(closeTimeoutSeconds));
    g_assert_true(harness.deadline->fireNext());
    g_assert_true(disconnectWait.wait(environment));
    disconnectWait.assertSingle(environment);

    GError *error = nullptr;
    g_assert_false(purple_connection_disconnect_finish(
        harness.connection, disconnectWait.result, &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT);
    g_clear_error(&error);
    g_assert_true(TdPollingBackend::hasActiveWorkers());
    g_assert_true(telegram_tdlib_session_module_busy());

    disconnectWait.clear();
    gpointer weakConnection = harness.connection;
    g_object_add_weak_pointer(
        G_OBJECT(harness.connection), &weakConnection);
    harness.releaseConnection();
    g_assert_null(weakConnection);

    harness.control->push(closedUpdate());
    g_assert_true(harness.control->waitForDestroyed());
    g_assert_true(environment.iterateUntil([]() {
        return !TdPollingBackend::hasActiveWorkers() &&
               !telegram_tdlib_session_module_busy();
    }));
    g_assert_cmpuint(disconnectWait.callbackCount, ==, 1);
}

static void testConnectionForeignCancellationWinsReadyRace()
{
    SessionEnvironment environment;
    ConnectionHarness harness(environment, lowerAccountId);
    AsyncWait connectWait;
    harness.connect(connectWait);
    g_assert_true(harness.control->waitForFunction(getOption::ID));

    const std::size_t expectedResponses =
        harness.control->responseCount() + 1;
    harness.control->push(
        authorizationUpdate(make_object<authorizationStateReady>()));
    g_assert_true(
        harness.control->waitForResponsesTaken(expectedResponses));
    const unsigned receiveCalls = harness.control->receiveCalls();
    g_assert_true(
        harness.control->waitForReceiveCalls(receiveCalls + 1));

    GCancellable *cancellable = G_CANCELLABLE(g_object_ref(
        purple_connection_get_cancellable(harness.connection)));
    std::thread cancellationThread([cancellable]() {
        g_cancellable_cancel(cancellable);
        g_object_unref(cancellable);
    });
    cancellationThread.join();

    g_assert_true(connectWait.wait(environment));
    connectWait.assertSingle(environment);
    GError *error = nullptr;
    g_assert_false(purple_connection_connect_finish(
        harness.connection, connectWait.result, &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
    g_clear_error(&error);
    g_assert_false(purple_account_get_connected(harness.account));
    g_assert_true(
        purple_account_get_connection(harness.account) == nullptr);
    g_assert_true(harness.control->waitForFunction(close::ID));
    g_assert_cmpuint(harness.control->countFunction(close::ID), ==, 1);

    harness.control->push(closedUpdate());
    g_assert_true(harness.control->waitForDestroyed());
    environment.drain();
    connectWait.assertSingle(environment);

    connectWait.clear();
    gpointer weakConnection = harness.connection;
    g_object_add_weak_pointer(
        G_OBJECT(harness.connection), &weakConnection);
    harness.releaseConnection();
    g_assert_null(weakConnection);
    g_assert_true(environment.iterateUntil([]() {
        return !telegram_tdlib_session_module_busy();
    }));
}

} // namespace

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, nullptr);

    GTypeModule *module = G_TYPE_MODULE(g_object_new(
        SESSION_TEST_TYPE_MODULE, nullptr));
    g_assert_nonnull(module);
    g_assert_true(g_type_module_use(module));
    telegram_tdlib_connection_register_module_for_test(module);
    g_assert_cmpuint(TELEGRAM_TDLIB_TYPE_CONNECTION, !=, G_TYPE_INVALID);

    g_test_add_func(
        "/purple3/session/qr-order-rotation-ready",
        testQrOrderingRotationAndReadyExactOnce);
    g_test_add_func(
        "/purple3/session/password-ready",
        testPasswordSubmissionReachesReady);
    g_test_add_func(
        "/purple3/session/close-awaits-closed",
        testExplicitCloseWaitsForClosedUpdate);
    g_test_add_func(
        "/purple3/session/logging-out-reauthorizes",
        testLoggingOutRequestsReauthorizationAfterPhysicalClose);
    g_test_add_func(
        "/purple3/session/logging-out-cleanup-timeout",
        testLoggingOutCleanupTimeoutFailsClosed);
    g_test_add_func(
        "/purple3/session/qr-user-cancel",
        testQrUserCancellationIsPreReadyFailure);
    g_test_add_func(
        "/purple3/session/backend-failure",
        testBackendFailureIsReportedOnceBeforeReady);
    g_test_add_func(
        "/purple3/session/runtime-failure",
        testBackendFailureAfterReadyIsRuntimeFailure);
    g_test_add_func(
        "/purple3/session/timeout-session-busy-late-close",
        testTimeoutKeepsNormalizedSessionKeyBusyUntilLateClose);
    g_test_add_func(
        "/purple3/session/cancel-before-start",
        testCancelBeforeStartSettlesWithoutWorker);
    g_test_add_func(
        "/purple3/session/prepare-unload-open-qr",
        testPrepareUnloadCancelsOpenQrPrompt);
    g_test_add_func(
        "/purple3/connection/pre-cancel-skips-session",
        testConnectionPreCancelledSkipsSessionFactory);
    g_test_add_func(
        "/purple3/connection/invalid-account-state-skips-session",
        testConnectionInvalidAccountStateSkipsSessionFactory);
    g_test_add_func(
        "/purple3/connection/ready-joined-disconnect",
        testConnectionReadyAndJoinedDisconnectTasks);
    g_test_add_func(
        "/purple3/connection/receive-failure-disconnects-account",
        testConnectionReceiveFailureDisconnectsAccount);
    g_test_add_func(
        "/purple3/connection/auth-failure-disconnects-account",
        testConnectionAuthorizationFailureDisconnectsAccount);
    g_test_add_func(
        "/purple3/connection/logging-out-reconnects",
        testConnectionLoggingOutReconnectsAfterPhysicalClose);
    g_test_add_func(
        "/purple3/connection/logging-out-before-ready-reconnects",
        testConnectionLoggingOutBeforeReadyReconnectsOnce);
    g_test_add_func(
        "/purple3/connection/pre-ready-recovery-disable-completes",
        testConnectionPreReadyRecoveryDisableCompletesConnect);
    g_test_add_func(
        "/purple3/connection/disconnect-timeout-late-reap",
        testConnectionDisconnectTimeoutAndLateReap);
    g_test_add_func(
        "/purple3/connection/foreign-cancel-wins-ready",
        testConnectionForeignCancellationWinsReadyRace);

    const int result = g_test_run();
    purple3_test_application_credentials_set_unavailable();

    /* Dynamic GObject types retain their module for the process lifetime. */
    (void)module;
    return result;
}
