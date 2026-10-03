// Client protocol for Quinco Chat.
//
// Translates decoded WebSocket text frames into Store/Hub operations and
// produces the replies. It owns no socket state: which account a link is
// bound to lives in the Hub, which keeps this layer free of per-connection
// bookkeeping and directly testable.
//
// Every handler is total: a malformed or hostile frame produces an `error`
// reply rather than an exception or a crash.

#pragma once

#include <string>

#include "hub.h"
#include "json.h"
#include "store.h"

namespace quinco {

class Protocol {
 public:
  Protocol(Store& store, Hub& hub) : store_(store), hub_(hub) {}

  // Handles one decoded text frame.
  void handleText(ClientLink* link, const std::string& text);

  // Called once a link has been removed from the Hub, so presence can be
  // re-broadcast.
  void handleDisconnect(ClientLink* link);

  // Opening frame sent the moment a socket is upgraded.
  void sendHello(ClientLink* link);

 private:
  // --- shared helpers ---------------------------------------------------

  void sendError(ClientLink* link, const std::string& message,
                 const std::string& code = std::string());
  void sendRooms(ClientLink* link, const std::string& userId);
  void broadcastPresence();

  // Fills `userId` from the link's binding. Replies and returns false when
  // the link has not authenticated.
  bool requireAuth(ClientLink* link, std::string& userId);

  // Verifies membership. Replies and returns false when the viewer is not
  // allowed in the room.
  bool requireRoomMember(ClientLink* link, const std::string& userId,
                         const std::string& roomId);

  json::Value directoryPayload(const std::string& viewerId,
                               const std::string& query) const;

  json::Value buildRoomList(const std::string& userId) const;

  // Binds the link, replies with auth_ok and the initial state, and tells
  // everyone the presence set changed.
  void completeAuth(ClientLink* link, const User& user,
                    const std::string& token);

  // --- handlers ---------------------------------------------------------

  void handleRegister(ClientLink* link, const json::Value& message);
  void handleLogin(ClientLink* link, const json::Value& message);
  void handleResume(ClientLink* link, const json::Value& message);
  void handleLogout(ClientLink* link, const json::Value& message);

  void handleBootstrap(ClientLink* link, const json::Value& message);
  void handleDirectory(ClientLink* link, const json::Value& message);
  void handleProfileUpdate(ClientLink* link, const json::Value& message);

  void handleRoomCreate(ClientLink* link, const json::Value& message);
  void handleRoomOpenDirect(ClientLink* link, const json::Value& message);
  void handleRoomJoin(ClientLink* link, const json::Value& message);
  void handleRoomLeave(ClientLink* link, const json::Value& message);
  void handleHistory(ClientLink* link, const json::Value& message);
  void handleMessageSend(ClientLink* link, const json::Value& message);
  void handleTyping(ClientLink* link, const json::Value& message);

  void handleCallInvite(ClientLink* link, const json::Value& message);
  void handleCallAccept(ClientLink* link, const json::Value& message);
  void handleCallDecline(ClientLink* link, const json::Value& message);
  void handleCallSignal(ClientLink* link, const json::Value& message);
  void handleCallHangup(ClientLink* link, const json::Value& message);

  Store& store_;
  Hub& hub_;
};

}  // namespace quinco
