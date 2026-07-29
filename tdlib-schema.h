#ifndef _TDLIB_SCHEMA_H
#define _TDLIB_SCHEMA_H

#include <type_traits>
#include <td/telegram/td_api.h>

namespace TdlibSchema {

using namespace td::td_api;

static_assert(
    std::is_same<decltype(message::topic_id_), object_ptr<MessageTopic>>::value,
    "TDLib message.topic_id must use the typed MessageTopic API"
);
static_assert(
    std::is_same<decltype(messageTopicForum::forum_topic_id_), int32>::value,
    "TDLib forum topic IDs must use int32"
);
static_assert(
    std::is_same<decltype(sendMessage::topic_id_), object_ptr<MessageTopic>>::value,
    "TDLib sendMessage.topic_id must use the typed MessageTopic API"
);
static_assert(
    std::is_same<decltype(getForumTopicHistory::forum_topic_id_), int32>::value,
    "TDLib must provide typed forum-topic history"
);
static_assert(
    std::is_same<decltype(getForumTopics::offset_forum_topic_id_), int32>::value,
    "TDLib must provide typed forum-topic pagination"
);

}

#endif
