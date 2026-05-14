#include "platform/http_client.hpp"

#include <cctype>
#include <map>
#include <stdexcept>
#include <string>

#include "httplib.h"

namespace {

std::string Trim(const std::string& value) {
  std::size_t first = 0;
  std::size_t last = value.size();
  while (first < value.size() && std::isspace(static_cast<unsigned char>(value[first]))) {
    ++first;
  }
  while (last > first && std::isspace(static_cast<unsigned char>(value[last - 1]))) {
    --last;
  }
  return value.substr(first, last - first);
}

struct ParsedUrl {
  std::string scheme;
  std::string host;
  int port = 80;
};

ParsedUrl ParseUrl(const std::string& base_url) {
  ParsedUrl parsed;
  const auto scheme_end = base_url.find("://");
  if (scheme_end == std::string::npos) {
    throw std::invalid_argument("Base URL must include a scheme (e.g., http://127.0.0.1:8080)");
  }

  parsed.scheme = base_url.substr(0, scheme_end);
  auto remainder = base_url.substr(scheme_end + 3);
  const auto slash_pos = remainder.find('/');
  if (slash_pos != std::string::npos) {
    remainder = remainder.substr(0, slash_pos);
  }

  const auto colon_pos = remainder.find(':');
  if (colon_pos == std::string::npos) {
    parsed.host = remainder;
    parsed.port = (parsed.scheme == "https") ? 443 : 80;
  } else {
    parsed.host = remainder.substr(0, colon_pos);
    const auto port_str = remainder.substr(colon_pos + 1);
    parsed.port = std::stoi(port_str);
    if (parsed.port <= 0 || parsed.port > 65535) {
      throw std::invalid_argument("Port extracted from base URL is invalid");
    }
  }
  return parsed;
}

std::string UrlEncode(const std::string& value) {
  static constexpr char kHex[] = "0123456789ABCDEF";
  std::string encoded;
  for (unsigned char ch : value) {
    if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ||
        ch == '-' || ch == '_' || ch == '.' || ch == '~') {
      encoded.push_back(static_cast<char>(ch));
    } else {
      encoded.push_back('%');
      encoded.push_back(kHex[(ch >> 4) & 0x0F]);
      encoded.push_back(kHex[ch & 0x0F]);
    }
  }
  return encoded;
}

httplib::Headers ToHeaders(const std::map<std::string, std::string>& headers) {
  httplib::Headers converted;
  for (const auto& entry : headers) {
    converted.emplace(entry.first, entry.second);
  }
  if (converted.find("Connection") == converted.end()) {
    converted.emplace("Connection", "close");
  }
  return converted;
}

}  // namespace

namespace platform {

HttpClient::HttpClient(std::string base_url) {
  auto parsed = ParseUrl(base_url);
  scheme_ = std::move(parsed.scheme);
  host_ = std::move(parsed.host);
  port_ = parsed.port;
  if (scheme_ != "http") {
    throw std::invalid_argument("Only http:// URLs are supported for the MCP client bridge");
  }
}

HttpClientResponse HttpClient::Get(const std::string& path,
                                   const std::map<std::string, std::string>& query,
                                   const std::map<std::string, std::string>& headers) const {
  httplib::Client client(host_.c_str(), port_);
  client.set_connection_timeout(timeout_seconds_);
  client.set_read_timeout(timeout_seconds_);
  client.set_write_timeout(timeout_seconds_);
  client.set_keep_alive(false);

  const auto target = BuildTarget(path, query);
  auto response = client.Get(target.c_str(), ToHeaders(headers));
  if (!response) {
    Raise(target);
  }
  return ConvertResponse(*response);
}

HttpClientResponse HttpClient::Post(const std::string& path, const std::string& body,
                                    const std::map<std::string, std::string>& query,
                                    const std::string& content_type,
                                    const std::map<std::string, std::string>& headers) const {
  httplib::Client client(host_.c_str(), port_);
  client.set_connection_timeout(timeout_seconds_);
  client.set_read_timeout(timeout_seconds_);
  client.set_write_timeout(timeout_seconds_);
  client.set_keep_alive(false);

  const auto target = BuildTarget(path, query);
  auto response = client.Post(target.c_str(), ToHeaders(headers), body, content_type.c_str());
  if (!response) {
    Raise(target);
  }
  return ConvertResponse(*response);
}

HttpClientResponse HttpClient::Patch(const std::string& path, const std::string& body,
                                     const std::map<std::string, std::string>& query,
                                     const std::string& content_type,
                                     const std::map<std::string, std::string>& headers) const {
  httplib::Client client(host_.c_str(), port_);
  client.set_connection_timeout(timeout_seconds_);
  client.set_read_timeout(timeout_seconds_);
  client.set_write_timeout(timeout_seconds_);
  client.set_keep_alive(false);

  const auto target = BuildTarget(path, query);
  auto response = client.Patch(target.c_str(), ToHeaders(headers), body, content_type.c_str());
  if (!response) {
    Raise(target);
  }
  return ConvertResponse(*response);
}

HttpClientResponse HttpClient::Delete(const std::string& path,
                                      const std::map<std::string, std::string>& query,
                                      const std::map<std::string, std::string>& headers) const {
  httplib::Client client(host_.c_str(), port_);
  client.set_connection_timeout(timeout_seconds_);
  client.set_read_timeout(timeout_seconds_);
  client.set_write_timeout(timeout_seconds_);
  client.set_keep_alive(false);

  const auto target = BuildTarget(path, query);
  auto response = client.Delete(target.c_str(), ToHeaders(headers));
  if (!response) {
    Raise(target);
  }
  return ConvertResponse(*response);
}

std::string HttpClient::BuildTarget(const std::string& path,
                                    const std::map<std::string, std::string>& query) const {
  if (query.empty()) {
    return path;
  }

  std::string target = path;
  target.push_back('?');
  bool first = true;
  for (const auto& [key, value] : query) {
    if (!first) {
      target.push_back('&');
    }
    target += UrlEncode(key);
    target.push_back('=');
    target += UrlEncode(value);
    first = false;
  }
  return target;
}

HttpClientResponse HttpClient::ConvertResponse(const httplib::Response& result) const {
  HttpClientResponse response;
  response.status = result.status;
  response.content_type = Trim(result.get_header_value("Content-Type"));
  response.body = result.body;
  for (const auto& header : result.headers) {
    response.headers[header.first] = header.second;
  }
  return response;
}

[[noreturn]] void HttpClient::Raise(const std::string& target) const {
  throw std::runtime_error("HTTP request failed for " + target);
}

}  // namespace platform
