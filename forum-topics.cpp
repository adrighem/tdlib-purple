#include "forum-topics.h"

#include <string>
#include <utility>

namespace {

ChatId chatIdFromValue(td::td_api::int53 value)
{
    const std::string text = std::to_string(value);
    return ChatId::fromString(text.c_str());
}

}

bool adaptForumTopicInfo(const td::td_api::forumTopicInfo &info,
                         ForumTopicMetadata &result)
{
    const ChatId chatId = chatIdFromValue(info.chat_id_);
    const ForumTopicId topicId = ForumTopicId::fromValue(info.forum_topic_id_);
    if (!chatId.valid() || !topicId.valid())
        return false;

    const bool isGeneralId = topicId == ForumTopicId::general();
    if (info.is_general_ != isGeneralId)
        return false;

    ForumTopicMetadata converted;
    converted.target = ChatTarget::forumTopic(chatId, topicId);
    converted.name = info.name_;
    converted.closed = info.is_closed_;
    converted.hidden = info.is_hidden_;
    result = std::move(converted);
    return true;
}
