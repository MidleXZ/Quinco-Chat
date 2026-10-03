// RFC 6455 framing implementation for Quinco Chat (see ws.h).

#include "ws.h"

#include "crypto.h"

namespace quinco {
namespace ws {
namespace {

const char kHandshakeGuid[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

constexpr size_t kMinHeaderBytes = 2;
constexpr size_t kMaskKeyBytes = 4;
constexpr size_t kCompactionThreshold = 64u * 1024u;

bool isControlOpcode(Opcode opcode) {
  return opcode == Opcode::Close || opcode == Opcode::Ping ||
         opcode == Opcode::Pong;
}

bool isValidOpcode(uint8_t raw) {
  switch (raw) {
    case 0x0:
    case 0x1:
    case 0x2:
    case 0x8:
    case 0x9:
    case 0xA:
      return true;
    default:
      return false;
  }
}

uint64_t readBigEndian(const uint8_t* bytes, size_t count) {
  uint64_t value = 0;
  for (size_t i = 0; i < count; ++i) {
    value = (value << 8) | static_cast<uint64_t>(bytes[i]);
  }
  return value;
}

void appendBigEndian(std::string& out, uint64_t value, size_t count) {
  for (size_t i = 0; i < count; ++i) {
    const size_t shift = 8 * (count - 1 - i);
    out.push_back(static_cast<char>((value >> shift) & 0xFF));
  }
}

}  // namespace

std::string acceptKey(const std::string& clientKey) {
  // The key must decode to exactly 16 bytes (RFC 6455 §4.1).
  std::string decoded;
  if (!crypto::base64Decode(clientKey, decoded) || decoded.size() != 16) {
    return std::string();
  }
  const crypto::Digest digest = crypto::sha1(clientKey + kHandshakeGuid);
  return crypto::base64Encode(digest);
}

std::string encodeFrame(Opcode opcode, const std::string& payload, bool fin) {
  std::string out;
  out.reserve(payload.size() + 10);
  out.push_back(static_cast<char>((fin ? 0x80 : 0x00) |
                                  static_cast<uint8_t>(opcode)));
  const size_t length = payload.size();
  if (length <= 125) {
    out.push_back(static_cast<char>(length));
  } else if (length <= 0xFFFF) {
    out.push_back(static_cast<char>(126));
    appendBigEndian(out, length, 2);
  } else {
    out.push_back(static_cast<char>(127));
    appendBigEndian(out, length, 8);
  }
  out += payload;
  return out;
}

std::string encodeText(const std::string& text) {
  return encodeFrame(Opcode::Text, text, true);
}

std::string encodePing(const std::string& payload) {
  return encodeFrame(Opcode::Ping, payload, true);
}

std::string encodeClose(uint16_t code, const std::string& reason) {
  std::string payload;
  if (code != 0) {
    appendBigEndian(payload, code, 2);
    // Trim the reason so the whole control payload stays within 125 bytes.
    const size_t room = kMaxControlPayloadBytes - 2;
    payload += reason.size() > room ? reason.substr(0, room) : reason;
  }
  return encodeFrame(Opcode::Close, payload, true);
}

void decodeClose(const std::string& payload, uint16_t& code,
                 std::string& reason) {
  code = 0;
  reason.clear();
  if (payload.size() >= 2) {
    code = static_cast<uint16_t>((static_cast<uint8_t>(payload[0]) << 8) |
                                 static_cast<uint8_t>(payload[1]));
    reason = payload.substr(2);
  }
}

void FrameParser::feed(const char* data, size_t length) {
  if (failed_ || length == 0) return;
  buffer_.append(data, length);
}

void FrameParser::fail(const std::string& reason) {
  failed_ = true;
  error_ = reason;
  buffer_.clear();
  cursor_ = 0;
  fragmentPayload_.clear();
  inFragment_ = false;
}

bool FrameParser::parseOne(Frame& raw) {
  const size_t available = buffer_.size() - cursor_;
  if (available < kMinHeaderBytes) return false;

  const uint8_t* base =
      reinterpret_cast<const uint8_t*>(buffer_.data()) + cursor_;
  const uint8_t byte0 = base[0];
  const uint8_t byte1 = base[1];

  const bool fin = (byte0 & 0x80) != 0;
  const uint8_t rsv = static_cast<uint8_t>((byte0 & 0x70) >> 4);
  const uint8_t opcodeRaw = static_cast<uint8_t>(byte0 & 0x0F);
  const bool masked = (byte1 & 0x80) != 0;
  const uint8_t lengthCode = static_cast<uint8_t>(byte1 & 0x7F);

  if (rsv != 0) {
    fail("reserved bits must be zero (no extension negotiated)");
    return false;
  }
  if (!isValidOpcode(opcodeRaw)) {
    fail("unknown opcode");
    return false;
  }
  // Every frame from a client must be masked (RFC 6455 §5.1).
  if (!masked) {
    fail("client frames must be masked");
    return false;
  }

  size_t headerLength = kMinHeaderBytes;
  uint64_t payloadLength = lengthCode;
  if (lengthCode == 126) {
    headerLength += 2;
  } else if (lengthCode == 127) {
    headerLength += 8;
  }
  headerLength += kMaskKeyBytes;

  if (available < headerLength) return false;

  if (lengthCode == 126) {
    payloadLength = readBigEndian(base + 2, 2);
    if (payloadLength < 126) {
      fail("non-minimal 2-byte payload length");
      return false;
    }
  } else if (lengthCode == 127) {
    payloadLength = readBigEndian(base + 2, 8);
    // The high bit must be clear and the encoding must be minimal.
    if ((payloadLength & 0x8000000000000000ull) != 0) {
      fail("payload length high bit must be zero");
      return false;
    }
    if (payloadLength <= 0xFFFF) {
      fail("non-minimal 8-byte payload length");
      return false;
    }
  }

  const Opcode opcode = static_cast<Opcode>(opcodeRaw);

  if (isControlOpcode(opcode)) {
    if (!fin) {
      fail("control frames must not be fragmented");
      return false;
    }
    if (payloadLength > kMaxControlPayloadBytes) {
      fail("control frame payload exceeds 125 bytes");
      return false;
    }
  } else if (payloadLength > kMaxMessageBytes) {
    fail("message exceeds maximum size");
    return false;
  }

  const size_t totalLength = headerLength + static_cast<size_t>(payloadLength);
  if (available < totalLength) return false;

  const uint8_t* maskKey = base + headerLength - kMaskKeyBytes;
  const uint8_t* payloadBytes = base + headerLength;

  std::string payload(static_cast<size_t>(payloadLength), '\0');
  for (size_t i = 0; i < payload.size(); ++i) {
    payload[i] = static_cast<char>(payloadBytes[i] ^ maskKey[i % 4]);
  }

  cursor_ += totalLength;
  raw.fin = fin;
  raw.opcode = opcode;
  raw.payload = std::move(payload);
  return true;
}

bool FrameParser::next(Frame& out) {
  if (failed_) return false;

  for (;;) {
    Frame raw;
    if (!parseOne(raw)) {
      if (failed_) return false;
      // Compact the consumed prefix so a long-lived connection does not
      // keep replaying dead bytes on every read.
      if (cursor_ > 0 && cursor_ == buffer_.size()) {
        buffer_.clear();
        cursor_ = 0;
      } else if (cursor_ > kCompactionThreshold) {
        buffer_.erase(0, cursor_);
        cursor_ = 0;
      }
      return false;
    }

    if (isControlOpcode(raw.opcode)) {
      out.opcode = raw.opcode;
      out.fin = true;
      out.payload = std::move(raw.payload);
      return true;
    }

    if (raw.opcode == Opcode::Continuation) {
      if (!inFragment_) {
        fail("continuation frame without an initial frame");
        return false;
      }
      if (fragmentPayload_.size() + raw.payload.size() > kMaxMessageBytes) {
        fail("fragmented message exceeds maximum size");
        return false;
      }
      fragmentPayload_ += raw.payload;
      if (!raw.fin) continue;
      out.opcode = fragmentOpcode_;
      out.fin = true;
      out.payload = std::move(fragmentPayload_);
      fragmentPayload_.clear();
      inFragment_ = false;
      return true;
    }

    // Text or binary.
    if (inFragment_) {
      fail("new data frame started before the previous one finished");
      return false;
    }
    if (raw.fin) {
      out.opcode = raw.opcode;
      out.fin = true;
      out.payload = std::move(raw.payload);
      return true;
    }
    inFragment_ = true;
    fragmentOpcode_ = raw.opcode;
    fragmentPayload_ = std::move(raw.payload);
    // Loop again to pick up the continuation frames.
  }
}

}  // namespace ws
}  // namespace quinco
