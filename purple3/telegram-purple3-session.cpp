/*
 * tdlib-purple - Telegram client for libpurple using TDLib
 * Copyright (C) tdlib-purple contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 3 of the License, or (at your option)
 * any later version.
 */

#include "telegram-purple3-session-private.h"

#include "telegram-purple3-auth-presenter.h"

#include "module-activity.h"
#include "td-auth-controller.h"
#include "td-transport.h"

#include <glib/gstdio.h>
#include <glib/gi18n-lib.h>

#include <td/telegram/td_api.hpp>
#include <td/telegram/Client.h>

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <memory>
#include <mutex>
#include <new>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr const char *secretChatsSetting = "enable-secret-chats";

void wipeSensitiveString(std::string &value) noexcept
{
    if (!value.empty()) {
        volatile char *bytes = &value[0];
        for (std::size_t index = 0; index < value.size(); ++index)
            bytes[index] = '\0';
    }
    value.clear();
}

class SensitiveStringWipe final {
public:
    explicit SensitiveStringWipe(std::string &value) noexcept
        : m_value(value)
    {
    }

    ~SensitiveStringWipe()
    {
        wipeSensitiveString(m_value);
    }

    SensitiveStringWipe(const SensitiveStringWipe &) = delete;
    SensitiveStringWipe &operator=(const SensitiveStringWipe &) = delete;

private:
    std::string &m_value;
};

class SessionState;

std::mutex &sessionRegistryMutex()
{
    static std::mutex mutex;
    return mutex;
}

std::vector<std::weak_ptr<SessionState>> &sessionRegistry()
{
    static std::vector<std::weak_ptr<SessionState>> sessions;
    return sessions;
}

void registerSession(const std::shared_ptr<SessionState> &state)
{
    std::lock_guard<std::mutex> lock(sessionRegistryMutex());
    std::vector<std::weak_ptr<SessionState>> &sessions = sessionRegistry();
    std::vector<std::weak_ptr<SessionState>> retained;
    retained.reserve(sessions.size() + 1);
    for (const std::weak_ptr<SessionState> &session : sessions) {
        if (!session.expired())
            retained.push_back(session);
    }
    retained.push_back(state);
    sessions.swap(retained);
}

std::vector<std::shared_ptr<SessionState>> activeSessions()
{
    std::vector<std::shared_ptr<SessionState>> active;
    std::lock_guard<std::mutex> lock(sessionRegistryMutex());
    std::vector<std::weak_ptr<SessionState>> &sessions = sessionRegistry();
    std::vector<std::weak_ptr<SessionState>> retained;
    retained.reserve(sessions.size());
    for (const std::weak_ptr<SessionState> &session : sessions) {
        std::shared_ptr<SessionState> state = session.lock();
        if (state) {
            retained.push_back(state);
            active.push_back(std::move(state));
        }
    }
    sessions.swap(retained);
    return active;
}

std::atomic<std::uint64_t> &nextSessionGeneration()
{
    static std::atomic<std::uint64_t> generation(0);
    return generation;
}

std::uint64_t allocateSessionGeneration() noexcept
{
    std::uint64_t generation =
        nextSessionGeneration().fetch_add(1, std::memory_order_relaxed) + 1;
    if (generation == 0) {
        generation =
            nextSessionGeneration().fetch_add(1, std::memory_order_relaxed) +
            1;
    }
    return generation;
}

bool copyApiHash(
    const TdlibPurpleApplicationCredentials &credentials,
    std::string &apiHash) noexcept
{
    std::size_t length = 0;
    while (length <= TDLIB_PURPLE_API_HASH_LENGTH &&
           credentials.api_hash[length] != '\0') {
        ++length;
    }
    if (length != TDLIB_PURPLE_API_HASH_LENGTH)
        return false;

    try {
        apiHash.assign(credentials.api_hash, length);
        return true;
    } catch (...) {
        return false;
    }
}

bool prepareStorage(
    PurpleConnection *connection,
    const TelegramTdlibSessionDependencies &dependencies,
    std::string &databaseDirectory,
    std::string &sessionKey,
    GError **error) noexcept
{
    PurpleAccount *account = purple_connection_get_account(connection);
    const char *accountId = PURPLE_IS_ACCOUNT(account)
                                ? purple_account_get_id(account)
                                : nullptr;
    if (!accountId || !g_uuid_string_is_valid(accountId)) {
        g_set_error_literal(
            error,
            G_IO_ERROR,
            G_IO_ERROR_INVALID_ARGUMENT,
            _("The Telegram account has an invalid identity."));
        return false;
    }

    gchar *normalizedId = g_ascii_strdown(accountId, -1);
    const char *dataRoot = dependencies.dataRoot.empty()
                               ? purple_data_dir()
                               : dependencies.dataRoot.c_str();
    gchar *path = normalizedId && dataRoot
                      ? g_build_filename(
                            dataRoot,
                            "telegram-tdlib",
                            "accounts",
                            normalizedId,
                            nullptr)
                      : nullptr;
    gchar *canonicalPath = path
                               ? g_canonicalize_filename(path, nullptr)
                               : nullptr;
    g_free(path);
    g_free(normalizedId);

    if (!canonicalPath) {
        g_set_error_literal(
            error,
            G_IO_ERROR,
            G_IO_ERROR_NO_SPACE,
            _("Telegram account storage could not be prepared."));
        return false;
    }

    bool prepared = g_mkdir_with_parents(canonicalPath, 0700) == 0 &&
                    g_file_test(canonicalPath, G_FILE_TEST_IS_DIR) &&
                    g_chmod(canonicalPath, 0700) == 0;
    if (!prepared) {
        g_free(canonicalPath);
        g_set_error_literal(
            error,
            G_IO_ERROR,
            g_io_error_from_errno(errno),
            _("Telegram account storage could not be prepared."));
        return false;
    }

    gchar *digest = g_compute_checksum_for_string(
        G_CHECKSUM_SHA256, canonicalPath, -1);
    if (!digest) {
        g_free(canonicalPath);
        g_set_error_literal(
            error,
            G_IO_ERROR,
            G_IO_ERROR_NO_SPACE,
            _("Telegram account storage could not be prepared."));
        return false;
    }

    try {
        databaseDirectory.assign(canonicalPath);
        sessionKey.assign("purple3:");
        sessionKey.append(digest);
    } catch (...) {
        databaseDirectory.clear();
        sessionKey.clear();
    }
    g_free(digest);
    g_free(canonicalPath);

    if (databaseDirectory.empty() || sessionKey.empty()) {
        g_set_error_literal(
            error,
            G_IO_ERROR,
            G_IO_ERROR_NO_SPACE,
            _("Telegram account storage could not be prepared."));
        return false;
    }
    return true;
}

class SessionState final
    : public TdAuthObserver,
      public std::enable_shared_from_this<SessionState> {
public:
    enum class Phase : std::uint8_t {
        New,
        Connecting,
        Ready,
        Closing,
        Settled,
    };

    SessionState(
        PurpleConnection *connection,
        TdlibPurpleApplicationCredentials credentials,
        GCancellable *connectionCancellable,
        TelegramTdlibSessionCallbacks callbacks,
        TelegramTdlibSessionDependencies dependencies,
        std::string databaseDirectory,
        std::string sessionKey,
        std::string apiHash,
        bool useSecretChats)
        : m_credentials(credentials),
          m_callbacks(callbacks),
          m_dependencies(std::move(dependencies)),
          m_databaseDirectory(std::move(databaseDirectory)),
          m_sessionKey(std::move(sessionKey)),
          m_apiHash(std::move(apiHash)),
          m_useSecretChats(useSecretChats),
          m_generation(allocateSessionGeneration()),
          m_ownerContext(g_main_context_ref_thread_default()),
          m_connectionCancellable(connectionCancellable
                                      ? G_CANCELLABLE(g_object_ref(
                                            connectionCancellable))
                                      : nullptr)
    {
        g_weak_ref_init(&m_connection, connection);
    }

    ~SessionState() override
    {
        if (m_connectionCancellable && m_cancellationHandler != 0) {
            g_cancellable_disconnect(
                m_connectionCancellable, m_cancellationHandler);
        }
        m_cancellationHandler = 0;

        try {
            if (m_controller)
                m_controller->shutdown();
        } catch (...) {
        }
        try {
            if (m_presenter)
                m_presenter->closeAll();
        } catch (...) {
        }
        try {
            if (m_transport)
                m_transport->shutdown();
        } catch (...) {
        }

        m_controller.reset();
        m_presenter.reset();
        m_transport.reset();
        m_backend.reset();
        g_clear_object(&m_connectionCancellable);
        g_weak_ref_clear(&m_connection);
        g_main_context_unref(m_ownerContext);

        wipeSensitiveString(m_apiHash);
        m_credentials = TdlibPurpleApplicationCredentials{};
    }

    bool initialize(GError **error) noexcept
    {
        try {
            m_backend.reset(new TdPollingBackend(
                m_sessionKey,
                m_dependencies.closeTimeoutSeconds,
                m_dependencies.pollTimeoutSeconds,
                std::move(m_dependencies.clientFactory),
                std::move(m_dependencies.closeTimeoutSourceFactory)));

            std::weak_ptr<SessionState> weak(shared_from_this());
            m_transport.reset(new TdTransport(
                m_backend->sender(),
                [weak](TdTransport::ObjectPtr object) {
                    std::shared_ptr<SessionState> self = weak.lock();
                    if (self)
                        self->processUpdate(std::move(object));
                }));

            PurpleUi *ui = m_dependencies.ui;
            if (!ui) {
                PurpleCore *core = purple_core_get_default();
                ui = PURPLE_IS_CORE(core) ? purple_core_get_ui(core) : nullptr;
            }

            TelegramTdlibAuthPresenterActions actions;
            actions.cancelPrompt =
                [weak](std::uint64_t generation, TdAuthPromptId prompt) {
                    std::shared_ptr<SessionState> self = weak.lock();
                    if (self)
                        self->cancelPrompt(generation, prompt);
                };
            actions.failPrompt =
                [weak](
                    std::uint64_t generation,
                    TdAuthPromptId prompt,
                    TdAuthPresentationFailure failure) {
                    std::shared_ptr<SessionState> self = weak.lock();
                    if (self)
                        self->failPrompt(generation, prompt, failure);
                };
            actions.submitPassword =
                [weak](
                    std::uint64_t generation,
                    TdAuthPromptId prompt,
                    std::string password) {
                    SensitiveStringWipe passwordWipe(password);
                    std::shared_ptr<SessionState> self = weak.lock();
                    if (self) {
                        self->submitPassword(
                            generation, prompt, std::move(password));
                    }
                };
            GObject *ownerObject = static_cast<GObject *>(
                g_weak_ref_get(&m_connection));
            PurpleConnection *owner = ownerObject &&
                                              PURPLE_IS_CONNECTION(ownerObject)
                                          ? PURPLE_CONNECTION(ownerObject)
                                          : nullptr;
            m_presenter = telegramTdlibCreateAuthPresenter(
                owner, ui, m_generation, std::move(actions));
            if (ownerObject)
                g_object_unref(ownerObject);
            if (!m_presenter) {
                g_set_error_literal(
                    error,
                    G_IO_ERROR,
                    G_IO_ERROR_NOT_SUPPORTED,
                    _("The Purple interface cannot present Telegram "
                      "authorization."));
                return false;
            }

            TdAuthConfiguration configuration(
                m_credentials.api_id,
                m_apiHash,
                m_databaseDirectory,
                m_useSecretChats,
                TdAuthMode::QrCode);
            m_controller.reset(new TdAuthController(
                std::move(configuration),
                [weak](
                    TdAuthController::FunctionPtr function,
                    TdAuthController::ResponseCallback response) {
                    std::shared_ptr<SessionState> self = weak.lock();
                    if (!self || !self->m_transport) {
                        throw std::runtime_error(
                            "Telegram session transport is unavailable");
                    }
                    return self->m_transport->send(
                        std::move(function), std::move(response));
                },
                *this));
            wipeSensitiveString(m_apiHash);
            m_credentials = TdlibPurpleApplicationCredentials{};
            return true;
        } catch (...) {
            g_set_error_literal(
                error,
                G_IO_ERROR,
                G_IO_ERROR_FAILED,
                _("Telegram connection could not be initialized."));
            return false;
        }
    }

    bool start(GError **error)
    {
        if (m_phase != Phase::New || !m_backend || !m_transport ||
            !m_controller) {
            g_set_error_literal(
                error,
                G_IO_ERROR,
                G_IO_ERROR_FAILED,
                _("Telegram connection could not be started."));
            return false;
        }
        if (m_connectionCancellable &&
            g_cancellable_is_cancelled(m_connectionCancellable)) {
            g_set_error_literal(
                error,
                G_IO_ERROR,
                G_IO_ERROR_CANCELLED,
                _("Telegram connection was cancelled."));
            return false;
        }

        m_phase = Phase::Connecting;
        std::weak_ptr<SessionState> weak(shared_from_this());
        std::unique_ptr<std::weak_ptr<SessionState>> cancellationContext;
        if (m_connectionCancellable) {
            cancellationContext.reset(
                new (std::nothrow) std::weak_ptr<SessionState>(weak));
            if (!cancellationContext) {
                m_phase = Phase::New;
                g_set_error_literal(
                    error,
                    G_IO_ERROR,
                    G_IO_ERROR_NO_SPACE,
                    _("Telegram connection could not be started."));
                return false;
            }
        }
        const TdPollingBackend::StartResult result = m_backend->start(
            m_transport->acknowledgedReceiver(),
            [weak]() {
                std::shared_ptr<SessionState> self = weak.lock();
                if (self)
                    self->backendFailed();
            });
        if (result != TdPollingBackend::StartResult::Started) {
            m_phase = Phase::New;
            g_set_error_literal(
                error,
                G_IO_ERROR,
                result == TdPollingBackend::StartResult::SessionBusy
                    ? G_IO_ERROR_BUSY
                    : G_IO_ERROR_FAILED,
                result == TdPollingBackend::StartResult::SessionBusy
                    ? _("Telegram account storage is still in use.")
                    : _("Telegram connection could not be started."));
            return false;
        }

        if (m_connectionCancellable) {
            m_cancellationHandler = g_cancellable_connect(
                m_connectionCancellable,
                G_CALLBACK(connectionCancelled),
                cancellationContext.release(),
                destroyWeakState);
            if (m_cancellationHandler == 0 &&
                g_cancellable_is_cancelled(m_connectionCancellable)) {
                cancel();
            }
        }
        return true;
    }

    void cancel() noexcept
    {
        if (m_phase == Phase::Settled || m_phase == Phase::Closing)
            return;
        if (m_phase == Phase::New) {
            notifyConnectFailure(
                TELEGRAM_TDLIB_SESSION_FAILURE_CANCELLED);
            beginClose();
            return;
        }

        bool completed = false;
        try {
            if (m_controller)
                completed = m_controller->cancel();
        } catch (...) {
            if (m_phase == Phase::Connecting) {
                notifyConnectFailure(
                    TELEGRAM_TDLIB_SESSION_FAILURE_BACKEND);
            }
        }
        if (!completed && m_phase == Phase::Connecting) {
            notifyConnectFailure(
                TELEGRAM_TDLIB_SESSION_FAILURE_CANCELLED);
        }
        beginClose();
    }

    void prepareForUnload()
    {
        if (m_phase != Phase::Connecting)
            return;
        dispatchCancel();
    }

    void close() noexcept
    {
        m_explicitClose = true;
        if (m_phase == Phase::Settled)
            return;
        if (m_phase == Phase::Connecting) {
            bool completed = false;
            try {
                completed = m_controller && m_controller->cancel();
            } catch (...) {
            }
            if (!completed) {
                notifyConnectFailure(
                    TELEGRAM_TDLIB_SESSION_FAILURE_CANCELLED);
            }
        }
        beginClose();
    }

    bool closeResult(TelegramTdlibSessionCloseResult &result) const noexcept
    {
        if (m_phase != Phase::Settled)
            return false;
        result = m_closeResult;
        return true;
    }

private:
    struct DeferredCancel {
        explicit DeferredCancel(
            std::weak_ptr<SessionState> weakState) noexcept
            : state(std::move(weakState))
        {
        }

        ModuleActivityGuard activity;
        std::weak_ptr<SessionState> state;
    };

    static gboolean deferredCancel(gpointer data) noexcept
    {
        DeferredCancel *context = static_cast<DeferredCancel *>(data);
        try {
            std::shared_ptr<SessionState> self = context->state.lock();
            if (self)
                self->cancel();
        } catch (...) {
        }
        return G_SOURCE_REMOVE;
    }

    static void destroyDeferredCancel(gpointer data) noexcept
    {
        delete static_cast<DeferredCancel *>(data);
    }

    void dispatchCancel() noexcept
    {
        if (g_main_context_is_owner(m_ownerContext)) {
            cancel();
            return;
        }

        DeferredCancel *context = new (std::nothrow) DeferredCancel(
            std::weak_ptr<SessionState>(shared_from_this()));
        if (!context)
            return;
        GSource *source = g_idle_source_new();
        if (!source) {
            delete context;
            return;
        }
        g_source_set_callback(
            source,
            deferredCancel,
            context,
            destroyDeferredCancel);
        if (g_source_attach(source, m_ownerContext) == 0)
            g_source_destroy(source);
        g_source_unref(source);
    }

    static void destroyWeakState(gpointer data) noexcept
    {
        delete static_cast<std::weak_ptr<SessionState> *>(data);
    }

    static void connectionCancelled(
        G_GNUC_UNUSED GCancellable *cancellable,
        gpointer data) noexcept
    {
        try {
            std::weak_ptr<SessionState> *weak =
                static_cast<std::weak_ptr<SessionState> *>(data);
            std::shared_ptr<SessionState> self = weak ? weak->lock() : nullptr;
            if (self)
                self->dispatchCancel();
        } catch (...) {
        }
    }

    template <typename Callback>
    void notifyConnection(Callback callback) noexcept
    {
        GObject *object = static_cast<GObject *>(
            g_weak_ref_get(&m_connection));
        if (!object)
            return;
        if (PURPLE_IS_CONNECTION(object)) {
            try {
                callback(PURPLE_CONNECTION(object));
            } catch (...) {
            }
        }
        g_object_unref(object);
    }

    void notifyReady() noexcept
    {
        if (m_connectOutcomeSelected)
            return;
        m_connectOutcomeSelected = true;
        notifyConnection([this](PurpleConnection *connection) {
            if (m_callbacks.ready)
                m_callbacks.ready(connection);
        });
    }

    void notifyConnectFailure(
        TelegramTdlibSessionFailure failure) noexcept
    {
        if (m_connectOutcomeSelected)
            return;
        m_connectOutcomeSelected = true;
        notifyConnection([this, failure](PurpleConnection *connection) {
            if (m_callbacks.connect_failed)
                m_callbacks.connect_failed(connection, failure);
        });
    }

    void notifyRuntimeFailure() noexcept
    {
        if (m_runtimeFailureNotified)
            return;
        m_runtimeFailureNotified = true;
        notifyConnection([this](PurpleConnection *connection) {
            if (m_callbacks.runtime_failed)
                m_callbacks.runtime_failed(connection);
        });
    }

    void notifyClosed(TelegramTdlibSessionCloseResult result) noexcept
    {
        notifyConnection([this, result](PurpleConnection *connection) {
            if (m_callbacks.closed)
                m_callbacks.closed(connection, result);
        });
    }

    void processUpdate(TdTransport::ObjectPtr object) noexcept
    {
        if (!object || !m_controller || m_phase == Phase::Settled)
            return;
        try {
            if (object->get_id() !=
                td::td_api::updateAuthorizationState::ID) {
                return;
            }
            td::td_api::object_ptr<td::td_api::updateAuthorizationState>
                update = td::move_tl_object_as<
                    td::td_api::updateAuthorizationState>(object);
            m_controller->onAuthorizationState(
                update ? update->authorization_state_.get() : nullptr);
        } catch (...) {
            authorizationFailed();
        }
    }

    void cancelPrompt(
        std::uint64_t generation,
        TdAuthPromptId prompt)
    {
        if (generation != m_generation || !m_controller)
            return;
        (void)m_controller->cancelPrompt(prompt);
    }

    void failPrompt(
        std::uint64_t generation,
        TdAuthPromptId prompt,
        TdAuthPresentationFailure failure)
    {
        if (generation != m_generation || !m_controller)
            return;
        (void)m_controller->failPrompt(prompt, failure);
    }

    void submitPassword(
        std::uint64_t generation,
        TdAuthPromptId prompt,
        std::string password)
    {
        SensitiveStringWipe passwordWipe(password);
        if (generation != m_generation || !m_controller)
            return;
        (void)m_controller->submitPassword(prompt, password);
    }

    void failUnsupported(TdAuthPromptId prompt)
    {
        if (m_controller) {
            (void)m_controller->failPrompt(
                prompt, TdAuthPresentationFailure::Unsupported);
        }
    }

    void authorizationFailed()
    {
        if (m_phase == Phase::Connecting) {
            notifyConnectFailure(
                TELEGRAM_TDLIB_SESSION_FAILURE_AUTHORIZATION);
            beginClose();
        } else if (m_phase == Phase::Ready) {
            notifyRuntimeFailure();
            beginClose();
        }
    }

    void backendFailed()
    {
        if (m_phase == Phase::Connecting) {
            notifyConnectFailure(
                TELEGRAM_TDLIB_SESSION_FAILURE_BACKEND);
        } else if (m_phase == Phase::Ready) {
            notifyRuntimeFailure();
        }
        beginClose();
    }

    void beginClose() noexcept
    {
        if (m_phase == Phase::Settled)
            return;
        if (m_phase != Phase::Closing) {
            m_phase = Phase::Closing;
            try {
                if (m_controller)
                    m_controller->shutdown();
            } catch (...) {
            }
            try {
                if (m_presenter)
                    m_presenter->closeAll();
            } catch (...) {
            }
            try {
                if (m_transport)
                    m_transport->shutdown();
            } catch (...) {
            }
        }
        if (m_closeStarted)
            return;
        m_closeStarted = true;

        if (!m_backend) {
            backendClosed(TdPollingBackend::CloseResult::Failed);
            return;
        }

        try {
            std::weak_ptr<SessionState> weak(shared_from_this());
            m_backend->close([weak](TdPollingBackend::CloseResult result) {
                std::shared_ptr<SessionState> self = weak.lock();
                if (self)
                    self->backendClosed(result);
            });
        } catch (...) {
            backendClosed(TdPollingBackend::CloseResult::Failed);
        }
    }

    void backendClosed(TdPollingBackend::CloseResult result) noexcept
    {
        if (m_phase == Phase::Settled)
            return;
        m_phase = Phase::Settled;
        switch (result) {
        case TdPollingBackend::CloseResult::Closed:
            m_closeResult = TELEGRAM_TDLIB_SESSION_CLOSE_CLOSED;
            break;
        case TdPollingBackend::CloseResult::TimedOut:
            m_closeResult = TELEGRAM_TDLIB_SESSION_CLOSE_TIMED_OUT;
            break;
        case TdPollingBackend::CloseResult::Failed:
            m_closeResult = TELEGRAM_TDLIB_SESSION_CLOSE_FAILED;
            break;
        }
        if (m_explicitClose && !m_closeNotified) {
            m_closeNotified = true;
            notifyClosed(m_closeResult);
        }
    }

    void onPhoneNumberRequired(TdAuthPromptId prompt) override
    {
        failUnsupported(prompt);
    }

    void onPremiumPurchaseRequired(TdAuthPromptId prompt) override
    {
        failUnsupported(prompt);
    }

    void onEmailAddressRequired(
        TdAuthPromptId prompt,
        const TdAuthEmailAddressChallenge &) override
    {
        failUnsupported(prompt);
    }

    void onEmailCodeRequired(
        TdAuthPromptId prompt,
        const TdAuthEmailCodeChallenge &) override
    {
        failUnsupported(prompt);
    }

    void onCodeRequired(
        TdAuthPromptId prompt,
        const TdAuthCodeChallenge &) override
    {
        failUnsupported(prompt);
    }

    void onQrLinkChanged(
        TdAuthPromptId prompt,
        const std::string &link) override
    {
        if (m_presenter)
            m_presenter->showQr(prompt, link);
    }

    void onRegistrationRequired(
        TdAuthPromptId prompt,
        const TdAuthRegistrationChallenge &) override
    {
        failUnsupported(prompt);
    }

    void onPasswordRequired(
        TdAuthPromptId prompt,
        const TdAuthPasswordChallenge &challenge) override
    {
        if (m_presenter)
            m_presenter->showPassword(prompt, challenge);
    }

    void onPromptClosed(
        TdAuthPromptId prompt,
        TdAuthPromptType,
        TdAuthPromptCloseReason) override
    {
        if (m_presenter)
            m_presenter->closePrompt(prompt);
    }

    void onRequestFailed(const TdAuthRequestFailure &) override
    {
        /* The controller follows a recoverable rejection with a fresh prompt. */
    }

    void onAuthorizationReady() override
    {
        if (m_phase != Phase::Connecting)
            return;
        if (m_connectionCancellable &&
            g_cancellable_is_cancelled(m_connectionCancellable)) {
            notifyConnectFailure(
                TELEGRAM_TDLIB_SESSION_FAILURE_CANCELLED);
            beginClose();
            return;
        }
        if (m_presenter)
            m_presenter->closeAll();
        if (m_connectionCancellable && m_cancellationHandler != 0) {
            g_cancellable_disconnect(
                m_connectionCancellable, m_cancellationHandler);
            m_cancellationHandler = 0;
        }
        if (m_connectionCancellable &&
            g_cancellable_is_cancelled(m_connectionCancellable)) {
            notifyConnectFailure(
                TELEGRAM_TDLIB_SESSION_FAILURE_CANCELLED);
            beginClose();
            return;
        }
        m_phase = Phase::Ready;
        notifyReady();
    }

    void onAuthorizationCancelled() override
    {
        if (m_phase == Phase::Connecting) {
            notifyConnectFailure(
                TELEGRAM_TDLIB_SESSION_FAILURE_CANCELLED);
        }
        beginClose();
    }

    void onAuthorizationFailed(const TdAuthFailure &) override
    {
        authorizationFailed();
    }

    void onLoggingOut() override
    {
        authorizationFailed();
    }

    void onClosing() override
    {
        authorizationFailed();
    }

    void onClosed() override
    {
        authorizationFailed();
    }

    ModuleActivityGuard m_activity;
    TdlibPurpleApplicationCredentials m_credentials;
    TelegramTdlibSessionCallbacks m_callbacks;
    TelegramTdlibSessionDependencies m_dependencies;
    std::string m_databaseDirectory;
    std::string m_sessionKey;
    std::string m_apiHash;
    bool m_useSecretChats;
    std::uint64_t m_generation;
    GMainContext *m_ownerContext;
    GWeakRef m_connection;
    GCancellable *m_connectionCancellable;
    gulong m_cancellationHandler = 0;
    std::unique_ptr<TdPollingBackend> m_backend;
    std::unique_ptr<TdTransport> m_transport;
    std::unique_ptr<TdAuthController> m_controller;
    std::unique_ptr<TelegramTdlibAuthPresenter> m_presenter;
    Phase m_phase = Phase::New;
    bool m_connectOutcomeSelected = false;
    bool m_runtimeFailureNotified = false;
    bool m_explicitClose = false;
    bool m_closeStarted = false;
    bool m_closeNotified = false;
    TelegramTdlibSessionCloseResult m_closeResult =
        TELEGRAM_TDLIB_SESSION_CLOSE_FAILED;
};

} // namespace

struct TelegramTdlibSession {
    std::shared_ptr<SessionState> state;
};

TelegramTdlibSession *telegramTdlibSessionCreate(
    PurpleConnection *connection,
    const TdlibPurpleApplicationCredentials &credentials,
    GCancellable *connectionCancellable,
    const TelegramTdlibSessionCallbacks &callbacks,
    TelegramTdlibSessionDependencies dependencies,
    GError **error) noexcept
{
    g_return_val_if_fail(PURPLE_IS_CONNECTION(connection), nullptr);
    g_return_val_if_fail(
        connectionCancellable == nullptr ||
            G_IS_CANCELLABLE(connectionCancellable),
        nullptr);
    g_return_val_if_fail(error == nullptr || *error == nullptr, nullptr);

    std::string apiHash;
    std::string databaseDirectory;
    std::string sessionKey;
    if (credentials.api_id <= 0 || !copyApiHash(credentials, apiHash)) {
        g_set_error_literal(
            error,
            G_IO_ERROR,
            G_IO_ERROR_INVALID_ARGUMENT,
            _("Telegram application credentials are invalid."));
        return nullptr;
    }
    if (!prepareStorage(
            connection,
            dependencies,
            databaseDirectory,
            sessionKey,
            error)) {
        wipeSensitiveString(apiHash);
        return nullptr;
    }

    PurpleAccount *account = purple_connection_get_account(connection);
    PurpleAccountSettings *settings = PURPLE_IS_ACCOUNT(account)
                                          ? purple_account_get_settings(account)
                                          : nullptr;
    const bool useSecretChats = settings
                                    ? purple_account_settings_get_boolean(
                                          settings, secretChatsSetting, TRUE)
                                    : true;

    try {
        std::shared_ptr<SessionState> state = std::make_shared<SessionState>(
            connection,
            credentials,
            connectionCancellable,
            callbacks,
            std::move(dependencies),
            std::move(databaseDirectory),
            std::move(sessionKey),
            std::move(apiHash),
            useSecretChats);
        if (!state->initialize(error))
            return nullptr;

        std::unique_ptr<TelegramTdlibSession> session(
            new TelegramTdlibSession());
        session->state = std::move(state);
        registerSession(session->state);
        return session.release();
    } catch (...) {
        wipeSensitiveString(apiHash);
        g_set_error_literal(
            error,
            G_IO_ERROR,
            G_IO_ERROR_FAILED,
            _("Telegram connection could not be initialized."));
        return nullptr;
    }
}

extern "C" TelegramTdlibSession *telegram_tdlib_session_new(
    PurpleConnection *connection,
    const TdlibPurpleApplicationCredentials *credentials,
    GCancellable *connectionCancellable,
    const TelegramTdlibSessionCallbacks *callbacks,
    GError **error)
{
    if (!credentials || !callbacks) {
        g_set_error_literal(
            error,
            G_IO_ERROR,
            G_IO_ERROR_INVALID_ARGUMENT,
            _("Telegram connection configuration is incomplete."));
        return nullptr;
    }
    TelegramTdlibSessionDependencies dependencies;
    return telegramTdlibSessionCreate(
        connection,
        *credentials,
        connectionCancellable,
        *callbacks,
        std::move(dependencies),
        error);
}

extern "C" gboolean telegram_tdlib_session_start(
    TelegramTdlibSession *session,
    GError **error)
{
    g_return_val_if_fail(session != nullptr, FALSE);
    g_return_val_if_fail(error == nullptr || *error == nullptr, FALSE);
    try {
        return session->state && session->state->start(error);
    } catch (...) {
        g_set_error_literal(
            error,
            G_IO_ERROR,
            G_IO_ERROR_FAILED,
            _("Telegram connection could not be started."));
        return FALSE;
    }
}

extern "C" void telegram_tdlib_session_cancel(
    TelegramTdlibSession *session)
{
    try {
        if (session && session->state)
            session->state->cancel();
    } catch (...) {
    }
}

extern "C" void telegram_tdlib_session_close(
    TelegramTdlibSession *session)
{
    try {
        if (session && session->state)
            session->state->close();
    } catch (...) {
    }
}

extern "C" gboolean telegram_tdlib_session_get_close_result(
    TelegramTdlibSession *session,
    TelegramTdlibSessionCloseResult *result)
{
    g_return_val_if_fail(result != nullptr, FALSE);
    try {
        return session && session->state &&
               session->state->closeResult(*result);
    } catch (...) {
        return FALSE;
    }
}

extern "C" void telegram_tdlib_session_free(
    TelegramTdlibSession *session)
{
    try {
        delete session;
    } catch (...) {
    }
}

extern "C" gboolean telegram_tdlib_session_initialize_runtime(
    GError **error)
{
    g_return_val_if_fail(error == nullptr || *error == nullptr, FALSE);
    try {
        td::td_api::object_ptr<td::td_api::Object> verbosity =
            td::ClientManager::execute(
                td::td_api::make_object<
                    td::td_api::setLogVerbosityLevel>(0));
        td::td_api::object_ptr<td::td_api::Object> stream =
            td::ClientManager::execute(
                td::td_api::make_object<td::td_api::setLogStream>(
                    td::td_api::make_object<td::td_api::logStreamEmpty>()));
        if (verbosity && stream &&
            verbosity->get_id() == td::td_api::ok::ID &&
            stream->get_id() == td::td_api::ok::ID) {
            return TRUE;
        }
    } catch (...) {
    }

    g_set_error_literal(
        error,
        G_IO_ERROR,
        G_IO_ERROR_FAILED,
        _("Telegram diagnostics could not be initialized safely."));
    return FALSE;
}

extern "C" void telegram_tdlib_session_prepare_unload(void)
{
    try {
        const std::vector<std::shared_ptr<SessionState>> sessions =
            activeSessions();
        for (const std::shared_ptr<SessionState> &session : sessions)
            session->prepareForUnload();
    } catch (...) {
    }
}

extern "C" gboolean telegram_tdlib_session_module_busy(void)
{
    try {
        return moduleActivityPending() ||
               TdPollingBackend::hasActiveWorkers();
    } catch (...) {
        return TRUE;
    }
}
