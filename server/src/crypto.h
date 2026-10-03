// Self-contained cryptography helpers for Quinco Chat.
//
// The server has no OpenSSL link dependency: everything it needs is
// implemented here over <cstdint>. That keeps the build a plain `make`
// with no package install.
//
// Contents:
//   sha1()            - SHA-1 digest (required by RFC 6455 §4.1 handshake)
//   hmacSha1()        - HMAC-SHA1 (RFC 2104)
//   pbkdf2Sha1()      - PBKDF2-HMAC-SHA1 (RFC 2898) for password storage
//   base64Encode()    - standard alphabet, padded
//   base64Decode()    - tolerant of whitespace and missing padding
//   randomBytes()     - CSPRNG read from the OS entropy pool
//   randomToken()     - URL-safe random identifier
//   constantTimeEquals() - timing-safe comparison for secrets

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace quinco {
namespace crypto {

constexpr size_t kSha1DigestSize = 20;

using Digest = std::vector<uint8_t>;

// SHA-1 over a byte buffer.
Digest sha1(const void* data, size_t length);
Digest sha1(const std::string& data);

// HMAC-SHA1. `key` may be any length (long keys are hashed per RFC 2104).
Digest hmacSha1(const std::string& key, const void* data, size_t length);
Digest hmacSha1(const std::string& key, const std::string& data);

// PBKDF2-HMAC-SHA1 with an explicit iteration count and derived length.
Digest pbkdf2Sha1(const std::string& password, const std::string& salt,
                  uint32_t iterations, size_t derivedLength);

std::string base64Encode(const void* data, size_t length);
std::string base64Encode(const std::string& text);
std::string base64Encode(const Digest& digest);
bool base64Decode(const std::string& text, std::string& out);

// Fills `out` with `count` cryptographically random bytes. Returns false if
// the OS entropy source could not be read.
bool randomBytes(void* out, size_t count);

// A URL-safe random identifier: `byteCount` bytes rendered as lowercase hex.
std::string randomToken(size_t byteCount = 16);

// Renders a digest as lowercase hex (used for stored password records).
std::string toHex(const Digest& digest);

// Compares two strings without short-circuiting on the first difference.
bool constantTimeEquals(const std::string& a, const std::string& b);

}  // namespace crypto
}  // namespace quinco
