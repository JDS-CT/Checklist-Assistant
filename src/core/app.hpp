#pragma once

#include <chrono>
#include <string>
#include <vector>

namespace core {
class ChecklistStore;
class OAuthStore;
}

namespace platform {
class HttpServer;
}  // namespace platform

namespace core {

struct ServerConfig {
  std::string host = "127.0.0.1";
  int port = 8080;
  std::string database_path = ".chax/checklists.db";
  bool seed_demo_data = true;
  std::string base_url;
  std::string oauth_client_id;
  std::string oauth_client_secret;
  bool oauth_client_generated = false;
  std::vector<std::string> oauth_redirect_uris;
  std::vector<std::string> oauth_scopes = {"checklist:read", "checklist:write"};
  std::chrono::seconds auth_code_ttl{300};
  std::chrono::seconds access_token_ttl{3600};
  std::chrono::seconds refresh_token_ttl{2592000};
  int token_rate_limit = 10;
  std::chrono::seconds token_rate_window{60};
  bool issue_refresh_tokens = true;
  std::string admin_user = "admin";
  std::string admin_password_hash;
  bool admin_password_generated = false;
  std::string admin_password_plain;
  std::string auth_provider = "oauth";
  bool simple_authorize = false;
  bool simple_authorize_overridden = false;
  std::string entity_salt = "dev-entity-salt";
  std::string guest_provider = "none";
  std::string guest_name = "guest";
  bool localhost_noauth = false;
  bool localhost_noauth_overridden = false;
  bool serve_ui = true;
  bool open_browser = true;
  std::string ui_root = "CHAX-CLIENT/web";
  bool serve_vui = true;
  std::string vui_root = "CHAX-CLIENT/vui";
  bool whisper_autostart = false;
  std::string whisper_host = "127.0.0.1";
  int whisper_port = 8081;
  std::string whisper_server_path;
  std::string whisper_model_path;
  std::string whisper_model_fallback_path;
  std::string shutdown_token;
  bool background_processes_enabled = true;
  std::string background_processes_root = "background_processes";
  int predicate_chain_depth = 1;
};

void ConfigureServer(platform::HttpServer& server, ChecklistStore& store, OAuthStore& oauth_store,
                     const ServerConfig& config);
ServerConfig LoadServerConfig();

}  // namespace core
