// RFC 6455 WebSocket framing for Quinco Chat.
//
// Implements the server half of the protocol:
//   * the handshake accept-key derivation (SHA-1 + base64)
//   * server->client frame encoding (never masked)
//   * an incremental parser for client->server frames that unmasked
//     payloads, enforces the control-frame rules, reassembles fragmented
//     messages, and rejects anything oversized.
//
// The parser is deliberately streaming: a frame may arrive split across any
// number of TCP reads, so callers push raw bytes in and pull whole messages
// out whenever one is complete.

#pragma once

#include <cstdint>
#include <string>

namespace quinco {
namespace ws {

enum class Opcode : uint8_t {
  Continuation = 0x0,
  Text = 0x1,
  Binary = 0x2,
  Close = 0x8,
  Ping = 0x9,
  Pong = 0xA,
};

// Largest payload the server will accept or emit in a single message.
// Chat traffic is text plus attachment URLs, so this is generous.
constexpr size_t kMaxMessageBytes = 256u * 1024u;

// Control frames may carry at most 125 bytes (RFC 6455 §5.5).
constexpr size_t kMaxControlPayloadBytes = 125;

// Portions of a close frame.
constexpr uint16_t kCloseNormal = 1000;
constexpr uint16_t kCloseGoingAway = 1001;
constexpr uint16_t kCloseProtocolError = 1002;
constexpr uint16_t kCloseUnsupportedData = 1003;
constexpr uint16_t kCloseMessageTooBig = 1009;

struct Frame {
  Opcode opcode = Opcode::Text;
  bool fin = true;
  std::string payload;
};

// Computes the value of the Sec-WebSocket-Accept response header from the
// client's Sec-WebSocket-Key. Returns an empty string when `clientKey` is
// not a valid 16-byte base64 nonce.
std::string acceptKey(const std::string& clientKey);

// Builds a server frame. Server frames are never masked.
std::string encodeFrame(Opcode opcode, const std::string& payload,
                        bool fin = true);

// Convenience wrappers used by the connection layer.
std::string encodeText(const std::string& text);
std::string encodePing(const std::string& payload = std::string());
std::string encodeClose(uint16_t code, const std::string& reason);

// Incremental frame decoder.
class FrameParser {
 public:
  // Appends freshly received bytes to the internal buffer.
  void feed(const char* data, size_t length);

  // Extracts the next complete *message*. Returns false when more bytes are
  // needed (or when the stream has failed - check failed()).
  // Fragmented data messages are reassembled transparently; control frames
  // are returned as-is and may appear between data fragments.
  bool next(Frame& out);

  bool failed() const { return failed_; }
  const std::string& error() const { return error_; }

 private:
  bool parseOne(Frame& raw);
  void fail(const std::string& reason);

  std::string buffer_;
  size_t cursor_ = 0;
  bool failed_ = false;
  std::string error_;

  // Fragmentation state.
  bool inFragment_ = false;
  Opcode fragmentOpcode_ = Opcode::Text;
  std::string fragmentPayload_;
};

// Decodes a close frame's payload into a status code + reason.
void decodeClose(const std::string& payload, uint16_t& code,
                 std::string& reason);

}  // namespace ws
}  // namespace quinco
