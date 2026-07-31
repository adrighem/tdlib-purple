#include "td-auth-controller.h"
#include "td-transport.h"

#include <gtest/gtest.h>

#include <deque>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace td::td_api;

enum class ObservedEventType {
    Phone,
    Premium,
    EmailAddress,
    EmailCode,
    Code,
    QrLink,
    Registration,
    Password,
    PromptClosed,
    RequestFailed,
    Ready,
    Cancelled,
    Failed,
    LoggingOut,
    Closing,
    Closed,
};

struct ObservedEvent {
    ObservedEventType type = ObservedEventType::Failed;
    TdAuthPromptId prompt;
    TdAuthPromptType promptType = TdAuthPromptType::None;
    TdAuthPromptCloseReason closeReason =
        TdAuthPromptCloseReason::Failed;
    TdAuthEmailAddressChallenge emailAddress;
    TdAuthEmailCodeChallenge emailCode;
    TdAuthCodeChallenge code;
    TdAuthRegistrationChallenge registration;
    TdAuthPasswordChallenge password;
    TdAuthRequestFailure requestFailure;
    TdAuthFailure failure;
    std::string qrLink;
};

class RecordingAuthObserver : public TdAuthObserver {
public:
    std::vector<ObservedEvent> events;
    std::function<void(TdAuthPromptId)> phoneHook;
    std::function<void(
        TdAuthPromptId,
        TdAuthPromptType,
        TdAuthPromptCloseReason)> closeHook;
    std::function<void()> loggingOutHook;
    bool throwOnPhone = false;
    bool throwOnClose = false;

    void onPhoneNumberRequired(TdAuthPromptId prompt) override
    {
        ObservedEvent event;
        event.type = ObservedEventType::Phone;
        event.prompt = prompt;
        events.push_back(event);
        if (throwOnPhone)
            throw std::runtime_error("synthetic observer failure");
        if (phoneHook)
            phoneHook(prompt);
    }

    void onPremiumPurchaseRequired(TdAuthPromptId prompt) override
    {
        ObservedEvent event;
        event.type = ObservedEventType::Premium;
        event.prompt = prompt;
        events.push_back(event);
    }

    void onEmailAddressRequired(
        TdAuthPromptId prompt,
        const TdAuthEmailAddressChallenge &challenge) override
    {
        ObservedEvent event;
        event.type = ObservedEventType::EmailAddress;
        event.prompt = prompt;
        event.emailAddress = challenge;
        events.push_back(event);
    }

    void onEmailCodeRequired(
        TdAuthPromptId prompt,
        const TdAuthEmailCodeChallenge &challenge) override
    {
        ObservedEvent event;
        event.type = ObservedEventType::EmailCode;
        event.prompt = prompt;
        event.emailCode = challenge;
        events.push_back(event);
    }

    void onCodeRequired(
        TdAuthPromptId prompt,
        const TdAuthCodeChallenge &challenge) override
    {
        ObservedEvent event;
        event.type = ObservedEventType::Code;
        event.prompt = prompt;
        event.code = challenge;
        events.push_back(event);
    }

    void onQrLinkChanged(
        TdAuthPromptId prompt,
        const std::string &link) override
    {
        ObservedEvent event;
        event.type = ObservedEventType::QrLink;
        event.prompt = prompt;
        event.qrLink = link;
        events.push_back(event);
    }

    void onRegistrationRequired(
        TdAuthPromptId prompt,
        const TdAuthRegistrationChallenge &challenge) override
    {
        ObservedEvent event;
        event.type = ObservedEventType::Registration;
        event.prompt = prompt;
        event.registration = challenge;
        events.push_back(event);
    }

    void onPasswordRequired(
        TdAuthPromptId prompt,
        const TdAuthPasswordChallenge &challenge) override
    {
        ObservedEvent event;
        event.type = ObservedEventType::Password;
        event.prompt = prompt;
        event.password = challenge;
        events.push_back(event);
    }

    void onPromptClosed(
        TdAuthPromptId prompt,
        TdAuthPromptType type,
        TdAuthPromptCloseReason reason) override
    {
        ObservedEvent event;
        event.type = ObservedEventType::PromptClosed;
        event.prompt = prompt;
        event.promptType = type;
        event.closeReason = reason;
        events.push_back(event);
        const auto hook = closeHook;
        if (hook)
            hook(prompt, type, reason);
        if (throwOnClose)
            throw std::runtime_error("synthetic close failure");
    }

    void onRequestFailed(
        const TdAuthRequestFailure &failure) override
    {
        ObservedEvent event;
        event.type = ObservedEventType::RequestFailed;
        event.requestFailure = failure;
        events.push_back(event);
    }

    void onAuthorizationReady() override
    {
        ObservedEvent event;
        event.type = ObservedEventType::Ready;
        events.push_back(event);
    }

    void onAuthorizationCancelled() override
    {
        ObservedEvent event;
        event.type = ObservedEventType::Cancelled;
        events.push_back(event);
    }

    void onAuthorizationFailed(const TdAuthFailure &failure) override
    {
        ObservedEvent event;
        event.type = ObservedEventType::Failed;
        event.failure = failure;
        events.push_back(event);
    }

    void onLoggingOut() override
    {
        ObservedEvent event;
        event.type = ObservedEventType::LoggingOut;
        events.push_back(event);
        const auto hook = loggingOutHook;
        if (hook)
            hook();
    }

    void onClosing() override
    {
        ObservedEvent event;
        event.type = ObservedEventType::Closing;
        events.push_back(event);
    }

    void onClosed() override
    {
        ObservedEvent event;
        event.type = ObservedEventType::Closed;
        events.push_back(event);
    }

    std::string capturedText() const
    {
        std::string result;
        for (const ObservedEvent &event: events) {
            result += event.qrLink;
            result += event.emailCode.emailAddressPattern;
            result += event.code.code.pattern;
            result += event.code.nextCode.pattern;
            result += event.password.hint;
            result += event.password.recoveryEmailAddressPattern;
        }
        return result;
    }
};

struct RecordedRequest {
    std::uint64_t id = 0;
    TdTransport::FunctionPtr function;
};

template <typename State>
object_ptr<AuthorizationState> authState(object_ptr<State> state)
{
    return td::move_tl_object_as<AuthorizationState>(std::move(state));
}

class UnknownAuthorizationState final : public AuthorizationState {
public:
    std::int32_t get_id() const final
    {
        return 123456789;
    }

    void store(td::TlStorerToString &, const char *) const final
    {}
};

class TdAuthControllerTest : public testing::Test {
public:
    static const std::int32_t TestApiId = 1234567;

    void SetUp() override
    {
        context = g_main_context_new();
        ASSERT_NE(context, nullptr);
        g_main_context_push_thread_default(context);
        start(TdAuthMode::PhoneNumber);
    }

    void TearDown() override
    {
        stop();
        g_main_context_pop_thread_default(context);
        g_main_context_unref(context);
    }

    TdAuthConfiguration configuration(TdAuthMode mode) const
    {
        return TdAuthConfiguration(
            TestApiId,
            "0123456789abcdef0123456789abcdef",
            "/tmp/td-auth-controller-test",
            true,
            mode);
    }

    void start(TdAuthMode mode)
    {
        stop();
        observer.events.clear();
        observer.phoneHook = std::function<void(TdAuthPromptId)>();
        observer.closeHook = decltype(observer.closeHook)();
        observer.loggingOutHook = std::function<void()>();
        observer.throwOnPhone = false;
        observer.throwOnClose = false;
        transport.reset(new TdTransport(
            [this](
                std::uint64_t requestId,
                TdTransport::FunctionPtr function) {
                RecordedRequest request;
                request.id = requestId;
                request.function = std::move(function);
                requests.push_back(std::move(request));
            },
            TdTransport::UpdateCallback(),
            TdTransport::TimeoutSourceFactory(),
            context));
        controller.reset(new TdAuthController(
            configuration(mode),
            [this](
                TdAuthController::FunctionPtr function,
                TdAuthController::ResponseCallback callback) {
                return transport->send(
                    std::move(function), std::move(callback));
            },
            observer));
    }

    void startWithConfiguration(TdAuthConfiguration config)
    {
        stop();
        observer.events.clear();
        observer.phoneHook = std::function<void(TdAuthPromptId)>();
        observer.closeHook = decltype(observer.closeHook)();
        observer.loggingOutHook = std::function<void()>();
        observer.throwOnPhone = false;
        observer.throwOnClose = false;
        transport.reset(new TdTransport(
            [this](
                std::uint64_t requestId,
                TdTransport::FunctionPtr function) {
                RecordedRequest request;
                request.id = requestId;
                request.function = std::move(function);
                requests.push_back(std::move(request));
            },
            TdTransport::UpdateCallback(),
            TdTransport::TimeoutSourceFactory(),
            context));
        controller.reset(new TdAuthController(
            std::move(config),
            [this](
                TdAuthController::FunctionPtr function,
                TdAuthController::ResponseCallback callback) {
                return transport->send(
                    std::move(function), std::move(callback));
            },
            observer));
    }

    void stop()
    {
        if (controller)
            controller->shutdown();
        controller.reset();
        if (transport)
            transport->shutdown();
        transport.reset();
        drain();
        requests.clear();
    }

    void drain()
    {
        if (!context)
            return;
        while (g_main_context_pending(context))
            g_main_context_iteration(context, FALSE);
    }

    void update(object_ptr<AuthorizationState> state)
    {
        ASSERT_NE(state, nullptr);
        controller->onAuthorizationState(state.get());
    }

    RecordedRequest takeRequest()
    {
        EXPECT_FALSE(requests.empty());
        if (requests.empty())
            return RecordedRequest();
        RecordedRequest request = std::move(requests.front());
        requests.pop_front();
        return request;
    }

    void reply(RecordedRequest request, TdTransport::ObjectPtr object)
    {
        transport->receive(request.id, std::move(object));
        drain();
    }

    GMainContext *context = nullptr;
    std::unique_ptr<TdTransport> transport;
    std::unique_ptr<TdAuthController> controller;
    RecordingAuthObserver observer;
    std::deque<RecordedRequest> requests;
};

const std::int32_t TdAuthControllerTest::TestApiId;

TEST_F(TdAuthControllerTest, HandlesEveryPinnedAuthorizationState)
{
    struct StateCase {
        const char *name;
        std::function<object_ptr<AuthorizationState>()> makeState;
        bool expectsRequest;
        ObservedEventType expectedEvent;
    };

    const std::vector<StateCase> cases = {
        {
            "parameters",
            []() {
                return authState(
                    make_object<authorizationStateWaitTdlibParameters>());
            },
            true,
            ObservedEventType::Failed,
        },
        {
            "phone",
            []() {
                return authState(
                    make_object<authorizationStateWaitPhoneNumber>());
            },
            false,
            ObservedEventType::Phone,
        },
        {
            "premium",
            []() {
                return authState(
                    make_object<authorizationStateWaitPremiumPurchase>(
                        "product", 30, "support.invalid", "subject"));
            },
            false,
            ObservedEventType::Premium,
        },
        {
            "email-address",
            []() {
                return authState(
                    make_object<authorizationStateWaitEmailAddress>(
                        true, false));
            },
            false,
            ObservedEventType::EmailAddress,
        },
        {
            "email-code",
            []() {
                return authState(
                    make_object<authorizationStateWaitEmailCode>(
                        true,
                        false,
                        make_object<emailAddressAuthenticationCodeInfo>(
                            "u***@invalid", 6),
                        nullptr));
            },
            false,
            ObservedEventType::EmailCode,
        },
        {
            "code",
            []() {
                return authState(make_object<authorizationStateWaitCode>(
                    make_object<authenticationCodeInfo>(
                        "",
                        make_object<authenticationCodeTypeTelegramMessage>(
                            5),
                        make_object<authenticationCodeTypeSms>(5),
                        60)));
            },
            false,
            ObservedEventType::Code,
        },
        {
            "other-device",
            []() {
                return authState(make_object<
                    authorizationStateWaitOtherDeviceConfirmation>(
                    "tg://login?token=test"));
            },
            false,
            ObservedEventType::QrLink,
        },
        {
            "registration",
            []() {
                return authState(
                    make_object<authorizationStateWaitRegistration>(
                        make_object<termsOfService>(
                            make_object<formattedText>(
                                "terms",
                                std::vector<object_ptr<textEntity>>()),
                            18,
                            true)));
            },
            false,
            ObservedEventType::Registration,
        },
        {
            "password",
            []() {
                return authState(
                    make_object<authorizationStateWaitPassword>(
                        "hint", true, false, "r***@invalid"));
            },
            false,
            ObservedEventType::Password,
        },
        {
            "ready",
            []() {
                return authState(make_object<authorizationStateReady>());
            },
            false,
            ObservedEventType::Ready,
        },
        {
            "logging-out",
            []() {
                return authState(make_object<authorizationStateLoggingOut>());
            },
            false,
            ObservedEventType::Failed,
        },
        {
            "closing",
            []() {
                return authState(make_object<authorizationStateClosing>());
            },
            false,
            ObservedEventType::Failed,
        },
        {
            "closed",
            []() {
                return authState(make_object<authorizationStateClosed>());
            },
            false,
            ObservedEventType::Failed,
        },
    };

    for (const StateCase &testCase: cases) {
        SCOPED_TRACE(testCase.name);
        start(TdAuthMode::PhoneNumber);
        update(testCase.makeState());
        if (testCase.expectsRequest) {
            ASSERT_EQ(requests.size(), 1u);
            EXPECT_EQ(
                requests.front().function->get_id(),
                setTdlibParameters::ID);
            EXPECT_TRUE(observer.events.empty());
        } else {
            EXPECT_TRUE(requests.empty());
            ASSERT_EQ(observer.events.size(), 1u);
            EXPECT_EQ(observer.events.front().type, testCase.expectedEvent);
        }
    }
}

TEST_F(TdAuthControllerTest, BuildsParametersFromImmutableConfiguration)
{
    update(authState(
        make_object<authorizationStateWaitTdlibParameters>()));

    ASSERT_EQ(requests.size(), 1u);
    RecordedRequest request = takeRequest();
    ASSERT_NE(request.function, nullptr);
    ASSERT_EQ(request.function->get_id(), setTdlibParameters::ID);
    const setTdlibParameters &parameters =
        static_cast<const setTdlibParameters &>(*request.function);
    EXPECT_TRUE(parameters.api_id_ == TestApiId);
    EXPECT_TRUE(
        parameters.api_hash_ ==
        "0123456789abcdef0123456789abcdef");
    EXPECT_EQ(
        parameters.database_directory_,
        "/tmp/td-auth-controller-test");
    EXPECT_TRUE(parameters.use_chat_info_database_);
    EXPECT_TRUE(parameters.use_message_database_);
    EXPECT_TRUE(parameters.use_secret_chats_);
    EXPECT_EQ(parameters.system_language_code_, "en");
    EXPECT_EQ(parameters.device_model_, "Desktop");
    EXPECT_EQ(parameters.system_version_, "Unknown");
    EXPECT_EQ(parameters.application_version_, "1.0");
}

TEST_F(TdAuthControllerTest, RejectsInvalidConfigurationWithoutSending)
{
    startWithConfiguration(TdAuthConfiguration(
        0,
        "not-a-valid-hash",
        "/tmp/td-auth-controller-test",
        true,
        TdAuthMode::PhoneNumber));

    update(authState(
        make_object<authorizationStateWaitTdlibParameters>()));

    EXPECT_TRUE(requests.empty());
    ASSERT_EQ(observer.events.size(), 1u);
    EXPECT_EQ(observer.events[0].type, ObservedEventType::Failed);
    EXPECT_EQ(
        observer.events[0].failure.type,
        TdAuthFailureType::InvalidConfiguration);
}

TEST_F(TdAuthControllerTest, CopiesChallengeMetadataWithoutOpaqueTokens)
{
    update(authState(make_object<authorizationStateWaitCode>(
        make_object<authenticationCodeInfo>(
            "",
            make_object<authenticationCodeTypeFlashCall>("pattern"),
            make_object<authenticationCodeTypeFragment>(
                "https://fragment.invalid/private-token", 7),
            90))));

    ASSERT_EQ(observer.events.size(), 1u);
    const TdAuthCodeChallenge &code = observer.events[0].code;
    EXPECT_EQ(code.code.type, TdAuthCodeType::FlashCall);
    EXPECT_EQ(code.code.pattern, "pattern");
    EXPECT_EQ(code.nextCode.type, TdAuthCodeType::Fragment);
    EXPECT_EQ(code.nextCode.length, 7);
    EXPECT_TRUE(code.nextCode.pattern.empty());
    EXPECT_EQ(code.timeout, 90);

    start(TdAuthMode::PhoneNumber);
    update(authState(make_object<authorizationStateWaitEmailCode>(
        true,
        true,
        make_object<emailAddressAuthenticationCodeInfo>(
            "u***@invalid", 8),
        make_object<emailAddressResetStatePending>(42))));
    ASSERT_EQ(observer.events.size(), 1u);
    EXPECT_TRUE(observer.events[0].emailCode.allowAppleId);
    EXPECT_TRUE(observer.events[0].emailCode.allowGoogleId);
    EXPECT_EQ(observer.events[0].emailCode.length, 8);
    EXPECT_EQ(
        observer.events[0].emailCode.resetState,
        TdAuthEmailResetState::Pending);
    EXPECT_EQ(observer.events[0].emailCode.resetSeconds, 42);
}

TEST_F(TdAuthControllerTest, DeduplicatesIdenticalStatesAndRotatesChangedPrompts)
{
    auto makeCodeState = [](std::int32_t length) {
        return authState(make_object<authorizationStateWaitCode>(
            make_object<authenticationCodeInfo>(
                "",
                make_object<authenticationCodeTypeSms>(length),
                nullptr,
                30)));
    };

    update(makeCodeState(5));
    ASSERT_EQ(observer.events.size(), 1u);
    const TdAuthPromptId firstPrompt = observer.events[0].prompt;

    update(makeCodeState(5));
    EXPECT_EQ(observer.events.size(), 1u);

    update(makeCodeState(6));
    ASSERT_EQ(observer.events.size(), 3u);
    EXPECT_EQ(observer.events[1].type, ObservedEventType::PromptClosed);
    EXPECT_EQ(
        observer.events[1].closeReason,
        TdAuthPromptCloseReason::StateChanged);
    EXPECT_EQ(observer.events[2].type, ObservedEventType::Code);
    EXPECT_NE(observer.events[2].prompt, firstPrompt);

    start(TdAuthMode::PhoneNumber);
    update(authState(
        make_object<authorizationStateWaitTdlibParameters>()));
    update(authState(
        make_object<authorizationStateWaitTdlibParameters>()));
    EXPECT_EQ(requests.size(), 1u);
}

TEST_F(
    TdAuthControllerTest,
    RepeatedStateAfterAcceptedInputIssuesFreshPrompt)
{
    auto makeCodeState = []() {
        return authState(make_object<authorizationStateWaitCode>(
            make_object<authenticationCodeInfo>(
                "",
                make_object<authenticationCodeTypeSms>(5),
                nullptr,
                30)));
    };

    update(makeCodeState());
    ASSERT_EQ(observer.events.size(), 1u);
    const TdAuthPromptId firstPrompt = observer.events[0].prompt;
    ASSERT_EQ(
        controller->submitCode(firstPrompt, "SYNTHETIC_CODE"),
        TdAuthSubmissionResult::Accepted);
    RecordedRequest request = takeRequest();
    reply(std::move(request), make_object<ok>());

    update(makeCodeState());

    ASSERT_EQ(observer.events.size(), 3u);
    EXPECT_EQ(observer.events[1].type, ObservedEventType::PromptClosed);
    EXPECT_EQ(observer.events[2].type, ObservedEventType::Code);
    EXPECT_NE(observer.events[2].prompt, firstPrompt);
}

TEST_F(TdAuthControllerTest, SubmitsEverySupportedInputWithPromptTokens)
{
    struct InputCase {
        const char *name;
        std::function<object_ptr<AuthorizationState>()> makeState;
        std::function<TdAuthSubmissionResult(
            TdAuthController &, TdAuthPromptId)> submit;
        std::int32_t expectedRequestId;
    };

    const std::string submittedValue = "SYNTHETIC_AUTH_INPUT";
    const std::vector<InputCase> cases = {
        {
            "phone",
            []() {
                return authState(
                    make_object<authorizationStateWaitPhoneNumber>());
            },
            [submittedValue](TdAuthController &auth, TdAuthPromptId prompt) {
                return auth.submitPhoneNumber(prompt, submittedValue);
            },
            setAuthenticationPhoneNumber::ID,
        },
        {
            "email-address",
            []() {
                return authState(
                    make_object<authorizationStateWaitEmailAddress>(
                        false, false));
            },
            [submittedValue](TdAuthController &auth, TdAuthPromptId prompt) {
                return auth.submitEmailAddress(prompt, submittedValue);
            },
            setAuthenticationEmailAddress::ID,
        },
        {
            "email-code",
            []() {
                return authState(
                    make_object<authorizationStateWaitEmailCode>());
            },
            [submittedValue](TdAuthController &auth, TdAuthPromptId prompt) {
                return auth.submitEmailCode(prompt, submittedValue);
            },
            checkAuthenticationEmailCode::ID,
        },
        {
            "code",
            []() {
                return authState(
                    make_object<authorizationStateWaitCode>());
            },
            [submittedValue](TdAuthController &auth, TdAuthPromptId prompt) {
                return auth.submitCode(prompt, submittedValue);
            },
            checkAuthenticationCode::ID,
        },
        {
            "password",
            []() {
                return authState(
                    make_object<authorizationStateWaitPassword>());
            },
            [submittedValue](TdAuthController &auth, TdAuthPromptId prompt) {
                return auth.submitPassword(prompt, submittedValue);
            },
            checkAuthenticationPassword::ID,
        },
        {
            "registration",
            []() {
                return authState(
                    make_object<authorizationStateWaitRegistration>());
            },
            [submittedValue](TdAuthController &auth, TdAuthPromptId prompt) {
                return auth.submitRegistration(
                    prompt, submittedValue, "Last");
            },
            registerUser::ID,
        },
    };

    for (const InputCase &testCase: cases) {
        SCOPED_TRACE(testCase.name);
        start(TdAuthMode::PhoneNumber);
        update(testCase.makeState());
        ASSERT_EQ(observer.events.size(), 1u);
        const TdAuthPromptId prompt = observer.events[0].prompt;

        EXPECT_EQ(
            testCase.submit(*controller, prompt),
            TdAuthSubmissionResult::Accepted);
        ASSERT_EQ(requests.size(), 1u);
        EXPECT_EQ(
            requests.front().function->get_id(),
            testCase.expectedRequestId);
        EXPECT_EQ(
            testCase.submit(*controller, prompt),
            TdAuthSubmissionResult::RequestInFlight);

        RecordedRequest request = takeRequest();
        switch (testCase.expectedRequestId) {
        case setAuthenticationPhoneNumber::ID:
            EXPECT_TRUE(
                static_cast<const setAuthenticationPhoneNumber &>(
                    *request.function).phone_number_ == submittedValue);
            break;
        case setAuthenticationEmailAddress::ID:
            EXPECT_TRUE(
                static_cast<const setAuthenticationEmailAddress &>(
                    *request.function).email_address_ == submittedValue);
            break;
        case checkAuthenticationEmailCode::ID: {
            const checkAuthenticationEmailCode &emailCode =
                static_cast<const checkAuthenticationEmailCode &>(
                    *request.function);
            ASSERT_NE(emailCode.code_, nullptr);
            ASSERT_EQ(
                emailCode.code_->get_id(),
                emailAddressAuthenticationCode::ID);
            EXPECT_TRUE(
                static_cast<const emailAddressAuthenticationCode &>(
                    *emailCode.code_).code_ == submittedValue);
            break;
        }
        case checkAuthenticationCode::ID:
            EXPECT_TRUE(
                static_cast<const checkAuthenticationCode &>(
                    *request.function).code_ == submittedValue);
            break;
        case checkAuthenticationPassword::ID:
            EXPECT_TRUE(
                static_cast<const checkAuthenticationPassword &>(
                    *request.function).password_ == submittedValue);
            break;
        case registerUser::ID:
            EXPECT_TRUE(
                static_cast<const registerUser &>(
                    *request.function).first_name_ == submittedValue);
            EXPECT_EQ(
                static_cast<const registerUser &>(
                    *request.function).last_name_,
                "Last");
            break;
        default:
            FAIL() << "Unexpected request type";
        }
    }
}

TEST_F(TdAuthControllerTest, RejectsWrongStaleAndEmptySubmissions)
{
    update(authState(make_object<authorizationStateWaitCode>()));
    ASSERT_EQ(observer.events.size(), 1u);
    const TdAuthPromptId codePrompt = observer.events[0].prompt;

    EXPECT_EQ(
        controller->submitPassword(codePrompt, "value"),
        TdAuthSubmissionResult::WrongPromptType);
    EXPECT_EQ(
        controller->submitCode(codePrompt, ""),
        TdAuthSubmissionResult::InvalidInput);
    EXPECT_TRUE(requests.empty());

    update(authState(make_object<authorizationStateWaitPassword>()));
    EXPECT_EQ(
        controller->submitCode(codePrompt, "value"),
        TdAuthSubmissionResult::StalePrompt);
    EXPECT_TRUE(requests.empty());
}

TEST_F(TdAuthControllerTest, RetriesRejectedUserInputWithoutExposingErrorText)
{
    static const char submittedMarker[] =
        "SYNTHETIC_SUBMITTED_SECRET_DO_NOT_PRINT";
    static const char responseMarker[] =
        "SYNTHETIC_TDLIB_ERROR_SECRET_DO_NOT_PRINT";

    update(authState(make_object<authorizationStateWaitCode>()));
    ASSERT_EQ(observer.events.size(), 1u);
    const TdAuthPromptId firstPrompt = observer.events[0].prompt;
    ASSERT_EQ(
        controller->submitCode(firstPrompt, submittedMarker),
        TdAuthSubmissionResult::Accepted);
    RecordedRequest request = takeRequest();

    reply(
        std::move(request),
        make_object<error>(400, responseMarker));

    ASSERT_EQ(observer.events.size(), 4u);
    EXPECT_EQ(observer.events[1].type, ObservedEventType::PromptClosed);
    EXPECT_EQ(
        observer.events[1].closeReason,
        TdAuthPromptCloseReason::Submitted);
    EXPECT_EQ(observer.events[2].type, ObservedEventType::RequestFailed);
    EXPECT_EQ(observer.events[2].requestFailure.errorCode, 400);
    EXPECT_EQ(observer.events[3].type, ObservedEventType::Code);
    const TdAuthPromptId retryPrompt = observer.events[3].prompt;
    EXPECT_NE(retryPrompt, firstPrompt);
    EXPECT_EQ(
        controller->submitCode(firstPrompt, "new-value"),
        TdAuthSubmissionResult::StalePrompt);
    EXPECT_TRUE(
        observer.capturedText().find(responseMarker) ==
        std::string::npos);
    EXPECT_TRUE(
        observer.capturedText().find(submittedMarker) ==
        std::string::npos);
}

TEST_F(TdAuthControllerTest, TreatsStateUpdatesAsAuthoritativeOverLateResponses)
{
    update(authState(make_object<authorizationStateWaitCode>()));
    const TdAuthPromptId codePrompt = observer.events[0].prompt;
    ASSERT_EQ(
        controller->submitCode(codePrompt, "12345"),
        TdAuthSubmissionResult::Accepted);
    RecordedRequest request = takeRequest();

    update(authState(make_object<authorizationStateWaitPassword>()));
    const std::size_t eventCount = observer.events.size();
    reply(
        std::move(request),
        make_object<error>(400, "stale response"));

    EXPECT_EQ(observer.events.size(), eventCount);
    EXPECT_TRUE(requests.empty());
}

TEST_F(TdAuthControllerTest, QrRequestIsOnceAndUpdatePrecedesAcknowledgement)
{
    start(TdAuthMode::QrCode);
    update(authState(
        make_object<authorizationStateWaitTdlibParameters>()));
    update(authState(
        make_object<authorizationStateWaitPhoneNumber>()));
    update(authState(
        make_object<authorizationStateWaitPhoneNumber>()));

    ASSERT_EQ(requests.size(), 2u);
    RecordedRequest parameters = takeRequest();
    EXPECT_EQ(parameters.function->get_id(), setTdlibParameters::ID);
    RecordedRequest qrRequest = takeRequest();
    EXPECT_EQ(qrRequest.function->get_id(), requestQrCodeAuthentication::ID);

    update(authState(make_object<
        authorizationStateWaitOtherDeviceConfirmation>(
        "tg://login?token=first")));
    ASSERT_EQ(observer.events.size(), 1u);
    EXPECT_EQ(observer.events[0].type, ObservedEventType::QrLink);
    const TdAuthPromptId qrPrompt = observer.events[0].prompt;

    reply(
        std::move(qrRequest),
        make_object<error>(400, "late QR acknowledgement"));
    EXPECT_EQ(observer.events.size(), 1u);

    update(authState(make_object<
        authorizationStateWaitOtherDeviceConfirmation>(
        "tg://login?token=first")));
    EXPECT_EQ(observer.events.size(), 1u);

    update(authState(make_object<
        authorizationStateWaitOtherDeviceConfirmation>(
        "tg://login?token=second")));
    ASSERT_EQ(observer.events.size(), 2u);
    EXPECT_EQ(observer.events[1].type, ObservedEventType::QrLink);
    EXPECT_EQ(observer.events[1].prompt, qrPrompt);
}

TEST_F(TdAuthControllerTest, QrTransitionClosesPresentationBeforeNextPrompt)
{
    start(TdAuthMode::QrCode);
    update(authState(make_object<
        authorizationStateWaitOtherDeviceConfirmation>(
        "tg://login?token=first")));
    ASSERT_EQ(observer.events.size(), 1u);
    const TdAuthPromptId qrPrompt = observer.events[0].prompt;

    update(authState(make_object<authorizationStateWaitPassword>(
        "hint", false, false, "")));

    ASSERT_EQ(observer.events.size(), 3u);
    EXPECT_EQ(observer.events[1].type, ObservedEventType::PromptClosed);
    EXPECT_EQ(observer.events[1].prompt, qrPrompt);
    EXPECT_EQ(observer.events[1].promptType, TdAuthPromptType::QrCode);
    EXPECT_EQ(
        observer.events[1].closeReason,
        TdAuthPromptCloseReason::StateChanged);
    EXPECT_EQ(observer.events[2].type, ObservedEventType::Password);
}

TEST_F(TdAuthControllerTest, AutomaticRequestErrorsFailExactlyOnce)
{
    struct AutomaticCase {
        const char *name;
        TdAuthMode mode;
        std::function<void(TdAuthControllerTest &)> begin;
        TdAuthOperation operation;
    };

    const std::vector<AutomaticCase> cases = {
        {
            "parameters",
            TdAuthMode::PhoneNumber,
            [](TdAuthControllerTest &test) {
                test.update(authState(
                    make_object<authorizationStateWaitTdlibParameters>()));
            },
            TdAuthOperation::SetTdlibParameters,
        },
        {
            "qr",
            TdAuthMode::QrCode,
            [](TdAuthControllerTest &test) {
                test.update(authState(
                    make_object<authorizationStateWaitPhoneNumber>()));
            },
            TdAuthOperation::RequestQrCode,
        },
    };

    for (const AutomaticCase &testCase: cases) {
        SCOPED_TRACE(testCase.name);
        start(testCase.mode);
        testCase.begin(*this);
        ASSERT_EQ(requests.size(), 1u);
        RecordedRequest request = takeRequest();
        reply(
            std::move(request),
            make_object<error>(400, "value-free failure expected"));

        ASSERT_FALSE(observer.events.empty());
        const ObservedEvent &failure = observer.events.back();
        EXPECT_EQ(failure.type, ObservedEventType::Failed);
        EXPECT_EQ(failure.failure.operation, testCase.operation);
        EXPECT_EQ(
            failure.failure.type,
            TdAuthFailureType::RequestRejected);
        const std::size_t eventCount = observer.events.size();
        update(authState(make_object<authorizationStateReady>()));
        EXPECT_EQ(observer.events.size(), eventCount);
    }
}

TEST_F(TdAuthControllerTest, MalformedResponsesFailWithoutHanging)
{
    controller->shutdown();
    TdAuthController::ResponseCallback nullResponseCallback;
    controller.reset(new TdAuthController(
        configuration(TdAuthMode::PhoneNumber),
        [&nullResponseCallback](
            TdAuthController::FunctionPtr,
            TdAuthController::ResponseCallback callback) {
            nullResponseCallback = std::move(callback);
            return std::uint64_t(1);
        },
        observer));
    observer.events.clear();
    update(authState(
        make_object<authorizationStateWaitTdlibParameters>()));
    ASSERT_TRUE(static_cast<bool>(nullResponseCallback));
    nullResponseCallback(1, nullptr);

    ASSERT_EQ(observer.events.size(), 1u);
    EXPECT_EQ(observer.events[0].type, ObservedEventType::Failed);
    EXPECT_EQ(
        observer.events[0].failure.type,
        TdAuthFailureType::MalformedResponse);

    start(TdAuthMode::PhoneNumber);
    update(authState(
        make_object<authorizationStateWaitTdlibParameters>()));
    RecordedRequest unexpectedResponseRequest = takeRequest();
    reply(
        std::move(unexpectedResponseRequest),
        make_object<authorizationStateReady>());

    ASSERT_EQ(observer.events.size(), 1u);
    EXPECT_EQ(observer.events[0].type, ObservedEventType::Failed);
    EXPECT_EQ(
        observer.events[0].failure.type,
        TdAuthFailureType::MalformedResponse);
}

TEST_F(TdAuthControllerTest, ReadyAndLifecycleEventsAreIdempotent)
{
    update(authState(make_object<authorizationStateWaitPassword>()));
    const TdAuthPromptId prompt = observer.events[0].prompt;
    update(authState(make_object<authorizationStateReady>()));
    update(authState(make_object<authorizationStateReady>()));

    ASSERT_EQ(observer.events.size(), 3u);
    EXPECT_EQ(observer.events[1].type, ObservedEventType::PromptClosed);
    EXPECT_EQ(
        observer.events[1].closeReason,
        TdAuthPromptCloseReason::Ready);
    EXPECT_EQ(observer.events[2].type, ObservedEventType::Ready);
    EXPECT_EQ(
        controller->submitPassword(prompt, "late"),
        TdAuthSubmissionResult::Stopped);
    EXPECT_FALSE(controller->cancel());

    update(authState(make_object<authorizationStateLoggingOut>()));
    update(authState(make_object<authorizationStateLoggingOut>()));
    update(authState(make_object<authorizationStateClosing>()));
    update(authState(make_object<authorizationStateClosing>()));
    update(authState(make_object<authorizationStateClosed>()));
    update(authState(make_object<authorizationStateClosed>()));

    ASSERT_EQ(observer.events.size(), 6u);
    EXPECT_EQ(observer.events[3].type, ObservedEventType::LoggingOut);
    EXPECT_EQ(observer.events[4].type, ObservedEventType::Closing);
    EXPECT_EQ(observer.events[5].type, ObservedEventType::Closed);
}

TEST_F(TdAuthControllerTest, CancellationAndPresentationFailureAreExactOnce)
{
    update(authState(make_object<authorizationStateWaitPassword>()));
    const TdAuthPromptId prompt = observer.events[0].prompt;
    EXPECT_EQ(
        controller->cancelPrompt(prompt),
        TdAuthSubmissionResult::Accepted);
    EXPECT_EQ(
        controller->cancelPrompt(prompt),
        TdAuthSubmissionResult::Stopped);
    update(authState(make_object<authorizationStateReady>()));

    ASSERT_EQ(observer.events.size(), 3u);
    EXPECT_EQ(observer.events[1].type, ObservedEventType::PromptClosed);
    EXPECT_EQ(
        observer.events[1].closeReason,
        TdAuthPromptCloseReason::Cancelled);
    EXPECT_EQ(observer.events[2].type, ObservedEventType::Cancelled);

    start(TdAuthMode::PhoneNumber);
    update(authState(make_object<authorizationStateWaitEmailAddress>()));
    const TdAuthPromptId emailPrompt = observer.events[0].prompt;
    EXPECT_EQ(
        controller->failPrompt(
            emailPrompt, TdAuthPresentationFailure::Unsupported),
        TdAuthSubmissionResult::Accepted);
    ASSERT_EQ(observer.events.size(), 3u);
    EXPECT_EQ(observer.events[2].type, ObservedEventType::Failed);
    EXPECT_EQ(
        observer.events[2].failure.type,
        TdAuthFailureType::PresentationUnavailable);
}

TEST_F(TdAuthControllerTest, UnknownNullAndEmptyQrStatesFailSafely)
{
    UnknownAuthorizationState unknown;
    controller->onAuthorizationState(&unknown);
    ASSERT_EQ(observer.events.size(), 1u);
    EXPECT_EQ(observer.events[0].type, ObservedEventType::Failed);
    EXPECT_EQ(
        observer.events[0].failure.type,
        TdAuthFailureType::UnsupportedState);
    EXPECT_EQ(observer.events[0].failure.rawStateId, unknown.get_id());

    start(TdAuthMode::PhoneNumber);
    controller->onAuthorizationState(nullptr);
    ASSERT_EQ(observer.events.size(), 1u);
    EXPECT_EQ(
        observer.events[0].failure.type,
        TdAuthFailureType::MalformedState);

    start(TdAuthMode::QrCode);
    update(authState(make_object<
        authorizationStateWaitOtherDeviceConfirmation>("")));
    ASSERT_EQ(observer.events.size(), 1u);
    EXPECT_EQ(
        observer.events[0].failure.type,
        TdAuthFailureType::MalformedState);
}

TEST_F(TdAuthControllerTest, ImmediateObserverSubmissionIsReentrantSafe)
{
    observer.phoneHook = [this](TdAuthPromptId prompt) {
        EXPECT_EQ(
            controller->submitPhoneNumber(prompt, "+100000"),
            TdAuthSubmissionResult::Accepted);
    };

    update(authState(
        make_object<authorizationStateWaitPhoneNumber>()));

    ASSERT_EQ(requests.size(), 1u);
    EXPECT_EQ(
        requests.front().function->get_id(),
        setAuthenticationPhoneNumber::ID);
    ASSERT_EQ(observer.events.size(), 2u);
    EXPECT_EQ(observer.events[0].type, ObservedEventType::Phone);
    EXPECT_EQ(observer.events[1].type, ObservedEventType::PromptClosed);
}

TEST_F(TdAuthControllerTest, DestructionInvalidatesPendingResponseCallbacks)
{
    update(authState(
        make_object<authorizationStateWaitTdlibParameters>()));
    RecordedRequest request = takeRequest();
    controller.reset();
    const std::size_t eventCount = observer.events.size();

    reply(std::move(request), make_object<error>(500, "late"));

    EXPECT_EQ(observer.events.size(), eventCount);
}

TEST_F(TdAuthControllerTest, SenderFailureCompletesWithoutHanging)
{
    controller->shutdown();
    controller.reset(new TdAuthController(
        configuration(TdAuthMode::PhoneNumber),
        [](TdAuthController::FunctionPtr,
           TdAuthController::ResponseCallback) {
            return std::uint64_t(0);
        },
        observer));
    observer.events.clear();

    update(authState(
        make_object<authorizationStateWaitPhoneNumber>()));
    ASSERT_EQ(observer.events.size(), 1u);
    const TdAuthPromptId prompt = observer.events[0].prompt;
    EXPECT_EQ(
        controller->submitPhoneNumber(prompt, "+100000"),
        TdAuthSubmissionResult::Accepted);

    ASSERT_EQ(observer.events.size(), 3u);
    EXPECT_EQ(observer.events[2].type, ObservedEventType::Failed);
    EXPECT_EQ(
        observer.events[2].failure.type,
        TdAuthFailureType::TransportUnavailable);
}

TEST_F(TdAuthControllerTest, ThrowingSenderIsContained)
{
    controller->shutdown();
    controller.reset(new TdAuthController(
        configuration(TdAuthMode::PhoneNumber),
        [](TdAuthController::FunctionPtr,
           TdAuthController::ResponseCallback) -> std::uint64_t {
            throw std::runtime_error("synthetic sender failure");
        },
        observer));
    observer.events.clear();

    EXPECT_NO_THROW(update(authState(
        make_object<authorizationStateWaitTdlibParameters>())));

    ASSERT_EQ(observer.events.size(), 1u);
    EXPECT_EQ(observer.events[0].type, ObservedEventType::Failed);
    EXPECT_EQ(
        observer.events[0].failure.type,
        TdAuthFailureType::TransportUnavailable);
}

TEST_F(
    TdAuthControllerTest,
    CloseCallbackInjectedStateKeepsControllerCoherent)
{
    update(authState(make_object<authorizationStateWaitCode>()));
    ASSERT_EQ(observer.events.size(), 1u);

    observer.closeHook = [this](
        TdAuthPromptId,
        TdAuthPromptType,
        TdAuthPromptCloseReason) {
        observer.closeHook = decltype(observer.closeHook)();
        object_ptr<AuthorizationState> email = authState(
            make_object<authorizationStateWaitEmailAddress>(
                false, false));
        controller->onAuthorizationState(email.get());
    };

    update(authState(make_object<authorizationStateWaitPassword>()));

    ASSERT_EQ(observer.events.size(), 3u);
    EXPECT_EQ(observer.events[1].type, ObservedEventType::PromptClosed);
    EXPECT_EQ(observer.events[2].type, ObservedEventType::EmailAddress);
    const TdAuthPromptId emailPrompt = observer.events[2].prompt;
    EXPECT_EQ(
        controller->submitEmailAddress(
            emailPrompt, "person@example.invalid"),
        TdAuthSubmissionResult::Accepted);
    RecordedRequest request = takeRequest();
    reply(
        std::move(request),
        make_object<error>(400, "SYNTHETIC_EMAIL_REJECTION"));

    ASSERT_FALSE(observer.events.empty());
    EXPECT_EQ(
        observer.events.back().type,
        ObservedEventType::EmailAddress);
    for (const ObservedEvent &event: observer.events)
        EXPECT_NE(event.type, ObservedEventType::Password);
}

TEST_F(
    TdAuthControllerTest,
    ReadyCompletionPrecedesLifecycleInjectedFromClose)
{
    update(authState(make_object<authorizationStateWaitPassword>()));
    bool cancelResult = true;
    observer.closeHook = [this, &cancelResult](
        TdAuthPromptId,
        TdAuthPromptType,
        TdAuthPromptCloseReason reason) {
        if (reason != TdAuthPromptCloseReason::Ready)
            return;
        observer.closeHook = decltype(observer.closeHook)();
        cancelResult = controller->cancel();
        object_ptr<AuthorizationState> closed =
            authState(make_object<authorizationStateClosed>());
        controller->onAuthorizationState(closed.get());
    };

    update(authState(make_object<authorizationStateReady>()));

    EXPECT_FALSE(cancelResult);
    ASSERT_EQ(observer.events.size(), 4u);
    EXPECT_EQ(observer.events[0].type, ObservedEventType::Password);
    EXPECT_EQ(observer.events[1].type, ObservedEventType::PromptClosed);
    EXPECT_EQ(observer.events[2].type, ObservedEventType::Ready);
    EXPECT_EQ(observer.events[3].type, ObservedEventType::Closed);
}

TEST_F(TdAuthControllerTest, LifecycleReentrancyPreservesArrivalOrder)
{
    update(authState(make_object<authorizationStateWaitPassword>()));
    observer.closeHook = [this](
        TdAuthPromptId,
        TdAuthPromptType,
        TdAuthPromptCloseReason reason) {
        if (reason != TdAuthPromptCloseReason::Ready)
            return;
        observer.closeHook = decltype(observer.closeHook)();
        object_ptr<AuthorizationState> loggingOut =
            authState(make_object<authorizationStateLoggingOut>());
        controller->onAuthorizationState(loggingOut.get());
        object_ptr<AuthorizationState> closing =
            authState(make_object<authorizationStateClosing>());
        controller->onAuthorizationState(closing.get());
    };
    observer.loggingOutHook = [this]() {
        observer.loggingOutHook = std::function<void()>();
        object_ptr<AuthorizationState> closed =
            authState(make_object<authorizationStateClosed>());
        controller->onAuthorizationState(closed.get());
    };

    update(authState(make_object<authorizationStateReady>()));

    ASSERT_EQ(observer.events.size(), 6u);
    EXPECT_EQ(observer.events[0].type, ObservedEventType::Password);
    EXPECT_EQ(observer.events[1].type, ObservedEventType::PromptClosed);
    EXPECT_EQ(observer.events[2].type, ObservedEventType::Ready);
    EXPECT_EQ(observer.events[3].type, ObservedEventType::LoggingOut);
    EXPECT_EQ(observer.events[4].type, ObservedEventType::Closing);
    EXPECT_EQ(observer.events[5].type, ObservedEventType::Closed);
}

TEST_F(
    TdAuthControllerTest,
    SynchronousSenderShutdownKeepsCallableAliveUntilReturn)
{
    controller->shutdown();
    controller.reset();
    transport->shutdown();
    transport.reset();
    requests.clear();
    observer.events.clear();

    std::shared_ptr<int> marker = std::make_shared<int>(1);
    std::weak_ptr<int> weakMarker(marker);
    bool markerAliveInsideSender = false;
    TdAuthController *controllerPointer = nullptr;
    controller.reset(new TdAuthController(
        configuration(TdAuthMode::PhoneNumber),
        [&, marker](
            TdAuthController::FunctionPtr,
            TdAuthController::ResponseCallback) {
            controllerPointer->shutdown();
            markerAliveInsideSender = !weakMarker.expired();
            return std::uint64_t(1);
        },
        observer));
    controllerPointer = controller.get();
    marker.reset();

    update(authState(
        make_object<authorizationStateWaitTdlibParameters>()));

    EXPECT_TRUE(markerAliveInsideSender);
    EXPECT_TRUE(weakMarker.expired());
}

TEST_F(TdAuthControllerTest, ObserverExceptionsAreContained)
{
    update(authState(make_object<authorizationStateWaitCode>()));
    observer.throwOnClose = true;

    EXPECT_NO_THROW(controller->shutdown());
    EXPECT_EQ(
        controller->submitCode(observer.events[0].prompt, "12345"),
        TdAuthSubmissionResult::Stopped);

    start(TdAuthMode::PhoneNumber);
    observer.throwOnPhone = true;
    EXPECT_NO_THROW(update(authState(
        make_object<authorizationStateWaitPhoneNumber>())));
    ASSERT_EQ(observer.events.size(), 3u);
    EXPECT_EQ(observer.events[0].type, ObservedEventType::Phone);
    EXPECT_EQ(observer.events[1].type, ObservedEventType::PromptClosed);
    EXPECT_EQ(observer.events[2].type, ObservedEventType::Failed);
    EXPECT_EQ(
        observer.events[2].failure.type,
        TdAuthFailureType::InternalError);
    EXPECT_FALSE(controller->cancel());
}

TEST_F(TdAuthControllerTest, QrWaitPhoneReentryFailsInsteadOfHanging)
{
    start(TdAuthMode::QrCode);
    update(authState(
        make_object<authorizationStateWaitPhoneNumber>()));
    update(authState(
        make_object<authorizationStateWaitPhoneNumber>()));
    ASSERT_EQ(requests.size(), 1u);

    update(authState(make_object<
        authorizationStateWaitOtherDeviceConfirmation>(
        "tg://login?token=SYNTHETIC_FIRST")));
    update(authState(
        make_object<authorizationStateWaitPhoneNumber>()));

    EXPECT_EQ(requests.size(), 1u);
    ASSERT_EQ(observer.events.size(), 3u);
    EXPECT_EQ(observer.events[0].type, ObservedEventType::QrLink);
    EXPECT_EQ(observer.events[1].type, ObservedEventType::PromptClosed);
    EXPECT_EQ(observer.events[2].type, ObservedEventType::Failed);
    EXPECT_EQ(
        observer.events[2].failure.type,
        TdAuthFailureType::UnsupportedState);
    EXPECT_EQ(
        observer.events[2].failure.operation,
        TdAuthOperation::RequestQrCode);
}

TEST_F(TdAuthControllerTest, EmptyInteractiveInputIsRedisplayed)
{
    update(authState(make_object<authorizationStateWaitCode>()));
    ASSERT_EQ(observer.events.size(), 1u);
    const TdAuthPromptId firstPrompt = observer.events[0].prompt;

    EXPECT_EQ(
        controller->submitCode(firstPrompt, ""),
        TdAuthSubmissionResult::InvalidInput);

    EXPECT_TRUE(requests.empty());
    ASSERT_EQ(observer.events.size(), 4u);
    EXPECT_EQ(observer.events[1].type, ObservedEventType::PromptClosed);
    EXPECT_EQ(observer.events[2].type, ObservedEventType::RequestFailed);
    EXPECT_EQ(observer.events[2].requestFailure.errorCode, 0);
    EXPECT_EQ(observer.events[3].type, ObservedEventType::Code);
    const TdAuthPromptId retryPrompt = observer.events[3].prompt;
    EXPECT_NE(retryPrompt, firstPrompt);
    EXPECT_EQ(
        controller->submitCode(firstPrompt, "12345"),
        TdAuthSubmissionResult::StalePrompt);
    EXPECT_EQ(
        controller->submitCode(retryPrompt, "12345"),
        TdAuthSubmissionResult::Accepted);
    ASSERT_EQ(requests.size(), 1u);
    EXPECT_EQ(
        requests[0].function->get_id(),
        checkAuthenticationCode::ID);
}

TEST_F(TdAuthControllerTest, PhoneResponseErrorRetriesWithFreshPrompt)
{
    static const char responseMarker[] =
        "SYNTHETIC_PHONE_ERROR_SECRET_DO_NOT_PRINT";
    update(authState(
        make_object<authorizationStateWaitPhoneNumber>()));
    ASSERT_EQ(observer.events.size(), 1u);
    const TdAuthPromptId firstPrompt = observer.events[0].prompt;
    EXPECT_EQ(
        controller->submitPhoneNumber(firstPrompt, "+100000"),
        TdAuthSubmissionResult::Accepted);
    RecordedRequest request = takeRequest();

    reply(
        std::move(request),
        make_object<error>(400, responseMarker));

    ASSERT_EQ(observer.events.size(), 4u);
    EXPECT_EQ(observer.events[2].type, ObservedEventType::RequestFailed);
    EXPECT_EQ(observer.events[3].type, ObservedEventType::Phone);
    const TdAuthPromptId retryPrompt = observer.events[3].prompt;
    EXPECT_NE(retryPrompt, firstPrompt);
    EXPECT_EQ(
        controller->submitPhoneNumber(retryPrompt, "+100000"),
        TdAuthSubmissionResult::Accepted);
    ASSERT_EQ(requests.size(), 1u);
    EXPECT_TRUE(
        observer.capturedText().find(responseMarker) ==
        std::string::npos);
}

} // namespace
