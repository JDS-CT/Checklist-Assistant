#pragma once

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace platform {

enum class HttpMethod { kGet = 0, kPost, kOptions, kPatch, kDelete };

struct HttpRequest {
  std::string path;
  std::string body;
  std::map<std::string, std::string> query_params;
  std::map<std::string, std::string> headers;
  std::string remote_address;
  std::vector<std::string> path_params;
};

struct HttpResponse {
  int status = 200;
  std::string content_type = "application/json";
  std::string body;
  std::map<std::string, std::string> headers;
};

using HttpHandler = std::function<HttpResponse(const HttpRequest&)>;

class HttpServer {
 public:
  HttpServer();
  ~HttpServer();

  HttpServer(const HttpServer&) = delete;
  HttpServer& operator=(const HttpServer&) = delete;

  void AddHandler(HttpMethod method, const std::string& path, HttpHandler handler);
  void Start(const std::string& host, int port);
  void Stop();

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace platform
