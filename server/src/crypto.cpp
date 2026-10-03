// Cryptography implementation for Quinco Chat (see crypto.h).

#include "crypto.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <random>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <bcrypt.h>
#elif defined(__linux__)
#include <sys/random.h>
#endif
#if !defined(_WIN32)
#include <fcntl.h>
#include <unistd.h>
#endif

namespace quinco {
namespace crypto {
namespace {

inline uint32_t rotl32(uint32_t value, int bits) {
  return (value << bits) | (value >> (32 - bits));
}

// One-shot SHA-1 over a contiguous byte range.
Digest sha1Raw(const uint8_t* data, size_t length) {
  uint32_t h[5] = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u,
                   0xC3D2E1F0u};

  // Message + 1 bit + zero padding + 64-bit big-endian bit length.
  const uint64_t bitLength = static_cast<uint64_t>(length) * 8ull;
  const size_t paddedLength = ((length + 8) / 64 + 1) * 64;
  std::vector<uint8_t> buffer(paddedLength, 0);
  if (length > 0) std::memcpy(buffer.data(), data, length);
  buffer[length] = 0x80;
  for (int i = 0; i < 8; ++i) {
    buffer[paddedLength - 1 - static_cast<size_t>(i)] =
        static_cast<uint8_t>((bitLength >> (8 * i)) & 0xFF);
  }

  for (size_t offset = 0; offset < paddedLength; offset += 64) {
    uint32_t w[80];
    for (int i = 0; i < 16; ++i) {
      const uint8_t* p = buffer.data() + offset + static_cast<size_t>(i) * 4;
      w[i] = (static_cast<uint32_t>(p[0]) << 24) |
             (static_cast<uint32_t>(p[1]) << 16) |
             (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
    }
    for (int i = 16; i < 80; ++i) {
      w[i] = rotl32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    }

    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
    for (int i = 0; i < 80; ++i) {
      uint32_t f, k;
      if (i < 20) {
        f = (b & c) | ((~b) & d);
        k = 0x5A827999u;
      } else if (i < 40) {
        f = b ^ c ^ d;
        k = 0x6ED9EBA1u;
      } else if (i < 60) {
        f = (b & c) | (b & d) | (c & d);
        k = 0x8F1BBCDCu;
      } else {
        f = b ^ c ^ d;
        k = 0xCA62C1D6u;
      }
      const uint32_t temp = rotl32(a, 5) + f + e + k + w[i];
      e = d;
      d = c;
      c = rotl32(b, 30);
      b = a;
      a = temp;
    }

    h[0] += a;
    h[1] += b;
    h[2] += c;
    h[3] += d;
    h[4] += e;
  }

  Digest digest(kSha1DigestSize);
  for (int i = 0; i < 5; ++i) {
    digest[static_cast<size_t>(i) * 4 + 0] =
        static_cast<uint8_t>((h[i] >> 24) & 0xFF);
    digest[static_cast<size_t>(i) * 4 + 1] =
        static_cast<uint8_t>((h[i] >> 16) & 0xFF);
    digest[static_cast<size_t>(i) * 4 + 2] =
        static_cast<uint8_t>((h[i] >> 8) & 0xFF);
    digest[static_cast<size_t>(i) * 4 + 3] = static_cast<uint8_t>(h[i] & 0xFF);
  }
  return digest;
}

constexpr size_t kHmacBlockSize = 64;

}  // namespace

Digest sha1(const void* data, size_t length) {
  return sha1Raw(static_cast<const uint8_t*>(data), length);
}

Digest sha1(const std::string& data) {
  return sha1Raw(reinterpret_cast<const uint8_t*>(data.data()), data.size());
}

Digest hmacSha1(const std::string& key, const void* data, size_t length) {
  std::vector<uint8_t> blockKey(kHmacBlockSize, 0);
  if (key.size() > kHmacBlockSize) {
    const Digest hashedKey = sha1Raw(
        reinterpret_cast<const uint8_t*>(key.data()), key.size());
    std::memcpy(blockKey.data(), hashedKey.data(), hashedKey.size());
  } else if (!key.empty()) {
    std::memcpy(blockKey.data(), key.data(), key.size());
  }

  std::vector<uint8_t> inner;
  inner.reserve(kHmacBlockSize + length);
  for (size_t i = 0; i < kHmacBlockSize; ++i) {
    inner.push_back(static_cast<uint8_t>(blockKey[i] ^ 0x36));
  }
  const uint8_t* bytes = static_cast<const uint8_t*>(data);
  inner.insert(inner.end(), bytes, bytes + length);
  const Digest innerDigest = sha1Raw(inner.data(), inner.size());

  std::vector<uint8_t> outer;
  outer.reserve(kHmacBlockSize + kSha1DigestSize);
  for (size_t i = 0; i < kHmacBlockSize; ++i) {
    outer.push_back(static_cast<uint8_t>(blockKey[i] ^ 0x5C));
  }
  outer.insert(outer.end(), innerDigest.begin(), innerDigest.end());
  return sha1Raw(outer.data(), outer.size());
}

Digest hmacSha1(const std::string& key, const std::string& data) {
  return hmacSha1(key, data.data(), data.size());
}

Digest pbkdf2Sha1(const std::string& password, const std::string& salt,
                  uint32_t iterations, size_t derivedLength) {
  if (iterations == 0) iterations = 1;
  Digest derived;
  derived.reserve(derivedLength);

  const size_t blockCount =
      (derivedLength + kSha1DigestSize - 1) / kSha1DigestSize;
  for (size_t blockIndex = 1; blockIndex <= blockCount; ++blockIndex) {
    std::string seed = salt;
    seed.push_back(static_cast<char>((blockIndex >> 24) & 0xFF));
    seed.push_back(static_cast<char>((blockIndex >> 16) & 0xFF));
    seed.push_back(static_cast<char>((blockIndex >> 8) & 0xFF));
    seed.push_back(static_cast<char>(blockIndex & 0xFF));

    Digest u = hmacSha1(password, seed);
    Digest accumulator = u;
    for (uint32_t round = 1; round < iterations; ++round) {
      u = hmacSha1(password, u.data(), u.size());
      for (size_t i = 0; i < accumulator.size(); ++i) {
        accumulator[i] = static_cast<uint8_t>(accumulator[i] ^ u[i]);
      }
    }
    for (uint8_t byte : accumulator) {
      if (derived.size() < derivedLength) derived.push_back(byte);
    }
  }
  return derived;
}

namespace {
const char kBase64Alphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

int base64Value(char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+') return 62;
  if (c == '/') return 63;
  return -1;
}
}  // namespace

std::string base64Encode(const void* data, size_t length) {
  const uint8_t* bytes = static_cast<const uint8_t*>(data);
  std::string out;
  out.reserve(((length + 2) / 3) * 4);
  size_t i = 0;
  while (i + 3 <= length) {
    const uint32_t chunk = (static_cast<uint32_t>(bytes[i]) << 16) |
                           (static_cast<uint32_t>(bytes[i + 1]) << 8) |
                           static_cast<uint32_t>(bytes[i + 2]);
    out.push_back(kBase64Alphabet[(chunk >> 18) & 0x3F]);
    out.push_back(kBase64Alphabet[(chunk >> 12) & 0x3F]);
    out.push_back(kBase64Alphabet[(chunk >> 6) & 0x3F]);
    out.push_back(kBase64Alphabet[chunk & 0x3F]);
    i += 3;
  }
  const size_t remaining = length - i;
  if (remaining == 1) {
    const uint32_t chunk = static_cast<uint32_t>(bytes[i]) << 16;
    out.push_back(kBase64Alphabet[(chunk >> 18) & 0x3F]);
    out.push_back(kBase64Alphabet[(chunk >> 12) & 0x3F]);
    out += "==";
  } else if (remaining == 2) {
    const uint32_t chunk = (static_cast<uint32_t>(bytes[i]) << 16) |
                           (static_cast<uint32_t>(bytes[i + 1]) << 8);
    out.push_back(kBase64Alphabet[(chunk >> 18) & 0x3F]);
    out.push_back(kBase64Alphabet[(chunk >> 12) & 0x3F]);
    out.push_back(kBase64Alphabet[(chunk >> 6) & 0x3F]);
    out.push_back('=');
  }
  return out;
}

std::string base64Encode(const std::string& text) {
  return base64Encode(text.data(), text.size());
}

std::string base64Encode(const Digest& digest) {
  return base64Encode(digest.data(), digest.size());
}

bool base64Decode(const std::string& text, std::string& out) {
  out.clear();
  int accumulator = 0;
  int bitCount = 0;
  for (char c : text) {
    if (c == '=' ) break;
    if (c == '\n' || c == '\r' || c == ' ' || c == '\t') continue;
    const int value = base64Value(c);
    if (value < 0) return false;
    accumulator = (accumulator << 6) | value;
    bitCount += 6;
    if (bitCount >= 8) {
      bitCount -= 8;
      out.push_back(static_cast<char>((accumulator >> bitCount) & 0xFF));
    }
  }
  return true;
}

bool randomBytes(void* out, size_t count) {
  if (count == 0) return true;
  uint8_t* bytes = static_cast<uint8_t*>(out);
#if defined(_WIN32)
  size_t filled = 0;
  while (filled < count) {
    const ULONG chunk = static_cast<ULONG>(
        std::min(count - filled, static_cast<size_t>(0xFFFFFFFFu)));
    if (BCryptGenRandom(nullptr, bytes + filled, chunk,
                        BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
      return false;
    }
    filled += chunk;
  }
  return true;
#else
#if defined(__linux__)
  size_t filled = 0;
  while (filled < count) {
    const ssize_t got = ::getrandom(bytes + filled, count - filled, 0);
    if (got > 0) {
      filled += static_cast<size_t>(got);
      continue;
    }
    if (got < 0 && (errno == EINTR || errno == EAGAIN)) continue;
    break;  // fall through to /dev/urandom
  }
  if (filled == count) return true;
  if (filled > 0) return false;
#endif
  const int fd = ::open("/dev/urandom", O_RDONLY);
  if (fd < 0) return false;
  size_t readBytes = 0;
  while (readBytes < count) {
    const ssize_t got = ::read(fd, bytes + readBytes, count - readBytes);
    if (got <= 0) {
      if (got < 0 && errno == EINTR) continue;
      ::close(fd);
      return false;
    }
    readBytes += static_cast<size_t>(got);
  }
  ::close(fd);
  return true;
#endif
}

std::string randomToken(size_t byteCount) {
  std::vector<uint8_t> bytes(byteCount == 0 ? 1 : byteCount);
  if (!randomBytes(bytes.data(), bytes.size())) {
    // Extremely unlikely. Degrade to a process-unique mixture of the clock,
    // a monotonic counter and random_device rather than returning an empty
    // token, which would let two sessions collide.
    static std::atomic<uint64_t> counter{0};
    const uint64_t ticks = static_cast<uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    const uint64_t sequence = counter.fetch_add(1, std::memory_order_relaxed);
    std::random_device device;
    for (size_t i = 0; i < bytes.size(); ++i) {
      const uint64_t mixed =
          ticks * 0x9E3779B97F4A7C15ull + sequence * 0xBF58476D1CE4E5B9ull +
          static_cast<uint64_t>(device()) * 0x94D049BB133111EBull +
          static_cast<uint64_t>(i) * 0x2545F4914F6CDD1Dull;
      bytes[i] = static_cast<uint8_t>((mixed >> ((i % 8) * 8)) & 0xFF);
    }
  }
  static const char* kHex = "0123456789abcdef";
  std::string out;
  out.reserve(bytes.size() * 2);
  for (uint8_t byte : bytes) {
    out.push_back(kHex[(byte >> 4) & 0xF]);
    out.push_back(kHex[byte & 0xF]);
  }
  return out;
}

std::string toHex(const Digest& digest) {
  static const char* kHex = "0123456789abcdef";
  std::string out;
  out.reserve(digest.size() * 2);
  for (uint8_t byte : digest) {
    out.push_back(kHex[(byte >> 4) & 0xF]);
    out.push_back(kHex[byte & 0xF]);
  }
  return out;
}

bool constantTimeEquals(const std::string& a, const std::string& b) {
  if (a.size() != b.size()) return false;
  unsigned char diff = 0;
  for (size_t i = 0; i < a.size(); ++i) {
    diff |= static_cast<unsigned char>(a[i] ^ b[i]);
  }
  return diff == 0;
}

}  // namespace crypto
}  // namespace quinco
