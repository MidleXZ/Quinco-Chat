// Protocol implementation (see protocol.h).

#include "protocol.h"

#include <set>
#include <utility>
#include <vector>

#include "util.h"

namespace quinco {
namespace {

constexpr size_t kDirectoryLimit = 60;
constexpr size_t kHistoryPageSize = 60;
constexpr size_t kMaxHistoryPageSize = 200;
constexpr size_t kMaxRoomMembers = 250;

json::Value makeObject() { return json::Value::object(); }

json::Value stringArray(const std::set<std::string>& values) {
  json::Value out = json::Value::array();
  for (const std::string& value : values) out.push(json::Value(value));
  return out;
}

// Reads an attachment reference out of a client frame. Only locations this
// server issued are accepted, so a client cannot point a message at an
// arbitrary URL.
Attachment attachmentFromMessage(const json::Value& node, std::string& error) {
  Attachment attachment;
  if (node.isNull() || !node.isObject()) return attachment;

  const std::string url = node["url"].asString();
  if (url.empty()) return attachment;
  // Only accept locations this server handed out.
  if (url.rfind("/uploads/", 0) != 0) {
    error = "That attachment is not from this server.";
    return attachment;
  }
  attachment.url = url;
  attachment.name = node["name"].asString();
  attachment.mime = node["mime"].asString();
  attachment.size = node["size"].asInt();
  if (attachment.name.empty()) attachment.name = "attachment";
  return attachment;
}

}  // namespace

// ---------------------------------------------------------------------------
// Shared helpers
// ---------------------------------------------------------------------------

void Protocol::sendError(ClientLink* link, const std::string& message,
                         const std::string& code) {
  json::Value payload = makeObject();
  payload.set("t", json::Value("error"));
  payload.set("message", json::Value(message));
  if (!code.empty()) payload.set("code", json::Value(code));
  link->send(payload);
}

bool Protocol::requireAuth(ClientLink* link, std::string& userId) {
  userId = hub_.userIdForLink(link->linkId());
  if (userId.empty()) {
    sendError(link, "Sign in to keep going.", "unauthenticated");
    return false;
  }
  return true;
}

bool Protocol::requireRoomMember(ClientLink* link, const std::string& userId,
                                 const std::string& roomId) {
  if (roomId.empty()) {
    sendError(link, "Which room?");
    return false;
  }
  if (!store_.isMember(roomId, userId)) {
    sendError(link, "You are not part of that room.", "forbidden");
    return false;
  }
  return true;
}

json::Value Protocol::buildRoomList(const std::string& userId) const {
  const std::vector<Room> rooms = store_.roomsForUser(userId);
  json::Value list = json::Value::array();
  for (const Room& room : rooms) {
    list.push(roomToJson(room, store_.lastActivity(room.id),
                         room.members.size()));
  }
  return list;
}

void Protocol::sendRooms(ClientLink* link, const std::string& userId) {
  json::Value payload = makeObject();
  payload.set("t", json::Value("rooms"));
  payload.set("rooms", buildRoomList(userId));
  link->send(payload);
}

void Protocol::sendRoomLists(const std::string& roomId,
                             const std::vector<std::string>& extraUsers) {
  std::set<std::string> userIds;
  for (const std::string& userId : store_.roomMembers(roomId)) {
    userIds.insert(userId);
  }
  for (const std::string& userId : extraUsers) {
    if (!userId.empty()) userIds.insert(userId);
  }
  for (const std::string& userId : userIds) {
    json::Value payload = makeObject();
    payload.set("t", json::Value("rooms"));
    payload.set("rooms", buildRoomList(userId));
    hub_.sendToUser(userId, payload);
  }
}

json::Value Protocol::directoryPayload(const std::string& viewerId,
                                       const std::string& query) const {
  const std::vector<User> users =
      store_.searchUsers(query, viewerId, kDirectoryLimit);

  json::Value list = json::Value::array();
  for (const User& user : users) list.push(userToJson(user));

  json::Value publicRooms = json::Value::array();
  for (const Room& room : store_.publicRooms(viewerId, kDirectoryLimit)) {
    publicRooms.push(roomToJson(room, store_.lastActivity(room.id),
                                room.members.size()));
  }

  json::Value payload = makeObject();
  payload.set("users", std::move(list));
  payload.set("rooms", std::move(publicRooms));
  payload.set("online", stringArray(hub_.onlineUsers()));
  payload.set("query", json::Value(query));
  return payload;
}

void Protocol::broadcastPresence() {
  const std::set<std::string> online = hub_.onlineUsers();
  if (online.empty()) return;

  json::Value payload = makeObject();
  payload.set("t", json::Value("presence"));
  payload.set("online", stringArray(online));

  const std::vector<std::string> recipients(online.begin(), online.end());
  hub_.sendToUsers(recipients, payload);
}

void Protocol::sendHello(ClientLink* link) {
  json::Value payload = makeObject();
  payload.set("t", json::Value("hello"));
  payload.set("server", json::Value("Quinco Chat"));
  payload.set("version", json::Value("1.0.0"));
  payload.set("maxMessageLength", json::Value(static_cast<int64_t>(4000)));
  link->send(payload);
}

void Protocol::completeAuth(ClientLink* link, const User& user,
                            const std::string& token) {
  hub_.bindUser(link, user.id);

  json::Value payload = makeObject();
  payload.set("t", json::Value("auth_ok"));
  payload.set("token", json::Value(token));
  payload.set("user", userToJson(user));
  payload.set("rooms", buildRoomList(user.id));
  payload.set("directory", directoryPayload(user.id, std::string()));
  link->send(payload);

  // Everyone already connected needs to see the new arrival.
  broadcastPresence();
}

void Protocol::handleDisconnect(ClientLink* link) {
  (void)link;
  // The Hub has already unbound the link; re-broadcast so peers drop the
  // avatar dot.
  broadcastPresence();
}

// ---------------------------------------------------------------------------
// Authentication
// ---------------------------------------------------------------------------

void Protocol::handleRegister(ClientLink* link, const json::Value& message) {
  const Store::AuthOutcome outcome = store_.registerUser(
      message["username"].asString(), message["displayName"].asString(),
      message["password"].asString());

  if (!outcome.ok) {
    json::Value payload = makeObject();
    payload.set("t", json::Value("auth_error"));
    payload.set("message", json::Value(outcome.error));
    link->send(payload);
    return;
  }

  const std::string token = store_.createSession(outcome.user.id);
  completeAuth(link, outcome.user, token);
}

void Protocol::handleLogin(ClientLink* link, const json::Value& message) {
  const Store::AuthOutcome outcome = store_.authenticate(
      message["username"].asString(), message["password"].asString());

  if (!outcome.ok) {
    json::Value payload = makeObject();
    payload.set("t", json::Value("auth_error"));
    payload.set("message", json::Value(outcome.error));
    link->send(payload);
    return;
  }

  const std::string token = store_.createSession(outcome.user.id);
  completeAuth(link, outcome.user, token);
}

void Protocol::handleResume(ClientLink* link, const json::Value& message) {
  const std::string token = message["token"].asString();
  const std::string userId = store_.resolveSession(token);

  if (userId.empty()) {
    json::Value payload = makeObject();
    payload.set("t", json::Value("auth_error"));
    payload.set("message", json::Value("That session has expired. Sign in again."));
    link->send(payload);
    return;
  }

  User user;
  if (!store_.findUser(userId, user)) {
    store_.revokeSession(token);
    json::Value payload = makeObject();
    payload.set("t", json::Value("auth_error"));
    payload.set("message", json::Value("That account no longer exists."));
    link->send(payload);
    return;
  }

  store_.touchSession(token);
  completeAuth(link, user, token);
}

void Protocol::handleLogout(ClientLink* link, const json::Value& message) {
  const std::string token = message["token"].asString();
  if (!token.empty()) store_.revokeSession(token);

  json::Value payload = makeObject();
  payload.set("t", json::Value("logged_out"));
  link->send(payload);

  // Dropping the socket clears the binding and re-broadcasts presence.
  link->closeLink();
}

void Protocol::handleBootstrap(ClientLink* link, const json::Value&) {
  std::string userId;
  if (!requireAuth(link, userId)) return;

  User user;
  if (!store_.findUser(userId, user)) {
    sendError(link, "That account no longer exists.", "account_missing");
    return;
  }

  json::Value payload = makeObject();
  payload.set("t", json::Value("bootstrap"));
  payload.set("user", userToJson(user));
  payload.set("rooms", buildRoomList(userId));
  payload.set("directory", directoryPayload(userId, std::string()));
  link->send(payload);
}

void Protocol::handleDirectory(ClientLink* link, const json::Value& message) {
  std::string userId;
  if (!requireAuth(link, userId)) return;

  json::Value payload = directoryPayload(userId, message["query"].asString());
  payload.set("t", json::Value("directory"));
  link->send(payload);
}

void Protocol::handleProfileUpdate(ClientLink* link,
                                   const json::Value& message) {
  std::string userId;
  if (!requireAuth(link, userId)) return;

  User current;
  if (!store_.findUser(userId, current)) {
    sendError(link, "That account no longer exists.", "account_missing");
    return;
  }
  std::string error;
  const std::string displayName = message.has("displayName")
                                      ? message["displayName"].asString()
                                      : current.displayName;
  const std::string tagline = message.has("tagline")
                                  ? message["tagline"].asString()
                                  : current.tagline;
  const int64_t rawHue = message.has("avatarHue")
                             ? message["avatarHue"].asInt(current.avatarHue)
                             : current.avatarHue;
  const int32_t hue = static_cast<int32_t>(rawHue % 360);
  if (!store_.updateProfile(userId, displayName, tagline, hue, error)) {
    sendError(link, error);
    return;
  }

  User updated;
  store_.findUser(userId, updated);
  json::Value payload = makeObject();
  payload.set("t", json::Value("profile_updated"));
  payload.set("user", userToJson(updated));
  link->send(payload);
  const std::set<std::string> onlineSet = hub_.onlineUsers();
  const std::vector<std::string> online(onlineSet.begin(), onlineSet.end());
  hub_.sendToUsers(online, payload, userId);
}

void Protocol::handleRoomCreate(ClientLink* link, const json::Value& message) {
  std::string userId;
  if (!requireAuth(link, userId)) return;

  std::vector<std::string> extraMembers;
  const json::Value& memberIds = message["memberIds"];
  if (memberIds.isArray()) {
    for (const json::Value& memberId : memberIds.asArray()) {
      if (!memberId.asString().empty()) extraMembers.push_back(memberId.asString());
      if (extraMembers.size() >= kMaxRoomMembers) break;
    }
  }
  const Store::RoomOutcome outcome = store_.createRoom(
      message["name"].asString(), userId, extraMembers);
  if (!outcome.ok) {
    sendError(link, outcome.error);
    return;
  }

  json::Value payload = makeObject();
  payload.set("t", json::Value("room_created"));
  payload.set("room", roomToJson(outcome.room,
                                 store_.lastActivity(outcome.room.id),
                                 outcome.room.members.size()));
  link->send(payload);
  sendRoomLists(outcome.room.id);
}

void Protocol::handleRoomOpenDirect(ClientLink* link,
                                    const json::Value& message) {
  std::string userId;
  if (!requireAuth(link, userId)) return;

  const std::string username = message["username"].asString();
  const Store::RoomOutcome outcome =
      store_.openOrCreateDirectRoomByName(userId, username);
  if (!outcome.ok) {
    sendError(link, outcome.error);
    return;
  }

  json::Value payload = makeObject();
  payload.set("t", json::Value("room_opened"));
  payload.set("room", roomToJson(outcome.room,
                                 store_.lastActivity(outcome.room.id),
                                 outcome.room.members.size()));
  link->send(payload);
  sendRoomLists(outcome.room.id);
}

void Protocol::handleRoomJoin(ClientLink* link, const json::Value& message) {
  std::string userId;
  if (!requireAuth(link, userId)) return;
  const std::string roomId = message["roomId"].asString();
  Room room;
  if (!store_.findRoom(roomId, room) || room.direct ||
      !store_.joinRoom(roomId, userId)) {
    sendError(link, "That room cannot be joined.", "room_unavailable");
    return;
  }
  store_.findRoom(roomId, room);

  json::Value payload = makeObject();
  payload.set("t", json::Value("room_joined"));
  payload.set("room", roomToJson(room, store_.lastActivity(room.id),
                   store_.roomMembers(roomId).size()));
  link->send(payload);
  sendRoomLists(roomId);

  User user;
  if (store_.findUser(userId, user)) {
    json::Value notice = makeObject();
    notice.set("t", json::Value("member_joined"));
    notice.set("roomId", json::Value(roomId));
    notice.set("user", userToJson(user));
    hub_.sendToUsers(store_.roomMembers(roomId), notice, userId);
  }
}

void Protocol::handleRoomLeave(ClientLink* link, const json::Value& message) {
  std::string userId;
  if (!requireAuth(link, userId)) return;
  const std::string roomId = message["roomId"].asString();
  std::string error;
  if (!store_.leaveRoom(roomId, userId, error)) {
    sendError(link, error);
    return;
  }

  json::Value payload = makeObject();
  payload.set("t", json::Value("room_left"));
  payload.set("roomId", json::Value(roomId));
  link->send(payload);
  sendRoomLists(roomId, {userId});
}

void Protocol::handleHistory(ClientLink* link, const json::Value& message) {
  std::string userId;
  if (!requireAuth(link, userId)) return;
  const std::string roomId = message["roomId"].asString();
  if (!requireRoomMember(link, userId, roomId)) return;

  const int64_t before = message["before"].asInt();
  int64_t requestedLimit = message["limit"].asInt(kHistoryPageSize);
  if (requestedLimit < 1) requestedLimit = 1;
  if (requestedLimit > static_cast<int64_t>(kMaxHistoryPageSize)) {
    requestedLimit = static_cast<int64_t>(kMaxHistoryPageSize);
  }
  const std::vector<Message> messages = store_.messages(
      roomId, before, static_cast<size_t>(requestedLimit));
  json::Value list = json::Value::array();
  for (const Message& item : messages) list.push(messageToJson(item));

  json::Value payload = makeObject();
  payload.set("t", json::Value("history"));
  payload.set("roomId", json::Value(roomId));
  payload.set("messages", std::move(list));
  payload.set("hasMore", json::Value(messages.size() ==
                                      static_cast<size_t>(requestedLimit)));
  link->send(payload);
}

void Protocol::handleMessageSend(ClientLink* link,
                                 const json::Value& message) {
  std::string userId;
  if (!requireAuth(link, userId)) return;
  const std::string roomId = message["roomId"].asString();
  if (!requireRoomMember(link, userId, roomId)) return;

  std::string attachmentError;
  const Attachment attachment =
      attachmentFromMessage(message["attachment"], attachmentError);
  if (!attachmentError.empty()) {
    sendError(link, attachmentError, "invalid_attachment");
    return;
  }
  const Store::MessageOutcome outcome = store_.appendMessage(
      roomId, userId, message["body"].asString(), attachment);
  if (!outcome.ok) {
    sendError(link, outcome.error);
    return;
  }

  json::Value payload = makeObject();
  payload.set("t", json::Value("message_new"));
  payload.set("message", messageToJson(outcome.message));
  hub_.sendToUsers(store_.roomMembers(roomId), payload);
  sendRoomLists(roomId);
}

void Protocol::handleTyping(ClientLink* link, const json::Value& message) {
  std::string userId;
  if (!requireAuth(link, userId)) return;
  const std::string roomId = message["roomId"].asString();
  if (!requireRoomMember(link, userId, roomId)) return;

  json::Value payload = makeObject();
  payload.set("t", json::Value("typing"));
  payload.set("roomId", json::Value(roomId));
  payload.set("userId", json::Value(userId));
  payload.set("active", json::Value(message["active"].asBool()));
  hub_.sendToUsers(store_.roomMembers(roomId), payload, userId);
}

void Protocol::handleCallInvite(ClientLink* link, const json::Value& message) {
  std::string callerId;
  if (!requireAuth(link, callerId)) return;
  const std::string roomId = message["roomId"].asString();
  if (!requireRoomMember(link, callerId, roomId)) return;

  Room room;
  if (!store_.findRoom(roomId, room) || !room.direct) {
    sendError(link, "Calls are available in direct conversations.",
              "invalid_call_room");
    return;
  }
  std::string calleeId = message["calleeId"].asString();
  if (calleeId.empty()) {
    for (const std::string& memberId : room.members) {
      if (memberId != callerId) calleeId = memberId;
    }
  }
  if (!store_.isMember(roomId, calleeId)) {
    sendError(link, "That person is not part of this conversation.");
    return;
  }
  if (!hub_.isOnline(calleeId)) {
    sendError(link, "That person is offline.", "user_offline");
    return;
  }

  std::string callId;
  std::string error;
  const bool video = message["video"].asBool();
  if (!hub_.startCall(callerId, calleeId, roomId, video, callId, error)) {
    sendError(link, error, "call_unavailable");
    return;
  }
  User caller;
  store_.findUser(callerId, caller);
  json::Value incoming = makeObject();
  incoming.set("t", json::Value("call_incoming"));
  incoming.set("callId", json::Value(callId));
  incoming.set("roomId", json::Value(roomId));
  incoming.set("video", json::Value(video));
  incoming.set("caller", userToJson(caller));
  hub_.sendToUser(calleeId, incoming);

  json::Value ringing = makeObject();
  ringing.set("t", json::Value("call_ringing"));
  ringing.set("callId", json::Value(callId));
  ringing.set("roomId", json::Value(roomId));
  ringing.set("video", json::Value(video));
  link->send(ringing);
}

void Protocol::handleCallAccept(ClientLink* link,
                                const json::Value& message) {
  std::string userId;
  if (!requireAuth(link, userId)) return;
  const std::string callId = message["callId"].asString();
  std::string error;
  if (!hub_.acceptCall(callId, userId, error)) {
    sendError(link, error, "call_unavailable");
    return;
  }
  std::string callerId;
  std::string calleeId;
  if (!hub_.callParticipants(callId, callerId, calleeId)) return;

  json::Value payload = makeObject();
  payload.set("t", json::Value("call_accepted"));
  payload.set("callId", json::Value(callId));
  payload.set("calleeId", json::Value(userId));
  hub_.sendToUser(callerId, payload);
  link->send(payload);
}

void Protocol::handleCallDecline(ClientLink* link,
                                 const json::Value& message) {
  std::string userId;
  if (!requireAuth(link, userId)) return;
  const std::string callId = message["callId"].asString();
  std::string otherUserId;
  if (!hub_.endCall(callId, userId, otherUserId)) {
    sendError(link, "That call has already ended.", "call_unavailable");
    return;
  }
  json::Value payload = makeObject();
  payload.set("t", json::Value("call_ended"));
  payload.set("callId", json::Value(callId));
  payload.set("reason", json::Value("declined"));
  hub_.sendToUser(otherUserId, payload);
  link->send(payload);
}

void Protocol::handleCallSignal(ClientLink* link,
                                const json::Value& message) {
  std::string userId;
  if (!requireAuth(link, userId)) return;
  const std::string callId = message["callId"].asString();
  std::string callerId;
  std::string calleeId;
  if (!hub_.callParticipants(callId, callerId, calleeId) ||
      (callerId != userId && calleeId != userId)) {
    sendError(link, "That call is no longer active.", "call_unavailable");
    return;
  }
  const json::Value& signal = message["signal"];
  if (!signal.isObject() || signal.dump().size() > 64u * 1024u) {
    sendError(link, "That call signal is invalid.", "invalid_signal");
    return;
  }

  json::Value payload = makeObject();
  payload.set("t", json::Value("call_signal"));
  payload.set("callId", json::Value(callId));
  payload.set("kind", json::Value(message["kind"].asString()));
  payload.set("signal", signal);
  hub_.sendToUser(callerId == userId ? calleeId : callerId, payload);
}

void Protocol::handleCallHangup(ClientLink* link,
                                const json::Value& message) {
  std::string userId;
  if (!requireAuth(link, userId)) return;
  const std::string callId = message["callId"].asString();
  std::string otherUserId;
  if (!hub_.endCall(callId, userId, otherUserId)) {
    sendError(link, "That call has already ended.", "call_unavailable");
    return;
  }
  json::Value payload = makeObject();
  payload.set("t", json::Value("call_ended"));
  payload.set("callId", json::Value(callId));
  payload.set("reason", json::Value("hangup"));
  hub_.sendToUser(otherUserId, payload);
  link->send(payload);
}

void Protocol::handleText(ClientLink* link, const std::string& text) {
  json::Value message;
  std::string parseError;
  if (!json::Value::parse(text, message, parseError) || !message.isObject()) {
    sendError(link, "That message is not valid JSON.", "invalid_message");
    return;
  }

  const std::string type = message["t"].asString();
  if (type == "register") {
    handleRegister(link, message);
  } else if (type == "login") {
    handleLogin(link, message);
  } else if (type == "resume") {
    handleResume(link, message);
  } else if (type == "logout") {
    handleLogout(link, message);
  } else if (type == "bootstrap") {
    handleBootstrap(link, message);
  } else if (type == "directory") {
    handleDirectory(link, message);
  } else if (type == "profile_update") {
    handleProfileUpdate(link, message);
  } else if (type == "room_create") {
    handleRoomCreate(link, message);
  } else if (type == "room_open_direct") {
    handleRoomOpenDirect(link, message);
  } else if (type == "room_join") {
    handleRoomJoin(link, message);
  } else if (type == "room_leave") {
    handleRoomLeave(link, message);
  } else if (type == "history") {
    handleHistory(link, message);
  } else if (type == "message_send") {
    handleMessageSend(link, message);
  } else if (type == "typing") {
    handleTyping(link, message);
  } else if (type == "call_invite") {
    handleCallInvite(link, message);
  } else if (type == "call_accept") {
    handleCallAccept(link, message);
  } else if (type == "call_decline") {
    handleCallDecline(link, message);
  } else if (type == "call_signal") {
    handleCallSignal(link, message);
  } else if (type == "call_hangup") {
    handleCallHangup(link, message);
  } else {
    sendError(link, "That message type is not available yet.",
              "unsupported_message");
  }
}

}  // namespace quinco
