// JSON codec implementation for Quinco Chat.
// Single-pass recursive-descent parser with a hard depth limit, plus a
// serializer that prefers integer output and shortest round-trip doubles.

#include "json.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>

namespace quinco {
namespace json {
namespace {

constexpr int kMaxDepth = 128;

const char* kHexDigits = "0123456789abcdef";

void appendUtf8(std::string& out, uint32_t cp) {
  if (cp <= 0x7F) {
    out.push_back(static_cast<char>(cp));
  } else if (cp <= 0x7FF) {
    out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else if (cp <= 0xFFFF) {
    out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else {
    out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  }
}

// Shortest decimal representation of `d` that parses back to the same value.
std::string formatDouble(double d) {
  if (std::isnan(d) || std::isinf(d)) return "0";
  char buf[40];
  for (int precision = 15; precision <= 17; ++precision) {
    std::snprintf(buf, sizeof(buf), "%.*g", precision, d);
    double roundTrip = std::strtod(buf, nullptr);
    if (roundTrip == d) break;
  }
  std::string text(buf);
  // Normalise "-0" and exponent forms the JSON grammar dislikes.
  if (text == "-0") text = "0";
  return text;
}

bool isHexDigit(char c) {
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
         (c >= 'A' && c <= 'F');
}

uint32_t hexValue(char c) {
  if (c >= '0' && c <= '9') return static_cast<uint32_t>(c - '0');
  if (c >= 'a' && c <= 'f') return static_cast<uint32_t>(c - 'a' + 10);
  return static_cast<uint32_t>(c - 'A' + 10);
}

class Parser {
 public:
  Parser(const std::string& text) : text_(text) {}

  bool run(Value& out, std::string& error) {
    if (text_.size() >= 3 && static_cast<unsigned char>(text_[0]) == 0xEF &&
        static_cast<unsigned char>(text_[1]) == 0xBB &&
        static_cast<unsigned char>(text_[2]) == 0xBF) {
      pos_ = 3;  // skip a UTF-8 BOM if some client sends one
    }
    skipWhitespace();
    if (!parseValue(out, 0)) {
      error = error_.empty() ? "invalid JSON" : error_;
      return false;
    }
    skipWhitespace();
    if (pos_ != text_.size()) {
      setError("trailing characters after JSON value");
      error = error_;
      return false;
    }
    return true;
  }

 private:
  void setError(const char* message) {
    if (!error_.empty()) return;
    char buf[128];
    std::snprintf(buf, sizeof(buf), "%s at offset %zu", message, pos_);
    error_ = buf;
  }

  bool atEnd() const { return pos_ >= text_.size(); }
  char peek() const { return atEnd() ? '\0' : text_[pos_]; }

  void skipWhitespace() {
    while (pos_ < text_.size()) {
      char c = text_[pos_];
      if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
        ++pos_;
      } else {
        break;
      }
    }
  }

  bool literal(const char* expected, size_t len) {
    if (text_.compare(pos_, len, expected) != 0) {
      setError("unexpected token");
      return false;
    }
    pos_ += len;
    return true;
  }

  bool parseValue(Value& out, int depth) {
    if (depth > kMaxDepth) {
      setError("maximum nesting depth exceeded");
      return false;
    }
    if (atEnd()) {
      setError("unexpected end of input");
      return false;
    }
    switch (peek()) {
      case '{': return parseObject(out, depth);
      case '[': return parseArray(out, depth);
      case '"': {
        std::string s;
        if (!parseString(s)) return false;
        out = Value(std::move(s));
        return true;
      }
      case 't':
        if (!literal("true", 4)) return false;
        out = Value(true);
        return true;
      case 'f':
        if (!literal("false", 5)) return false;
        out = Value(false);
        return true;
      case 'n':
        if (!literal("null", 4)) return false;
        out = Value(nullptr);
        return true;
      default: return parseNumber(out);
    }
  }

  bool parseObject(Value& out, int depth) {
    ++pos_;  // consume '{'
    Object obj;
    skipWhitespace();
    if (peek() == '}') {
      ++pos_;
      out = Value(std::move(obj));
      return true;
    }
    for (;;) {
      skipWhitespace();
      if (peek() != '"') {
        setError("expected object key string");
        return false;
      }
      std::string key;
      if (!parseString(key)) return false;
      skipWhitespace();
      if (peek() != ':') {
        setError("expected ':' after object key");
        return false;
      }
      ++pos_;
      skipWhitespace();
      Value child;
      if (!parseValue(child, depth + 1)) return false;
      obj[std::move(key)] = std::move(child);
      skipWhitespace();
      char c = peek();
      if (c == ',') {
        ++pos_;
        continue;
      }
      if (c == '}') {
        ++pos_;
        out = Value(std::move(obj));
        return true;
      }
      setError("expected ',' or '}' in object");
      return false;
    }
  }

  bool parseArray(Value& out, int depth) {
    ++pos_;  // consume '['
    Array arr;
    skipWhitespace();
    if (peek() == ']') {
      ++pos_;
      out = Value(std::move(arr));
      return true;
    }
    for (;;) {
      skipWhitespace();
      Value child;
      if (!parseValue(child, depth + 1)) return false;
      arr.push_back(std::move(child));
      skipWhitespace();
      char c = peek();
      if (c == ',') {
        ++pos_;
        continue;
      }
      if (c == ']') {
        ++pos_;
        out = Value(std::move(arr));
        return true;
      }
      setError("expected ',' or ']' in array");
      return false;
    }
  }

  bool parseString(std::string& out) {
    ++pos_;  // consume opening quote
    out.clear();
    for (;;) {
      if (atEnd()) {
        setError("unterminated string");
        return false;
      }
      unsigned char c = static_cast<unsigned char>(text_[pos_]);
      if (c == '"') {
        ++pos_;
        return true;
      }
      if (c == '\\') {
        ++pos_;
        if (atEnd()) {
          setError("unterminated escape sequence");
          return false;
        }
        char esc = text_[pos_++];
        switch (esc) {
          case '"': out.push_back('"'); break;
          case '\\': out.push_back('\\'); break;
          case '/': out.push_back('/'); break;
          case 'b': out.push_back('\b'); break;
          case 'f': out.push_back('\f'); break;
          case 'n': out.push_back('\n'); break;
          case 'r': out.push_back('\r'); break;
          case 't': out.push_back('\t'); break;
          case 'u': {
            uint32_t cp = 0;
            if (!parseHex4(cp)) return false;
            if (cp >= 0xD800 && cp <= 0xDBFF) {
              // High surrogate: look for the matching low surrogate.
              if (pos_ + 1 < text_.size() && text_[pos_] == '\\' &&
                  text_[pos_ + 1] == 'u') {
                size_t save = pos_;
                pos_ += 2;
                uint32_t low = 0;
                if (!parseHex4(low)) return false;
                if (low >= 0xDC00 && low <= 0xDFFF) {
                  cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                } else {
                  pos_ = save;  // not a pair: emit the lone surrogate
                }
              }
            }
            appendUtf8(out, cp);
            break;
          }
          default:
            setError("invalid escape character");
            return false;
        }
        continue;
      }
      if (c < 0x20) {
        setError("unescaped control character in string");
        return false;
      }
      out.push_back(static_cast<char>(c));
      ++pos_;
    }
  }

  bool parseHex4(uint32_t& out) {
    if (pos_ + 4 > text_.size()) {
      setError("truncated \\u escape");
      return false;
    }
    for (int i = 0; i < 4; ++i) {
      if (!isHexDigit(text_[pos_ + static_cast<size_t>(i)])) {
        setError("invalid \\u escape");
        return false;
      }
    }
    out = (hexValue(text_[pos_]) << 12) | (hexValue(text_[pos_ + 1]) << 8) |
          (hexValue(text_[pos_ + 2]) << 4) | hexValue(text_[pos_ + 3]);
    pos_ += 4;
    return true;
  }

  bool parseNumber(Value& out) {
    size_t start = pos_;
    if (peek() == '-') ++pos_;
    if (atEnd() || peek() < '0' || peek() > '9') {
      setError("invalid number");
      return false;
    }
    if (peek() == '0') {
      ++pos_;
    } else {
      while (!atEnd() && peek() >= '0' && peek() <= '9') ++pos_;
    }
    bool isInteger = true;
    if (peek() == '.') {
      isInteger = false;
      ++pos_;
      if (atEnd() || peek() < '0' || peek() > '9') {
        setError("invalid fraction in number");
        return false;
      }
      while (!atEnd() && peek() >= '0' && peek() <= '9') ++pos_;
    }
    if (peek() == 'e' || peek() == 'E') {
      isInteger = false;
      ++pos_;
      if (peek() == '+' || peek() == '-') ++pos_;
      if (atEnd() || peek() < '0' || peek() > '9') {
        setError("invalid exponent in number");
        return false;
      }
      while (!atEnd() && peek() >= '0' && peek() <= '9') ++pos_;
    }
    std::string token = text_.substr(start, pos_ - start);
    if (isInteger) {
      errno = 0;
      char* end = nullptr;
      long long v = std::strtoll(token.c_str(), &end, 10);
      if (errno == 0 && end && *end == '\0') {
        out = Value(static_cast<int64_t>(v));
        return true;
      }
    }
    out = Value(std::strtod(token.c_str(), nullptr));
    return true;
  }

  const std::string& text_;
  size_t pos_ = 0;
  std::string error_;
};

}  // namespace

std::string escapeString(const std::string& in) {
  std::string out;
  out.reserve(in.size() + 2);
  out.push_back('"');
  for (unsigned char c : in) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\b': out += "\\b"; break;
      case '\f': out += "\\f"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20) {
          out += "\\u00";
          out.push_back(kHexDigits[(c >> 4) & 0xF]);
          out.push_back(kHexDigits[c & 0xF]);
        } else {
          out.push_back(static_cast<char>(c));
        }
    }
  }
  out.push_back('"');
  return out;
}

void Value::dumpTo(std::string& out, int indent, int depth) const {
  switch (type_) {
    case Type::Null: out += "null"; return;
    case Type::Bool: out += bool_ ? "true" : "false"; return;
    case Type::Number:
      out += isInt_ ? std::to_string(int_) : formatDouble(num_);
      return;
    case Type::String: out += escapeString(str_); return;
    case Type::Array: {
      if (arr_.empty()) {
        out += "[]";
        return;
      }
      out.push_back('[');
      bool pretty = indent > 0;
      for (size_t i = 0; i < arr_.size(); ++i) {
        if (i) out.push_back(',');
        if (pretty) {
          out.push_back('\n');
          out.append(static_cast<size_t>((depth + 1) * indent), ' ');
        }
        arr_[i].dumpTo(out, indent, depth + 1);
      }
      if (pretty) {
        out.push_back('\n');
        out.append(static_cast<size_t>(depth * indent), ' ');
      }
      out.push_back(']');
      return;
    }
    case Type::Object: {
      if (obj_.empty()) {
        out += "{}";
        return;
      }
      out.push_back('{');
      bool pretty = indent > 0;
      bool first = true;
      for (const auto& entry : obj_) {
        if (!first) out.push_back(',');
        first = false;
        if (pretty) {
          out.push_back('\n');
          out.append(static_cast<size_t>((depth + 1) * indent), ' ');
        }
        out += escapeString(entry.first);
        out.push_back(':');
        if (pretty) out.push_back(' ');
        entry.second.dumpTo(out, indent, depth + 1);
      }
      if (pretty) {
        out.push_back('\n');
        out.append(static_cast<size_t>(depth * indent), ' ');
      }
      out.push_back('}');
      return;
    }
  }
}

std::string Value::dump() const { return dump(0); }

std::string Value::dump(int indent) const {
  std::string out;
  out.reserve(128);
  dumpTo(out, indent, 0);
  return out;
}

bool Value::parse(const std::string& text, Value& out, std::string& error) {
  Parser parser(text);
  return parser.run(out, error);
}

}  // namespace json
}  // namespace quinco
