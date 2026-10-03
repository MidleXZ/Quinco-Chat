// Hub implementation (see hub.h).

#include "hub.h"

#include <utility>

#include "util.h"

namespace quinco {

// ---------------------------------------------------------------------------
// Links
// ---------------------------------------------------------------------------

void Hub::addLink(ClientLink* link) {
  if (!link) return;
  std::lock_guard<std::mutex> lock(mutex_);
  linksById_[link->linkId()] = link;
}

void Hub::removeLinkLocked(ClientLink* link) {
  const uint64_t linkId = link->linkId();

  auto reverseIt = userIdByLinkId_.find(linkId);
  if (reverseIt != userIdByLinkId_.end()) {
    auto accountIt = linkIdsByUser_.find(reverseIt->second);
    if (accountIt != linkIdsByUser_.end()) {
      accountIt->second.erase(linkId);
      if (accountIt->second.empty()) linkIdsByUser_.erase(accountIt);
    }
    userIdByLinkId_.erase(reverseIt);
  }
  linksById_.erase(linkId);
}

void Hub::removeLink(ClientLink* link) {
  if (!link) return;

  std::string peerUserId;
  std::string endedCallId;

  {
    std::lock_guard<std::mutex> lock(mutex_);
    const uint64_t linkId = link->linkId();
    auto reverseIt = userIdByLinkId_.find(linkId);
    const std::string userId =
        reverseIt == userIdByLinkId_.end() ? std::string() : reverseIt->second;

    removeLinkLocked(link);

    if (userId.empty()) return;

    // Still connected from another tab: presence, and any call, stay up.
    auto remaining = linkIdsByUser_.find(userId);
    if (remaining != linkIdsByUser_.end() && !remaining->second.empty()) return;

    auto callIt = callIdByUser_.find(userId);
    if (callIt == callIdByUser_.end()) return;

    auto sessionIt = callsById_.find(callIt->second);
    if (sessionIt != callsById_.end()) {
      const CallSession& session = sessionIt->second;
      peerUserId = session.callerId == userId ? session.calleeId : session.callerId;
      endedCallId = session.id;
    }
    dropCallLocked(callIt->second);
  }

  if (!peerUserId.empty()) {
    // Tell the surviving participant outside the lock: the notice is a
    // normal fan-out and needs the mutex itself.
    json::Value notice = json::Value::object();
    notice.set("t", json::Value("call_ended"));
    notice.set("callId", json::Value(endedCallId));
    notice.set("reason", json::Value("peer_left"));
    sendToUser(peerUserId, notice);
  }
}

void Hub::bindUser(ClientLink* link, const std::string& userId) {
  if (!link || userId.empty()) return;

  std::lock_guard<std::mutex> lock(mutex_);
  const uint64_t linkId = link->linkId();
  if (linksById_.find(linkId) == linksById_.end()) return;  // unknown link

  auto reverseIt = userIdByLinkId_.find(linkId);
  if (reverseIt != userIdByLinkId_.end()) {
    if (reverseIt->second == userId) return;  // already bound
    auto previousIt = linkIdsByUser_.find(reverseIt->second);
    if (previousIt != linkIdsByUser_.end()) {
      previousIt->second.erase(linkId);
      if (previousIt->second.empty()) linkIdsByUser_.erase(previousIt);
    }
  }

  userIdByLinkId_[linkId] = userId;
  linkIdsByUser_[userId].insert(linkId);
}

// ---------------------------------------------------------------------------
// Presence
// ---------------------------------------------------------------------------

std::set<std::string> Hub::onlineUsers() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::set<std::string> result;
  for (const auto& entry : linkIdsByUser_) {
    if (!entry.second.empty()) result.insert(entry.first);
  }
  return result;
}

bool Hub::isOnline(const std::string& userId) const {
  if (userId.empty()) return false;
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = linkIdsByUser_.find(userId);
  return it != linkIdsByUser_.end() && !it->second.empty();
}

size_t Hub::linkCount() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return linksById_.size();
}

// ---------------------------------------------------------------------------
// Fan-out
// ---------------------------------------------------------------------------

size_t Hub::sendToUser(const std::string& userId, const json::Value& payload) {
  if (userId.empty()) return 0;

  std::lock_guard<std::mutex> lock(mutex_);
  auto accountIt = linkIdsByUser_.find(userId);
  if (accountIt == linkIdsByUser_.end()) return 0;

  size_t reached = 0;
  std::vector<uint64_t> dead;
  for (uint64_t linkId : accountIt->second) {
    auto linkIt = linksById_.find(linkId);
    if (linkIt == linksById_.end() || linkIt->second == nullptr) {
      dead.push_back(linkId);
      continue;
    }
    if (linkIt->second->send(payload)) {
      ++reached;
    } else {
      // The link reported itself closed; drop it now rather than waiting
      // for the read loop to notice.
      dead.push_back(linkId);
    }
  }

  for (uint64_t linkId : dead) {
    linksById_.erase(linkId);
    userIdByLinkId_.erase(linkId);
    accountIt->second.erase(linkId);
  }
  if (accountIt->second.empty()) linkIdsByUser_.erase(accountIt);
  return reached;
}

size_t Hub::sendToUsers(const std::vector<std::string>& userIds,
                        const json::Value& payload,
                        const std::string& skipUserId) {
  std::set<std::string> delivered;
  size_t reached = 0;
  for (const std::string& userId : userIds) {
    if (userId.empty()) continue;
    if (!skipUserId.empty() && userId == skipUserId) continue;
    if (!delivered.insert(userId).second) continue;  // de-duplicate
    reached += sendToUser(userId, payload);
  }
  return reached;
}

// ---------------------------------------------------------------------------
// Calls
// ---------------------------------------------------------------------------

void Hub::dropCallLocked(const std::string& callId) {
  auto it = callsById_.find(callId);
  if (it == callsById_.end()) return;
  callIdByUser_.erase(it->second.callerId);
  callIdByUser_.erase(it->second.calleeId);
  callsById_.erase(it);
}

bool Hub::startCall(const std::string& callerId, const std::string& calleeId,
                    const std::string& roomId, bool video, std::string& callId,
                    std::string& error) {
  callId.clear();
  if (callerId.empty() || calleeId.empty() || callerId == calleeId) {
    error = "That is not someone you can call.";
    return false;
  }

  std::lock_guard<std::mutex> lock(mutex_);
  if (callIdByUser_.find(callerId) != callIdByUser_.end()) {
    error = "You are already on a call.";
    return false;
  }
  if (callIdByUser_.find(calleeId) != callIdByUser_.end()) {
    error = "They are already on another call.";
    return false;
  }

  CallSession session;
  session.id = util::newId("call");
  session.roomId = roomId;
  session.callerId = callerId;
  session.calleeId = calleeId;
  session.video = video;
  session.answered = false;
  session.startedAt = util::nowMillis();

  callsById_[session.id] = session;
  callIdByUser_[callerId] = session.id;
  callIdByUser_[calleeId] = session.id;
  callId = session.id;
  return true;
}

bool Hub::acceptCall(const std::string& callId, const std::string& userId,
                     std::string& error) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = callsById_.find(callId);
  if (it == callsById_.end()) {
    error = "That call is no longer ringing.";
    return false;
  }
  if (it->second.calleeId != userId) {
    error = "That call is not yours to answer.";
    return false;
  }
  it->second.answered = true;
  return true;
}

bool Hub::endCall(const std::string& callId, const std::string& userId,
                  std::string& otherUserId) {
  otherUserId.clear();
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = callsById_.find(callId);
  if (it == callsById_.end()) return false;
  if (it->second.callerId != userId && it->second.calleeId != userId) {
    return false;
  }
  otherUserId = it->second.callerId == userId ? it->second.calleeId
                                              : it->second.callerId;
  dropCallLocked(callId);
  return true;
}

bool Hub::endCallForUser(const std::string& userId, std::string& otherUserId) {
  otherUserId.clear();
  if (userId.empty()) return false;

  std::lock_guard<std::mutex> lock(mutex_);
  auto callIt = callIdByUser_.find(userId);
  if (callIt == callIdByUser_.end()) return false;

  auto sessionIt = callsById_.find(callIt->second);
  if (sessionIt == callsById_.end()) {
    callIdByUser_.erase(callIt);
    return false;
  }
  otherUserId = sessionIt->second.callerId == userId ? sessionIt->second.calleeId
                                                     : sessionIt->second.callerId;
  dropCallLocked(sessionIt->second.id);
  return true;
}

bool Hub::callParticipants(const std::string& callId, std::string& callerId,
                           std::string& calleeId) const {
  callerId.clear();
  calleeId.clear();
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = callsById_.find(callId);
  if (it == callsById_.end()) return false;
  callerId = it->second.callerId;
  calleeId = it->second.calleeId;
  return true;
}

std::string Hub::activeCallFor(const std::string& userId) const {
  if (userId.empty()) return std::string();
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = callIdByUser_.find(userId);
  return it == callIdByUser_.end() ? std::string() : it->second;
}

std::string Hub::userIdForLink(uint64_t linkId) const {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = userIdByLinkId_.find(linkId);
  return it == userIdByLinkId_.end() ? std::string() : it->second;
}

}  // namespace quinco
