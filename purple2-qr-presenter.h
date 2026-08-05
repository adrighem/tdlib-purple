#ifndef TDLIB_PURPLE_PURPLE2_QR_PRESENTER_H
#define TDLIB_PURPLE_PURPLE2_QR_PRESENTER_H

#include "td-auth-controller.h"

#include <purple.h>

#include <functional>
#include <memory>
#include <string>

enum class Purple2QrPresentationResult : std::uint8_t {
    Presented,
    Unsupported,
    Failed,
};

// The presenter never logs or persists QR links. It wipes its active temporary
// rendering buffers before releasing them.
class Purple2QrPresenter {
public:
    using CancelCallback = std::function<void(TdAuthPromptId)>;

    Purple2QrPresenter(
        PurpleConnection *connection,
        PurpleAccount *account,
        CancelCallback cancelCallback);
    ~Purple2QrPresenter();

    Purple2QrPresenter(const Purple2QrPresenter &) = delete;
    Purple2QrPresenter &operator=(const Purple2QrPresenter &) = delete;

    static bool available() noexcept;

    Purple2QrPresentationResult show(
        TdAuthPromptId prompt,
        const std::string &link) noexcept;
    void closePrompt(TdAuthPromptId prompt) noexcept;
    void closeAll() noexcept;

private:
    struct State;
    std::shared_ptr<State> m_state;
};

#endif
