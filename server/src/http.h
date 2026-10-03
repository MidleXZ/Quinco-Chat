// Minimal HTTP/1.1 layer for Quinco Chat.
//
// Responsibilities:
//   * parse requests incrementally (headers may span many TCP reads)
//   * honour Content-Length bodies, with hard caps
//   * serialise responses, including keep-alive semantics
//   * serve the static SPA bundle from disk with path-traversal guards
//
// It deliberately does not implement chunked transfer decoding: browsers
// only use chunked for request bodies we never need, and every response this
// server writes has a known length.

#pragma once

#include <cstdint>
#include <map>
#include <string>

namespace quinco {
namespace http {

// Limits that keep a hostile client from exhausting memory.
constexpr size_t kMaxHeaderBytes = 32u * 1024u;
constexpr size_t kMaxBodyBytes = 12u * 1024u * 1024u;

constexpr size_t kDefaultQueryLimit = 100;

struct Request {
  std::string method;
  std::string target;   // exactly as sent, e.g. "/chat?room=1"
  std::string path;     // percent-decoded, query stripped
  std::string query;    // raw query string, no leading '?'
  std::string body;
  std::map<std::string, std::string> headers;  // keys lowercased
  std::map<std::string, std::string> params;   // decoded query params
  std::string clientIp;
  bool keepAlive = true;

  // Case-insensitive header lookup.
  std::string header(const std::string& name) const;
  bool hasHeader(const std::string& name) const;
  // Query parameter lookup with a default.
  std::string param(const std::string& name,
                    const std::string& fallback = std::string()) const;
  int64_t paramInt(const std::string& name, int64_t fallback) const;
  std::string contentType() const;
};

// Incremental request reader. Push bytes from the socket, poll for state.
class RequestReader {
 public:
  enum class State { NeedMore, Ready, Error };

  State feed(const char* data, size_t length);
  State state() const { return state_; }
  const std::string& error() const { return error_; }
  const Request& request() const { return request_; }

  // Number of bytes already consumed from the stream for this request, so a
  // keep-alive loop can discard exactly the prefix it has processed.
  size_t consumed() const { return consumed_; }

  // Ready the reader for the next request on the same connection.
  void reset();

 private:
  bool parseHead();

  std::string buffer_;
  Request request_;
  State state_ = State::NeedMore;
  std::string error_;
  size_t headerEnd_ = 0;
  size_t expectedTotal_ = 0;
  size_t consumed_ = 0;
};

struct Response {
  int status = 200;
  std::string contentType = "text/plain; charset=utf-8";
  std::map<std::string, std::string> headers;
  std::string body;
  bool keepAlive = true;
  // Populated for HEAD requests so the body is omitted but the length kept.
  bool omitBody = false;

  static Response text(int status, const std::string& body);
  static Response html(int status, const std::string& body);
  static Response json(int status, const std::string& body);
  static Response notFound(const std::string& detail = std::string());
  static Response error(int status, const std::string& detail);

  std::string serialize() const;
};

// Serves files beneath a root directory.
class StaticFiles {
 public:
  explicit StaticFiles(std::string root);

  // Maps a URL path to a file and builds the response. When the path names a
  // directory or is missing, `fallback` (an absolute URL path, normally
  // "/index.html") is served instead so the SPA can route client-side.
  Response serve(const std::string& urlPath, const std::string& method,
                 const std::string& fallback) const;

  bool isRegularFile(const std::string& urlPath) const;

 private:
  // Resolves a URL path to an absolute filesystem path. Returns false when
  // the path escapes the root or contains a NUL byte.
  bool resolve(const std::string& urlPath, std::string& out) const;

  std::string root_;
};

// Percent-decodes a URL component. Invalid escapes are passed through.
std::string urlDecode(const std::string& text);

// Percent-encodes everything outside the unreserved set.
std::string urlEncode(const std::string& text);

std::string mimeTypeFor(const std::string& path);
const char* statusText(int code);

// Formats an HTTP date header value (RFC 7231 IMF-fixdate).
std::string httpDate();

}  // namespace http
}  // namespace quinco
