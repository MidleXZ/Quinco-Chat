// Store implementation (see store.h).

#include "store.h"

#include <algorithm>
#include <utility>

#include "crypto.h"
#include "util.h"

namespace quinco {
namespace {

constexpr size_t kMaxRoomNameLength = 48;

std::string hashPassword(const std::string& password, const std::string& salt) {
  return crypto::toHex(crypto::pbkdf2Sha1(password, salt, kPasswordIterations,
                                          kPasswordHashBytes));
}

json::Value attachmentToJson(const Attachment& attachment) {
  if (attachment.empty()) return json::Value(nullptr);
  json::Value out = json::Value::object();
  out.set("url", json::Value(attachment.url));
  out.set("name", json::Value(attachment.name));
  out.set("mime", json::Value(attachment.mime));
  out.set("size", json::Value(attachment.size));
  return out;
}

Attachment attachmentFromJson(const json::Value& value) {
  Attachment attachment;
  if (!value.isObject()) return attachment;
  attachment.url = value["url"].asString();
  attachment.name = value["name"].asString();
  attachment.mime = value["mime"].asString();
  attachment.size = value["size"].asInt();
  return attachment;
}

User userFromJson(const json::Value& value) {
  User user;
  user.id = value["id"].asString();
  user.username = value["username"].asString();
  user.displayName = value["displayName"].asString();
  user.tagline = value["tagline"].asString();
  user.avatarHue = static_cast<int32_t>(value["avatarHue"].asInt(160));
  user.passwordSalt = value["passwordSalt"].asString();
  user.passwordHash = value["passwordHash"].asString();
  user.createdAt = value["createdAt"].asInt();
  return user;
}

// The on-disk record carries the password material; the wire shape does not.
json::Value userRecordToJson(const User& user) {
  json::Value out = json::Value::object();
  out.set("id", json::Value(user.id));
  out.set("username", json::Value(user.username));
  out.set("displayName", json::Value(user.displayName));
  out.set("tagline", json::Value(user.tagline));
  out.set("avatarHue", json::Value(static_cast<int64_t>(user.avatarHue)));
  out.set("passwordSalt", json::Value(user.passwordSalt));
  out.set("passwordHash", json::Value(user.passwordHash));
  out.set("createdAt", json::Value(user.createdAt));
  return out;
}

Room roomFromJson(const json::Value& value) {
  Room room;
  room.id = value["id"].asString();
  room.name = value["name"].asString();
  room.direct = value["direct"].asBool();
  room.createdBy = value["createdBy"].asString();
  room.createdAt = value["createdAt"].asInt();
  for (const json::Value& member : value["members"].asArray()) {
    const std::string memberId = member.asString();
    if (!memberId.empty()) room.members.push_back(memberId);
  }
  return room;
}

json::Value roomRecordToJson(const Room& room) {
  json::Value out = json::Value::object();
  out.set("id", json::Value(room.id));
  out.set("name", json::Value(room.name));
  out.set("direct", json::Value(room.direct));
  out.set("createdBy", json::Value(room.createdBy));
  out.set("createdAt", json::Value(room.createdAt));
  json::Value members = json::Value::array();
  for (const std::string& memberId : room.members) {
    members.push(json::Value(memberId));
  }
  out.set("members", std::move(members));
  return out;
}

std::string cleanedRoomName(const std::string& name) {
  std::string cleaned = util::trimAscii(name);
  if (cleaned.size() > kMaxRoomNameLength) {
    cleaned.resize(kMaxRoomNameLength);
  }
  return cleaned;
}

}  // namespace

// ---------------------------------------------------------------------------
// Wire shapes
// ---------------------------------------------------------------------------

json::Value userToJson(const User& user) {
  json::Value out = json::Value::object();
  out.set("id", json::Value(user.id));
  out.set("username", json::Value(user.username));
  out.set("displayName", json::Value(user.displayName));
  out.set("tagline", json::Value(user.tagline));
  out.set("avatarHue", json::Value(static_cast<int64_t>(user.avatarHue)));
  out.set("createdAt", json::Value(user.createdAt));
  return out;
}

json::Value messageToJson(const Message& message) {
  json::Value out = json::Value::object();
  out.set("id", json::Value(message.id));
  out.set("roomId", json::Value(message.roomId));
  out.set("senderId", json::Value(message.senderId));
  out.set("body", json::Value(message.body));
  out.set("attachment", attachmentToJson(message.attachment));
  out.set("sentAt", json::Value(message.sentAt));
  out.set("sentAtIso", json::Value(util::formatIso8601(message.sentAt)));
  out.set("system", json::Value(message.system));
  return out;
}

json::Value roomToJson(const Room& room, int64_t lastActivityMillis,
                       size_t memberCount) {
  json::Value out = json::Value::object();
  out.set("id", json::Value(room.id));
  out.set("name", json::Value(room.name));
  out.set("direct", json::Value(room.direct));
  out.set("createdBy", json::Value(room.createdBy));
  out.set("createdAt", json::Value(room.createdAt));
  out.set("memberCount", json::Value(static_cast<int64_t>(memberCount)));
  out.set("lastActivity", json::Value(lastActivityMillis));
  json::Value members = json::Value::array();
  for (const std::string& memberId : room.members) {
    members.push(json::Value(memberId));
  }
  out.set("members", std::move(members));
  return out;
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

Store::Store(std::string dataDirectory)
    : dataDirectory_(std::move(dataDirectory)) {}

std::string Store::uploadsDirectory() const {
  return util::joinPath(dataDirectory_, "uploads");
}

bool Store::open(std::string& error) {
  if (!util::ensureDirectory(dataDirectory_)) {
    error = "could not create data directory: " + dataDirectory_;
    return false;
  }
  if (!util::ensureDirectory(uploadsDirectory())) {
    error = "could not create the uploads directory";
    return false;
  }
  if (!loadUsers(error)) return false;
  if (!loadRooms(error)) return false;
  if (!loadMessages(error)) return false;
  return true;
}

bool Store::loadUsers(std::string& error) {
  const std::string path = util::joinPath(dataDirectory_, "users.json");
  if (!util::pathExists(path)) return true;

  std::string text;
  if (!util::readWholeFile(path, text)) {
    error = "could not read " + path;
    return false;
  }
  json::Value root;
  std::string parseError;
  if (!json::Value::parse(text, root, parseError)) {
    error = "users.json is not valid JSON: " + parseError;
    return false;
  }

  std::lock_guard<std::mutex> lock(mutex_);
  for (const json::Value& record : root["users"].asArray()) {
    User user = userFromJson(record);
    if (user.id.empty() || user.username.empty()) continue;
    userIdByUsername_[user.username] = user.id;
    usersById_[user.id] = user;
  }
  return true;
}

bool Store::loadRooms(std::string& error) {
  const std::string path = util::joinPath(dataDirectory_, "rooms.json");
  if (!util::pathExists(path)) return true;

  std::string text;
  if (!util::readWholeFile(path, text)) {
    error = "could not read " + path;
    return false;
  }
  json::Value root;
  std::string parseError;
  if (!json::Value::parse(text, root, parseError)) {
    error = "rooms.json is not valid JSON: " + parseError;
    return false;
  }

  std::lock_guard<std::mutex> lock(mutex_);
  for (const json::Value& record : root["rooms"].asArray()) {
    Room room = roomFromJson(record);
    if (room.id.empty()) continue;
    if (room.direct && room.members.size() == 2) {
      roomIdByDirectKey_[directKey(room.members[0], room.members[1])] = room.id;
    }
    roomsById_[room.id] = room;
  }
  return true;
}

bool Store::loadMessages(std::string& error) {
  const std::string path = util::joinPath(dataDirectory_, "messages.jsonl");
  if (!util::pathExists(path)) return true;

  std::string text;
  if (!util::readWholeFile(path, text)) {
    error = "could not read " + path;
    return false;
  }

  std::lock_guard<std::mutex> lock(mutex_);
  size_t cursor = 0;
  while (cursor <= text.size()) {
    const size_t newline = text.find('\n', cursor);
    const std::string line =
        text.substr(cursor, newline == std::string::npos ? std::string::npos
                                                         : newline - cursor);
    if (!line.empty()) {
      json::Value record;
      std::string parseError;
      // A single corrupt line is skipped rather than aborting startup:
      // losing one message beats losing the whole history.
      if (json::Value::parse(line, record, parseError)) {
        Message message;
        message.id = record["id"].asString();
        message.roomId = record["roomId"].asString();
        message.senderId = record["senderId"].asString();
        message.body = record["body"].asString();
        message.attachment = attachmentFromJson(record["attachment"]);
        message.sentAt = record["sentAt"].asInt();
        message.system = record["system"].asBool();
        if (!message.id.empty() && !message.roomId.empty()) {
          messagesByRoom_[message.roomId].push_back(std::move(message));
        }
      }
    }
    if (newline == std::string::npos) break;
    cursor = newline + 1;
  }

  for (auto& entry : messagesByRoom_) {
    std::stable_sort(entry.second.begin(), entry.second.end(),
                     [](const Message& a, const Message& b) {
                       return a.sentAt < b.sentAt;
                     });
  }
  return true;
}

bool Store::persistUsers() const {
  json::Value records = json::Value::array();
  for (const auto& entry : usersById_) {
    records.push(userRecordToJson(entry.second));
  }
  json::Value root = json::Value::object();
  root.set("users", std::move(records));
  return util::writeFileAtomically(util::joinPath(dataDirectory_, "users.json"),
                                   root.dump(2) + "\n");
}

bool Store::persistRooms() const {
  json::Value records = json::Value::array();
  for (const auto& entry : roomsById_) {
    records.push(roomRecordToJson(entry.second));
  }
  json::Value root = json::Value::object();
  root.set("rooms", std::move(records));
  return util::writeFileAtomically(util::joinPath(dataDirectory_, "rooms.json"),
                                   root.dump(2) + "\n");
}

std::string Store::directKey(const std::string& a, const std::string& b) {
  return a < b ? a + "|" + b : b + "|" + a;
}
namespace {

// A pleasant, stable-ish hue for a new avatar.
uint32_t randomHue() {
  const std::string token = crypto::randomToken(2);
  uint32_t value = 0;
  for (char c : token) {
    const uint32_t digit =
        (c >= '0' && c <= '9') ? static_cast<uint32_t>(c - '0')
                               : static_cast<uint32_t>(c - 'a' + 10);
    value = value * 16 + digit;
  }
  return value % 360;
}

constexpr size_t kMaxMessageLength = 4000;
constexpr size_t kMinPasswordLength = 8;
constexpr size_t kMaxPasswordLength = 200;

}  // namespace

// ---------------------------------------------------------------------------
// Accounts
// ---------------------------------------------------------------------------

Store::AuthOutcome Store::registerUser(const std::string& username,
                                       const std::string& displayName,
                                       const std::string& password) {
  std::lock_guard<std::mutex> lock(mutex_);
  return registerUserLocked(username, displayName, password);
}

Store::AuthOutcome Store::registerUserLocked(const std::string& username,
                                             const std::string& displayName,
                                             const std::string& password) {
  AuthOutcome outcome;
  const std::string key = util::toLowerAscii(util::trimAscii(username));

  if (!util::isValidUsername(key)) {
    outcome.error =
        "Pick a username of 3-24 characters using letters, numbers, dot, "
        "dash or underscore.";
    return outcome;
  }
  const std::string trimmedDisplay = util::trimAscii(displayName);
  const std::string effectiveDisplay =
      trimmedDisplay.empty() ? key : trimmedDisplay;
  if (!util::isValidDisplayName(effectiveDisplay)) {
    outcome.error = "Your display name must be 1-48 characters.";
    return outcome;
  }
  if (password.size() < kMinPasswordLength) {
    outcome.error = "Passwords need at least 8 characters.";
    return outcome;
  }
  if (password.size() > kMaxPasswordLength) {
    outcome.error = "That password is too long.";
    return outcome;
  }
  if (userIdByUsername_.find(key) != userIdByUsername_.end()) {
    outcome.error = "That username is already taken.";
    return outcome;
  }

  User user;
  user.id = util::newId("usr");
  user.username = key;
  user.displayName = effectiveDisplay;
  user.avatarHue = static_cast<int32_t>(randomHue());
  user.passwordSalt = crypto::randomToken(kPasswordSaltBytes);
  user.passwordHash = hashPassword(password, user.passwordSalt);
  user.createdAt = util::nowMillis();

  usersById_[user.id] = user;
  userIdByUsername_[key] = user.id;
  if (!persistUsers()) {
    usersById_.erase(user.id);
    userIdByUsername_.erase(key);
    outcome.error = "Could not save the account. Is the data directory writable?";
    return outcome;
  }

  outcome.ok = true;
  outcome.user = std::move(user);
  return outcome;
}

Store::AuthOutcome Store::authenticate(const std::string& username,
                                       const std::string& password) {
  std::lock_guard<std::mutex> lock(mutex_);
  AuthOutcome outcome;

  const std::string key = util::toLowerAscii(util::trimAscii(username));
  auto indexIt = userIdByUsername_.find(key);
  if (indexIt == userIdByUsername_.end()) {
    // Same message either way so the response cannot be used to enumerate
    // which usernames exist.
    outcome.error = "That username and password do not match.";
    return outcome;
  }
  auto userIt = usersById_.find(indexIt->second);
  if (userIt == usersById_.end()) {
    outcome.error = "That username and password do not match.";
    return outcome;
  }

  const std::string candidate = hashPassword(password, userIt->second.passwordSalt);
  if (!crypto::constantTimeEquals(candidate, userIt->second.passwordHash)) {
    outcome.error = "That username and password do not match.";
    return outcome;
  }

  outcome.ok = true;
  outcome.user = userIt->second;
  return outcome;
}

bool Store::findUser(const std::string& userId, User& out) const {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = usersById_.find(userId);
  if (it == usersById_.end()) return false;
  out = it->second;
  return true;
}

std::vector<User> Store::listUsers(const std::string& excludeId,
                                   size_t limit) const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<User> result;
  for (const auto& entry : usersById_) {
    if (entry.first == excludeId) continue;
    result.push_back(entry.second);
    if (result.size() >= limit) break;
  }
  return result;
}

std::vector<User> Store::searchUsers(const std::string& query,
                                     const std::string& excludeId,
                                     size_t limit) const {
  const std::string needle = util::toLowerAscii(util::trimAscii(query));
  std::lock_guard<std::mutex> lock(mutex_);

  std::vector<User> result;
  if (needle.empty()) {
    for (const auto& entry : usersById_) {
      if (entry.first == excludeId) continue;
      result.push_back(entry.second);
      if (result.size() >= limit) break;
    }
    return result;
  }

  // Exact username matches sort before partial ones.
  std::vector<User> exact;
  std::vector<User> partial;
  for (const auto& entry : usersById_) {
    if (entry.first == excludeId) continue;
    const User& user = entry.second;
    if (user.username == needle) {
      exact.push_back(user);
      continue;
    }
    const bool matchesUsername = user.username.find(needle) != std::string::npos;
    const bool matchesDisplay =
        util::toLowerAscii(user.displayName).find(needle) != std::string::npos;
    if (matchesUsername || matchesDisplay) partial.push_back(user);
  }
  for (const User& user : exact) {
    if (result.size() >= limit) break;
    result.push_back(user);
  }
  for (const User& user : partial) {
    if (result.size() >= limit) break;
    result.push_back(user);
  }
  return result;
}

bool Store::updateProfile(const std::string& userId,
                          const std::string& displayName,
                          const std::string& tagline, int32_t avatarHue,
                          std::string& error) {
  const std::string trimmedDisplay = util::trimAscii(displayName);
  if (!util::isValidDisplayName(trimmedDisplay)) {
    error = "Your display name must be 1-48 characters.";
    return false;
  }
  std::string trimmedTagline = util::trimAscii(tagline);
  if (trimmedTagline.size() > 96) trimmedTagline.resize(96);
  int32_t hue = avatarHue % 360;
  if (hue < 0) hue += 360;

  std::lock_guard<std::mutex> lock(mutex_);
  auto it = usersById_.find(userId);
  if (it == usersById_.end()) {
    error = "Account not found.";
    return false;
  }
  const std::string previousDisplay = it->second.displayName;
  const std::string previousTagline = it->second.tagline;
  const int32_t previousHue = it->second.avatarHue;

  it->second.displayName = trimmedDisplay;
  it->second.tagline = trimmedTagline;
  it->second.avatarHue = hue;

  if (!persistUsers()) {
    it->second.displayName = previousDisplay;
    it->second.tagline = previousTagline;
    it->second.avatarHue = previousHue;
    error = "Could not save your profile.";
    return false;
  }
  return true;
}

// ---------------------------------------------------------------------------
// Sessions
// ---------------------------------------------------------------------------

std::string Store::createSession(const std::string& userId) {
  Session session;
  session.token = crypto::randomToken(24);
  session.userId = userId;
  session.createdAt = util::nowMillis();
  session.lastSeenAt = session.createdAt;

  std::lock_guard<std::mutex> lock(mutex_);
  sessionsByToken_[session.token] = session;
  return session.token;
}

std::string Store::resolveSession(const std::string& token) const {
  if (token.empty()) return std::string();
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = sessionsByToken_.find(token);
  return it == sessionsByToken_.end() ? std::string() : it->second.userId;
}

void Store::revokeSession(const std::string& token) {
  std::lock_guard<std::mutex> lock(mutex_);
  sessionsByToken_.erase(token);
}

void Store::touchSession(const std::string& token) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = sessionsByToken_.find(token);
  if (it != sessionsByToken_.end()) it->second.lastSeenAt = util::nowMillis();
}
// ---------------------------------------------------------------------------
// Rooms
// ---------------------------------------------------------------------------

Store::RoomOutcome Store::createRoom(
    const std::string& name, const std::string& creatorId,
    const std::vector<std::string>& extraMembers) {
  std::lock_guard<std::mutex> lock(mutex_);
  return createRoomLocked(name, creatorId, extraMembers);
}

Store::RoomOutcome Store::createRoomLocked(
    const std::string& name, const std::string& creatorId,
    const std::vector<std::string>& extraMembers) {
  RoomOutcome outcome;

  const std::string cleaned = cleanedRoomName(name);
  if (cleaned.empty()) {
    outcome.error = "Give the room a name first.";
    return outcome;
  }
  if (usersById_.find(creatorId) == usersById_.end()) {
    outcome.error = "Account not found.";
    return outcome;
  }

  Room room;
  room.id = util::newId("room");
  room.name = cleaned;
  room.direct = false;
  room.createdBy = creatorId;
  room.createdAt = util::nowMillis();
  room.members.push_back(creatorId);

  for (const std::string& memberId : extraMembers) {
    if (memberId.empty() || memberId == creatorId) continue;
    if (usersById_.find(memberId) == usersById_.end()) continue;
    if (std::find(room.members.begin(), room.members.end(), memberId) !=
        room.members.end()) {
      continue;
    }
    room.members.push_back(memberId);
  }

  roomsById_[room.id] = room;
  if (!persistRooms()) {
    roomsById_.erase(room.id);
    outcome.error = "Could not save the room.";
    return outcome;
  }
  outcome.ok = true;
  outcome.room = std::move(room);
  return outcome;
}

Store::RoomOutcome Store::openDirectRoom(const std::string& firstUserId,
                                         const std::string& secondUserId) {
  std::lock_guard<std::mutex> lock(mutex_);
  return openDirectRoomLocked(firstUserId, secondUserId);
}

Store::RoomOutcome Store::openDirectRoomLocked(
    const std::string& firstUserId, const std::string& secondUserId) {
  RoomOutcome outcome;

  if (firstUserId == secondUserId) {
    outcome.error = "You cannot start a conversation with yourself.";
    return outcome;
  }
  if (usersById_.find(firstUserId) == usersById_.end() ||
      usersById_.find(secondUserId) == usersById_.end()) {
    outcome.error = "That person could not be found.";
    return outcome;
  }

  const std::string key = directKey(firstUserId, secondUserId);
  auto existing = roomIdByDirectKey_.find(key);
  if (existing != roomIdByDirectKey_.end()) {
    auto roomIt = roomsById_.find(existing->second);
    if (roomIt != roomsById_.end()) {
      outcome.ok = true;
      outcome.room = roomIt->second;
      return outcome;
    }
    // Dangling index entry: fall through and rebuild it.
    roomIdByDirectKey_.erase(existing);
  }

  Room room;
  room.id = util::newId("dm");
  // Direct rooms carry no name; the client labels them with the other
  // member's display name, which differs per viewer.
  room.name.clear();
  room.direct = true;
  room.createdBy = firstUserId;
  room.createdAt = util::nowMillis();
  room.members.push_back(firstUserId);
  room.members.push_back(secondUserId);

  roomsById_[room.id] = room;
  roomIdByDirectKey_[key] = room.id;
  if (!persistRooms()) {
    roomsById_.erase(room.id);
    roomIdByDirectKey_.erase(key);
    outcome.error = "Could not open that conversation.";
    return outcome;
  }
  outcome.ok = true;
  outcome.room = std::move(room);
  return outcome;
}

Store::RoomOutcome Store::openOrCreateDirectRoomByName(
    const std::string& creatorId, const std::string& otherUsername) {
  const std::string key = util::toLowerAscii(util::trimAscii(otherUsername));

  std::lock_guard<std::mutex> lock(mutex_);
  RoomOutcome outcome;
  auto it = userIdByUsername_.find(key);
  if (it == userIdByUsername_.end()) {
    outcome.error = "Nobody here goes by that username.";
    return outcome;
  }
  return openDirectRoomLocked(creatorId, it->second);
}

bool Store::findRoom(const std::string& roomId, Room& out) const {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = roomsById_.find(roomId);
  if (it == roomsById_.end()) return false;
  out = it->second;
  return true;
}

std::vector<Room> Store::roomsForUser(const std::string& userId) const {
  std::lock_guard<std::mutex> lock(mutex_);

  std::vector<Room> result;
  for (const auto& entry : roomsById_) {
    const Room& room = entry.second;
    if (std::find(room.members.begin(), room.members.end(), userId) ==
        room.members.end()) {
      continue;
    }
    result.push_back(room);
  }

  // Most recent activity first, falling back to creation time.
  auto activityOf = [this](const Room& room) {
    auto messageIt = messagesByRoom_.find(room.id);
    if (messageIt == messagesByRoom_.end() || messageIt->second.empty()) {
      return room.createdAt;
    }
    return messageIt->second.back().sentAt;
  };
  std::stable_sort(result.begin(), result.end(),
                   [&activityOf](const Room& a, const Room& b) {
                     return activityOf(a) > activityOf(b);
                   });
  return result;
}

bool Store::isMember(const std::string& roomId,
                     const std::string& userId) const {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = roomsById_.find(roomId);
  if (it == roomsById_.end()) return false;
  return std::find(it->second.members.begin(), it->second.members.end(),
                   userId) != it->second.members.end();
}

bool Store::joinRoom(const std::string& roomId, const std::string& userId) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto roomIt = roomsById_.find(roomId);
  if (roomIt == roomsById_.end()) return false;
  if (roomIt->second.direct) return false;  // direct rooms are not joinable
  if (usersById_.find(userId) == usersById_.end()) return false;

  auto& members = roomIt->second.members;
  if (std::find(members.begin(), members.end(), userId) != members.end()) {
    return true;  // already a member
  }
  members.push_back(userId);
  if (!persistRooms()) {
    members.pop_back();
    return false;
  }
  return true;
}

bool Store::leaveRoom(const std::string& roomId, const std::string& userId,
                      std::string& error) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto roomIt = roomsById_.find(roomId);
  if (roomIt == roomsById_.end()) {
    error = "That room no longer exists.";
    return false;
  }
  if (roomIt->second.direct) {
    error = "You cannot leave a direct conversation.";
    return false;
  }

  auto& members = roomIt->second.members;
  auto memberIt = std::find(members.begin(), members.end(), userId);
  if (memberIt == members.end()) {
    error = "You are not in that room.";
    return false;
  }
  const size_t index = static_cast<size_t>(memberIt - members.begin());
  members.erase(memberIt);
  if (!persistRooms()) {
    members.insert(members.begin() + static_cast<long>(index), userId);
    error = "Could not leave the room.";
    return false;
  }
  return true;
}

std::vector<std::string> Store::roomMembers(const std::string& roomId) const {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = roomsById_.find(roomId);
  if (it == roomsById_.end()) return {};
  return it->second.members;
}

std::vector<Room> Store::publicRooms(const std::string& viewerId,
                                     size_t limit) const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<Room> result;
  for (const auto& entry : roomsById_) {
    const Room& room = entry.second;
    if (room.direct) continue;
    if (std::find(room.members.begin(), room.members.end(), viewerId) !=
        room.members.end()) {
      continue;  // already joined
    }
    result.push_back(room);
    if (result.size() >= limit) break;
  }
  return result;
}

// ---------------------------------------------------------------------------
// Messages
// ---------------------------------------------------------------------------

Store::MessageOutcome Store::appendMessage(const std::string& roomId,
                                           const std::string& senderId,
                                           const std::string& body,
                                           const Attachment& attachment,
                                           bool system) {
  std::lock_guard<std::mutex> lock(mutex_);
  return appendMessageLocked(roomId, senderId, body, attachment, system);
}

Store::MessageOutcome Store::appendMessageLocked(
    const std::string& roomId, const std::string& senderId,
    const std::string& body, const Attachment& attachment, bool system) {
  MessageOutcome outcome;

  auto roomIt = roomsById_.find(roomId);
  if (roomIt == roomsById_.end()) {
    outcome.error = "That room no longer exists.";
    return outcome;
  }
  if (std::find(roomIt->second.members.begin(), roomIt->second.members.end(),
                senderId) == roomIt->second.members.end()) {
    outcome.error = "You are not a member of that room.";
    return outcome;
  }

  // Trim the outer whitespace but keep the message's internal line breaks.
  std::string trimmed = util::trimAscii(body);
  if (trimmed.size() > kMaxMessageLength) {
    outcome.error = "That message is too long.";
    return outcome;
  }
  if (trimmed.empty() && attachment.empty()) {
    outcome.error = "Nothing to send yet.";
    return outcome;
  }
  if (!attachment.empty()) {
    if (attachment.name.size() > 160 || attachment.url.size() > 512) {
      outcome.error = "That attachment cannot be sent.";
      return outcome;
    }
  }

  Message message;
  message.id = util::newId("msg");
  message.roomId = roomId;
  message.senderId = senderId;
  message.body = std::move(trimmed);
  message.attachment = attachment;
  message.sentAt = util::nowMillis();
  message.system = system;

  messagesByRoom_[roomId].push_back(message);

  json::Value record = json::Value::object();
  record.set("id", json::Value(message.id));
  record.set("roomId", json::Value(message.roomId));
  record.set("senderId", json::Value(message.senderId));
  record.set("body", json::Value(message.body));
  record.set("attachment", attachmentToJson(message.attachment));
  record.set("sentAt", json::Value(message.sentAt));
  record.set("system", json::Value(message.system));

  const std::string log = util::joinPath(dataDirectory_, "messages.jsonl");
  if (!util::appendLine(log, record.dump())) {
    messagesByRoom_[roomId].pop_back();
    outcome.error = "Could not save the message.";
    return outcome;
  }

  outcome.ok = true;
  outcome.message = std::move(message);
  return outcome;
}

std::vector<Message> Store::messages(const std::string& roomId,
                                     int64_t beforeMillis,
                                     size_t limit) const {
  std::lock_guard<std::mutex> lock(mutex_);

  auto it = messagesByRoom_.find(roomId);
  if (it == messagesByRoom_.end()) return {};

  const std::vector<Message>& all = it->second;
  size_t end = all.size();
  if (beforeMillis > 0) {
    end = 0;
    while (end < all.size() && all[end].sentAt < beforeMillis) ++end;
  }
  const size_t start = end > limit ? end - limit : 0;

  std::vector<Message> result;
  result.reserve(end - start);
  for (size_t i = start; i < end; ++i) result.push_back(all[i]);
  return result;
}

size_t Store::messageCount(const std::string& roomId) const {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = messagesByRoom_.find(roomId);
  return it == messagesByRoom_.end() ? 0 : it->second.size();
}

int64_t Store::lastActivity(const std::string& roomId) const {
  std::lock_guard<std::mutex> lock(mutex_);
  auto messageIt = messagesByRoom_.find(roomId);
  if (messageIt != messagesByRoom_.end() && !messageIt->second.empty()) {
    return messageIt->second.back().sentAt;
  }
  auto roomIt = roomsById_.find(roomId);
  return roomIt == roomsById_.end() ? 0 : roomIt->second.createdAt;
}

}  // namespace quinco
