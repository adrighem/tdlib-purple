#ifndef TDLIB_PURPLE_FORUM_TOPICS_H
#define TDLIB_PURPLE_FORUM_TOPICS_H

#include "identifiers.h"

#include <memory>
#include <string>

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

class ForumTopicsAdapter {
public:
    ForumTopicsAdapter(TdTransceiver &transceiver, TdAccountData &account);
    ~ForumTopicsAdapter();

    ForumTopicsAdapter(const ForumTopicsAdapter &) = delete;
    ForumTopicsAdapter &operator=(const ForumTopicsAdapter &) = delete;

    void markRoomListsPending();
    void markRoomListsReady();
    void startRoomList(PurpleRoomlist *roomList);
    void cancelRoomList(PurpleRoomlist *roomList);
    void shutdown();
    void processUpdate(const td::td_api::updateForumTopicInfo &update);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

#endif
