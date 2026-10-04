// HTTP/1.1 implementation for Quinco Chat (see http.h).

#include "http.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <utility>

namespace quinco {
namespace http {
namespace {

constexpr const char* kServerName = "QuincoChat/1.0";

bool isRegularFilePath(const std::string& path) {
  std::error_code error;
  return std::filesystem::is_regular_file(path, error) && !error;
}

std::string toLower(const std::string& text) {
  std::string out = text;
  std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return out;
}

std::string trim(const std::string& text) {
  size_t begin = 0;
  size_t end = text.size();
  while (begin < end && (text[begin] == ' ' || text[begin] == '\t')) ++begin;
  while (end > begin &&
         (text[end - 1] == ' ' || text[end - 1] == '\t' ||
          text[end - 1] == '\r' || text[end - 1] == '\n')) {
    --end;
  }
  return text.substr(begin, end - begin);
}

int hexDigitValue(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

bool isUnreserved(unsigned char c) {
  return std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~';
}

struct MimeEntry {
  const char* extension;
  const char* type;
};

// Extensions the SPA actually ships, plus a few media types it can serve
// back for uploaded attachments.
const MimeEntry kMimeTable[] = {
    {"html", "text/html; charset=utf-8"},
    {"htm", "text/html; charset=utf-8"},
    {"js", "text/javascript; charset=utf-8"},
    {"mjs", "text/javascript; charset=utf-8"},
    {"css", "text/css; charset=utf-8"},
    {"json", "application/json; charset=utf-8"},
    {"map", "application/json; charset=utf-8"},
    {"svg", "image/svg+xml"},
    {"png", "image/png"},
    {"jpg", "image/jpeg"},
    {"jpeg", "image/jpeg"},
    {"gif", "image/gif"},
    {"webp", "image/webp"},
    {"avif", "image/avif"},
    {"ico", "image/x-icon"},
    {"woff", "font/woff"},
    {"woff2", "font/woff2"},
    {"ttf", "font/ttf"},
    {"otf", "font/otf"},
    {"mp3", "audio/mpeg"},
    {"ogg", "audio/ogg"},
    {"wav", "audio/wav"},
    {"webm", "video/webm"},
    {"mp4", "video/mp4"},
    {"txt", "text/plain; charset=utf-8"},
    {"webmanifest", "application/manifest+json"},
};

bool readWholeFile(const std::string& path, std::string& out) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) return false;
  std::ostringstream buffer;
  buffer << stream.rdbuf();
  if (stream.bad()) return false;
  out = buffer.str();
  return true;
}

}  // namespace

std::string urlDecode(const std::string& text) {
  std::string out;
  out.reserve(text.size());
  for (size_t i = 0; i < text.size(); ++i) {
    const char c = text[i];
    if (c == '+') {
      // application/x-www-form-urlencoded treats '+' as a space in the
      // query string. Path segments keep it literal, but a literal '+' in a
      // path is harmless to normalise the same way for our routes.
      out.push_back(' ');
      continue;
    }
    if (c != '%' || i + 2 >= text.size()) {
      out.push_back(c);
      continue;
    }
    const int high = hexDigitValue(text[i + 1]);
    const int low = hexDigitValue(text[i + 2]);
    if (high < 0 || low < 0) {
      out.push_back(c);
      continue;
    }
    out.push_back(static_cast<char>((high << 4) | low));
    i += 2;
  }
  return out;
}

std::string urlEncode(const std::string& text) {
  static const char* kHex = "0123456789ABCDEF";
  std::string out;
  out.reserve(text.size());
  for (unsigned char c : text) {
    if (isUnreserved(c)) {
      out.push_back(static_cast<char>(c));
    } else {
      out.push_back('%');
      out.push_back(kHex[(c >> 4) & 0xF]);
      out.push_back(kHex[c & 0xF]);
    }
  }
  return out;
}

std::string mimeTypeFor(const std::string& path) {
  const size_t dot = path.find_last_of('.');
  if (dot == std::string::npos) return "application/octet-stream";
  const std::string extension = toLower(path.substr(dot + 1));
  for (const MimeEntry& entry : kMimeTable) {
    if (extension == entry.extension) return entry.type;
  }
  return "application/octet-stream";
}

const char* statusText(int code) {
  switch (code) {
    case 200: return "OK";
    case 201: return "Created";
    case 204: return "No Content";
    case 206: return "Partial Content";
    case 301: return "Moved Permanently";
    case 302: return "Found";
    case 304: return "Not Modified";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 408: return "Request Timeout";
    case 409: return "Conflict";
    case 413: return "Payload Too Large";
    case 415: return "Unsupported Media Type";
    case 422: return "Unprocessable Entity";
    case 426: return "Upgrade Required";
    case 429: return "Too Many Requests";
    case 431: return "Request Header Fields Too Large";
    case 500: return "Internal Server Error";
    case 501: return "Not Implemented";
    case 503: return "Service Unavailable";
    default: return "Unknown";
  }
}

std::string httpDate() {
  std::time_t now = std::time(nullptr);
  std::tm utc{};
#if defined(_WIN32)
  gmtime_s(&utc, &now);
#else
  gmtime_r(&now, &utc);
#endif
  char buffer[64];
  // RFC 7231 IMF-fixdate, e.g. "Sun, 06 Nov 1994 08:49:37 GMT".
  std::strftime(buffer, sizeof(buffer), "%a, %d %b %Y %H:%M:%S GMT", &utc);
  return buffer;
}

// ---------------------------------------------------------------------------
// Request
// ---------------------------------------------------------------------------

std::string Request::header(const std::string& name) const {
  auto it = headers.find(toLower(name));
  return it == headers.end() ? std::string() : it->second;
}

bool Request::hasHeader(const std::string& name) const {
  return headers.find(toLower(name)) != headers.end();
}

std::string Request::param(const std::string& name,
                           const std::string& fallback) const {
  auto it = params.find(name);
  return it == params.end() ? fallback : it->second;
}

int64_t Request::paramInt(const std::string& name, int64_t fallback) const {
  const std::string raw = param(name);
  if (raw.empty()) return fallback;
  char* end = nullptr;
  const long long value = std::strtoll(raw.c_str(), &end, 10);
  if (end == raw.c_str() || (end && *end != '\0')) return fallback;
  return static_cast<int64_t>(value);
}

std::string Request::contentType() const { return header("Content-Type"); }

// ---------------------------------------------------------------------------
// Response
// ---------------------------------------------------------------------------

Response Response::text(int status, const std::string& body) {
  Response response;
  response.status = status;
  response.contentType = "text/plain; charset=utf-8";
  response.body = body;
  return response;
}

Response Response::html(int status, const std::string& body) {
  Response response;
  response.status = status;
  response.contentType = "text/html; charset=utf-8";
  response.body = body;
  return response;
}

Response Response::json(int status, const std::string& body) {
  Response response;
  response.status = status;
  response.contentType = "application/json; charset=utf-8";
  response.body = body;
  return response;
}

Response Response::notFound(const std::string& detail) {
  return Response::text(404, detail.empty() ? "Not Found" : detail);
}

Response Response::error(int status, const std::string& detail) {
  return Response::text(status, detail);
}

std::string Response::serialize() const {
  std::string out;
  out.reserve(body.size() + 256);
  out += "HTTP/1.1 ";
  out += std::to_string(status);
  out.push_back(' ');
  out += statusText(status);
  out += "\r\n";

  bool hasContentType = false;
  bool hasDate = false;
  for (const auto& entry : headers) {
    if (toLower(entry.first) == "content-type") hasContentType = true;
    if (toLower(entry.first) == "date") hasDate = true;
    out += entry.first;
    out += ": ";
    out += entry.second;
    out += "\r\n";
  }
  if (!hasContentType && !contentType.empty()) {
    out += "Content-Type: ";
    out += contentType;
    out += "\r\n";
  }
  if (!hasDate) {
    out += "Date: ";
    out += httpDate();
    out += "\r\n";
  }
  out += "Server: ";
  out += kServerName;
  out += "\r\n";
  out += "Content-Length: ";
  out += std::to_string(body.size());
  out += "\r\n";
  out += keepAlive ? "Connection: keep-alive\r\n" : "Connection: close\r\n";
  out += "\r\n";
  if (!omitBody) out += body;
  return out;
}
// ---------------------------------------------------------------------------
// RequestReader
// ---------------------------------------------------------------------------

namespace {

// Splits a raw request target into a decoded path and its raw query string.
void splitTarget(const std::string& target, std::string& path,
                 std::string& query) {
  const size_t question = target.find('?');
  if (question == std::string::npos) {
    path = urlDecode(target);
    query.clear();
    return;
  }
  path = urlDecode(target.substr(0, question));
  query = target.substr(question + 1);
}

}  // namespace

RequestReader::State RequestReader::feed(const char* data, size_t length) {
  if (state_ != State::NeedMore) return state_;
  buffer_.append(data, length);

  if (buffer_.size() > kMaxHeaderBytes + kMaxBodyBytes) {
    state_ = State::Error;
    error_ = "request exceeds maximum size";
    return state_;
  }

  if (headerEnd_ == 0) {
    if (buffer_.size() > kMaxHeaderBytes) {
      state_ = State::Error;
      error_ = "request headers too large";
      return state_;
    }
    if (!parseHead()) return State::NeedMore;
    if (state_ == State::Error) return state_;
  }

  if (buffer_.size() < expectedTotal_) return State::NeedMore;
  request_.body = buffer_.substr(headerEnd_, expectedTotal_ - headerEnd_);
  consumed_ = expectedTotal_;
  state_ = State::Ready;
  return state_;
}

bool RequestReader::parseHead() {
  // Accept both CRLF and bare LF terminators; some clients and most test
  // harnesses use the short form.
  size_t headerBlockEnd = buffer_.find("\r\n\r\n");
  size_t delimiterLength = 4;
  if (headerBlockEnd == std::string::npos) {
    headerBlockEnd = buffer_.find("\n\n");
    delimiterLength = 2;
  }
  if (headerBlockEnd == std::string::npos) return false;

  headerEnd_ = headerBlockEnd + delimiterLength;

  // --- request line -----------------------------------------------------
  const size_t lineEnd = buffer_.find('\n', 0);
  if (lineEnd == std::string::npos || lineEnd >= headerEnd_) {
    state_ = State::Error;
    error_ = "malformed request line";
    return true;
  }
  const std::string requestLine = trim(buffer_.substr(0, lineEnd));

  const size_t firstSpace = requestLine.find(' ');
  const size_t secondSpace =
      firstSpace == std::string::npos
          ? std::string::npos
          : requestLine.find(' ', firstSpace + 1);
  if (firstSpace == std::string::npos || secondSpace == std::string::npos) {
    state_ = State::Error;
    error_ = "malformed request line";
    return true;
  }

  request_.method = requestLine.substr(0, firstSpace);
  request_.target =
      requestLine.substr(firstSpace + 1, secondSpace - firstSpace - 1);
  const std::string version = requestLine.substr(secondSpace + 1);

  if (request_.target.empty() || request_.target[0] != '/') {
    state_ = State::Error;
    error_ = "unsupported request target";
    return true;
  }
  if (version != "HTTP/1.1" && version != "HTTP/1.0") {
    state_ = State::Error;
    error_ = "unsupported HTTP version";
    return true;
  }
  request_.keepAlive = (version == "HTTP/1.1");

  // --- header fields ----------------------------------------------------
  size_t cursor = lineEnd + 1;
  while (cursor < headerBlockEnd) {
    size_t end = buffer_.find('\n', cursor);
    if (end == std::string::npos || end > headerBlockEnd) end = headerBlockEnd;
    const std::string line = trim(buffer_.substr(cursor, end - cursor));
    cursor = end + 1;
    if (line.empty()) continue;
    const size_t colon = line.find(':');
    if (colon == std::string::npos) continue;
    const std::string name = toLower(trim(line.substr(0, colon)));
    if (name.empty()) continue;
    request_.headers[name] = trim(line.substr(colon + 1));
  }

  const std::string connection = toLower(request_.header("Connection"));
  if (connection.find("close") != std::string::npos) request_.keepAlive = false;
  if (connection.find("keep-alive") != std::string::npos) {
    request_.keepAlive = true;
  }

  if (toLower(request_.header("Transfer-Encoding")).find("chunked") !=
      std::string::npos) {
    state_ = State::Error;
    error_ = "chunked request bodies are not supported";
    return true;
  }

  size_t contentLength = 0;
  if (request_.hasHeader("Content-Length")) {
    const std::string raw = request_.header("Content-Length");
    char* end = nullptr;
    const unsigned long long parsed = std::strtoull(raw.c_str(), &end, 10);
    if (end == raw.c_str() || (end && *end != '\0')) {
      state_ = State::Error;
      error_ = "invalid Content-Length";
      return true;
    }
    if (parsed > kMaxBodyBytes) {
      state_ = State::Error;
      error_ = "request body too large";
      return true;
    }
    contentLength = static_cast<size_t>(parsed);
  }
  expectedTotal_ = headerEnd_ + contentLength;

  // --- target decomposition --------------------------------------------
  splitTarget(request_.target, request_.path, request_.query);
  if (!request_.query.empty()) {
    size_t start = 0;
    while (start <= request_.query.size()) {
      const size_t ampersand = request_.query.find('&', start);
      const std::string pair = request_.query.substr(
          start, ampersand == std::string::npos ? std::string::npos
                                                : ampersand - start);
      if (!pair.empty()) {
        const size_t equals = pair.find('=');
        const std::string key = urlDecode(
            equals == std::string::npos ? pair : pair.substr(0, equals));
        const std::string value =
            equals == std::string::npos ? std::string()
                                        : urlDecode(pair.substr(equals + 1));
        if (!key.empty() && request_.params.size() < kDefaultQueryLimit) {
          request_.params.emplace(key, value);
        }
      }
      if (ampersand == std::string::npos) break;
      start = ampersand + 1;
    }
  }

  return true;
}

void RequestReader::reset() {
  buffer_.clear();
  request_ = Request();
  state_ = State::NeedMore;
  error_.clear();
  headerEnd_ = 0;
  expectedTotal_ = 0;
  consumed_ = 0;
}
// ---------------------------------------------------------------------------
// StaticFiles
// ---------------------------------------------------------------------------

StaticFiles::StaticFiles(std::string root) : root_(std::move(root)) {
  while (root_.size() > 1 && root_.back() == '/') root_.pop_back();
}

bool StaticFiles::resolve(const std::string& urlPath, std::string& out) const {
  if (urlPath.find('\0') != std::string::npos) return false;

  size_t start = 0;
  while (start < urlPath.size() && urlPath[start] == '/') ++start;

  // Walk the segments, rejecting any that would climb out of the root.
  std::string cleaned;
  size_t cursor = start;
  while (cursor <= urlPath.size()) {
    const size_t slash = urlPath.find('/', cursor);
    const std::string segment =
        urlPath.substr(cursor, slash == std::string::npos ? std::string::npos
                                                          : slash - cursor);
    if (segment == "..") return false;
    if (!segment.empty() && segment != ".") {
      if (!cleaned.empty()) cleaned.push_back('/');
      cleaned += segment;
    }
    if (slash == std::string::npos) break;
    cursor = slash + 1;
  }

  if (cleaned.empty()) cleaned = "index.html";
  out = root_;
  out.push_back('/');
  out += cleaned;
  return true;
}

bool StaticFiles::isRegularFile(const std::string& urlPath) const {
  std::string full;
  if (!resolve(urlPath, full)) return false;
  return isRegularFilePath(full);
}

Response StaticFiles::serve(const std::string& urlPath,
                            const std::string& method,
                            const std::string& fallback) const {
  std::string full;
  bool found = resolve(urlPath, full) && isRegularFilePath(full);

  // Client-side routes resolve to the SPA shell.
  if (!found && !fallback.empty() && fallback != urlPath) {
    std::string fallbackFull;
    if (resolve(fallback, fallbackFull) && isRegularFilePath(fallbackFull)) {
      full = fallbackFull;
      found = true;
    }
  }

  if (!found) {
    // A traversal attempt is a client error rather than a missing file.
    std::string ignored;
    if (!resolve(urlPath, ignored)) return Response::error(403, "Forbidden");
    return Response::notFound("Not Found");
  }

  std::string body;
  if (!readWholeFile(full, body)) {
    return Response::error(500, "Failed to read asset");
  }

  Response response;
  response.status = 200;
  response.contentType = mimeTypeFor(full);
  response.body = std::move(body);
  response.omitBody = (method == "HEAD");
  // The HTML shell must always be revalidated so a rebuilt bundle is picked
  // up immediately; everything else can sit in the cache briefly.
  const bool isDocument = response.contentType.rfind("text/html", 0) == 0;
    const bool isClientCode =
      response.contentType.rfind("text/javascript", 0) == 0;
    response.headers["Cache-Control"] = isDocument || isClientCode
                        ? "no-cache"
                        : "public, max-age=300";
  return response;
}

}  // namespace http
}  // namespace quinco
