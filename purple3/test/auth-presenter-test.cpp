/*
 * tdlib-purple - Telegram protocol plugin for libpurple
 * Copyright (C) tdlib-purple contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
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

#include "../telegram-purple3-auth-presenter.h"

#include "../../module-activity.h"

#include <gio/gio.h>
#include <glib.h>

#include <cstdint>
#include <memory>
#include <string>

constexpr char syntheticQrOne[] = "tg://login?token=-_AA";
constexpr char syntheticQrTwo[] = "tg://login?token=-_AB";
constexpr char syntheticQrClose[] =
    "tg://login?token=c3ludGhldGljLWNsb3Nl";
constexpr char syntheticQrPresentReentrant[] =
    "tg://login?token=c3ludGhldGljLXByZXNlbnQtcmVlbnRyYW50";
constexpr char syntheticQrBeforeNotify[] =
    "tg://login?token=c3ludGhldGljLWJlZm9yZS1ub3RpZnk";
constexpr char syntheticQrNotifyReentrant[] =
    "tg://login?token=c3ludGhldGljLW5vdGlmeS1yZWVudHJhbnQ";
constexpr char syntheticQrUnsupported[] =
    "tg://login?token=c3ludGhldGljLXVuc3VwcG9ydGVk";
constexpr char syntheticQrLate[] =
    "tg://login?token=c3ludGhldGljLWxhdGU";

typedef struct _PresenterTestUi PresenterTestUi;
typedef struct _PresenterTestUiClass PresenterTestUiClass;

struct _PresenterTestUi {
    PurpleUi parent;

    PurpleQrCode *qr;
    GCancellable *qrCancellable;
    PurpleRequestPage *page;
    GTask *pageTask;
    guint qrPresentationCount;
    guint pageRequestCount;
    gboolean qrSupported;
    gboolean cancelQrDuringPresentation;
    gboolean cancelQrOnTextChange;
};

struct _PresenterTestUiClass {
    PurpleUiClass parentClass;
};

#define PRESENTER_TEST_TYPE_UI (presenter_test_ui_get_type())
#define PRESENTER_TEST_UI(obj) \
    (G_TYPE_CHECK_INSTANCE_CAST( \
        (obj), PRESENTER_TEST_TYPE_UI, PresenterTestUi))

GType presenter_test_ui_get_type(void);
G_DEFINE_TYPE(PresenterTestUi, presenter_test_ui, PURPLE_TYPE_UI)

static void
presenter_test_ui_qr_cancelled(
    G_GNUC_UNUSED GCancellable *cancellable,
    gpointer data)
{
    PresenterTestUi *ui = PRESENTER_TEST_UI(data);
    g_clear_object(&ui->qr);
    g_clear_object(&ui->qrCancellable);
}

static void presenter_test_ui_qr_text_changed(
    PurpleQrCode *qr,
    GParamSpec *pspec,
    gpointer data);

static gboolean
presenter_test_ui_present_qr_code(
    PurpleUi *purpleUi,
    PurpleQrCode *qr,
    GCancellable *cancellable,
    GError **error)
{
    PresenterTestUi *ui = PRESENTER_TEST_UI(purpleUi);
    ui->qrPresentationCount++;

    if (!ui->qrSupported) {
        g_set_error_literal(
            error,
            PURPLE_UI_ERROR,
            PURPLE_UI_ERROR_VFUNC_NOT_IMPLEMENTED,
            "QR presentation unavailable");
        return FALSE;
    }

    g_set_object(&ui->qr, qr);
    g_set_object(&ui->qrCancellable, cancellable);
    g_signal_connect_object(
        cancellable,
        "cancelled",
        G_CALLBACK(presenter_test_ui_qr_cancelled),
        ui,
        G_CONNECT_DEFAULT);
    g_signal_connect_object(
        qr,
        "notify::text",
        G_CALLBACK(presenter_test_ui_qr_text_changed),
        ui,
        G_CONNECT_DEFAULT);
    if (ui->cancelQrDuringPresentation)
        g_cancellable_cancel(cancellable);
    return TRUE;
}

static void
presenter_test_ui_qr_text_changed(
    G_GNUC_UNUSED PurpleQrCode *qr,
    G_GNUC_UNUSED GParamSpec *pspec,
    gpointer data)
{
    PresenterTestUi *ui = PRESENTER_TEST_UI(data);

    if (ui->cancelQrOnTextChange && ui->qrCancellable)
        g_cancellable_cancel(ui->qrCancellable);
}

static void
presenter_test_ui_complete_page_error(PresenterTestUi *ui)
{
    if (!ui->pageTask)
        return;

    GTask *task = ui->pageTask;
    ui->pageTask = nullptr;
    g_task_return_new_error_literal(
        task, G_IO_ERROR, G_IO_ERROR_CANCELLED, "page closed");
    g_object_unref(task);
    g_clear_object(&ui->page);
}

static void
presenter_test_ui_page_closed(
    G_GNUC_UNUSED PurpleRequestPage *page,
    gpointer data)
{
    presenter_test_ui_complete_page_error(PRESENTER_TEST_UI(data));
}

static void
presenter_test_ui_request_page_async(
    PurpleUi *purpleUi,
    PurpleRequestPage *page,
    GCancellable *cancellable,
    GAsyncReadyCallback callback,
    gpointer data)
{
    PresenterTestUi *ui = PRESENTER_TEST_UI(purpleUi);
    g_assert_null(ui->pageTask);

    ui->pageRequestCount++;
    ui->page = PURPLE_REQUEST_PAGE(g_object_ref(page));
    ui->pageTask = g_task_new(purpleUi, cancellable, callback, data);
    g_signal_connect_object(
        page,
        "close",
        G_CALLBACK(presenter_test_ui_page_closed),
        ui,
        G_CONNECT_DEFAULT);
}

static PurpleRequestPage *
presenter_test_ui_request_page_finish(
    G_GNUC_UNUSED PurpleUi *ui,
    GAsyncResult *result,
    GError **error)
{
    return PURPLE_REQUEST_PAGE(
        g_task_propagate_pointer(G_TASK(result), error));
}

static void
presenter_test_ui_finalize(GObject *object)
{
    PresenterTestUi *ui = PRESENTER_TEST_UI(object);
    g_assert_null(ui->pageTask);
    g_clear_object(&ui->page);
    g_clear_object(&ui->qr);
    g_clear_object(&ui->qrCancellable);

    G_OBJECT_CLASS(presenter_test_ui_parent_class)->finalize(object);
}

static void
presenter_test_ui_init(PresenterTestUi *ui)
{
    ui->qrSupported = TRUE;
}

static void
presenter_test_ui_class_init(PresenterTestUiClass *klass)
{
    GObjectClass *objectClass = G_OBJECT_CLASS(klass);
    PurpleUiClass *uiClass = PURPLE_UI_CLASS(klass);

    objectClass->finalize = presenter_test_ui_finalize;
    uiClass->present_qr_code = presenter_test_ui_present_qr_code;
    uiClass->request_page_async = presenter_test_ui_request_page_async;
    uiClass->request_page_finish = presenter_test_ui_request_page_finish;
}

static void
presenter_test_ui_accept_password(
    PresenterTestUi *ui,
    const char *password)
{
    g_assert_nonnull(ui->pageTask);
    g_assert_nonnull(ui->page);

    PurpleRequestField *field =
        purple_request_page_get_field(ui->page, "password");
    purple_request_field_string_set_value(
        PURPLE_REQUEST_FIELD_STRING(field), password);

    GTask *task = ui->pageTask;
    PurpleRequestPage *page = ui->page;
    ui->pageTask = nullptr;
    ui->page = nullptr;
    g_task_return_pointer(
        task, g_object_ref(page), g_object_unref);
    g_object_unref(task);
    g_object_unref(page);
}

static void
presenter_test_ui_cancel_password(PresenterTestUi *ui)
{
    presenter_test_ui_complete_page_error(ui);
}

static void
presenter_test_drain_context(void)
{
    while (g_main_context_iteration(nullptr, FALSE)) {
    }
}

struct RecordedActions {
    guint cancellations = 0;
    guint failures = 0;
    guint submissions = 0;
    std::uint64_t sessionGeneration = 0;
    TdAuthPromptId prompt;
    TdAuthPresentationFailure failure =
        TdAuthPresentationFailure::Failed;
    std::string password;
};

static TelegramTdlibAuthPresenterActions
presenter_test_actions(RecordedActions &recorded)
{
    TelegramTdlibAuthPresenterActions actions;
    actions.cancelPrompt = [&recorded](
                               std::uint64_t generation,
                               TdAuthPromptId prompt) {
        recorded.cancellations++;
        recorded.sessionGeneration = generation;
        recorded.prompt = prompt;
    };
    actions.failPrompt = [&recorded](
                             std::uint64_t generation,
                             TdAuthPromptId prompt,
                             TdAuthPresentationFailure failure) {
        recorded.failures++;
        recorded.sessionGeneration = generation;
        recorded.prompt = prompt;
        recorded.failure = failure;
    };
    actions.submitPassword = [&recorded](
                                 std::uint64_t generation,
                                 TdAuthPromptId prompt,
                                 std::string password) {
        recorded.submissions++;
        recorded.sessionGeneration = generation;
        recorded.prompt = prompt;
        recorded.password = std::move(password);
    };
    return actions;
}

struct PresenterFixture {
    PresenterFixture()
        : owner(PURPLE_CONNECTION(
              g_object_new(PURPLE_TYPE_CONNECTION, nullptr))),
          ui(PRESENTER_TEST_UI(
              g_object_new(PRESENTER_TEST_TYPE_UI, nullptr))),
          presenter(telegramTdlibCreateAuthPresenter(
              owner,
              PURPLE_UI(ui),
              17,
              presenter_test_actions(actions)))
    {
        g_assert_nonnull(presenter);
    }

    ~PresenterFixture()
    {
        presenter.reset();
        presenter_test_drain_context();
        g_object_unref(ui);
        if (owner)
            g_object_unref(owner);
        g_assert_false(moduleActivityPending());
    }

    PurpleConnection *owner;
    PresenterTestUi *ui;
    RecordedActions actions;
    std::unique_ptr<TelegramTdlibAuthPresenter> presenter;
};

static void
test_qr_rotates_and_user_cancel_is_exact(void)
{
    PresenterFixture fixture;
    const TdAuthPromptId prompt(41);

    fixture.presenter->showQr(prompt, syntheticQrOne);
    g_assert_cmpuint(fixture.ui->qrPresentationCount, ==, 1);
    g_assert_nonnull(fixture.ui->qr);
    g_assert_nonnull(fixture.ui->qrCancellable);
    g_assert_true(moduleActivityPending());

    PurpleQrCode *originalQr = fixture.ui->qr;
    GCancellable *originalCancellable = fixture.ui->qrCancellable;
    fixture.presenter->showQr(prompt, syntheticQrTwo);

    g_assert_cmpuint(fixture.ui->qrPresentationCount, ==, 1);
    g_assert_true(fixture.ui->qr == originalQr);
    g_assert_true(fixture.ui->qrCancellable == originalCancellable);
    g_assert_true(g_str_equal(
        purple_qr_code_get_text(fixture.ui->qr),
        syntheticQrTwo));

    GCancellable *cancellable = G_CANCELLABLE(
        g_object_ref(fixture.ui->qrCancellable));
    g_cancellable_cancel(cancellable);
    g_object_unref(cancellable);
    presenter_test_drain_context();

    g_assert_cmpuint(fixture.actions.cancellations, ==, 1);
    g_assert_cmpuint(fixture.actions.sessionGeneration, ==, 17);
    g_assert_true(fixture.actions.prompt == prompt);
    g_assert_null(fixture.ui->qr);
    g_assert_null(fixture.ui->qrCancellable);
    g_assert_false(moduleActivityPending());
}

static void
test_programmatic_qr_close_is_not_user_cancel(void)
{
    PresenterFixture fixture;
    const TdAuthPromptId prompt(42);

    fixture.presenter->showQr(prompt, syntheticQrClose);
    fixture.presenter->closePrompt(TdAuthPromptId(999));
    g_assert_nonnull(fixture.ui->qr);

    fixture.presenter->closePrompt(prompt);
    presenter_test_drain_context();

    g_assert_cmpuint(fixture.actions.cancellations, ==, 0);
    g_assert_null(fixture.ui->qr);
    g_assert_null(fixture.ui->qrCancellable);
    g_assert_false(moduleActivityPending());
}

static void
test_synchronous_qr_reentrancy_is_safe(void)
{
    {
        PresenterFixture fixture;
        fixture.ui->cancelQrDuringPresentation = TRUE;

        fixture.presenter->showQr(
            TdAuthPromptId(43), syntheticQrPresentReentrant);
        presenter_test_drain_context();

        g_assert_cmpuint(fixture.actions.cancellations, ==, 1);
        g_assert_null(fixture.ui->qr);
        g_assert_null(fixture.ui->qrCancellable);
    }

    {
        PresenterFixture fixture;
        const TdAuthPromptId prompt(44);
        fixture.presenter->showQr(prompt, syntheticQrBeforeNotify);
        fixture.ui->cancelQrOnTextChange = TRUE;

        fixture.presenter->showQr(
            prompt, syntheticQrNotifyReentrant);
        presenter_test_drain_context();

        g_assert_cmpuint(fixture.actions.cancellations, ==, 1);
        g_assert_null(fixture.ui->qr);
        g_assert_null(fixture.ui->qrCancellable);
    }
}

static void
test_unsafe_and_unsupported_qr_fail_without_presenting_secret(void)
{
    PresenterFixture fixture;
    std::string oversizedLink("tg://");
    oversizedLink.append(2044, 'a');
    const std::string invalidLinks[] = {
        "",
        "tg://",
        "https://invalid.example/login",
        oversizedLink,
    };

    for (guint index = 0; index < G_N_ELEMENTS(invalidLinks); ++index) {
        fixture.presenter->showQr(
            TdAuthPromptId(50 + index), invalidLinks[index]);
    }

    for (unsigned int byte = 0; byte <= 0x1f; ++byte) {
        std::string controlLink("tg://future");
        controlLink.push_back(static_cast<char>(byte));
        controlLink.append("value");
        fixture.presenter->showQr(
            TdAuthPromptId(100 + byte), controlLink);
    }
    const std::string deleteLink =
        std::string("tg://future") + static_cast<char>(0x7f) + "value";
    fixture.presenter->showQr(TdAuthPromptId(132), deleteLink);

    g_assert_true(
        fixture.actions.failure == TdAuthPresentationFailure::Failed);
    g_assert_cmpuint(fixture.ui->qrPresentationCount, ==, 0);

    const guint rejectedLinks = fixture.actions.failures;
    fixture.ui->qrSupported = FALSE;
    fixture.presenter->showQr(
        TdAuthPromptId(200), syntheticQrUnsupported);
    presenter_test_drain_context();
    g_assert_cmpuint(fixture.actions.failures, ==, rejectedLinks + 1);
    g_assert_true(
        fixture.actions.failure == TdAuthPresentationFailure::Unsupported);
    g_assert_null(fixture.ui->qr);
    g_assert_false(moduleActivityPending());
}

static void
test_future_tg_uri_shape_is_presented_unchanged(void)
{
    PresenterFixture fixture;
    const char futureQr[] =
        "tg://device-link/v2?token=synthetic%2Bvalue&mode=desktop";

    fixture.presenter->showQr(TdAuthPromptId(59), futureQr);

    g_assert_cmpuint(fixture.actions.failures, ==, 0);
    g_assert_cmpuint(fixture.ui->qrPresentationCount, ==, 1);
    g_assert_nonnull(fixture.ui->qr);
    g_assert_cmpstr(
        purple_qr_code_get_text(fixture.ui->qr), ==, futureQr);
}

static void
test_password_is_masked_required_and_submitted(void)
{
    PresenterFixture fixture;
    const TdAuthPromptId prompt(60);
    TdAuthPasswordChallenge challenge;
    challenge.hint = "synthetic hint";
    challenge.recoveryEmailAddressPattern = "s***@invalid";

    fixture.presenter->showPassword(prompt, challenge);
    g_assert_cmpuint(fixture.ui->pageRequestCount, ==, 1);
    g_assert_nonnull(fixture.ui->page);
    g_assert_true(purple_request_page_exists(
        fixture.ui->page, "password"));
    g_assert_false(purple_request_page_exists(
        fixture.ui->page, "remember"));

    PurpleRequestField *field =
        purple_request_page_get_field(fixture.ui->page, "password");
    g_assert_true(PURPLE_IS_REQUEST_FIELD_STRING(field));
    g_assert_true(purple_request_field_string_is_masked(
        PURPLE_REQUEST_FIELD_STRING(field)));
    g_assert_true(purple_request_field_is_required(field));
    g_assert_null(purple_request_field_string_get_default_value(
        PURPLE_REQUEST_FIELD_STRING(field)));
    g_assert_true(moduleActivityPending());

    presenter_test_ui_accept_password(
        fixture.ui, "synthetic-password");
    presenter_test_drain_context();

    g_assert_cmpuint(fixture.actions.submissions, ==, 1);
    g_assert_cmpuint(fixture.actions.sessionGeneration, ==, 17);
    g_assert_true(fixture.actions.prompt == prompt);
    g_assert_true(fixture.actions.password == "synthetic-password");
    g_assert_false(moduleActivityPending());
}

static void
test_password_user_and_programmatic_close_are_distinct(void)
{
    PresenterFixture fixture;
    TdAuthPasswordChallenge challenge;

    fixture.presenter->showPassword(TdAuthPromptId(70), challenge);
    fixture.presenter->closePrompt(TdAuthPromptId(70));
    presenter_test_drain_context();
    g_assert_cmpuint(fixture.actions.cancellations, ==, 0);
    g_assert_false(moduleActivityPending());

    fixture.presenter->showPassword(TdAuthPromptId(71), challenge);
    presenter_test_ui_cancel_password(fixture.ui);
    presenter_test_drain_context();
    g_assert_cmpuint(fixture.actions.cancellations, ==, 1);
    g_assert_true(fixture.actions.prompt == TdAuthPromptId(71));
    g_assert_false(moduleActivityPending());
}

static void
test_destroyed_owner_makes_late_callback_inert(void)
{
    PresenterFixture fixture;

    fixture.presenter->showQr(TdAuthPromptId(80), syntheticQrLate);
    g_object_unref(fixture.owner);
    fixture.owner = nullptr;

    GCancellable *cancellable = G_CANCELLABLE(
        g_object_ref(fixture.ui->qrCancellable));
    g_cancellable_cancel(cancellable);
    g_object_unref(cancellable);
    presenter_test_drain_context();

    g_assert_cmpuint(fixture.actions.cancellations, ==, 0);
    g_assert_false(moduleActivityPending());
}

int
main(int argc, char **argv)
{
    g_test_init(&argc, &argv, nullptr);

    g_test_add_func(
        "/purple3/auth-presenter/qr-rotation-user-cancel",
        test_qr_rotates_and_user_cancel_is_exact);
    g_test_add_func(
        "/purple3/auth-presenter/qr-programmatic-close",
        test_programmatic_qr_close_is_not_user_cancel);
    g_test_add_func(
        "/purple3/auth-presenter/qr-synchronous-reentrancy",
        test_synchronous_qr_reentrancy_is_safe);
    g_test_add_func(
        "/purple3/auth-presenter/qr-invalid-unsupported",
        test_unsafe_and_unsupported_qr_fail_without_presenting_secret);
    g_test_add_func(
        "/purple3/auth-presenter/qr-future-uri-shape",
        test_future_tg_uri_shape_is_presented_unchanged);
    g_test_add_func(
        "/purple3/auth-presenter/password-masked-submit",
        test_password_is_masked_required_and_submitted);
    g_test_add_func(
        "/purple3/auth-presenter/password-close-distinction",
        test_password_user_and_programmatic_close_are_distinct);
    g_test_add_func(
        "/purple3/auth-presenter/owner-destroyed",
        test_destroyed_owner_makes_late_callback_inert);

    return g_test_run();
}
