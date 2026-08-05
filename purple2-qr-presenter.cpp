#include "purple2-qr-presenter.h"

#include "buildopt.h"
#include "translate.h"

#include <algorithm>
#include <csetjmp>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#ifdef TDLIB_PURPLE_HAVE_PURPLE2_QR
#include <png.h>
#include <qrencode.h>
#endif

namespace {

constexpr std::size_t maximumQrLinkLength = 2048;
constexpr unsigned maximumImageSize = 128;
constexpr unsigned quietZoneModules = 4;

template <typename Value>
void wipeVector(std::vector<Value> &value) noexcept
{
    if (!value.empty()) {
        volatile unsigned char *bytes =
            reinterpret_cast<volatile unsigned char *>(value.data());
        const std::size_t byteCount = value.size() * sizeof(Value);
        for (std::size_t index = 0; index < byteCount; ++index)
            bytes[index] = 0;
    }
    value.clear();
}

bool validQrLink(const std::string &link) noexcept
{
    static const char prefix[] = "tg://login?";
    if (link.size() < sizeof(prefix) ||
        link.size() > maximumQrLinkLength ||
        link.compare(0, sizeof(prefix) - 1, prefix) != 0) {
        return false;
    }
    return std::none_of(
        link.begin(), link.end(), [](unsigned char character) {
            return character < 0x20 || character == 0x7f;
        });
}

#ifdef TDLIB_PURPLE_HAVE_PURPLE2_QR

struct PngOutput {
    std::vector<unsigned char> *bytes;
};

void writePng(png_structp png, png_bytep data, png_size_t size)
{
    PngOutput *output = static_cast<PngOutput *>(png_get_io_ptr(png));
    if (!output || !output->bytes)
        png_error(png, "missing PNG output");
    try {
        output->bytes->insert(
            output->bytes->end(), data, data + size);
    } catch (...) {
        png_error(png, "PNG output allocation failed");
    }
}

void flushPng(png_structp)
{
}

bool encodePng(
    const std::string &value,
    std::vector<unsigned char> &pngBytes) noexcept
{
    QRcode *code = nullptr;
    png_structp png = nullptr;
    png_infop info = nullptr;
    std::vector<unsigned char> pixels;
    std::vector<png_bytep> rows;
    bool success = false;

    try {
        code = QRcode_encodeString8bit(value.c_str(), 0, QR_ECLEVEL_L);
        if (!code || code->width <= 0 || !code->data)
            throw std::runtime_error("QR encoding failed");

        const unsigned modules = static_cast<unsigned>(code->width);
        if (modules > maximumImageSize - 2 * quietZoneModules)
            throw std::length_error("QR image is too large");
        const unsigned totalModules = modules + 2 * quietZoneModules;
        const unsigned scale = maximumImageSize / totalModules;
        if (scale == 0)
            throw std::length_error("QR image is too large");
        const unsigned dimension = totalModules * scale;
        if (dimension == 0 || dimension > maximumImageSize ||
            dimension > std::numeric_limits<std::size_t>::max() / dimension) {
            throw std::length_error("QR image dimensions are invalid");
        }

        pixels.assign(
            static_cast<std::size_t>(dimension) * dimension, 0xff);
        for (unsigned y = 0; y < modules; ++y) {
            for (unsigned x = 0; x < modules; ++x) {
                if ((code->data[y * modules + x] & 1U) == 0)
                    continue;
                const unsigned targetX = (x + quietZoneModules) * scale;
                const unsigned targetY = (y + quietZoneModules) * scale;
                for (unsigned dy = 0; dy < scale; ++dy) {
                    std::fill_n(
                        pixels.begin() +
                            static_cast<std::size_t>(targetY + dy) *
                                dimension +
                            targetX,
                        scale,
                        static_cast<unsigned char>(0));
                }
            }
        }

        rows.reserve(dimension);
        for (unsigned y = 0; y < dimension; ++y) {
            rows.push_back(
                pixels.data() + static_cast<std::size_t>(y) * dimension);
        }

        png = png_create_write_struct(
            PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
        if (!png)
            throw std::bad_alloc();
        info = png_create_info_struct(png);
        if (!info)
            throw std::bad_alloc();

        if (setjmp(png_jmpbuf(png)) != 0)
            throw std::runtime_error("PNG encoding failed");

        PngOutput output{&pngBytes};
        png_set_write_fn(png, &output, writePng, flushPng);
        png_set_IHDR(
            png,
            info,
            dimension,
            dimension,
            8,
            PNG_COLOR_TYPE_GRAY,
            PNG_INTERLACE_NONE,
            PNG_COMPRESSION_TYPE_DEFAULT,
            PNG_FILTER_TYPE_DEFAULT);
        png_write_info(png, info);
        png_write_image(png, rows.data());
        png_write_end(png, info);
        success = !pngBytes.empty();
    } catch (...) {
        success = false;
    }

    if (png || info)
        png_destroy_write_struct(&png, &info);
    if (code) {
        if (code->data && code->width > 0) {
            volatile unsigned char *data = code->data;
            const std::size_t count =
                static_cast<std::size_t>(code->width) * code->width;
            for (std::size_t index = 0; index < count; ++index)
                data[index] = 0;
        }
        QRcode_free(code);
    }
    wipeVector(rows);
    wipeVector(pixels);
    if (!success)
        wipeVector(pngBytes);
    return success;
}

bool rendererWorks() noexcept
{
    static const bool works = []() {
        std::vector<unsigned char> bytes;
        const bool result = encodePng("purple2-qr-renderer-check", bytes);
        wipeVector(bytes);
        return result;
    }();
    return works;
}

#endif

} // namespace

struct Purple2QrPresenter::State {
    struct CallbackContext {
        std::weak_ptr<State> state;
        TdAuthPromptId prompt;
        std::uint64_t revision;
    };

    State(
        PurpleConnection *connectionValue,
        PurpleAccount *accountValue,
        CancelCallback cancelValue)
        : connection(connectionValue),
          account(accountValue),
          cancelCallback(std::move(cancelValue))
    {
    }

    static void cancelled(CallbackContext *context, int)
    {
        if (!context)
            return;
        std::shared_ptr<State> state = context->state.lock();
        if (!state || !state->active ||
            state->prompt != context->prompt ||
            state->revision != context->revision) {
            return;
        }

        state->active = false;
        state->uiHandle = nullptr;
        state->prompt = TdAuthPromptId();
        ++state->revision;
        try {
            if (state->cancelCallback)
                state->cancelCallback(context->prompt);
        } catch (...) {
            // Never let a C UI callback unwind through libpurple.
        }
    }

    void closeActive() noexcept
    {
        if (!active)
            return;
        void *handle = uiHandle;
        active = false;
        uiHandle = nullptr;
        prompt = TdAuthPromptId();
        ++revision;
        if (handle)
            purple_request_close(PURPLE_REQUEST_ACTION, handle);
    }

    PurpleConnection *connection;
    PurpleAccount *account;
    CancelCallback cancelCallback;
    std::vector<std::unique_ptr<CallbackContext>> contexts;
    TdAuthPromptId prompt;
    std::uint64_t revision = 0;
    void *uiHandle = nullptr;
    bool active = false;
};

Purple2QrPresenter::Purple2QrPresenter(
    PurpleConnection *connection,
    PurpleAccount *account,
    CancelCallback cancelCallback)
    : m_state(std::make_shared<State>(
          connection, account, std::move(cancelCallback)))
{
}

Purple2QrPresenter::~Purple2QrPresenter()
{
    closeAll();
    if (m_state)
        m_state->cancelCallback = CancelCallback();
    m_state.reset();
}

bool Purple2QrPresenter::available() noexcept
{
#ifdef TDLIB_PURPLE_HAVE_PURPLE2_QR
    PurpleRequestUiOps *operations = purple_request_get_ui_ops();
    return operations && operations->request_action_with_icon &&
        operations->close_request && rendererWorks();
#else
    return false;
#endif
}

Purple2QrPresentationResult Purple2QrPresenter::show(
    TdAuthPromptId prompt,
    const std::string &link) noexcept
{
#ifndef TDLIB_PURPLE_HAVE_PURPLE2_QR
    (void)prompt;
    (void)link;
    return Purple2QrPresentationResult::Unsupported;
#else
    if (!m_state || !prompt.valid() || !validQrLink(link))
        return Purple2QrPresentationResult::Failed;
    if (!available())
        return Purple2QrPresentationResult::Unsupported;

    std::vector<unsigned char> pngBytes;
    if (!encodePng(link, pngBytes))
        return Purple2QrPresentationResult::Failed;

    std::shared_ptr<State> state = m_state;
    state->closeActive();
    state->prompt = prompt;
    ++state->revision;

    std::unique_ptr<State::CallbackContext> context(
        new (std::nothrow) State::CallbackContext{
            state, prompt, state->revision});
    if (!context) {
        wipeVector(pngBytes);
        state->prompt = TdAuthPromptId();
        return Purple2QrPresentationResult::Failed;
    }
    State::CallbackContext *rawContext = context.get();
    try {
        state->contexts.push_back(std::move(context));
    } catch (...) {
        wipeVector(pngBytes);
        state->prompt = TdAuthPromptId();
        ++state->revision;
        return Purple2QrPresentationResult::Failed;
    }

    void *handle = purple_request_action_with_icon(
        state->connection,
        _("Telegram authentication"),
        _("Scan this QR code with Telegram on another device."),
        _("Open Telegram, then go to Settings > Devices > Link Desktop "
          "Device. This code may refresh automatically."),
        0,
        state->account,
        nullptr,
        nullptr,
        pngBytes.data(),
        pngBytes.size(),
        rawContext,
        1,
        _("_Cancel"),
        G_CALLBACK(State::cancelled));
    wipeVector(pngBytes);
    if (!handle) {
        state->prompt = TdAuthPromptId();
        ++state->revision;
        return Purple2QrPresentationResult::Failed;
    }

    state->uiHandle = handle;
    state->active = true;
    return Purple2QrPresentationResult::Presented;
#endif
}

void Purple2QrPresenter::closePrompt(TdAuthPromptId prompt) noexcept
{
    if (m_state && m_state->active && m_state->prompt == prompt)
        m_state->closeActive();
}

void Purple2QrPresenter::closeAll() noexcept
{
    if (m_state)
        m_state->closeActive();
}
