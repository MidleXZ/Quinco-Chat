#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include "../src/crypto.h"
#include "../src/hub.h"
#include "../src/protocol.h"
#include "../src/store.h"
#include "../src/util.h"

namespace {

struct TestLink final : quinco::ClientLink {
  explicit TestLink(uint64_t id) : ClientLink(id) {}

  bool send(const quinco::json::Value& payload) override {
    received.push_back(payload);
    return true;
  }

  bool sendRaw(const std::string&) override { return true; }
  void closeLink() override { closed = true; }

  std::vector<quinco::json::Value> received;
  bool closed = false;
};

int checks = 0;
int failures = 0;

void expect(bool condition, const std::string& label) {
  ++checks;
  if (condition) return;
  ++failures;
  std::cerr << "FAIL  " << label << '\n';
}

quinco::json::Value objectWith(const std::string& key,
                               const std::string& value) {
  quinco::json::Value result = quinco::json::Value::object();
  result.set(key, quinco::json::Value(value));
  return result;
}

quinco::json::Value lastMessage(const TestLink& link,
                                const std::string& type) {
  for (auto it = link.received.rbegin(); it != link.received.rend(); ++it) {
    if ((*it)["t"].asString() == type) return *it;
  }
  return quinco::json::Value();
}

void clear(TestLink& link) { link.received.clear(); }

void send(quinco::Protocol& protocol, TestLink& link,
          quinco::json::Value message) {
  protocol.handleText(&link, message.dump());
}

}  // namespace

int main() {
  const std::filesystem::path dataDirectory =
      std::filesystem::temp_directory_path() /
      ("quinco-protocol-" + quinco::crypto::randomToken(8));
  quinco::Store store(dataDirectory.string());
  std::string error;
  expect(store.open(error), "opens an isolated store");
  if (!error.empty()) std::cerr << error << '\n';

  quinco::Hub hub;
  quinco::Protocol protocol(store, hub);
  TestLink alice(1);
  TestLink blair(2);
  hub.addLink(&alice);
  hub.addLink(&blair);

  quinco::json::Value registration = quinco::json::Value::object();
  registration.set("t", quinco::json::Value("register"));
  registration.set("username", quinco::json::Value("alice_test"));
  registration.set("displayName", quinco::json::Value("Alice"));
  registration.set("password", quinco::json::Value("password-123"));
  send(protocol, alice, registration);
  const quinco::json::Value aliceAuth = lastMessage(alice, "auth_ok");
  expect(!aliceAuth.isNull(), "registers and authenticates first user");
  const std::string aliceId = aliceAuth["user"]["id"].asString();

  clear(alice);
  registration.set("username", quinco::json::Value("blair_test"));
  registration.set("displayName", quinco::json::Value("Blair"));
  send(protocol, blair, registration);
  const quinco::json::Value blairAuth = lastMessage(blair, "auth_ok");
  expect(!blairAuth.isNull(), "registers and authenticates second user");
  const std::string blairId = blairAuth["user"]["id"].asString();
  clear(alice);
  clear(blair);

  quinco::json::Value createRoom = objectWith("t", "room_create");
  createRoom.set("name", quinco::json::Value("protocol-room"));
  send(protocol, alice, createRoom);
  const quinco::json::Value roomCreated = lastMessage(alice, "room_created");
  expect(!roomCreated.isNull(), "creates a public room");
  const std::string roomId = roomCreated["room"]["id"].asString();
  clear(alice);

  quinco::json::Value joinRoom = objectWith("t", "room_join");
  joinRoom.set("roomId", quinco::json::Value(roomId));
  send(protocol, blair, joinRoom);
  expect(!lastMessage(blair, "room_joined").isNull(),
         "joins a public room");
  clear(alice);
  clear(blair);

  quinco::json::Value message = objectWith("t", "message_send");
  message.set("roomId", quinco::json::Value(roomId));
  message.set("body", quinco::json::Value("persisted hello"));
  send(protocol, alice, message);
  expect(lastMessage(blair, "message_new")["message"]["body"].asString() ==
             "persisted hello",
         "delivers a message to the other room member");
  clear(alice);
  clear(blair);

  quinco::json::Value history = objectWith("t", "history");
  history.set("roomId", quinco::json::Value(roomId));
  send(protocol, blair, history);
  const quinco::json::Value historyReply = lastMessage(blair, "history");
  expect(historyReply["messages"].size() == 1 &&
         historyReply["messages"].asArray()[0]["body"].asString() ==
                 "persisted hello",
         "loads persisted room history");
  clear(alice);
  clear(blair);

  quinco::json::Value openDirect = objectWith("t", "room_open_direct");
  openDirect.set("username", quinco::json::Value("blair_test"));
  send(protocol, alice, openDirect);
  const quinco::json::Value directReply = lastMessage(alice, "room_opened");
  expect(!directReply.isNull(), "opens a direct conversation by username");
  const std::string directRoomId = directReply["room"]["id"].asString();
  clear(alice);
  clear(blair);

  quinco::json::Value callInvite = objectWith("t", "call_invite");
  callInvite.set("roomId", quinco::json::Value(directRoomId));
  callInvite.set("calleeId", quinco::json::Value(blairId));
  callInvite.set("video", quinco::json::Value(false));
  send(protocol, alice, callInvite);
  const quinco::json::Value incoming = lastMessage(blair, "call_incoming");
  expect(!incoming.isNull(), "delivers incoming call invitation");
  const std::string callId = incoming["callId"].asString();
  clear(alice);
  clear(blair);

  quinco::json::Value accept = objectWith("t", "call_accept");
  accept.set("callId", quinco::json::Value(callId));
  send(protocol, blair, accept);
  expect(!lastMessage(alice, "call_accepted").isNull(),
         "notifies caller that the call was accepted");
  clear(alice);
  clear(blair);

  quinco::json::Value signal = objectWith("t", "call_signal");
  signal.set("callId", quinco::json::Value(callId));
  signal.set("kind", quinco::json::Value("offer"));
  signal.set("signal", objectWith("type", "offer"));
  send(protocol, alice, signal);
  const quinco::json::Value relayed = lastMessage(blair, "call_signal");
  expect(relayed["kind"].asString() == "offer" &&
             relayed["signal"]["type"].asString() == "offer",
         "relays call negotiation signals to the participant");
  clear(alice);
  clear(blair);

  quinco::json::Value hangup = objectWith("t", "call_hangup");
  hangup.set("callId", quinco::json::Value(callId));
  send(protocol, alice, hangup);
  expect(lastMessage(blair, "call_ended")["reason"].asString() == "hangup",
         "notifies the other participant when a call ends");

  TestLink anonymous(3);
  hub.addLink(&anonymous);
  send(protocol, anonymous, createRoom);
  expect(lastMessage(anonymous, "error")["code"].asString() ==
             "unauthenticated",
         "rejects room operations before authentication");

  hub.removeLink(&alice);
  hub.removeLink(&blair);
  hub.removeLink(&anonymous);
  std::error_code cleanupError;
  std::filesystem::remove_all(dataDirectory, cleanupError);

  std::cout << checks << " protocol checks, " << failures << " failures\n";
  return failures == 0 ? 0 : 1;
}