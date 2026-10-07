// Minimal single-threaded HTTP/1.1 server for the local control panel.
// One request per connection (Connection: close); bodies up to 256 KiB.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <thread>

namespace dsv {

struct HttpRequest {
  std::string method;
  std::string path;   // without query string
  std::string query;  // after '?', undecoded
  std::map<std::string, std::string> headers;  // names lower-cased
  std::string body;

  std::string header(const std::string& name) const {
    auto it = headers.find(name);
    return it == headers.end() ? std::string() : it->second;
  }
};

struct HttpResponse {
  int status = 200;
  std::string content_type = "application/json";
  std::string body;
  std::string extra_headers;  // "Name: value\r\n" lines
};

class HttpServer {
 public:
  using Handler = std::function<HttpResponse(const HttpRequest&)>;
  ~HttpServer() { stop(); }
  // Binds addr:port (host byte order). Returns false if the port is taken.
  bool start(uint32_t addr, uint16_t port, Handler handler);
  void stop();

 private:
  void run();
  void serve(intptr_t client);

  intptr_t listen_fd_ = -1;
  Handler handler_;
  std::thread thread_;
  std::atomic<bool> running_{false};
};

}  // namespace dsv
