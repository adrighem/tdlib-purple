#include "libpurple-mock.h"
#include "purple-events.h"

#include <gtest/gtest.h>

TEST(LibpurpleConversationMockTest, SafelyReusesCurrentTitle)
{
    g_purpleEvents.discardEvents();

    PurpleAccount *account =
        purple_account_new("test", "prpl-telegram");
    PurpleConnection connection = {};
    connection.account = account;
    account->gc = &connection;
    PurpleConversation *conversation =
        purple_conversation_new(
            PURPLE_CONV_TYPE_CHAT, account, "chat");
    g_purpleEvents.discardEvents();

    purple_conversation_set_title(conversation, "Topic");
    g_purpleEvents.discardEvents();
    const char *currentTitle =
        purple_conversation_get_title(conversation);
    purple_conversation_set_title(conversation, currentTitle);

    EXPECT_STREQ(
        "Topic",
        purple_conversation_get_title(conversation));
    g_purpleEvents.verifyEvents(
        ConvSetTitleEvent("chat", "Topic"));

    purple_account_destroy(account);
    g_purpleEvents.verifyNoEvents();
}

TEST(LibpurpleRoomlistMockTest, PreservesListRoomAndFieldState)
{
    // CommTest fixtures discard account-cleanup events in their constructors.
    // This standalone mock test needs the same isolation from the preceding fixture.
    g_purpleEvents.discardEvents();

    PurpleAccount account = {};
    PurpleRoomlist *list = purple_roomlist_new(&account);

    ASSERT_NE(nullptr, list);
    EXPECT_EQ(&account, list->account);
    EXPECT_EQ(nullptr, list->fields);
    EXPECT_EQ(nullptr, list->rooms);
    EXPECT_FALSE(purple_roomlist_get_in_progress(list));
    EXPECT_EQ(nullptr, list->ui_data);
    EXPECT_EQ(nullptr, list->proto_data);
    EXPECT_EQ(1U, list->ref);

    GList *fields = NULL;
    fields = g_list_append(
        fields,
        purple_roomlist_field_new(PURPLE_ROOMLIST_FIELD_STRING, "Identifier", "id", TRUE));
    fields = g_list_append(
        fields,
        purple_roomlist_field_new(PURPLE_ROOMLIST_FIELD_STRING, "Description", "description", FALSE));
    fields = g_list_append(
        fields,
        purple_roomlist_field_new(PURPLE_ROOMLIST_FIELD_INT, "Order", "order", FALSE));
    fields = g_list_append(
        fields,
        purple_roomlist_field_new(PURPLE_ROOMLIST_FIELD_BOOL, "Closed", "closed", FALSE));
    purple_roomlist_set_fields(list, fields);
    purple_roomlist_set_fields(list, purple_roomlist_get_fields(list));

    EXPECT_EQ(fields, purple_roomlist_get_fields(list));
    ASSERT_EQ(4U, g_list_length(list->fields));
    const PurpleRoomlistField *idField =
        static_cast<const PurpleRoomlistField *>(list->fields->data);
    ASSERT_NE(nullptr, idField);
    EXPECT_EQ(PURPLE_ROOMLIST_FIELD_STRING, idField->type);
    EXPECT_STREQ("Identifier", idField->label);
    EXPECT_STREQ("id", idField->name);
    EXPECT_TRUE(idField->hidden);

    purple_roomlist_set_in_progress(list, TRUE);
    EXPECT_TRUE(purple_roomlist_get_in_progress(list));
    g_purpleEvents.verifyEvents(RoomlistInProgressEvent(list, true));

    PurpleRoomlistRoom *category =
        purple_roomlist_room_new(PURPLE_ROOMLIST_ROOMTYPE_CATEGORY, "Forum", NULL);
    purple_roomlist_room_add(list, category);
    g_purpleEvents.verifyEvents(RoomlistAddRoomEvent(
        list,
        PURPLE_ROOMLIST_ROOMTYPE_CATEGORY,
        "Forum",
        NULL,
        std::vector<std::string>()
    ));

    PurpleRoomlistRoom *topic =
        purple_roomlist_room_new(PURPLE_ROOMLIST_ROOMTYPE_ROOM, "Group / Topic", category);
    purple_roomlist_room_add_field(list, topic, "forum-7000-topic42");
    purple_roomlist_room_add_field(list, topic, "Topic description");
    purple_roomlist_room_add_field(list, topic, GINT_TO_POINTER(-5));
    purple_roomlist_room_add_field(list, topic, GINT_TO_POINTER(TRUE));
    purple_roomlist_room_add(list, topic);

    const std::vector<std::string> expectedValues = {
        "forum-7000-topic42",
        "Topic description",
        "-5",
        "true"
    };
    g_purpleEvents.verifyEvents(RoomlistAddRoomEvent(
        list,
        PURPLE_ROOMLIST_ROOMTYPE_ROOM,
        "Group / Topic",
        category,
        expectedValues
    ));

    EXPECT_EQ(PURPLE_ROOMLIST_ROOMTYPE_ROOM, purple_roomlist_room_get_type(topic));
    EXPECT_STREQ("Group / Topic", purple_roomlist_room_get_name(topic));
    EXPECT_EQ(category, purple_roomlist_room_get_parent(topic));
    ASSERT_EQ(4U, g_list_length(purple_roomlist_room_get_fields(topic)));
    EXPECT_STREQ(
        "forum-7000-topic42",
        static_cast<const char *>(g_list_nth_data(topic->fields, 0))
    );
    EXPECT_STREQ(
        "Topic description",
        static_cast<const char *>(g_list_nth_data(topic->fields, 1))
    );
    EXPECT_EQ(-5, GPOINTER_TO_INT(g_list_nth_data(topic->fields, 2)));
    EXPECT_TRUE(GPOINTER_TO_INT(g_list_nth_data(topic->fields, 3)));
    EXPECT_EQ(2U, g_list_length(list->rooms));

    purple_roomlist_ref(list);
    EXPECT_EQ(2U, list->ref);
    purple_roomlist_unref(list);
    EXPECT_EQ(1U, list->ref);
    EXPECT_EQ(topic, g_list_last(list->rooms)->data);

    purple_roomlist_set_in_progress(list, FALSE);
    EXPECT_FALSE(purple_roomlist_get_in_progress(list));
    g_purpleEvents.verifyEvents(RoomlistInProgressEvent(list, false));

    purple_roomlist_unref(list);
    g_purpleEvents.verifyNoEvents();
}

TEST(LibpurpleRoomlistMockTest, FullFieldVectorExpectationIsUsable)
{
    g_purpleEvents.discardEvents();

    PurpleAccount account = {};
    PurpleRoomlist *list = purple_roomlist_new(&account);
    GList *fields = NULL;
    fields = g_list_append(
        fields,
        purple_roomlist_field_new(PURPLE_ROOMLIST_FIELD_STRING, "", "id", TRUE));
    purple_roomlist_set_fields(list, fields);

    PurpleRoomlistRoom *room =
        purple_roomlist_room_new(PURPLE_ROOMLIST_ROOMTYPE_ROOM, "Topic", NULL);
    purple_roomlist_room_add_field(list, room, "forum-7000-topic42");
    purple_roomlist_room_add(list, room);

    g_purpleEvents.verifyEvents(RoomlistAddRoomEvent(
        list,
        std::vector<std::string>{"forum-7000-topic42"}
    ));

    purple_roomlist_unref(list);
    g_purpleEvents.verifyNoEvents();
}
