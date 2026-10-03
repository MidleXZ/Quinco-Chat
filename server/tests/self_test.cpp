// Self-tests for the Quinco Chat foundation layer.
//
// Verifies the primitives everything else depends on against published test
// vectors, so a silent regression in the crypto or framing code cannot reach
// the network layer:
//   * SHA-1        - FIPS 180-4 / RFC 3174 examples
//   * HMAC-SHA1    - RFC 2202 vectors
//   * PBKDF2-SHA1  - RFC 6070 vectors
//   * base64       - RFC 4648 examples
//   * WebSocket    - RFC 6455 §1.3 handshake example
//   * JSON         - round-trip, escapes, surrogates, numbers, error cases
//   * frame parser - masking, fragmentation, control frames, oversize rejection

#include <cstdio>
#include <string>

#include "../src/crypto.h"
#include "../src/json.h"
#include "../src/ws.h"

namespace {

int g_checks = 0;
int g_failures = 0;

void expectEq(const std::string& actual, const std::string& expected,
              const std::string& label) {
  ++g_checks;
  if (actual != expected) {
    ++g_failures;
    std::printf("FAIL  %s\n        expected: %s\n        actual:   %s\n",
                label.c_str(), expected.c_str(), actual.c_str());
  }
}

void expectTrue(bool value, const std::string& label) {
  ++g_checks;
  if (!value) {
    ++g_failures;
    std::printf("FAIL  %s\n", label.c_str());
  }
}

void expectFalse(bool value, const std::string& label) {
  expectTrue(!value, label);
}

// --- SHA-1 ---------------------------------------------------------------

void testSha1() {
  using quinco::crypto::sha1;
  using quinco::crypto::toHex;

  expectEq(toHex(sha1(std::string())),
           "da39a3ee5e6b4b0d3255bfef95601890afd80709", "sha1 of empty string");
  expectEq(toHex(sha1(std::string("abc"))),
           "a9993e364706816aba3e25717850c26c9cd0d89d", "sha1 of 'abc'");
  expectEq(
      toHex(sha1(std::string(
          "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"))),
      "84983e441c3bd26ebaae4aa1f95129e5e54670f1", "sha1 of the 56-byte vector");
  // Exercises multi-block padding: 64 bytes forces an extra padding block.
  expectEq(toHex(sha1(std::string(64, 'a'))),
           "0098ba824b5c16427bd7a1122a5a442a25ec644d",
           "sha1 of 64 'a' characters");

  std::string million(1000000, 'a');
  expectEq(toHex(sha1(million)), "34aa973cd4c4daa4f61eeb2bdbad27316534016f",
           "sha1 of one million 'a' characters");
}

// --- HMAC-SHA1 -----------------------------------------------------------

void testHmac() {
  using quinco::crypto::hmacSha1;
  using quinco::crypto::toHex;

  expectEq(toHex(hmacSha1(std::string(20, '\x0b'), std::string("Hi There"))),
           "b617318655057264e28bc0b6fb378c8ef146be00",
           "hmac-sha1 RFC 2202 case 1");
  expectEq(toHex(hmacSha1(std::string("Jefe"),
                          std::string("what do ya want for nothing?"))),
           "effcdf6ae5eb2fa2d27416d5f184df9c259a7c79",
           "hmac-sha1 RFC 2202 case 2");
  // A key longer than the 64-byte block exercises the key-hashing branch.
  expectEq(toHex(hmacSha1(std::string(80, '\xaa'),
                          std::string("Test Using Larger Than Block-Size "
                                      "Key - Hash Key First"))),
           "aa4ae5e15272d00e95705637ce8a3b55ed402112",
           "hmac-sha1 RFC 2202 case 6");
}

// --- PBKDF2-HMAC-SHA1 ----------------------------------------------------

void testPbkdf2() {
  using quinco::crypto::pbkdf2Sha1;
  using quinco::crypto::toHex;

  expectEq(toHex(pbkdf2Sha1("password", "salt", 1, 20)),
           "0c60c80f961f0e71f3a9b524af6012062fe037a6",
           "pbkdf2 RFC 6070 c=1");
  expectEq(toHex(pbkdf2Sha1("password", "salt", 2, 20)),
           "ea6c014dc72d6f8ccd1ed92ace1d41f0d8de8957",
           "pbkdf2 RFC 6070 c=2");
  expectEq(toHex(pbkdf2Sha1("password", "salt", 4096, 20)),
           "4b007901b765489abead49d926f721d065a429c1",
           "pbkdf2 RFC 6070 c=4096");
  // A derived length longer than one digest block exercises the outer loop.
  expectEq(toHex(pbkdf2Sha1("passwordPASSWORDpassword",
                            "saltSALTsaltSALTsaltSALTsaltSALTsalt", 4096, 25)),
           "3d2eec4fe41c849b80c8d83662c0e44a8b291a964cf2f07038",
           "pbkdf2 RFC 6070 multi-block");
}

// --- base64 --------------------------------------------------------------

void testBase64() {
  using quinco::crypto::base64Decode;
  using quinco::crypto::base64Encode;

  expectEq(base64Encode(std::string("")), "", "base64 of empty");
  expectEq(base64Encode(std::string("f")), "Zg==", "base64 of 'f'");
  expectEq(base64Encode(std::string("fo")), "Zm8=", "base64 of 'fo'");
  expectEq(base64Encode(std::string("foo")), "Zm9v", "base64 of 'foo'");
  expectEq(base64Encode(std::string("foob")), "Zm9vYg==", "base64 of 'foob'");
  expectEq(base64Encode(std::string("fooba")), "Zm9vYmE=",
           "base64 of 'fooba'");
  expectEq(base64Encode(std::string("foobar")), "Zm9vYmFy",
           "base64 of 'foobar'");

  std::string decoded;
  expectTrue(base64Decode("Zm9vYmFy", decoded), "base64 decode succeeds");
  expectEq(decoded, "foobar", "base64 round trip");
  expectFalse(base64Decode("!!!!", decoded), "base64 rejects bad alphabet");
}

// --- WebSocket handshake + framing ---------------------------------------

std::string maskedClientFrame(uint8_t opcode, const std::string& payload,
                              bool fin) {
  static const uint8_t kMask[4] = {0x37, 0xfa, 0x21, 0x3d};
  std::string out;
  out.push_back(static_cast<char>((fin ? 0x80 : 0x00) | opcode));
  const size_t length = payload.size();
  if (length <= 125) {
    out.push_back(static_cast<char>(0x80 | length));
  } else if (length <= 0xFFFF) {
    out.push_back(static_cast<char>(0x80 | 126));
    out.push_back(static_cast<char>((length >> 8) & 0xFF));
    out.push_back(static_cast<char>(length & 0xFF));
  } else {
    out.push_back(static_cast<char>(0x80 | 127));
    for (int shift = 56; shift >= 0; shift -= 8) {
      out.push_back(static_cast<char>((length >> shift) & 0xFF));
    }
  }
  for (int i = 0; i < 4; ++i) out.push_back(static_cast<char>(kMask[i]));
  for (size_t i = 0; i < length; ++i) {
    out.push_back(static_cast<char>(payload[i] ^ kMask[i % 4]));
  }
  return out;
}

void testHandshake() {
  using quinco::ws::acceptKey;
  // RFC 6455 §1.3 worked example.
  expectEq(acceptKey("dGhlIHNhbXBsZSBub25jZQ=="),
           "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=", "websocket accept key");
  expectEq(acceptKey("not-base64-at-all"), "",
           "accept key rejects an invalid nonce");
  expectEq(acceptKey("Zm9v"), "", "accept key rejects a short nonce");
}

void testFrameParser() {
  using quinco::ws::Frame;
  using quinco::ws::FrameParser;
  using quinco::ws::Opcode;

  // A single masked text frame.
  {
    FrameParser parser;
    const std::string raw = maskedClientFrame(0x1, "hello", true);
    parser.feed(raw.data(), raw.size());
    Frame frame;
    expectTrue(parser.next(frame), "parses a simple text frame");
    expectEq(frame.payload, "hello", "text frame payload");
    expectTrue(frame.opcode == Opcode::Text, "text frame opcode");
    expectFalse(parser.failed(), "simple frame does not fail the parser");
  }

  // The same frame delivered one byte at a time.
  {
    FrameParser parser;
    const std::string raw = maskedClientFrame(0x1, "drip feed", true);
    Frame frame;
    bool ready = false;
    for (size_t i = 0; i < raw.size(); ++i) {
      parser.feed(raw.data() + i, 1);
      if (parser.next(frame)) {
        ready = true;
        break;
      }
    }
    expectTrue(ready, "parses a frame split across many reads");
    expectEq(frame.payload, "drip feed", "split frame payload");
  }

  // A fragmented message: text, then two continuations.
  {
    FrameParser parser;
    std::string raw = maskedClientFrame(0x1, "frag", false);
    raw += maskedClientFrame(0x0, "ment", false);
    raw += maskedClientFrame(0x0, "ed", true);
    parser.feed(raw.data(), raw.size());
    Frame frame;
    expectTrue(parser.next(frame), "reassembles a fragmented message");
    expectEq(frame.payload, "fragmented", "fragmented payload");
  }

  // An extended 16-bit length.
  {
    FrameParser parser;
    const std::string body(1000, 'z');
    const std::string raw = maskedClientFrame(0x1, body, true);
    parser.feed(raw.data(), raw.size());
    Frame frame;
    expectTrue(parser.next(frame), "parses a 16-bit extended length");
    expectEq(frame.payload, body, "extended length payload");
  }

  // A ping is surfaced immediately so the caller can pong it.
  {
    FrameParser parser;
    const std::string raw = maskedClientFrame(0x9, "ping", true);
    parser.feed(raw.data(), raw.size());
    Frame frame;
    expectTrue(parser.next(frame), "surfaces a ping frame");
    expectTrue(frame.opcode == Opcode::Ping, "ping opcode");
    expectEq(frame.payload, "ping", "ping payload");
  }

  // An unmasked client frame is a protocol violation.
  {
    FrameParser parser;
    std::string raw;
    raw.push_back(static_cast<char>(0x81));
    raw.push_back(static_cast<char>(0x03));
    raw += "abc";
    parser.feed(raw.data(), raw.size());
    Frame frame;
    expectFalse(parser.next(frame), "rejects an unmasked client frame");
    expectTrue(parser.failed(), "unmasked frame marks the parser failed");
  }

  // A control frame larger than 125 bytes is illegal.
  {
    FrameParser parser;
    const std::string raw = maskedClientFrame(0x9, std::string(200, 'p'), true);
    parser.feed(raw.data(), raw.size());
    Frame frame;
    expectFalse(parser.next(frame), "rejects an oversized control frame");
    expectTrue(parser.failed(), "oversized control frame fails the parser");
  }

  // A continuation with nothing to continue is a protocol violation.
  {
    FrameParser parser;
    const std::string raw = maskedClientFrame(0x0, "orphan", true);
    parser.feed(raw.data(), raw.size());
    Frame frame;
    expectFalse(parser.next(frame), "rejects an orphan continuation");
    expectTrue(parser.failed(), "orphan continuation fails the parser");
  }
}

void testEncodeFrame() {
  using quinco::ws::encodeClose;
  using quinco::ws::encodeText;

  const std::string shortFrame = encodeText("hi");
  expectEq(shortFrame, std::string("\x81\x02hi", 4), "encodes a short text frame");

  const std::string longFrame = encodeText(std::string(200, 'x'));
  expectTrue(static_cast<unsigned char>(longFrame[1]) == 126,
             "uses a 16-bit length for a 200-byte payload");
  expectTrue(longFrame.size() == 4 + 200, "extended frame length is exact");

  const std::string closeFrame = encodeClose(1000, "bye");
  expectTrue(static_cast<unsigned char>(closeFrame[1]) == 5,
             "close frame carries code plus reason");
  expectTrue((static_cast<unsigned char>(closeFrame[2]) << 8 |
              static_cast<unsigned char>(closeFrame[3])) == 1000,
             "close frame status code is big-endian");
}

// --- JSON ----------------------------------------------------------------

void testJson() {
  using quinco::json::Value;

  // Round trip a nested document.
  {
    Value root = Value::object();
    root.set("name", Value("Quinco"));
    root.set("count", Value(static_cast<int64_t>(42)));
    root.set("ratio", Value(0.5));
    root.set("live", Value(true));
    Value tags = Value::array();
    tags.push(Value("a"));
    tags.push(Value("b"));
    root.set("tags", std::move(tags));
    Value nested = Value::object();
    nested.set("deep", Value("yes"));
    root.set("meta", std::move(nested));

    const std::string text = root.dump();
    Value parsed;
    std::string error;
    expectTrue(Value::parse(text, parsed, error), "round trip parses: " + error);
    expectEq(parsed["name"].asString(), "Quinco", "round trip string");
    expectTrue(parsed["count"].asInt() == 42, "round trip integer");
    expectTrue(parsed["ratio"].asNumber() == 0.5, "round trip double");
    expectTrue(parsed["live"].asBool(), "round trip bool");
    expectTrue(parsed["tags"].asArray().size() == 2, "round trip array");
    expectEq(parsed["meta"]["deep"].asString(), "yes", "round trip nesting");
  }

  // Escapes, including a surrogate pair for an emoji.
  {
    Value parsed;
    std::string error;
    expectTrue(Value::parse("\"line\\nbreak\\t\\\"q\\\"\"", parsed, error),
               "parses escape sequences");
    expectEq(parsed.asString(), "line\nbreak\t\"q\"", "decoded escapes");

    expectTrue(Value::parse("\"\\u00e9\\ud83c\\udf34\"", parsed, error),
               "parses unicode escapes");
    expectEq(parsed.asString(), "\xc3\xa9\xf0\x9f\x8c\xb4",
             "decodes a surrogate pair to UTF-8");

    Value emoji;
    const std::string dumped = Value("\xf0\x9f\x8c\xb4").dump();
    expectTrue(Value::parse(dumped, emoji, error), "reparses an emoji");
    expectEq(emoji.asString(), "\xf0\x9f\x8c\xb4", "emoji survives round trip");
  }

  // Whitespace, empty containers and bare values.
  {
    Value parsed;
    std::string error;
    expectTrue(Value::parse("  {  }  ", parsed, error), "parses empty object");
    expectTrue(parsed.isObject() && parsed.size() == 0, "empty object is empty");
    expectTrue(Value::parse("[]", parsed, error), "parses empty array");
    expectTrue(parsed.isArray() && parsed.size() == 0, "empty array is empty");
    expectTrue(Value::parse("null", parsed, error), "parses null");
    expectTrue(parsed.isNull(), "null is null");
    expectTrue(Value::parse("0", parsed, error), "parses zero");
    expectTrue(Value::parse("-17", parsed, error), "parses a negative integer");
    expectTrue(parsed.asInt() == -17, "negative integer value");
  }

  // Malformed input is rejected rather than silently coerced.
  {
    Value parsed;
    std::string error;
    expectFalse(Value::parse("{", parsed, error), "rejects an unterminated object");
    expectFalse(Value::parse("{\"a\":}", parsed, error), "rejects a missing value");
    expectFalse(Value::parse("[1,]", parsed, error), "rejects a trailing comma");
    expectFalse(Value::parse("nul", parsed, error), "rejects a bad literal");
    expectFalse(Value::parse("\"unterminated", parsed, error),
                "rejects an unterminated string");
    expectFalse(Value::parse("{} extra", parsed, error),
                "rejects trailing content");
    expectFalse(Value::parse("01", parsed, error), "rejects a leading zero");
  }

  // Deep nesting is bounded so a hostile payload cannot blow the stack.
  {
    std::string deep;
    for (int i = 0; i < 400; ++i) deep.push_back('[');
    Value parsed;
    std::string error;
    expectFalse(Value::parse(deep, parsed, error), "rejects excessive nesting");
  }

  // Missing members and wrong-typed access degrade instead of crashing.
  {
    Value parsed;
    std::string error;
    Value::parse("{\"a\":1}", parsed, error);
    expectEq(parsed["missing"].asString("fallback"), "fallback",
             "missing key uses the default");
    expectTrue(parsed["a"].asNumber() == 1.0, "numeric accessor works");
    expectEq(parsed["a"].asString("not-a-string"), "not-a-string",
             "wrong-type accessor uses the default");
    expectTrue(parsed["a"].asArray().empty(), "wrong-type array is empty");
  }
}

void testRandomToken() {
  using quinco::crypto::constantTimeEquals;
  using quinco::crypto::randomToken;

  const std::string first = randomToken(16);
  const std::string second = randomToken(16);
  expectTrue(first.size() == 32, "token is hex encoded");
  expectFalse(first == second, "tokens do not collide");
  expectTrue(constantTimeEquals(first, first), "constant time equality matches");
  expectFalse(constantTimeEquals(first, second), "constant time equality differs");
  expectFalse(constantTimeEquals(first, "short"), "different lengths differ");
  expectTrue(randomToken(0).size() == 2, "a zero-byte token is still non-empty");
}

}  // namespace

int main() {
  std::printf("Quinco Chat foundation self-test\n");
  std::printf("--------------------------------\n");
  testSha1();
  testHmac();
  testPbkdf2();
  testBase64();
  testHandshake();
  testEncodeFrame();
  testFrameParser();
  testJson();
  testRandomToken();

  std::printf("--------------------------------\n");
  std::printf("%d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
