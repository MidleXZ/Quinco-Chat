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

json::Value Protocol::directoryPayload(const std::string& viewerId,
                                       const std::string& query) const {
  const std::vector<User> users =
      store_.searchUsers(query, viewerId, kDirectoryLimit);

  json::Value list = json::Value::array();
  for (const User& user : users) list.push(userToJson(user));

  json::Value payload = makeObject();
  payload.set("users", std::move(list));
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
  } else {
    sendError(link, "That message type is not available yet.",
              "unsupported_message");
  }
}

}  // namespace quinco
