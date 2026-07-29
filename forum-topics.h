#ifndef TDLIB_PURPLE_FORUM_TOPICS_H
#define TDLIB_PURPLE_FORUM_TOPICS_H

#include "identifiers.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>

class TdAccountData;
class TdTransceiver;
typedef struct _PurpleRoomlist PurpleRoomlist;

struct ForumTopicMetadata {
    ForumTopicMetadata()
        : closed(false),
          hidden(false)
    {}

    ChatTarget target;
    std::string name;
    bool closed;
    bool hidden;
};

// Returns false for invalid identifiers or an inconsistent General topic
// marker. The result is left unchanged when conversion fails.
bool adaptForumTopicInfo(const td::td_api::forumTopicInfo &info,
                         ForumTopicMetadata &result);

enum class ForumTopicLookupStatus : uint8_t {
    Available,
    InvalidTarget,
    ParentUnavailable,
    ParentIneligible,
    Timeout,
    TdlibError,
    InvalidResponse,
    Superseded,
};

struct ForumTopicLookupResult {
    ChatTarget target;
    ForumTopicLookupStatus status;
    int32_t tdlibErrorCode;

    ForumTopicLookupResult(
        ChatTarget target, ForumTopicLookupStatus status,
        int32_t tdlibErrorCode = 0)
        : target(target),
          status(status),
          tdlibErrorCode(tdlibErrorCode)
    {}
};

using ForumTopicLookupCallback =
    std::function<void(const ForumTopicLookupResult &)>;
using ForumTopicChangedCallback =
    std::function<void(ChatTarget)>;

class ForumTopicsAdapter {
public:
    ForumTopicsAdapter(
        TdTransceiver &transceiver, TdAccountData &account,
        ForumTopicChangedCallback topicChanged = ForumTopicChangedCallback());
    ~ForumTopicsAdapter();

    ForumTopicsAdapter(const ForumTopicsAdapter &) = delete;
    ForumTopicsAdapter &operator=(const ForumTopicsAdapter &) = delete;

    void markRoomListsPending();
    void markRoomListsReady();
    void startRoomList(PurpleRoomlist *roomList);
    void cancelRoomList(PurpleRoomlist *roomList);
    void shutdown();
    void processUpdate(const td::td_api::updateForumTopicInfo &update);
    void resolveForumTopic(
        ChatTarget target, ForumTopicLookupCallback callback);
    void cancelForumTopicLookup(ChatTarget target);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

#endif
