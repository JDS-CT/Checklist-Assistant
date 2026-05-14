#include <chrono>
#include <cctype>
#include <cstdlib>
#include <exception>
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
  std::cout << "CHAX_STEP|oauth_flow_test|" << procedure << "|"
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

std::string ObtainAuthorizationCode(platform::HttpClient& client, const std::string& client_id,
                                    const std::string& redirect_uri, const std::string& scope,
                                    const std::string& username, const std::string& password,
                                    const std::string& state) {
  nlohmann::json login_payload{{"response_type", "code"},
                               {"client_id", client_id},
                               {"redirect_uri", redirect_uri},
                               {"scope", scope},
                               {"state", state},
                               {"username", username},
                               {"password", password},
                               {"action", "login"}};
  std::cout << "[test] -> POST /oauth/authorize login state=" << state << std::endl;
  const auto login_resp =
      client.Post("/oauth/authorize", login_payload.dump(), {}, "application/json",
                  WithConnectionClose());
  std::cout << "[test] <- POST /oauth/authorize login status=" << login_resp.status << std::endl;
  Assert(login_resp.status == 200, "Authorize login must return 200");
  const std::string cookie = StripCookieValue(GetHeaderIgnoreCase(login_resp, "Set-Cookie"));
  Assert(!cookie.empty(), "Authorize login must set cookie");

  nlohmann::json consent_payload{{"response_type", "code"},
                                 {"client_id", client_id},
                                 {"redirect_uri", redirect_uri},
                                 {"scope", scope},
                                 {"state", state},
                                 {"action", "approve"}};
  std::cout << "[test] -> POST /oauth/authorize approve state=" << state << std::endl;
  const auto consent_resp =
      client.Post("/oauth/authorize", consent_payload.dump(), {}, "application/json",
                  WithConnectionClose({{"Cookie", cookie}}));
  std::cout << "[test] <- POST /oauth/authorize approve status=" << consent_resp.status
            << std::endl;
  Assert(consent_resp.status == 302, "Consent approval must redirect");
  const auto location = GetHeaderIgnoreCase(consent_resp, "Location");
  Assert(!location.empty(), "Consent redirect must include Location");
  const auto code = ParseQueryValue(location, "code");
  const auto returned_state = ParseQueryValue(location, "state");
  Assert(returned_state == state, "State must round-trip");
  Assert(!code.empty(), "Authorization code must be present");
  return code;
}

std::string ExchangeCodeForToken(platform::HttpClient& client, const std::string& client_id,
                               const std::string& client_secret, const std::string& code,
                                const std::string& redirect_uri) {
  nlohmann::json token_payload{{"grant_type", "authorization_code"},
                               {"client_id", client_id},
                               {"client_secret", client_secret},
                               {"code", code},
                               {"redirect_uri", redirect_uri}};
  std::cout << "[test] -> POST /oauth/token code=" << code << std::endl;
  const auto token_resp =
      client.Post("/oauth/token", token_payload.dump(), {}, "application/json",
                  WithConnectionClose());
  std::cout << "[test] <- POST /oauth/token status=" << token_resp.status << std::endl;
  if (token_resp.status != 200) {
    return {};
  }
  const auto token_json =
      nlohmann::json::parse(token_resp.body, nullptr, /*allow_exceptions=*/false);
  return token_json.value("access_token", "");
}

class TestServer {
 public:
  TestServer(std::string db_path, std::chrono::seconds code_ttl)
      : db_path_(std::move(db_path)), store_(db_path_), oauth_store_(db_path_), code_ttl_(code_ttl) {
    store_.Initialize(false);
    oauth_store_.Initialize();
  }

  ~TestServer() {
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
    config_.oauth_client_id = client_id_;
    config_.oauth_client_secret = client_secret_;
    config_.oauth_redirect_uris = {config_.base_url + "/oauth/callback"};
    config_.oauth_scopes = {"checklist:read", "checklist:write"};
    config_.admin_user = admin_user_;
    config_.admin_password_hash = core::HashSecret(admin_password_);
    config_.auth_code_ttl = code_ttl_;
    config_.issue_refresh_tokens = true;

    core::ConfigureServer(server_, store_, oauth_store_, config_);
    const auto client = oauth_store_.GetClient(config_.oauth_client_id);
    Assert(client.has_value(), "OAuth client must be present after configuration");

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
  const std::string& ClientId() const { return client_id_; }
  const std::string& ClientSecret() const { return client_secret_; }
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
  std::chrono::seconds code_ttl_;
  std::string client_id_ = "oauth-client";
  std::string client_secret_ = "oauth-secret";
  std::string admin_user_ = "admin";
  std::string admin_password_ = "password";
};

}  // namespace

int main() {
  std::string current_step;
  try {
    core::logging::SetLogLevel(core::logging::LogLevel::kDebug);
    const auto db_path =
        (std::filesystem::temp_directory_path() / "chax-oauth-flow.db").string();
    RemoveIfExists(db_path);

    TestServer server(db_path, std::chrono::seconds(3));
    constexpr int kPort = 19993;
    server.Start("127.0.0.1", kPort);
    std::cout << "OAuth flow server started" << std::endl;

  platform::HttpClient client("http://127.0.0.1:" + std::to_string(kPort));
  const std::string redirect_uri = server.BaseUrl() + "/oauth/callback";
  const std::string scope_full = "checklist:read checklist:write";
  const std::string scope_read = "checklist:read";

  // state required
  current_step = "missing state";
  std::cout << "[test] -> GET /oauth/authorize missing state" << std::endl;
  const auto missing_state = client.Get(
      "/oauth/authorize",
      {{"response_type", "code"},
       {"client_id", server.ClientId()},
       {"redirect_uri", redirect_uri},
       {"scope", scope_read}},
      WithConnectionClose());
  std::cout << "[test] <- missing state status=" << missing_state.status << std::endl;
  std::cout << "[test] <- missing state body=" << missing_state.body << std::endl;
  Assert(missing_state.status == 400, "Missing state should fail authorize");
  std::cout << "[test] missing state assertion passed" << std::endl;
  RecordStep(current_step, true, "missing state blocked");

  // redirect allowlist enforcement
  current_step = "bad redirect";
  std::cout << "[test] -> GET /oauth/authorize bad redirect" << std::endl;
  const auto bad_redirect = client.Get(
      "/oauth/authorize",
      {{"response_type", "code"},
       {"client_id", server.ClientId()},
       {"redirect_uri", "https://not-allowed.example.com/callback"},
       {"scope", scope_read},
       {"state", "bad"}},
      WithConnectionClose());
  std::cout << "[test] <- bad redirect status=" << bad_redirect.status << std::endl;
  Assert(bad_redirect.status == 400, "Disallowed redirect should fail authorize");
  std::cout << "[test] bad redirect assertion passed" << std::endl;
  RecordStep(current_step, true, "bad redirect blocked");

    // single-use and expiry
    current_step = "single-use code";
    const auto code = ObtainAuthorizationCode(client, server.ClientId(), redirect_uri, scope_full,
                                              server.AdminUser(), server.AdminPassword(),
                                              "single-use");
  const auto access_token =
      ExchangeCodeForToken(client, server.ClientId(), server.ClientSecret(), code, redirect_uri);
    Assert(!access_token.empty(), "Token exchange should succeed");
    std::cout << "[test] single-use token issued" << std::endl;

    std::cout << "[test] -> POST /oauth/token second use" << std::endl;
    const auto second_use = client.Post(
        "/oauth/token",
        nlohmann::json{{"grant_type", "authorization_code"},
                       {"client_id", server.ClientId()},
                       {"client_secret", server.ClientSecret()},
                       {"code", code},
                       {"redirect_uri", redirect_uri}}
            .dump(),
        {},
        "application/json", WithConnectionClose());
    std::cout << "[test] <- POST /oauth/token second use status=" << second_use.status
              << std::endl;
    Assert(second_use.status == 400, "Auth codes must be single-use");
    RecordStep(current_step, true, "single-use enforced");

    current_step = "expired code";
    const auto expiring_code =
        ObtainAuthorizationCode(client, server.ClientId(), redirect_uri, scope_full,
                                server.AdminUser(), server.AdminPassword(), "expire");
    std::this_thread::sleep_for(std::chrono::seconds(4));
    const auto expired_token =
        ExchangeCodeForToken(client, server.ClientId(), server.ClientSecret(), expiring_code,
                             redirect_uri);
    Assert(expired_token.empty(), "Expired auth code should not exchange");
    std::cout << "[test] expiry path checked" << std::endl;
    RecordStep(current_step, true, "expiry enforced");

    // scope enforcement
    current_step = "scope enforcement";
    const auto read_code =
        ObtainAuthorizationCode(client, server.ClientId(), redirect_uri, scope_read,
                                server.AdminUser(), server.AdminPassword(), "read-only");
  const auto read_token =
      ExchangeCodeForToken(client, server.ClientId(), server.ClientSecret(), read_code,
                           redirect_uri);
  Assert(!read_token.empty(), "Read token should be issued");
  const std::map<std::string, std::string> read_headers =
      WithConnectionClose({{"Authorization", "Bearer " + read_token}});
  std::cout << "[test] -> GET /api/v1/health read-only" << std::endl;
  const auto read_ok = client.Get("/api/v1/health", {}, read_headers);
  std::cout << "[test] <- GET /api/v1/health read-only status=" << read_ok.status << std::endl;
  Assert(read_ok.status == 200, "Read token must allow GET");
  std::cout << "[test] -> POST /api/v1/echo read-only" << std::endl;
  const auto write_blocked = client.Post("/api/v1/echo", "{}", {}, "application/json", read_headers);
  std::cout << "[test] <- POST /api/v1/echo read-only status=" << write_blocked.status
            << std::endl;
  Assert(write_blocked.status == 403, "Read-only token must block write");
  std::cout << "[test] scope enforcement checked" << std::endl;
  RecordStep(current_step, true, "scope enforcement ok");

    // happy path
    current_step = "full access";
    const auto full_code =
        ObtainAuthorizationCode(client, server.ClientId(), redirect_uri, scope_full,
                                server.AdminUser(), server.AdminPassword(), "full");
  const auto full_token =
      ExchangeCodeForToken(client, server.ClientId(), server.ClientSecret(), full_code,
                           redirect_uri);
  Assert(!full_token.empty(), "Full token must be issued");
  const std::map<std::string, std::string> full_headers =
      WithConnectionClose({{"Authorization", "Bearer " + full_token}});
  std::cout << "[test] -> GET /api/v1/health full token" << std::endl;
  const auto full_health = client.Get("/api/v1/health", {}, full_headers);
  std::cout << "[test] <- GET /api/v1/health full token status=" << full_health.status
            << std::endl;
  Assert(full_health.status == 200, "Full token should allow GET");
  std::cout << "[test] -> POST /api/v1/echo full token" << std::endl;
  const auto full_echo =
        client.Post("/api/v1/echo", R"({"ping":"pong"})", {}, "application/json", full_headers);
  std::cout << "[test] <- POST /api/v1/echo full token status=" << full_echo.status
            << std::endl;
    Assert(full_echo.status == 200, "Full token should allow POST");
    std::cout << "[test] happy path complete" << std::endl;
    RecordStep(current_step, true, "full access ok");

    server.Stop();
    return 0;
  } catch (const std::exception& ex) {
    if (!current_step.empty()) {
      RecordStep(current_step, false, ex.what());
    }
    std::cerr << "oauth_flow_test failure: " << ex.what() << std::endl;
    return 1;
  }
}
