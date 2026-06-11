#include "printout.h"
#include <td/telegram/td_api.h>

using namespace td::td_api;

std::string requestToString(const td::TlObject &req)
{
#define C(class) case class::ID: return #class;
    switch (req.get_id()) {
        C(getContacts)
        C(loadChats)
        C(sendMessage)
        C(viewMessages)
        C(createPrivateChat)
        C(getBasicGroupFullInfo)
        C(getSupergroupFullInfo)
        C(getSupergroupMembers)
        C(getChatHistory)
        C(getMessage)
        C(downloadFile)
        C(preliminaryUploadFile)
        C(setTdlibParameters)
        C(checkAuthenticationCode)
        C(checkAuthenticationPassword)
        C(setAuthenticationPhoneNumber)
        C(registerUser)
        C(addContact)
        C(changeImportedContacts)
        C(searchPublicChat)
        C(sendChatAction)
        C(deleteChatHistory)
        C(closeSecretChat)
        C(createNewSecretChat)
    }
#undef C
    return "Id " + std::to_string(req.get_id());
}

std::string responseToString(const td::TlObject &object)
{
#define C(class) case class::ID: return #class;
    switch (object.get_id()) {
        C(ok)
        C(error)
        C(user)
        C(users)
        C(chat)
        C(chats)
        C(message)
        C(messages)
        C(basicGroupFullInfo)
        C(supergroupFullInfo)
        C(chatMembers)
        C(file)
        C(addedProxy)
        C(addedProxies)
        C(updateAuthorizationState)
        C(updateConnectionState)
        C(updateNewChat)
        C(updateNewMessage)
        C(updateUser)
        C(updateUserStatus)
        C(updateChatLastMessage)
        C(updateChatTitle)
        C(updateChatPosition)
        C(updateFile)
        C(updateOption)
        C(updateSecretChat)
        C(updateSupergroup)
        C(updateSupergroupFullInfo)
        C(updateBasicGroup)
        C(updateBasicGroupFullInfo)
        C(authorizationStateWaitTdlibParameters)
        C(authorizationStateWaitPhoneNumber)
        C(authorizationStateWaitCode)
        C(authorizationStateWaitPassword)
        C(authorizationStateReady)
        C(connectionStateConnecting)
        C(connectionStateUpdating)
        C(connectionStateReady)
    }
#undef C
    return "Id " + std::to_string(object.get_id());
}
