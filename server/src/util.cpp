// Shared helper implementations (see util.h).

#include "util.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include "crypto.h"

namespace quinco {
namespace util {
namespace {

constexpr size_t kMaxFileNameLength = 80;
constexpr size_t kMaxUsernameLength = 24;
constexpr size_t kMinUsernameLength = 3;
constexpr size_t kMaxDisplayNameLength = 48;

bool isUsernameCharacter(char c) {
  return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' ||
         c == '.' || c == '-';
}

// Counts UTF-8 code points so a multi-byte display name is measured in
// characters rather than bytes.
size_t utf8Length(const std::string& text) {
  size_t count = 0;
  for (unsigned char c : text) {
    if ((c & 0xC0) != 0x80) ++count;
  }
  return count;
}

}  // namespace

int64_t nowMillis() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

std::string toLowerAscii(const std::string& text) {
  std::string out = text;
  std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return out;
}

std::string trimAscii(const std::string& text) {
  size_t begin = 0;
  size_t end = text.size();
  while (begin < end && std::isspace(static_cast<unsigned char>(text[begin]))) {
    ++begin;
  }
  while (end > begin &&
         std::isspace(static_cast<unsigned char>(text[end - 1]))) {
    --end;
  }
  return text.substr(begin, end - begin);
}

bool isValidUsername(const std::string& username) {
  if (username.size() < kMinUsernameLength ||
      username.size() > kMaxUsernameLength) {
    return false;
  }
  for (char c : username) {
    if (!isUsernameCharacter(c)) return false;
  }
  return true;
}

bool isValidDisplayName(const std::string& name) {
  const std::string trimmed = trimAscii(name);
  if (trimmed.empty()) return false;
  return utf8Length(trimmed) <= kMaxDisplayNameLength;
}

std::string formatIso8601(int64_t millis) {
  const std::time_t seconds = static_cast<std::time_t>(millis / 1000);
  std::tm utc{};
#if defined(_WIN32)
  gmtime_s(&utc, &seconds);
#else
  gmtime_r(&seconds, &utc);
#endif
  char buffer[40];
  std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%S", &utc);
  char suffix[8];
  std::snprintf(suffix, sizeof(suffix), ".%03dZ",
                static_cast<int>(millis % 1000));
  return std::string(buffer) + suffix;
}

std::string joinPath(const std::string& base, const std::string& leaf) {
  if (base.empty()) return leaf;
  if (leaf.empty()) return base;
  std::string out = base;
  if (out.back() != '/') out.push_back('/');
  size_t start = 0;
  while (start < leaf.size() && leaf[start] == '/') ++start;
  out += leaf.substr(start);
  return out;
}

bool ensureDirectory(const std::string& path) {
  if (path.empty()) return false;
  std::error_code error;
  std::filesystem::create_directories(path, error);
  return !error && std::filesystem::is_directory(path, error) && !error;
}

bool pathExists(const std::string& path) {
  std::error_code error;
  return std::filesystem::exists(path, error) && !error;
}

bool pathIsRegularFile(const std::string& path) {
  std::error_code error;
  return std::filesystem::is_regular_file(path, error) && !error;
}

bool readWholeFile(const std::string& path, std::string& out) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) return false;
  std::ostringstream buffer;
  buffer << stream.rdbuf();
  if (stream.bad()) return false;
  out = buffer.str();
  return true;
}

bool appendLine(const std::string& path, const std::string& line) {
  std::ofstream stream(path, std::ios::binary | std::ios::app);
  if (!stream) return false;
  stream.write(line.data(), static_cast<std::streamsize>(line.size()));
  stream.put('\n');
  stream.flush();
  return stream.good();
}

bool writeFileAtomically(const std::string& path, const std::string& contents) {
  const std::string temporary = path + ".tmp";
  {
    std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
    if (!stream) return false;
    stream.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    stream.flush();
    if (!stream.good()) return false;
  }
#if defined(_WIN32)
  const std::filesystem::path source(temporary);
  const std::filesystem::path destination(path);
  return MoveFileExW(source.c_str(), destination.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
  std::error_code error;
  std::filesystem::rename(temporary, path, error);
  return !error;
#endif
}

std::string newId(const std::string& prefix) {
  return prefix + "_" + crypto::randomToken(9);
}

std::string sanitiseFileName(const std::string& name) {
  const size_t slash = name.find_last_of('/');
  std::string base = slash == std::string::npos ? name : name.substr(slash + 1);

  std::string cleaned;
  cleaned.reserve(base.size());
  for (unsigned char c : base) {
    const bool safe = std::isalnum(c) || c == '.' || c == '-' || c == '_';
    cleaned.push_back(safe ? static_cast<char>(c) : '_');
  }
  // Reject a name that is only dots, which would be "." or "..".
  bool hasContent = false;
  for (char c : cleaned) {
    if (c != '.') {
      hasContent = true;
      break;
    }
  }
  if (!hasContent) return std::string();
  if (cleaned.size() > kMaxFileNameLength) {
    // Keep the extension when truncating.
    const size_t dot = cleaned.find_last_of('.');
    std::string extension =
        (dot == std::string::npos) ? std::string() : cleaned.substr(dot);
    if (extension.size() > 16) extension.clear();
    cleaned = cleaned.substr(0, kMaxFileNameLength - extension.size()) + extension;
  }
  return cleaned;
}

std::string humanFileSize(int64_t bytes) {
  static const char* kUnits[] = {"B", "KB", "MB", "GB"};
  double value = static_cast<double>(bytes);
  int unit = 0;
  while (value >= 1024.0 && unit < 3) {
    value /= 1024.0;
    ++unit;
  }
  char buffer[32];
  if (unit == 0) {
    std::snprintf(buffer, sizeof(buffer), "%lld %s",
                  static_cast<long long>(bytes), kUnits[unit]);
  } else {
    std::snprintf(buffer, sizeof(buffer), "%.1f %s", value, kUnits[unit]);
  }
  return buffer;
}

std::string jsonEscape(const std::string& text) {
  static const char* kHex = "0123456789abcdef";
  std::string out;
  out.reserve(text.size() + 8);
  for (unsigned char c : text) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20) {
          out += "\\u00";
          out.push_back(kHex[(c >> 4) & 0xF]);
          out.push_back(kHex[c & 0xF]);
        } else {
          out.push_back(static_cast<char>(c));
        }
    }
  }
  return out;
}

}  // namespace util
}  // namespace quinco
