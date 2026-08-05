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

#include "telegram-purple3-auth-presenter.h"

#include "../module-activity.h"

#include <glib/gi18n-lib.h>

#include <cstddef>
#include <new>
#include <utility>

namespace {

// Pidgin's current renderer does not handle QR encoder failure. Telegram
// documents this value as a short tg:// URL, so this leaves substantial room
// while keeping oversized input away from the renderer.
constexpr std::size_t maximumQrLinkBytes = 2048;
constexpr char telegramUriPrefix[] = "tg://";

void wipeString(std::string &value) noexcept
{
    if (!value.empty()) {
        volatile char *bytes = &value[0];
        for (std::size_t index = 0; index < value.size(); ++index)
            bytes[index] = '\0';
    }
    value.clear();
}

class ScopedStringWipe final {
public:
    explicit ScopedStringWipe(std::string &value) noexcept
        : m_value(value)
    {
    }

    ~ScopedStringWipe()
    {
        wipeString(m_value);
    }

    ScopedStringWipe(const ScopedStringWipe &) = delete;
    ScopedStringWipe &operator=(const ScopedStringWipe &) = delete;

private:
    std::string &m_value;
};

bool validQrLink(const std::string &link) noexcept
{
    constexpr std::size_t prefixBytes =
        sizeof(telegramUriPrefix) - 1;
    if (link.size() <= prefixBytes || link.size() > maximumQrLinkBytes)
        return false;
    if (link.compare(0, prefixBytes, telegramUriPrefix) != 0)
        return false;

    for (std::size_t index = prefixBytes; index < link.size(); ++index) {
        const unsigned char byte =
            static_cast<unsigned char>(link[index]);
        if (byte <= 0x1f || byte == 0x7f)
            return false;
    }
    return true;
}

enum class PromptKind : unsigned char {
    None,
    Qr,
    Password,
};

enum class PasswordOutcome : unsigned char {
    Submit,
    Cancel,
    Unsupported,
    Failed,
};

} // namespace

class TelegramTdlibAuthPresenter::State final
    : public std::enable_shared_from_this<
          TelegramTdlibAuthPresenter::State> {
public:
    State(
        PurpleConnection *owner,
        PurpleUi *ui,
        std::uint64_t sessionGeneration,
        TelegramTdlibAuthPresenterActions actions)
        : m_sessionGeneration(sessionGeneration),
          m_actions(std::move(actions)),
          m_ownerContext(g_main_context_ref_thread_default())
    {
        g_weak_ref_init(&m_owner, owner);
        g_weak_ref_init(&m_ui, ui);
    }

    ~State()
    {
        shutdown();
        g_weak_ref_clear(&m_ui);
        g_weak_ref_clear(&m_owner);
        g_main_context_unref(m_ownerContext);
    }

    void showQr(
        TdAuthPromptId prompt,
        const std::string &link) noexcept
    {
        try {
            showQrImpl(prompt, link);
        } catch (...) {
            reportFailure(prompt, TdAuthPresentationFailure::Failed);
        }
    }

    void showPassword(
        TdAuthPromptId prompt,
        const TdAuthPasswordChallenge &challenge) noexcept
    {
        try {
            showPasswordImpl(prompt, challenge);
        } catch (...) {
            reportFailure(prompt, TdAuthPresentationFailure::Failed);
        }
    }

    void closePrompt(TdAuthPromptId prompt) noexcept
    {
        if (!m_alive || !prompt.valid() || m_activePrompt != prompt)
            return;
        dismiss(detachActive());
    }

    void closeAll() noexcept
    {
        if (!m_alive)
            return;
        dismiss(detachActive());
    }

    void shutdown() noexcept
    {
        if (!m_alive)
            return;
        m_alive = false;
        dismiss(detachActive());
    }

private:
    struct DetachedPrompt {
        PurpleQrCode *qr = nullptr;
        GCancellable *qrCancellable = nullptr;
        PurpleRequestPage *passwordPage = nullptr;
    };

    struct QrCancellationContext {
        QrCancellationContext(
            std::weak_ptr<State> weakState,
            std::uint64_t generation,
            TdAuthPromptId promptId) noexcept
            : state(std::move(weakState)),
              sessionGeneration(generation),
              prompt(promptId)
        {
        }

        ModuleActivityGuard activity;
        std::weak_ptr<State> state;
        std::uint64_t sessionGeneration;
        TdAuthPromptId prompt;
    };

    struct PasswordCallbackContext {
        PasswordCallbackContext(
            std::weak_ptr<State> weakState,
            std::uint64_t generation,
            TdAuthPromptId promptId) noexcept
            : state(std::move(weakState)),
              sessionGeneration(generation),
              prompt(promptId)
        {
        }

        ModuleActivityGuard activity;
        std::weak_ptr<State> state;
        std::uint64_t sessionGeneration;
        TdAuthPromptId prompt;
    };

    struct DeferredQrCancellation {
        DeferredQrCancellation(
            std::weak_ptr<State> weakState,
            std::uint64_t generation,
            TdAuthPromptId promptId) noexcept
            : state(std::move(weakState)),
              sessionGeneration(generation),
              prompt(promptId)
        {
        }

        ModuleActivityGuard activity;
        std::weak_ptr<State> state;
        std::uint64_t sessionGeneration;
        TdAuthPromptId prompt;
    };

    struct DeferredPasswordResult {
        DeferredPasswordResult(
            std::weak_ptr<State> weakState,
            std::uint64_t generation,
            TdAuthPromptId promptId,
            PasswordOutcome result,
            std::string passwordValue)
            : state(std::move(weakState)),
              sessionGeneration(generation),
              prompt(promptId),
              outcome(result),
              password(std::move(passwordValue))
        {
        }

        ~DeferredPasswordResult()
        {
            wipeString(password);
        }

        ModuleActivityGuard activity;
        std::weak_ptr<State> state;
        std::uint64_t sessionGeneration;
        TdAuthPromptId prompt;
        PasswordOutcome outcome;
        std::string password;
    };

    static void destroyQrCancellationContext(gpointer data) noexcept
    {
        try {
            delete static_cast<QrCancellationContext *>(data);
        } catch (...) {
        }
    }

    static void qrCancelled(
        G_GNUC_UNUSED GCancellable *cancellable,
        gpointer data) noexcept
    {
        QrCancellationContext *context =
            static_cast<QrCancellationContext *>(data);
        try {
            std::shared_ptr<State> state = context->state.lock();
            if (state) {
                state->dispatchQrCancellation(
                    context->sessionGeneration,
                    context->prompt);
            }
        } catch (...) {
        }
    }

    static gboolean deferredQrCancelled(gpointer data) noexcept
    {
        DeferredQrCancellation *context =
            static_cast<DeferredQrCancellation *>(data);
        try {
            std::shared_ptr<State> state = context->state.lock();
            if (state) {
                state->handleQrCancellation(
                    context->sessionGeneration,
                    context->prompt);
            }
        } catch (...) {
        }
        return G_SOURCE_REMOVE;
    }

    static void destroyDeferredQrCancellation(gpointer data) noexcept
    {
        try {
            delete static_cast<DeferredQrCancellation *>(data);
        } catch (...) {
        }
    }

    static gboolean deferredPasswordFinished(gpointer data) noexcept
    {
        DeferredPasswordResult *context =
            static_cast<DeferredPasswordResult *>(data);
        try {
            std::shared_ptr<State> state = context->state.lock();
            if (state) {
                state->handlePasswordResult(
                    context->sessionGeneration,
                    context->prompt,
                    context->outcome,
                    std::move(context->password));
            }
        } catch (...) {
        }
        return G_SOURCE_REMOVE;
    }

    static void destroyDeferredPasswordResult(gpointer data) noexcept
    {
        try {
            delete static_cast<DeferredPasswordResult *>(data);
        } catch (...) {
        }
    }

    static void passwordFinished(
        GObject *source,
        GAsyncResult *result,
        gpointer data) noexcept
    {
        std::unique_ptr<PasswordCallbackContext> context(
            static_cast<PasswordCallbackContext *>(data));
        PurpleRequestPage *page = nullptr;
        GError *error = nullptr;
        PasswordOutcome outcome = PasswordOutcome::Failed;
        std::string password;
        ScopedStringWipe passwordWipe(password);

        try {
            if (PURPLE_IS_UI(source)) {
                page = purple_ui_request_page_finish(
                    PURPLE_UI(source), result, &error);
            }

            if (error) {
                outcome = g_error_matches(
                              error,
                              PURPLE_UI_ERROR,
                              PURPLE_UI_ERROR_VFUNC_NOT_IMPLEMENTED)
                              ? PasswordOutcome::Unsupported
                              : PasswordOutcome::Cancel;
            } else if (PURPLE_IS_REQUEST_PAGE(page) &&
                       purple_request_page_exists(page, "password")) {
                PurpleRequestField *field =
                    purple_request_page_get_field(page, "password");
                if (PURPLE_IS_REQUEST_FIELD_STRING(field)) {
                    const char *value =
                        purple_request_page_get_string(page, "password");
                    password = value ? value : "";
                    outcome = PasswordOutcome::Submit;
                }
            }
        } catch (...) {
            outcome = PasswordOutcome::Failed;
            password.clear();
        }

        if (page)
            g_object_unref(page);
        g_clear_error(&error);

        try {
            std::shared_ptr<State> state = context->state.lock();
            if (state) {
                state->dispatchPasswordResult(
                    context->sessionGeneration,
                    context->prompt,
                    outcome,
                    std::move(password));
            }
        } catch (...) {
        }
    }

    static bool attachSource(
        GMainContext *mainContext,
        GSourceFunc callback,
        gpointer data,
        GDestroyNotify destroy) noexcept
    {
        GSource *source = g_idle_source_new();
        if (!source) {
            if (destroy)
                destroy(data);
            return false;
        }
        g_source_set_callback(source, callback, data, destroy);
        const guint sourceId = g_source_attach(source, mainContext);
        if (sourceId == 0)
            g_source_destroy(source);
        g_source_unref(source);
        return sourceId != 0;
    }

    bool ownerAlive() noexcept
    {
        GObject *owner = static_cast<GObject *>(g_weak_ref_get(&m_owner));
        if (!owner)
            return false;
        const bool alive = PURPLE_IS_CONNECTION(owner);
        g_object_unref(owner);
        return alive;
    }

    PurpleUi *refUi() noexcept
    {
        GObject *ui = static_cast<GObject *>(g_weak_ref_get(&m_ui));
        if (!ui)
            return nullptr;
        if (!PURPLE_IS_UI(ui)) {
            g_object_unref(ui);
            return nullptr;
        }
        return PURPLE_UI(ui);
    }

    bool matches(
        std::uint64_t sessionGeneration,
        TdAuthPromptId prompt,
        PromptKind kind) const noexcept
    {
        return m_alive && sessionGeneration == m_sessionGeneration &&
               prompt.valid() && prompt == m_activePrompt &&
               kind == m_activeKind;
    }

    DetachedPrompt detachActive() noexcept
    {
        DetachedPrompt detached;
        detached.qr = m_qr;
        detached.qrCancellable = m_qrCancellable;
        detached.passwordPage = m_passwordPage;

        m_qr = nullptr;
        m_qrCancellable = nullptr;
        m_passwordPage = nullptr;
        m_activePrompt = TdAuthPromptId();
        m_activeKind = PromptKind::None;
        return detached;
    }

    static void release(DetachedPrompt detached) noexcept
    {
        if (detached.passwordPage)
            g_object_unref(detached.passwordPage);
        if (detached.qr)
            g_object_unref(detached.qr);
        if (detached.qrCancellable)
            g_object_unref(detached.qrCancellable);
    }

    static void dismiss(DetachedPrompt detached) noexcept
    {
        try {
            if (detached.qrCancellable &&
                !g_cancellable_is_cancelled(detached.qrCancellable)) {
                g_cancellable_cancel(detached.qrCancellable);
            }
            if (detached.passwordPage)
                purple_request_page_close(detached.passwordPage);
        } catch (...) {
        }
        release(detached);
    }

    void invokeCancel(TdAuthPromptId prompt) noexcept
    {
        if (!ownerAlive())
            return;
        try {
            m_actions.cancelPrompt(m_sessionGeneration, prompt);
        } catch (...) {
        }
    }

    void invokeFailure(
        TdAuthPromptId prompt,
        TdAuthPresentationFailure failure) noexcept
    {
        if (!ownerAlive())
            return;
        try {
            m_actions.failPrompt(
                m_sessionGeneration, prompt, failure);
        } catch (...) {
        }
    }

    void invokePassword(
        TdAuthPromptId prompt,
        std::string password) noexcept
    {
        ScopedStringWipe passwordWipe(password);
        if (!ownerAlive())
            return;
        try {
            m_actions.submitPassword(
                m_sessionGeneration, prompt, std::move(password));
        } catch (...) {
        }
    }

    void reportFailure(
        TdAuthPromptId prompt,
        TdAuthPresentationFailure failure) noexcept
    {
        if (!m_alive || !prompt.valid())
            return;
        if (m_activePrompt.valid())
            dismiss(detachActive());
        invokeFailure(prompt, failure);
    }

    void dispatchQrCancellation(
        std::uint64_t sessionGeneration,
        TdAuthPromptId prompt) noexcept
    {
        if (g_main_context_is_owner(m_ownerContext)) {
            handleQrCancellation(sessionGeneration, prompt);
            return;
        }

        DeferredQrCancellation *context =
            new (std::nothrow) DeferredQrCancellation(
                weak_fromThis(), sessionGeneration, prompt);
        if (!context)
            return;
        attachSource(
            m_ownerContext,
            deferredQrCancelled,
            context,
            destroyDeferredQrCancellation);
    }

    void handleQrCancellation(
        std::uint64_t sessionGeneration,
        TdAuthPromptId prompt) noexcept
    {
        if (!matches(sessionGeneration, prompt, PromptKind::Qr))
            return;

        DetachedPrompt detached = detachActive();
        invokeCancel(prompt);
        release(detached);
    }

    void dispatchPasswordResult(
        std::uint64_t sessionGeneration,
        TdAuthPromptId prompt,
        PasswordOutcome outcome,
        std::string password) noexcept
    {
        ScopedStringWipe passwordWipe(password);
        if (g_main_context_is_owner(m_ownerContext)) {
            handlePasswordResult(
                sessionGeneration,
                prompt,
                outcome,
                std::move(password));
            return;
        }

        DeferredPasswordResult *context = nullptr;
        try {
            context = new (std::nothrow) DeferredPasswordResult(
                weak_fromThis(),
                sessionGeneration,
                prompt,
                outcome,
                std::move(password));
        } catch (...) {
            return;
        }
        if (!context)
            return;
        attachSource(
            m_ownerContext,
            deferredPasswordFinished,
            context,
            destroyDeferredPasswordResult);
    }

    void handlePasswordResult(
        std::uint64_t sessionGeneration,
        TdAuthPromptId prompt,
        PasswordOutcome outcome,
        std::string password) noexcept
    {
        ScopedStringWipe passwordWipe(password);
        if (!matches(
                sessionGeneration, prompt, PromptKind::Password)) {
            return;
        }

        // The UI has already completed or dismissed this page. Drop our
        // retained reference without emitting another close request.
        release(detachActive());

        switch (outcome) {
        case PasswordOutcome::Submit:
            invokePassword(prompt, std::move(password));
            break;
        case PasswordOutcome::Cancel:
            invokeCancel(prompt);
            break;
        case PasswordOutcome::Unsupported:
            invokeFailure(prompt, TdAuthPresentationFailure::Unsupported);
            break;
        case PasswordOutcome::Failed:
            invokeFailure(prompt, TdAuthPresentationFailure::Failed);
            break;
        }
    }

    std::weak_ptr<State> weak_fromThis()
    {
        return std::weak_ptr<State>(shared_from_this());
    }

    void showQrImpl(
        TdAuthPromptId prompt,
        const std::string &link)
    {
        if (!m_alive || !prompt.valid())
            return;
        if (!validQrLink(link)) {
            reportFailure(prompt, TdAuthPresentationFailure::Failed);
            return;
        }
        if (!ownerAlive())
            return;

        if (matches(m_sessionGeneration, prompt, PromptKind::Qr) && m_qr) {
            PurpleQrCode *qr = PURPLE_QR_CODE(g_object_ref(m_qr));
            purple_qr_code_set_text(qr, link.c_str());
            g_object_unref(qr);
            return;
        }

        dismiss(detachActive());

        PurpleUi *ui = refUi();
        if (!ui) {
            reportFailure(prompt, TdAuthPresentationFailure::Unsupported);
            return;
        }

        PurpleQrCode *qr = purple_qr_code_new(
            _("Log in to Telegram"),
            _("In Telegram on your phone, open Settings > Devices > Link "
              "Desktop Device, then scan this code."),
            link.c_str(),
            _("Telegram login QR code"));
        GCancellable *cancellable = g_cancellable_new();
        QrCancellationContext *context =
            new (std::nothrow) QrCancellationContext(
                weak_fromThis(), m_sessionGeneration, prompt);
        if (!qr || !cancellable || !context) {
            if (context)
                delete context;
            if (cancellable)
                g_object_unref(cancellable);
            if (qr)
                g_object_unref(qr);
            g_object_unref(ui);
            reportFailure(prompt, TdAuthPresentationFailure::Failed);
            return;
        }

        m_activePrompt = prompt;
        m_activeKind = PromptKind::Qr;
        m_qr = qr;
        m_qrCancellable = cancellable;

        (void)g_cancellable_connect(
            cancellable,
            G_CALLBACK(qrCancelled),
            context,
            destroyQrCancellationContext);

        GError *error = nullptr;
        PurpleQrCode *presentedQr =
            PURPLE_QR_CODE(g_object_ref(qr));
        GCancellable *presentedCancellable =
            G_CANCELLABLE(g_object_ref(cancellable));
        const gboolean presented = purple_ui_present_qr_code(
            ui, presentedQr, presentedCancellable, &error);
        g_object_unref(presentedCancellable);
        g_object_unref(presentedQr);
        g_object_unref(ui);

        TdAuthPresentationFailure failure =
            TdAuthPresentationFailure::Failed;
        if (error && g_error_matches(
                         error,
                         PURPLE_UI_ERROR,
                         PURPLE_UI_ERROR_VFUNC_NOT_IMPLEMENTED)) {
            failure = TdAuthPresentationFailure::Unsupported;
        }
        const bool failed = !presented || error;
        g_clear_error(&error);

        if (failed && matches(
                          m_sessionGeneration,
                          prompt,
                          PromptKind::Qr)) {
            reportFailure(prompt, failure);
        }
    }

    void showPasswordImpl(
        TdAuthPromptId prompt,
        const TdAuthPasswordChallenge &challenge)
    {
        if (!m_alive || !prompt.valid() || !ownerAlive())
            return;
        if (matches(
                m_sessionGeneration,
                prompt,
                PromptKind::Password)) {
            return;
        }

        dismiss(detachActive());

        PurpleUi *ui = refUi();
        if (!ui) {
            reportFailure(prompt, TdAuthPresentationFailure::Unsupported);
            return;
        }

        PurpleRequestPage *page = purple_request_page_new();
        PurpleRequestGroup *group = purple_request_group_new(nullptr);
        PurpleRequestField *field = purple_request_field_string_new(
            "password", _("Password"), nullptr, FALSE);
        PasswordCallbackContext *context =
            new (std::nothrow) PasswordCallbackContext(
                weak_fromThis(), m_sessionGeneration, prompt);
        if (!page || !group || !field || !context) {
            if (context)
                delete context;
            if (field)
                g_object_unref(field);
            if (group)
                g_object_unref(group);
            if (page)
                g_object_unref(page);
            g_object_unref(ui);
            reportFailure(prompt, TdAuthPresentationFailure::Failed);
            return;
        }

        purple_request_page_set_title(page, _("Password"));
        GString *subtitle = g_string_new(
            _("Enter password for two-factor authentication"));
        if (!challenge.hint.empty()) {
            g_string_append_c(subtitle, '\n');
            g_string_append_printf(
                subtitle, _("Hint: %s"), challenge.hint.c_str());
        }
        if (!challenge.recoveryEmailAddressPattern.empty()) {
            g_string_append_c(subtitle, '\n');
            g_string_append_printf(
                subtitle,
                _("Recovery e-mail may have been sent to %s"),
                challenge.recoveryEmailAddressPattern.c_str());
        }
        purple_request_page_set_subtitle(page, subtitle->str);
        g_string_free(subtitle, TRUE);

        purple_request_field_string_set_masked(
            PURPLE_REQUEST_FIELD_STRING(field), TRUE);
        purple_request_field_set_required(field, TRUE);
        purple_request_group_add_field(group, field);
        purple_request_page_add_group(page, group);

        m_activePrompt = prompt;
        m_activeKind = PromptKind::Password;
        m_passwordPage = page;

        PurpleRequestPage *presentedPage =
            PURPLE_REQUEST_PAGE(g_object_ref(page));
        purple_ui_request_page_async(
            ui, presentedPage, nullptr, passwordFinished, context);
        g_object_unref(presentedPage);
        g_object_unref(ui);
    }

    std::uint64_t m_sessionGeneration;
    const TelegramTdlibAuthPresenterActions m_actions;
    GMainContext *m_ownerContext;
    GWeakRef m_owner;
    GWeakRef m_ui;
    bool m_alive = true;
    TdAuthPromptId m_activePrompt;
    PromptKind m_activeKind = PromptKind::None;
    PurpleQrCode *m_qr = nullptr;
    GCancellable *m_qrCancellable = nullptr;
    PurpleRequestPage *m_passwordPage = nullptr;
};

TelegramTdlibAuthPresenter::TelegramTdlibAuthPresenter(
    std::shared_ptr<State> state) noexcept
    : m_state(std::move(state))
{
}

TelegramTdlibAuthPresenter::~TelegramTdlibAuthPresenter()
{
    std::shared_ptr<State> state = std::move(m_state);
    if (state)
        state->shutdown();
}

void TelegramTdlibAuthPresenter::showQr(
    TdAuthPromptId prompt,
    const std::string &link) noexcept
{
    std::shared_ptr<State> state = m_state;
    if (state)
        state->showQr(prompt, link);
}

void TelegramTdlibAuthPresenter::showPassword(
    TdAuthPromptId prompt,
    const TdAuthPasswordChallenge &challenge) noexcept
{
    std::shared_ptr<State> state = m_state;
    if (state)
        state->showPassword(prompt, challenge);
}

void TelegramTdlibAuthPresenter::closePrompt(
    TdAuthPromptId prompt) noexcept
{
    std::shared_ptr<State> state = m_state;
    if (state)
        state->closePrompt(prompt);
}

void TelegramTdlibAuthPresenter::closeAll() noexcept
{
    std::shared_ptr<State> state = m_state;
    if (state)
        state->closeAll();
}

std::unique_ptr<TelegramTdlibAuthPresenter>
telegramTdlibCreateAuthPresenter(
    PurpleConnection *owner,
    PurpleUi *ui,
    std::uint64_t sessionGeneration,
    TelegramTdlibAuthPresenterActions actions) noexcept
{
    if (!PURPLE_IS_CONNECTION(owner) || !PURPLE_IS_UI(ui) ||
        sessionGeneration == 0 || !actions.cancelPrompt ||
        !actions.failPrompt || !actions.submitPassword) {
        return nullptr;
    }

    try {
        std::shared_ptr<TelegramTdlibAuthPresenter::State> state =
            std::make_shared<TelegramTdlibAuthPresenter::State>(
                owner, ui, sessionGeneration, std::move(actions));
        return std::unique_ptr<TelegramTdlibAuthPresenter>(
            new TelegramTdlibAuthPresenter(std::move(state)));
    } catch (...) {
        return nullptr;
    }
}
