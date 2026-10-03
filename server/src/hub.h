// Live session registry for Quinco Chat.
//
// The hub owns everything that only exists while the server is running:
//   * which socket links are connected, and which account each is bound to
//   * which users are online (presence)
//   * fan-out of chat events to a room's online members
//   * one-to-one call sessions and the relay of SDP/ICE payloads between
//     the two participants
//
// It never touches disk; durability is the Store's job. Every public method
// is safe to call from any connection thread.
//
// Links are referenced by pointer rather than shared_ptr: a connection
// removes itself from the hub before it is destroyed, and the hub holds its
// mutex across every send, which is what makes that safe.

#pragma once

#include <cstdint>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "json.h"

namespace quinco {

// Implemented by the socket connection. All methods must be safe to call
// from any thread, and must not block for long: the hub holds its mutex
// while fanning out.
class ClientLink {
 public:
  virtual ~ClientLink() = default;

  // Queues a JSON payload as a text frame. Returns false once the link is
  // closed, which tells the hub to drop it.
  virtual bool send(const json::Value& payload) = 0;

  // Queues a pre-encoded frame (used for pong replies).
  virtual bool sendRaw(const std::string& frame) = 0;

  // Asks the connection to shut down; the connection detaches itself.
  virtual void closeLink() = 0;

  uint64_t linkId() const { return linkId_; }

 protected:
  explicit ClientLink(uint64_t linkId) : linkId_(linkId) {}

 private:
  uint64_t linkId_;
};

struct CallSession {
  std::string id;
  std::string roomId;
  std::string callerId;
  std::string calleeId;
  bool video = false;
  bool answered = false;
  int64_t startedAt = 0;
};

class Hub {
 public:
  Hub() = default;
  Hub(const Hub&) = delete;
  Hub& operator=(const Hub&) = delete;

  // --- links ------------------------------------------------------------

  // Registers a freshly connected socket. Links are anonymous until the
  // client authenticates.
  void addLink(ClientLink* link);
  // Removes a socket. Also ends any call it was part of.
  void removeLink(ClientLink* link);

  // Binds an authenticated account to a link. Idempotent.
  void bindUser(ClientLink* link, const std::string& userId);

  // --- presence ---------------------------------------------------------

  std::set<std::string> onlineUsers() const;
  bool isOnline(const std::string& userId) const;
  size_t linkCount() const;

  // --- fan-out ----------------------------------------------------------

  // Delivers to every link belonging to `userId`. Returns links reached.
  size_t sendToUser(const std::string& userId, const json::Value& payload);
  // Delivers to every link of every listed user except `skipUserId`.
  // Returns the number of links reached.
  size_t sendToUsers(const std::vector<std::string>& userIds,
                     const json::Value& payload,
                     const std::string& skipUserId = std::string());

  // --- calls ------------------------------------------------------------

  // Records a new call from `callerId` to `calleeId`, refusing when either
  // side is already in a call. On success `callId` receives the new id.
  bool startCall(const std::string& callerId, const std::string& calleeId,
                 const std::string& roomId, bool video, std::string& callId,
                 std::string& error);

  // Marks the call answered and confirms the answering user belongs to it.
  bool acceptCall(const std::string& callId, const std::string& userId,
                  std::string& error);

  // Ends a call. `userId` must be a participant. Returns false when the
  // call is unknown. On success `otherUserId` receives the peer's id.
  bool endCall(const std::string& callId, const std::string& userId,
               std::string& otherUserId);

  // Ends whatever call `userId` is currently in (used on disconnect).
  // Returns the peer's id when a call was ended.
  bool endCallForUser(const std::string& userId, std::string& otherUserId);

  // Resolves a call's participants. Returns false when unknown.
  bool callParticipants(const std::string& callId, std::string& callerId,
                        std::string& calleeId) const;

  // The call `userId` is currently in, or an empty string.
  std::string activeCallFor(const std::string& userId) const;

  // The account a socket is bound to, or an empty string when the link has
  // not authenticated yet. Lets the protocol layer stay stateless.
  std::string userIdForLink(uint64_t linkId) const;

 private:
  // Detaches a link from every index it appears in. Caller holds mutex_.
  void removeLinkLocked(ClientLink* link);
  // Drops a call and its per-user index entries. Caller holds mutex_.
  void dropCallLocked(const std::string& callId);

  mutable std::mutex mutex_;

  std::map<uint64_t, ClientLink*> linksById_;
  // Reverse index so removeLink() does not have to scan every account.
  std::map<uint64_t, std::string> userIdByLinkId_;
  std::map<std::string, std::set<uint64_t>> linkIdsByUser_;

  std::map<std::string, CallSession> callsById_;
  std::map<std::string, std::string> callIdByUser_;
};

}  // namespace quinco
