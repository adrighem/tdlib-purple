#ifndef TDLIB_PURPLE_FORUM_TOPICS_H
#define TDLIB_PURPLE_FORUM_TOPICS_H

#include "identifiers.h"

#include <string>

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

#endif
