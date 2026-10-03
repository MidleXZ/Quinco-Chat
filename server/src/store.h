// Persistent application state for Quinco Chat: accounts, sessions, rooms
// and message history.
//
// Layout on disk (under the configured data directory):
//   users.json      - account records, rewritten atomically on change
//   rooms.json      - room records, rewritten atomically on change
//   messages.jsonl  - append-only message log, one JSON object per line
//   uploads/        - attachment payloads served back over HTTP
//
// All public methods are safe to call from any connection thread; a single
// recursive-free mutex guards the in-memory state. Message history is kept
// in memory because rooms are small and the alternative (seeking a log file
// per read) buys nothing at this scale.

#pragma once

#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "json.h"

namespace quinco {

// PBKDF2-HMAC-SHA1 parameters. The hash is stored as hex alongside a random
// per-user salt, so identical passwords never share a stored value.
constexpr uint32_t kPasswordIterations = 100000;
constexpr size_t kPasswordHashBytes = 32;
constexpr size_t kPasswordSaltBytes = 16;

struct User {
  std::string id;
  std::string username;     // lowercase login key
  std::string displayName;
  std::string tagline;
  int32_t avatarHue = 160;
  std::string passwordSalt;
  std::string passwordHash;
  int64_t createdAt = 0;
};

struct Attachment {
  std::string url;
  std::string name;
  std::string mime;
  int64_t size = 0;
  bool empty() const { return url.empty() && name.empty(); }
};

struct Message {
  std::string id;
  std::string roomId;
  std::string senderId;
  std::string body;
  Attachment attachment;
  int64_t sentAt = 0;
  bool system = false;
};

struct Room {
  std::string id;
  std::string name;
  bool direct = false;
  std::string createdBy;
  int64_t createdAt = 0;
  std::vector<std::string> members;
};

struct Session {
  std::string token;
  std::string userId;
  int64_t createdAt = 0;
  int64_t lastSeenAt = 0;
};

// Serialises the wire shapes used by the client protocol.
json::Value userToJson(const User& user);
json::Value messageToJson(const Message& message);
json::Value roomToJson(const Room& room, int64_t lastActivityMillis,
                       size_t memberCount);

class Store {
 public:
  explicit Store(std::string dataDirectory);

  // Creates the data directory tree and loads any existing state.
  bool open(std::string& error);

  // --- accounts ---------------------------------------------------------

  struct AuthOutcome {
    bool ok = false;
    User user;
    std::string error;
  };

  AuthOutcome registerUser(const std::string& username,
                           const std::string& displayName,
                           const std::string& password);
  AuthOutcome authenticate(const std::string& username,
                           const std::string& password);

  bool findUser(const std::string& userId, User& out) const;
  // Directory listing: everything except the viewer, ordered by newest first.
  std::vector<User> listUsers(const std::string& excludeId, size_t limit) const;
  std::vector<User> searchUsers(const std::string& query,
                                const std::string& excludeId,
                                size_t limit) const;
  bool updateProfile(const std::string& userId,
                     const std::string& displayName,
                     const std::string& tagline, int32_t avatarHue,
                     std::string& error);

  // --- sessions ---------------------------------------------------------

  std::string createSession(const std::string& userId);
  // Returns the owning user id, or an empty string for an unknown token.
  std::string resolveSession(const std::string& token) const;
  void revokeSession(const std::string& token);
  void touchSession(const std::string& token);

  // --- rooms ------------------------------------------------------------

  struct RoomOutcome {
    bool ok = false;
    Room room;
    std::string error;
  };

  RoomOutcome createRoom(const std::string& name, const std::string& creatorId,
                         const std::vector<std::string>& extraMembers);
  // Returns the existing one-to-one room, creating it on first use.
  RoomOutcome openDirectRoom(const std::string& firstUserId,
                             const std::string& secondUserId);
  RoomOutcome openOrCreateDirectRoomByName(const std::string& creatorId,
                                           const std::string& otherUsername);

  bool findRoom(const std::string& roomId, Room& out) const;
  std::vector<Room> roomsForUser(const std::string& userId) const;
  bool isMember(const std::string& roomId, const std::string& userId) const;
  bool joinRoom(const std::string& roomId, const std::string& userId);
  bool leaveRoom(const std::string& roomId, const std::string& userId,
                 std::string& error);
  std::vector<std::string> roomMembers(const std::string& roomId) const;
  // Every room that is more than one-to-one, for the discover list.
  std::vector<Room> publicRooms(const std::string& viewerId,
                                size_t limit) const;

  // --- messages ---------------------------------------------------------

  struct MessageOutcome {
    bool ok = false;
    Message message;
    std::string error;
  };

  MessageOutcome appendMessage(const std::string& roomId,
                               const std::string& senderId,
                               const std::string& body,
                               const Attachment& attachment,
                               bool system = false);

  // Newest `limit` messages in the room with `sentAt` strictly before
  // `beforeMillis` (0 means "from the newest"). Returned oldest-first.
  std::vector<Message> messages(const std::string& roomId,
                                int64_t beforeMillis, size_t limit) const;
  size_t messageCount(const std::string& roomId) const;
  int64_t lastActivity(const std::string& roomId) const;

  // --- files ------------------------------------------------------------

  const std::string& dataDirectory() const { return dataDirectory_; }
  std::string uploadsDirectory() const;

 private:
  // The *Locked helpers assume mutex_ is already held.
  AuthOutcome registerUserLocked(const std::string& username,
                                 const std::string& displayName,
                                 const std::string& password);
  RoomOutcome createRoomLocked(const std::string& name,
                               const std::string& creatorId,
                               const std::vector<std::string>& extraMembers);
  RoomOutcome openDirectRoomLocked(const std::string& firstUserId,
                                   const std::string& secondUserId);
  MessageOutcome appendMessageLocked(const std::string& roomId,
                                     const std::string& senderId,
                                     const std::string& body,
                                     const Attachment& attachment,
                                     bool system);

  bool loadUsers(std::string& error);
  bool loadRooms(std::string& error);
  bool loadMessages(std::string& error);

  bool persistUsers() const;
  bool persistRooms() const;

  static std::string directKey(const std::string& a, const std::string& b);

  mutable std::mutex mutex_;
  std::string dataDirectory_;

  std::map<std::string, User> usersById_;
  std::map<std::string, std::string> userIdByUsername_;
  std::map<std::string, Session> sessionsByToken_;
  std::map<std::string, Room> roomsById_;
  std::map<std::string, std::string> roomIdByDirectKey_;
  std::map<std::string, std::vector<Message>> messagesByRoom_;
};

}  // namespace quinco
