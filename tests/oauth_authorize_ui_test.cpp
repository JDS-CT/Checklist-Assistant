#include <cctype>
#include <filesystem>
#include <iostream>
#include <map>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>

#include "core/app.hpp"
#include "core/checklist_store.hpp"
#include "core/logging.hpp"
#include "core/oauth.hpp"
#include "nlohmann/json.hpp"
#include "platform/http_client.hpp"
#include "platform/http_server.hpp"

namespace {

void Assert(bool condition, const std::string& message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

std::string SanitizeMessage(std::string value) {
  for (char& ch : value) {
    if (ch == '\n' || ch == '\r') {
      ch = ' ';
    } else if (ch == '|') {
      ch = '/';
    }
  }
  return value;
}

void RecordStep(const std::string& procedure, bool pass, const std::string& message) {
  std::cout << "CHAX_STEP|oauth_authorize_ui_test|" << procedure << "|"
            << (pass ? "Pass" : "Fail") << "|" << SanitizeMessage(message) << "\n";
}

void RemoveIfExists(const std::filesystem::path& path) {
  std::error_code ec;
  std::filesystem::remove(path, ec);
}

std::string Lower(std::string value) {
  for (auto& ch : value) {
    ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  }
  return value;
}

std::string GetHeaderIgnoreCase(const platform::HttpClientResponse& response,
                                const std::string& key) {
  const auto target = Lower(key);
  for (const auto& entry : response.headers) {
    if (Lower(entry.first) == target) {
      return entry.second;
    }
  }
  return {};
}

std::string ParseQueryValue(const std::string& url, const std::string& key) {
  const auto pos = url.find('?');
  if (pos == std::string::npos) {
    return "";
  }
  std::istringstream iss(url.substr(pos + 1));
  std::string pair;
  while (std::getline(iss, pair, '&')) {
    const auto eq = pair.find('=');
    if (eq == std::string::npos) continue;
    const auto name = pair.substr(0, eq);
    const auto value = pair.substr(eq + 1);
    if (name == key) {
      return value;
    }
  }
  return "";
}

std::string StripCookieValue(const std::string& header_value) {
  const auto semi = header_value.find(';');
  if (semi == std::string::npos) {
    return header_value;
  }
  return header_value.substr(0, semi);
}

std::map<std::string, std::string> WithConnectionClose(
    const std::map<std::string, std::string>& base = {}) {
  auto headers = base;
  headers.insert({"Connection", "close"});
  return headers;
}

class UiAuthorizeServer {
 public:
  explicit UiAuthorizeServer(std::string db_path)
      : db_path_(std::move(db_path)), store_(db_path_), oauth_store_(db_path_) {
    store_.Initialize(false);
    oauth_store_.Initialize();
  }

  ~UiAuthorizeServer() {
    try {
      Stop();
    } catch (...) {
    }
  }

  void Start(const std::string& host, int port) {
    host_ = host;
    port_ = port;
    config_.database_path = db_path_;
    config_.seed_demo_data = false;
    config_.host = host;
    config_.port = port;
    config_.base_url = "http://" + host + ":" + std::to_string(port);
    config_.serve_ui = true;
    config_.oauth_client_id = client_id_;
    config_.oauth_client_secret = client_secret_;
    config_.oauth_redirect_uris = {config_.base_url + "/oauth/callback"};
    config_.oauth_scopes = {"checklist:read", "checklist:write"};
    config_.admin_user = admin_user_;
    config_.admin_password_hash = core::HashSecret(admin_password_);

    core::ConfigureServer(server_, store_, oauth_store_, config_);

    worker_ = std::thread([this] {
      try {
        server_.Start(host_, port_);
      } catch (const std::exception& ex) {
        std::lock_guard<std::mutex> lock(mutex_);
        error_ = ex.what();
      }
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (!error_.empty()) {
        throw std::runtime_error(error_);
      }
    }
  }

  void Stop() {
    server_.Stop();
    if (worker_.joinable()) {
      worker_.join();
    }
    if (!error_.empty()) {
      throw std::runtime_error(error_);
    }
    if (!db_path_.empty()) {
      RemoveIfExists(db_path_);
    }
  }

  std::string BaseUrl() const { return config_.base_url; }
  std::string RedirectUri() const { return config_.base_url + "/ui/oauth_callback.html"; }
  const std::string& ClientId() const { return client_id_; }
  const std::string& AdminUser() const { return admin_user_; }
  const std::string& AdminPassword() const { return admin_password_; }

 private:
  platform::HttpServer server_;
  std::string db_path_;
  core::ChecklistStore store_;
  core::OAuthStore oauth_store_;
  core::ServerConfig config_;
  std::thread worker_;
  std::string host_;
  int port_ = 0;
  std::string error_;
  std::mutex mutex_;

  std::string client_id_ = "chax-ui-client";
  std::string client_secret_ = "chax-ui-secret";
  std::string admin_user_ = "admin";
  std::string admin_password_ = "admin-password";
};

}  // namespace

int main() {
  std::string current_step;
  try {
    const auto db_path =
        (std::filesystem::temp_directory_path() / "chax-oauth-authorize-ui.db").string();
    UiAuthorizeServer server(db_path);
    server.Start("127.0.0.1", 18100);

    platform::HttpClient client(server.BaseUrl());
    const std::string state = "ui-state";
    const std::string redirect_uri = server.RedirectUri();
    const std::string scope = "checklist:read checklist:write";

    const std::map<std::string, std::string> authorize_query{
        {"response_type", "code"},
        {"client_id", server.ClientId()},
        {"redirect_uri", redirect_uri},
        {"scope", scope},
        {"state", state},
        {"code_challenge", "abc"},
        {"code_challenge_method", "S256"}};
    current_step = "authorize page";
    const auto authorize_page =
        client.Get("/oauth/authorize", authorize_query, WithConnectionClose());
    Assert(authorize_page.status == 200, "Authorize GET must return login page");
    Assert(authorize_page.content_type.find("text/html") != std::string::npos,
           "Authorize GET must render HTML");
    RecordStep(current_step, true, "authorize page ok");

    current_step = "login";
    nlohmann::json login_payload{{"response_type", "code"},
                                 {"client_id", server.ClientId()},
                                 {"redirect_uri", redirect_uri},
                                 {"scope", scope},
                                 {"state", state},
                                 {"username", server.AdminUser()},
                                 {"password", server.AdminPassword()},
                                 {"action", "login"}};
    const auto login_resp = client.Post("/oauth/authorize", login_payload.dump(), {},
                                        "application/json", WithConnectionClose());
    Assert(login_resp.status == 200, "Authorize login must return 200");
    const std::string cookie = StripCookieValue(GetHeaderIgnoreCase(login_resp, "Set-Cookie"));
    Assert(!cookie.empty(), "Authorize login must set session cookie");
    RecordStep(current_step, true, "login ok");

    current_step = "approve redirect";
    nlohmann::json approve_payload{{"response_type", "code"},
                                   {"client_id", server.ClientId()},
                                   {"redirect_uri", redirect_uri},
                                   {"scope", scope},
                                   {"state", state},
                                   {"action", "approve"}};
    const auto approve_resp =
        client.Post("/oauth/authorize", approve_payload.dump(), {}, "application/json",
                    WithConnectionClose({{"Cookie", cookie}}));
    Assert(approve_resp.status == 302, "Authorize approve must redirect");
    const auto location = GetHeaderIgnoreCase(approve_resp, "Location");
    Assert(location.rfind(redirect_uri, 0) == 0, "Redirect URI must match the requested value");
    Assert(!ParseQueryValue(location, "code").empty(), "Redirect must include authorization code");
    Assert(ParseQueryValue(location, "state") == state, "Redirect must echo the state");
    RecordStep(current_step, true, "approve ok");

    server.Stop();
    return 0;
  } catch (const std::exception& ex) {
    if (!current_step.empty()) {
      RecordStep(current_step, false, ex.what());
    }
    std::cerr << "oauth_authorize_ui_test failure: " << ex.what() << std::endl;
    return 1;
  }
}
