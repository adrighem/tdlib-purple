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

#ifndef TELEGRAM_PURPLE3_AUTH_PRESENTER_H
#define TELEGRAM_PURPLE3_AUTH_PRESENTER_H

#include <purple.h>

#include "../td-auth-controller.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

struct TelegramTdlibAuthPresenterActions {
    using CancelPrompt =
        std::function<void(std::uint64_t, TdAuthPromptId)>;
    using FailPrompt = std::function<void(
        std::uint64_t,
        TdAuthPromptId,
        TdAuthPresentationFailure)>;
    using SubmitPassword = std::function<void(
        std::uint64_t,
        TdAuthPromptId,
        std::string)>;

    CancelPrompt cancelPrompt;
    FailPrompt failPrompt;
    SubmitPassword submitPassword;
};

// Presents the Purple 3 authorization prompts for one connection session.
//
// All public methods must be called on the Purple UI owner context. The
// presenter keeps only weak references to owner and ui. The caller owns the
// returned presenter and must destroy it before destroying state captured by
// the action callbacks. Action callbacks must not strongly retain the owning
// connection or session.
//
// QR links and passwords are copied only as required by Purple and TDLib. They
// are never logged or persisted by this class.
class TelegramTdlibAuthPresenter final {
public:
    ~TelegramTdlibAuthPresenter();

    TelegramTdlibAuthPresenter(
        const TelegramTdlibAuthPresenter &) = delete;
    TelegramTdlibAuthPresenter &operator=(
        const TelegramTdlibAuthPresenter &) = delete;

    // Reuses one PurpleQrCode and its dedicated cancellable when prompt is the
    // current QR prompt. A new prompt first closes the previous presentation.
    void showQr(
        TdAuthPromptId prompt,
        const std::string &link) noexcept;

    // Shows a required, masked field. No password-saving control is exposed.
    void showPassword(
        TdAuthPromptId prompt,
        const TdAuthPasswordChallenge &challenge) noexcept;

    // Programmatic close invalidates the prompt before dismissing its UI, so
    // the resulting UI callback cannot be mistaken for user cancellation.
    void closePrompt(TdAuthPromptId prompt) noexcept;
    void closeAll() noexcept;

private:
    class State;

    explicit TelegramTdlibAuthPresenter(
        std::shared_ptr<State> state) noexcept;

    std::shared_ptr<State> m_state;

    friend std::unique_ptr<TelegramTdlibAuthPresenter>
    telegramTdlibCreateAuthPresenter(
        PurpleConnection *,
        PurpleUi *,
        std::uint64_t,
        TelegramTdlibAuthPresenterActions) noexcept;
};

// owner and ui are transfer-none. sessionGeneration must be nonzero. All
// callbacks are required. Returns null when the arguments are invalid or the
// presenter cannot be allocated.
std::unique_ptr<TelegramTdlibAuthPresenter>
telegramTdlibCreateAuthPresenter(
    PurpleConnection *owner,
    PurpleUi *ui,
    std::uint64_t sessionGeneration,
    TelegramTdlibAuthPresenterActions actions) noexcept;

#endif
