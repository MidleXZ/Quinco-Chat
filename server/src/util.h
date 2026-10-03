// Small shared helpers used across the application layer.

#pragma once

#include <cstdint>
#include <string>

namespace quinco {
namespace util {

// Milliseconds since the Unix epoch, from the system clock.
int64_t nowMillis();

std::string toLowerAscii(const std::string& text);
std::string trimAscii(const std::string& text);

// Usernames are the login key: 3-24 characters from [a-z0-9_.-], always
// stored lowercase. Display names are freer but bounded.
bool isValidUsername(const std::string& username);
bool isValidDisplayName(const std::string& name);

// RFC 3339 style timestamp, used for message metadata sent to clients.
std::string formatIso8601(int64_t millis);

std::string joinPath(const std::string& base, const std::string& leaf);
bool ensureDirectory(const std::string& path);
bool pathIsRegularFile(const std::string& path);
bool pathExists(const std::string& path);
bool readWholeFile(const std::string& path, std::string& out);
bool appendLine(const std::string& path, const std::string& line);

// Writes via a temporary file and rename so a crash mid-write cannot leave a
// truncated JSON document behind.
bool writeFileAtomically(const std::string& path, const std::string& contents);

// A short random identifier with a readable prefix, e.g. "usr_9f3c1a...".
std::string newId(const std::string& prefix);

// Strips directory components and characters that are awkward in a URL or
// on disk, keeping the extension.
std::string sanitiseFileName(const std::string& name);

// "1.4 MB" style rendering for attachment chips.
std::string humanFileSize(int64_t bytes);

// Escapes a value for safe inclusion in a JSON string literal.
std::string jsonEscape(const std::string& text);

}  // namespace util
}  // namespace quinco
