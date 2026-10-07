#include "http_server.h"

#include <cctype>
#include <cstring>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
using socklen_t = int;
#define VIRGIL_CLOSE closesocket
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#define VIRGIL_CLOSE ::close
#endif

#include "virgil/log.h"
#include "virgil/net.h"

namespace virgil {

namespace {

constexpr size_t kMaxHeader = 16 * 1024;
constexpr size_t kMaxBody = 256 * 1024;

const char* reason(int status) {
  switch (status) {
    case 200: return "OK";
    case 204: return "No Content";
    case 400: return "Bad Request";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 413: return "Payload Too Large";
    case 500: return "Internal Server Error";
    case 503: return "Service Unavailable";
    default: return "Error";
  }
}

void set_timeout(intptr_t fd, int ms) {
#if defined(_WIN32)
  DWORD v = DWORD(ms);
  setsockopt(SOCKET(fd), SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&v), sizeof v);
  setsockopt(SOCKET(fd), SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&v), sizeof v);
#else
  timeval tv{ms / 1000, (ms % 1000) * 1000};
  setsockopt(int(fd), SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
  setsockopt(int(fd), SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
#endif
}

bool send_all(intptr_t fd, const std::string& data) {
  size_t off = 0;
  while (off < data.size()) {
    const int n = int(send(decltype(socket(0, 0, 0))(fd), data.data() + off,
                           int(data.size() - off), 0));
    if (n <= 0) return false;
    off += size_t(n);
  }
  return true;
}

std::string lower(std::string s) {
  for (auto& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

std::string trim(const std::string& s) {
  size_t b = 0, e = s.size();
  while (b < e && (s[b] == ' ' || s[b] == '\t')) ++b;
  while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r')) --e;
  return s.substr(b, e - b);
}

}  // namespace

bool HttpServer::start(uint32_t addr, uint16_t port, Handler handler) {
  stop();
  UdpSocket warm;  // initialises Winsock via the shared helper
  warm.open(0, false);
  warm.close();

  auto fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (fd == decltype(fd)(-1)) return false;
  int one = 1;
#if !defined(_WIN32)
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
#else
  // Windows SO_REUSEADDR allows port hijacking; use exclusive use instead.
  setsockopt(fd, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&one), sizeof one);
#endif
  sockaddr_in sa{};
  sa.sin_family = AF_INET;
  sa.sin_port = htons(port);
  sa.sin_addr.s_addr = htonl(addr);
  if (bind(fd, reinterpret_cast<sockaddr*>(&sa), sizeof sa) != 0 || listen(fd, 16) != 0) {
    VIRGIL_CLOSE(fd);
    return false;
  }
  listen_fd_ = intptr_t(fd);
  handler_ = std::move(handler);
  running_ = true;
  thread_ = std::thread([this] { run(); });
  return true;
}

void HttpServer::stop() {
  running_ = false;
  if (thread_.joinable()) thread_.join();
  if (listen_fd_ != -1) VIRGIL_CLOSE(decltype(socket(0, 0, 0))(listen_fd_));
  listen_fd_ = -1;
}

void HttpServer::run() {
  const auto lfd = decltype(socket(0, 0, 0))(listen_fd_);
  while (running_) {
    fd_set set;
    FD_ZERO(&set);
    FD_SET(lfd, &set);
    timeval tv{0, 200000};
    if (select(int(lfd) + 1, &set, nullptr, nullptr, &tv) <= 0) continue;
    sockaddr_in peer{};
    socklen_t len = sizeof peer;
    auto c = accept(lfd, reinterpret_cast<sockaddr*>(&peer), &len);
    if (c == decltype(c)(-1)) continue;
    serve(intptr_t(c));
    VIRGIL_CLOSE(c);
  }
}

void HttpServer::serve(intptr_t fd) {
  set_timeout(fd, 2000);
  const auto sfd = decltype(socket(0, 0, 0))(fd);
  std::string buf;
  char chunk[4096];
  size_t header_end = std::string::npos;
  while (header_end == std::string::npos) {
    const int n = int(recv(sfd, chunk, sizeof chunk, 0));
    if (n <= 0) return;
    buf.append(chunk, size_t(n));
    header_end = buf.find("\r\n\r\n");
    if (header_end == std::string::npos && buf.size() > kMaxHeader) return;
  }

  HttpRequest req;
  {
    size_t line_end = buf.find("\r\n");
    const std::string request_line = buf.substr(0, line_end);
    const size_t sp1 = request_line.find(' ');
    const size_t sp2 = request_line.find(' ', sp1 + 1);
    if (sp1 == std::string::npos || sp2 == std::string::npos) return;
    req.method = request_line.substr(0, sp1);
    std::string target = request_line.substr(sp1 + 1, sp2 - sp1 - 1);
    const size_t q = target.find('?');
    req.path = target.substr(0, q);
    if (q != std::string::npos) req.query = target.substr(q + 1);
    size_t pos = line_end + 2;
    while (pos < header_end) {
      size_t e = buf.find("\r\n", pos);
      const std::string line = buf.substr(pos, e - pos);
      const size_t colon = line.find(':');
      if (colon != std::string::npos)
        req.headers[lower(trim(line.substr(0, colon)))] = trim(line.substr(colon + 1));
      pos = e + 2;
    }
  }

  HttpResponse resp;
  const size_t want = size_t(std::strtoull(req.header("content-length").c_str(), nullptr, 10));
  if (want > kMaxBody) {
    resp.status = 413;
    resp.body = "{\"error\":\"request too large\"}";
  } else {
    req.body = buf.substr(header_end + 4);
    while (req.body.size() < want) {
      const int n = int(recv(sfd, chunk, sizeof chunk, 0));
      if (n <= 0) return;
      req.body.append(chunk, size_t(n));
    }
    req.body.resize(want);
    try {
      resp = handler_(req);
    } catch (const std::exception& e) {
      resp.status = 500;
      resp.body = std::string("{\"error\":\"internal error\"}");
      VIRGIL_LOG_WARN("control: handler failed: %s", e.what());
    }
  }

  std::string out = "HTTP/1.1 " + std::to_string(resp.status) + " " + reason(resp.status) + "\r\n";
  out += "Content-Type: " + resp.content_type + "\r\n";
  out += "Content-Length: " + std::to_string(resp.body.size()) + "\r\n";
  out += "Cache-Control: no-store\r\n";
  out += "X-Content-Type-Options: nosniff\r\n";
  out += "X-Frame-Options: DENY\r\n";
  out += "Connection: close\r\n";
  out += resp.extra_headers;
  out += "\r\n";
  if (req.method != "HEAD") out += resp.body;
  send_all(fd, out);
}

}  // namespace virgil
