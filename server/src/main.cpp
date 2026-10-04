#include <atomic>
#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <limits>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <shellapi.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <spawn.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
extern char** environ;
#endif

#include "http.h"
#include "hub.h"
#include "protocol.h"
#include "store.h"
#include "ws.h"

namespace {

#if defined(_WIN32)
using SocketHandle = SOCKET;
using SocketLength = int;
constexpr SocketHandle kInvalidSocket = INVALID_SOCKET;
constexpr int kSendFlags = 0;

int lastSocketError() { return WSAGetLastError(); }
void configureNoSigPipe(SocketHandle) {}
bool isInterrupted(int error) { return error == WSAEINTR; }
void closeSocket(SocketHandle socket) { ::closesocket(socket); }
void shutdownSocket(SocketHandle socket) { ::shutdown(socket, SD_BOTH); }

class SocketRuntime {
 public:
  SocketRuntime() : ready_(WSAStartup(MAKEWORD(2, 2), &data_) == 0) {}
  ~SocketRuntime() {
    if (ready_) WSACleanup();
  }
  bool ready() const { return ready_; }

 private:
  WSADATA data_{};
  bool ready_;
};
#else
using SocketHandle = int;
using SocketLength = socklen_t;
constexpr SocketHandle kInvalidSocket = -1;
#if defined(MSG_NOSIGNAL)
constexpr int kSendFlags = MSG_NOSIGNAL;
#else
constexpr int kSendFlags = 0;
#endif

int lastSocketError() { return errno; }
void configureNoSigPipe(SocketHandle socket) {
#if defined(SO_NOSIGPIPE)
  int enabled = 1;
  ::setsockopt(socket, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled));
#else
  (void)socket;
#endif
}
bool isInterrupted(int error) { return error == EINTR; }
void closeSocket(SocketHandle socket) { ::close(socket); }
void shutdownSocket(SocketHandle socket) { ::shutdown(socket, SHUT_RDWR); }
#endif

volatile std::sig_atomic_t g_stopping = 0;

void handleSignal(int) { g_stopping = 1; }

bool sendAll(SocketHandle socket, const std::string& bytes) {
  size_t sent = 0;
  while (sent < bytes.size()) {
    const size_t remaining = bytes.size() - sent;
    const int chunk = static_cast<int>(std::min(
        remaining, static_cast<size_t>(std::numeric_limits<int>::max())));
    const auto count = ::send(socket, bytes.data() + sent, chunk, kSendFlags);
    if (count < 0) {
      if (isInterrupted(lastSocketError())) continue;
      return false;
    }
    if (count == 0) return false;
    sent += static_cast<size_t>(count);
  }
  return true;
}

bool hasToken(const std::string& header, const std::string& token) {
  std::string lowered = header;
  for (char& ch : lowered) {
    if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
  }
  size_t start = 0;
  while (start <= lowered.size()) {
    const size_t end = lowered.find(',', start);
    size_t first = start;
    size_t last = end == std::string::npos ? lowered.size() : end;
    while (first < last && lowered[first] == ' ') ++first;
    while (last > first && lowered[last - 1] == ' ') --last;
    if (lowered.substr(first, last - first) == token) return true;
    if (end == std::string::npos) break;
    start = end + 1;
  }
  return false;
}

class SocketLink final : public quinco::ClientLink {
 public:
  SocketLink(SocketHandle socket, uint64_t id) : ClientLink(id), socket_(socket) {}

  bool send(const quinco::json::Value& payload) override {
    return sendRaw(quinco::ws::encodeText(payload.dump()));
  }

  bool sendRaw(const std::string& frame) override {
    std::lock_guard<std::mutex> lock(sendMutex_);
    if (closed_) return false;
    if (sendAll(socket_, frame)) return true;
    closed_ = true;
    shutdownSocket(socket_);
    return false;
  }

  void closeLink() override {
    std::lock_guard<std::mutex> lock(sendMutex_);
    if (closed_) return;
    closed_ = true;
    shutdownSocket(socket_);
  }

 private:
  SocketHandle socket_;
  std::mutex sendMutex_;
  bool closed_ = false;
};

struct ServerContext {
  ServerContext(quinco::Store& storeValue, quinco::Hub& hubValue,
                quinco::Protocol& protocolValue,
                quinco::http::StaticFiles& webFilesValue,
                quinco::http::StaticFiles& uploadFilesValue)
      : store(storeValue),
        hub(hubValue),
        protocol(protocolValue),
        webFiles(webFilesValue),
        uploadFiles(uploadFilesValue) {}

  quinco::Store& store;
  quinco::Hub& hub;
  quinco::Protocol& protocol;
  quinco::http::StaticFiles& webFiles;
  quinco::http::StaticFiles& uploadFiles;
  std::atomic<uint64_t> nextLinkId{1};
  std::mutex clientsMutex;
  std::set<SocketHandle> clients;
};

bool serveWebSocket(SocketHandle socket, const quinco::http::Request& request,
                    ServerContext& context) {
  if (request.method != "GET" || request.path != "/ws" ||
      !hasToken(request.header("Connection"), "upgrade") ||
      !hasToken(request.header("Upgrade"), "websocket") ||
      request.header("Sec-WebSocket-Version") != "13") {
    return false;
  }

  const std::string accept =
      quinco::ws::acceptKey(request.header("Sec-WebSocket-Key"));
  if (accept.empty()) return false;

  const std::string response =
      "HTTP/1.1 101 Switching Protocols\r\n"
      "Upgrade: websocket\r\n"
      "Connection: Upgrade\r\n"
      "Sec-WebSocket-Accept: " + accept + "\r\n\r\n";
  if (!sendAll(socket, response)) return true;

  SocketLink link(socket, context.nextLinkId.fetch_add(1));
  context.hub.addLink(&link);
  context.protocol.sendHello(&link);

  quinco::ws::FrameParser parser;
  char buffer[8192];
  while (!g_stopping) {
    const auto count = ::recv(socket, buffer, static_cast<int>(sizeof(buffer)), 0);
    if (count < 0) {
      if (isInterrupted(lastSocketError())) continue;
      break;
    }
    if (count == 0) break;
    parser.feed(buffer, static_cast<size_t>(count));

    quinco::ws::Frame frame;
    while (parser.next(frame)) {
      if (frame.opcode == quinco::ws::Opcode::Ping) {
        if (!link.sendRaw(quinco::ws::encodeFrame(
                quinco::ws::Opcode::Pong, frame.payload))) {
          break;
        }
      } else if (frame.opcode == quinco::ws::Opcode::Close) {
        link.sendRaw(quinco::ws::encodeFrame(
            quinco::ws::Opcode::Close, frame.payload));
        link.closeLink();
        break;
      } else if (frame.opcode == quinco::ws::Opcode::Text) {
        context.protocol.handleText(&link, frame.payload);
      }
    }
    if (parser.failed()) break;
  }

  context.hub.removeLink(&link);
  context.protocol.handleDisconnect(&link);
  return true;
}

void serveHttp(SocketHandle socket, const quinco::http::Request& request,
               ServerContext& context) {
  using quinco::http::Response;
  Response response;
  if (request.path == "/health" && request.method == "GET") {
    response = Response::json(
        200, "{\"status\":\"ok\",\"service\":\"Quinco Chat\"}");
  } else if (request.path == "/ws") {
    response = Response::error(426, "WebSocket upgrade required");
    response.headers["Upgrade"] = "websocket";
  } else if (request.path.rfind("/uploads/", 0) == 0 &&
             (request.method == "GET" || request.method == "HEAD")) {
    response = context.uploadFiles.serve(
        request.path.substr(std::string("/uploads").size()), request.method,
        std::string());
  } else if (request.method == "GET" || request.method == "HEAD") {
    response = context.webFiles.serve(request.path, request.method,
                                      "/index.html");
  } else {
    response = Response::error(405, "Method Not Allowed");
    response.headers["Allow"] = "GET, HEAD";
  }
  response.keepAlive = false;
  sendAll(socket, response.serialize());
}

void serveClient(SocketHandle socket, ServerContext& context) {
  quinco::http::RequestReader reader;
  char buffer[8192];
  while (!g_stopping) {
    const auto count = ::recv(socket, buffer, static_cast<int>(sizeof(buffer)), 0);
    if (count < 0) {
      if (isInterrupted(lastSocketError())) continue;
      break;
    }
    if (count == 0) break;

    const auto state = reader.feed(buffer, static_cast<size_t>(count));
    if (state == quinco::http::RequestReader::State::NeedMore) continue;
    if (state == quinco::http::RequestReader::State::Error) {
      quinco::http::Response response =
          quinco::http::Response::error(400, reader.error());
      response.keepAlive = false;
      sendAll(socket, response.serialize());
      break;
    }
    if (serveWebSocket(socket, reader.request(), context)) break;
    serveHttp(socket, reader.request(), context);
    break;
  }
}

bool parsePort(const std::string& text, uint16_t& port) {
  char* end = nullptr;
  const long value = std::strtol(text.c_str(), &end, 10);
  if (end == text.c_str() || *end != '\0' || value < 1 || value > 65535) {
    return false;
  }
  port = static_cast<uint16_t>(value);
  return true;
}

#if defined(_WIN32)
std::wstring widen(const std::string& text) {
  if (text.empty()) return {};
  const int size = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
  if (size <= 0) return {};
  std::wstring result(static_cast<size_t>(size), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, result.data(), size);
  result.pop_back();
  return result;
}

bool launchWindowsBrowser(const std::string& url) {
  const std::wstring wideUrl = widen(url);
  std::vector<std::filesystem::path> candidates;
  for (const wchar_t* variable : {L"ProgramFiles", L"ProgramFiles(x86)",
                                  L"LOCALAPPDATA"}) {
    wchar_t value[32768];
    const DWORD length = GetEnvironmentVariableW(variable, value,
                                                  32768);
    if (length == 0 || length >= 32768) continue;
    const std::filesystem::path base(value);
    candidates.push_back(base / L"Google/Chrome/Application/chrome.exe");
    candidates.push_back(base / L"Microsoft/Edge/Application/msedge.exe");
  }

  wchar_t localAppData[32768];
  const DWORD localAppDataLength =
      GetEnvironmentVariableW(L"LOCALAPPDATA", localAppData, 32768);
  std::wstring browserProfile;
  if (localAppDataLength > 0 && localAppDataLength < 32768) {
    const std::filesystem::path profile =
        std::filesystem::path(localAppData) / L"Quinco Chat" / L"Browser Profile";
    std::error_code error;
    std::filesystem::create_directories(profile, error);
    if (!error) browserProfile = profile.wstring();
  }

  for (const std::filesystem::path& browser : candidates) {
    std::error_code error;
    if (!std::filesystem::is_regular_file(browser, error) || error) continue;
    std::wstring command = L"\"" + browser.wstring() +
                           L"\" --no-first-run --new-window";
    if (!browserProfile.empty()) {
      command += L" --user-data-dir=\"" + browserProfile + L"\"";
    }
    command += L" --app=\"" + wideUrl + L"\"";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (CreateProcessW(browser.c_str(), command.data(), nullptr, nullptr, FALSE,
                       CREATE_NEW_PROCESS_GROUP, nullptr, nullptr, &startup,
                       &process)) {
      CloseHandle(process.hThread);
      CloseHandle(process.hProcess);
      return true;
    }
  }

  return reinterpret_cast<intptr_t>(ShellExecuteW(
             nullptr, L"open", wideUrl.c_str(), nullptr, nullptr, SW_SHOWNORMAL)) > 32;
}
#else
bool spawnProgram(const std::string& program,
                  std::vector<std::string> arguments) {
  std::vector<char*> rawArguments;
  rawArguments.reserve(arguments.size() + 1);
  for (std::string& argument : arguments) rawArguments.push_back(argument.data());
  rawArguments.push_back(nullptr);
  pid_t process = 0;
  return posix_spawnp(&process, program.c_str(), nullptr, nullptr,
                      rawArguments.data(), environ) == 0;
}

#if !defined(__APPLE__)
std::string linuxBrowserProfile() {
  const char* configHome = std::getenv("XDG_CONFIG_HOME");
  const char* home = std::getenv("HOME");
  std::filesystem::path base = configHome && *configHome
                                   ? std::filesystem::path(configHome)
                                   : home && *home
                                         ? std::filesystem::path(home) / ".config"
                                         : std::filesystem::temp_directory_path();
  const std::filesystem::path profile =
      base / "quinco-chat" / "browser-profile";
  std::error_code error;
  std::filesystem::create_directories(profile, error);
  return error ? std::string() : profile.string();
}
#endif

bool launchUnixBrowser(const std::string& url) {
#if defined(__APPLE__)
  const char* home = std::getenv("HOME");
  const std::filesystem::path profile = home && *home
      ? std::filesystem::path(home) / "Library/Application Support/Quinco Chat/Browser Profile"
      : std::filesystem::temp_directory_path() / "quinco-chat-browser-profile";
  std::error_code error;
  std::filesystem::create_directories(profile, error);
  if (!error && spawnProgram("open", {"open", "-na", "Google Chrome", "--args",
                                       "--no-first-run", "--user-data-dir=" +
                                           profile.string(), "--app=" + url})) {
    return true;
  }
  return spawnProgram("open", {"open", url});
#else
  const std::string profile = linuxBrowserProfile();
  for (const char* browser : {"google-chrome", "google-chrome-stable",
                              "chromium", "chromium-browser"}) {
    std::vector<std::string> arguments = {
        browser, "--no-first-run", "--no-default-browser-check", "--new-window"};
    if (!profile.empty()) arguments.push_back("--user-data-dir=" + profile);
    arguments.push_back("--app=" + url);
    if (spawnProgram(browser, std::move(arguments))) {
      return true;
    }
  }
  return spawnProgram("xdg-open", {"xdg-open", url});
#endif
}
#endif

bool launchBrowserWindow(const std::string& host, uint16_t port) {
  const std::string browserHost = host == "0.0.0.0" ? "127.0.0.1" : host;
  const std::string url = "http://" + browserHost + ':' + std::to_string(port);
#if defined(_WIN32)
  return launchWindowsBrowser(url);
#else
  return launchUnixBrowser(url);
#endif
}

std::string defaultWebDirectory(const char* executablePath) {
  const char* appDirectory = std::getenv("APPDIR");
  if (appDirectory && *appDirectory) {
    const std::filesystem::path appWeb =
        std::filesystem::path(appDirectory) / "usr/share/quinco-chat/public";
    std::error_code error;
    if (std::filesystem::is_directory(appWeb, error) && !error) {
      return appWeb.string();
    }
  }

  std::error_code error;
  const std::filesystem::path executable =
      std::filesystem::absolute(executablePath, error);
  if (!error) {
    const std::filesystem::path binaryDirectory = executable.parent_path();
    const std::filesystem::path candidates[] = {
        binaryDirectory / "../share/quinco-chat/public",
        binaryDirectory / "public",
        binaryDirectory.parent_path() / "public",
        binaryDirectory.parent_path().parent_path() / "public"};
    for (const std::filesystem::path& candidate : candidates) {
      error.clear();
      if (std::filesystem::is_directory(candidate, error) && !error) {
        return candidate.lexically_normal().string();
      }
    }
  }
  return "public";
}

}  // namespace

int main(int argc, char** argv) {
  std::string host = "127.0.0.1";
  uint16_t port = 8080;
  std::string dataDirectory = "data";
  std::string webDirectory = defaultWebDirectory(argv[0]);
  bool openBrowser = false;
  bool portSpecified = false;
  for (int index = 1; index < argc; ++index) {
    const std::string argument = argv[index];
    if (argument == "--help" || argument == "-h") {
      std::cout << "Usage: quinco-chat-server [--host ADDRESS] [--port PORT] "
                   "[--data DIR] [--web DIR] [--open-browser]\n";
      return 0;
    }
    if (argument == "--open-browser") {
      openBrowser = true;
      continue;
    }
    if (index + 1 >= argc) {
      std::cerr << "Missing value for " << argument << '\n';
      return 2;
    }
    const std::string value = argv[++index];
    if (argument == "--host") {
      host = value;
    } else if (argument == "--port") {
      if (!parsePort(value, port)) {
        std::cerr << "Invalid port: " << value << '\n';
        return 2;
      }
      portSpecified = true;
    } else if (argument == "--data") {
      dataDirectory = value;
    } else if (argument == "--web") {
      webDirectory = value;
    } else {
      std::cerr << "Unknown argument: " << argument << '\n';
      return 2;
    }
  }

#if defined(_WIN32)
  SocketRuntime socketRuntime;
  if (!socketRuntime.ready()) {
    std::cerr << "Could not initialize Windows sockets\n";
    return 1;
  }
#endif

  quinco::Store store(dataDirectory);
  std::string error;
  if (!store.open(error)) {
    std::cerr << "Could not open data store: " << error << '\n';
    return 1;
  }
  quinco::Hub hub;
  quinco::Protocol protocol(store, hub);
  quinco::http::StaticFiles webFiles(webDirectory);
  quinco::http::StaticFiles uploadFiles(store.uploadsDirectory());
  ServerContext context{store, hub, protocol, webFiles, uploadFiles};

  const SocketHandle listener = ::socket(AF_INET, SOCK_STREAM, 0);
  if (listener == kInvalidSocket) {
    std::cerr << "Could not create listening socket\n";
    return 1;
  }
  configureNoSigPipe(listener);
  int reuseAddress = 1;
#if defined(_WIN32)
  ::setsockopt(listener, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char*>(&reuseAddress),
               sizeof(reuseAddress));
#else
  ::setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuseAddress,
               sizeof(reuseAddress));
#endif

  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(port);
  if (::inet_pton(AF_INET, host.c_str(), &address.sin_addr) != 1) {
    std::cerr << "Could not listen on " << host << ':' << port << '\n';
    closeSocket(listener);
    return 1;
  }

  bool bound = ::bind(listener, reinterpret_cast<sockaddr*>(&address),
                      static_cast<SocketLength>(sizeof(address))) == 0;
  if (!bound && openBrowser && !portSpecified && port == 8080) {
    for (uint16_t candidate = 8081; candidate <= 8099; ++candidate) {
      address.sin_port = htons(candidate);
      if (::bind(listener, reinterpret_cast<sockaddr*>(&address),
                 static_cast<SocketLength>(sizeof(address))) == 0) {
        port = candidate;
        bound = true;
        std::cerr << "Port 8080 is busy; using port " << port << '\n';
        break;
      }
    }
  }
  if (!bound || ::listen(listener, 128) < 0) {
    std::cerr << "Could not listen on " << host << ':' << port << '\n';
    closeSocket(listener);
    return 1;
  }

  std::signal(SIGINT, handleSignal);
  std::signal(SIGTERM, handleSignal);

  std::vector<std::thread> workers;
  std::cout << "Quinco Chat listening on http://" << host << ':' << port
            << " (WebSocket: /ws, health: /health)\n";
  std::cout << "Serving web files from " << webDirectory << '\n';
  if (openBrowser && !launchBrowserWindow(host, port)) {
    std::cerr << "Could not open a browser window; visit http://127.0.0.1:"
              << port << '\n';
  }
  while (!g_stopping) {
    fd_set readable;
    FD_ZERO(&readable);
    FD_SET(listener, &readable);
    timeval timeout{1, 0};
#if defined(_WIN32)
    const int ready = ::select(0, &readable, nullptr, nullptr, &timeout);
#else
    const int ready = ::select(listener + 1, &readable, nullptr, nullptr, &timeout);
#endif
    if (ready == 0) continue;
    if (ready < 0) {
      if (isInterrupted(lastSocketError())) continue;
      if (!g_stopping) std::cerr << "Socket wait failed\n";
      break;
    }
    const SocketHandle client = ::accept(listener, nullptr, nullptr);
    if (client == kInvalidSocket) {
      if (isInterrupted(lastSocketError())) continue;
      if (!g_stopping) std::cerr << "Accept failed\n";
      break;
    }
    configureNoSigPipe(client);
    {
      std::lock_guard<std::mutex> lock(context.clientsMutex);
      context.clients.insert(client);
    }
    workers.emplace_back([client, &context] {
      serveClient(client, context);
      {
        std::lock_guard<std::mutex> lock(context.clientsMutex);
        context.clients.erase(client);
      }
      closeSocket(client);
    });
  }

  closeSocket(listener);
  {
    std::lock_guard<std::mutex> lock(context.clientsMutex);
    for (SocketHandle client : context.clients) shutdownSocket(client);
  }
  for (std::thread& worker : workers) worker.join();
  return 0;
}