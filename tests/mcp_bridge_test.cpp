#include <chrono>
#include <exception>
#include <filesystem>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "core/app.hpp"
#include "core/checklist_store.hpp"
#include "core/mcp_bridge.hpp"
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
  std::cout << "CHAX_STEP|mcp_bridge_test|" << procedure << "|"
            << (pass ? "Pass" : "Fail") << "|" << SanitizeMessage(message) << "\n";
}

void RemoveIfExists(const std::string& path) {
  std::error_code ec;
  std::filesystem::remove(path, ec);
}

class TestServer {
 public:
  explicit TestServer(std::string db_path)
      : db_path_(std::move(db_path)), store_(db_path_) {
    store_.Initialize(true);
    oauth_store_ = std::make_unique<core::OAuthStore>(db_path_);
    oauth_store_->Initialize();
  }

  ~TestServer() {
    try {
      Stop();
    } catch (...) {
      // swallow in destructor
    }
  }

  void Start(const std::string& host, int port) {
    host_ = host;
    port_ = port;
    config_.database_path = db_path_;
    config_.seed_demo_data = true;
    config_.host = host;
    config_.port = port;
    config_.base_url = "http://" + host + ":" + std::to_string(port);
    config_.oauth_client_id = client_id_;
    config_.oauth_client_secret = client_secret_;
    config_.oauth_redirect_uris = {config_.base_url + "/oauth/callback"};
    config_.oauth_scopes = {"checklist:read", "checklist:write"};
    config_.admin_user = admin_user_;
    config_.admin_password_hash = core::HashSecret(admin_password_);
    config_.issue_refresh_tokens = true;

    if (!configured_) {
      core::ConfigureServer(server_, store_, *oauth_store_, config_);
      configured_ = true;
    }
    const auto issued = oauth_store_->IssueTokens(config_.oauth_client_id, admin_user_,
                                                  "checklist:read checklist:write",
                                                  config_.access_token_ttl,
                                                  std::optional<std::chrono::seconds>{});
    access_token_ = issued.access_token;

    worker_ = std::thread([this] {
      try {
        server_.Start(host_, port_);
      } catch (const std::exception& ex) {
        std::lock_guard<std::mutex> lock(mutex_);
        error_ = ex.what();
      }
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
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
      std::error_code ec;
      std::filesystem::remove(db_path_, ec);
    }
  }

  const std::string& AccessToken() const { return access_token_; }

 private:
  platform::HttpServer server_;
  std::string db_path_;
  core::ChecklistStore store_;
  std::unique_ptr<core::OAuthStore> oauth_store_;
  core::ServerConfig config_;
  std::string client_id_ = "mcp-client";
  std::string client_secret_ = "mcp-secret";
  std::string admin_user_ = "admin";
  std::string admin_password_ = "password";
  bool configured_ = false;
  std::string access_token_;
  std::thread worker_;
  std::string host_;
  int port_ = 0;
  std::string error_;
  std::mutex mutex_;
};

bool RunTests() {
  std::string current_step;
  try {
    constexpr int kTestPort = 18888;
    const auto db_path =
        (std::filesystem::temp_directory_path() / "chax-mcp-bridge-test.db").string();
    RemoveIfExists(db_path);
    TestServer server(db_path);
    server.Start("127.0.0.1", kTestPort);

    core::mcp::Bridge bridge("http://127.0.0.1:" + std::to_string(kTestPort));
    bridge.SetBearerToken(server.AccessToken());

    current_step = "tool schemas";
    const auto tools = bridge.ToolSchemasJson();
    Assert(tools.is_array(), "Tool schema response must be an array");
    Assert(tools.size() == 23, "Unexpected number of MCP tools exposed");
    RecordStep(current_step, true, "tool schemas ok");

    current_step = "hello and echo";
    const auto hello_response =
        bridge.CallTool("chax.hello", nlohmann::json::object({{"name", "Agent"}}));
    Assert(hello_response.status == 200, "chax.hello status must be 200");
    const auto hello_json = nlohmann::json::parse(hello_response.body, nullptr, false);
    Assert(hello_json.value("ok", false), "chax.hello must return ok=true");
    Assert(hello_json["data"].value("message", "") == "Hello, Agent!",
           "chax.hello message mismatch");

    const auto echo_response =
        bridge.CallTool("chax.echo", nlohmann::json::object({{"payload", R"({"client":"cpp"})"}}));
    Assert(echo_response.status == 200, "chax.echo status must be 200");
    const auto echo_json = nlohmann::json::parse(echo_response.body, nullptr, false);
    Assert(echo_json["data"].value("received", "") == R"({"client":"cpp"})",
           "chax.echo payload mismatch");
    RecordStep(current_step, true, "hello and echo ok");

    current_step = "export list slugs";
    const auto export_response = bridge.CallTool("chax.export_json", nlohmann::json::object());
    Assert(export_response.status == 200, "chax.export_json status must be 200");
    const auto export_json = nlohmann::json::parse(export_response.body, nullptr, false);
    Assert(export_json["data"].is_array(), "chax.export_json must return an array");
    Assert(!export_json["data"].empty(), "chax.export_json should return seeded slugs");

    const auto address_id = export_json["data"].at(0).value("address_id", "");
    const auto second_address_id = export_json["data"].at(1).value("address_id", "");
    const auto slug_id = export_json["data"].at(0).value("slug_id", "");
    const auto second_slug_id = export_json["data"].at(1).value("slug_id", "");
    Assert(!address_id.empty(), "Seed slug must include address_id");
    Assert(!second_address_id.empty(), "Second seed slug must include address_id");
    Assert(!slug_id.empty() && !second_slug_id.empty(), "Seed slugs must include slug_id values");
    const auto checklist_name = export_json["data"].at(0).value("checklist", "");

    const auto list_response = bridge.CallTool(
        "chax.list_slugs",
        nlohmann::json::object({{"checklist", checklist_name}, {"limit", 5}, {"cursor", 0}}));
    Assert(list_response.status == 200, "chax.list_slugs status must be 200");
    const auto list_json = nlohmann::json::parse(list_response.body, nullptr, false);
    Assert(list_json["data"]["items"].is_array(), "chax.list_slugs must return an items array");
    RecordStep(current_step, true, "export/list ok");

    current_step = "get and update slug";
    const auto slug_response =
        bridge.CallTool("chax.get_slug", nlohmann::json::object({{"address_id", address_id}}));
    Assert(slug_response.status == 200, "chax.get_slug status must be 200");
    const auto slug_json = nlohmann::json::parse(slug_response.body, nullptr, false);
    Assert(slug_json["data"].value("address_id", "") == address_id, "Slug ID mismatch");

    const std::string updated_comment = "Updated via MCP test";
    const auto update_response = bridge.CallTool(
        "chax.update_slug",
        nlohmann::json::object({{"address_id", address_id}, {"comment", updated_comment}}));
    Assert(update_response.status == 200, "chax.update_slug status must be 200");
    const auto update_json = nlohmann::json::parse(update_response.body, nullptr, false);
    Assert(update_json["data"].value("comment", "") == updated_comment,
           "chax.update_slug did not persist comment");
    RecordStep(current_step, true, "get/update ok");

    current_step = "template relationships";
    const auto template_rel_create = bridge.CallTool(
        "chax.create_template_relationship",
        nlohmann::json::object({{"subject_slug_id", slug_id},
                                {"predicate", "passVerifyValidatedPass"},
                                {"target_slug_id", second_slug_id}}));
    Assert(template_rel_create.status == 201,
           "chax.create_template_relationship status must be 201");

    const auto template_rel_list =
        bridge.CallTool("chax.list_template_relationships",
                        nlohmann::json::object({{"subject_slug_id", slug_id}, {"limit", 10}}));
    Assert(template_rel_list.status == 200, "chax.list_template_relationships status must be 200");
    const auto template_rel_json = nlohmann::json::parse(template_rel_list.body, nullptr, false);
    Assert(template_rel_json["data"]["items"].is_array(), "Template relationship list must be array");
    RecordStep(current_step, true, "template relationships ok");

    current_step = "address relationships";
    const auto address_rel_create = bridge.CallTool(
        "chax.create_address_relationship",
        nlohmann::json::object({{"subject_address_id", address_id},
                                {"predicate", "passPropagateValidatedPass"},
                                {"target_address_id", second_address_id}}));
    Assert(address_rel_create.status == 201, "chax.create_address_relationship status must be 201");

    const auto address_rel_list =
        bridge.CallTool("chax.list_address_relationships",
                        nlohmann::json::object({{"subject_address_id", address_id}}));
    Assert(address_rel_list.status == 200, "chax.list_address_relationships status must be 200");
    const auto address_rel_json = nlohmann::json::parse(address_rel_list.body, nullptr, false);
    Assert(address_rel_json["data"]["items"].is_array(), "Address relationship list must be array");

    const auto relationships_response = bridge.CallTool(
        "chax.relationships", nlohmann::json::object({{"address_id", address_id}}));
    Assert(relationships_response.status == 200, "chax.relationships status must be 200");
    RecordStep(current_step, true, "address relationships ok");

    current_step = "evaluate graph";
    const auto eval_slug_response =
        bridge.CallTool("chax.evaluate_slug", nlohmann::json::object({{"address_id", address_id}}));
    Assert(eval_slug_response.status == 200, "chax.evaluate_slug status must be 200");
    const auto eval_slug_json = nlohmann::json::parse(eval_slug_response.body, nullptr, false);
    Assert(eval_slug_json["data"].contains("effective_status"),
           "chax.evaluate_slug must return effective_status");

    const auto eval_graph_response =
        bridge.CallTool("chax.evaluate_graph",
                        nlohmann::json::object(
                            {{"root_address_ids", nlohmann::json::array({address_id, second_address_id})}}));
    Assert(eval_graph_response.status == 200, "chax.evaluate_graph status must be 200");
    const auto eval_graph_json = nlohmann::json::parse(eval_graph_response.body, nullptr, false);
    Assert(eval_graph_json["data"]["nodes"].is_array(), "chax.evaluate_graph must return nodes array");
    RecordStep(current_step, true, "graph evaluation ok");

    current_step = "entity and instance";
    const auto entity_response =
        bridge.CallTool("chax.create_entity",
                        nlohmann::json::object({{"principal", "user||voice-smoke"},
                                                {"kind", "user"},
                                                {"display_name", "Voice Smoke User"}}));
    Assert(entity_response.status == 200, "chax.create_entity status must be 200");
    const auto entity_json = nlohmann::json::parse(entity_response.body, nullptr, false);
    const auto created_entity_id = entity_json["data"].value("entity_id", "");
    Assert(!created_entity_id.empty(), "chax.create_entity must return entity_id");

    const auto entity_list_response =
        bridge.CallTool("chax.list_entities", nlohmann::json::object({{"limit", 5}}));
    Assert(entity_list_response.status == 200, "chax.list_entities status must be 200");
    const auto entity_list_json = nlohmann::json::parse(entity_list_response.body, nullptr, false);
    Assert(entity_list_json["data"]["items"].is_array(), "chax.list_entities must return items");

    const auto instance_response =
        bridge.CallTool("chax.create_instance",
                        nlohmann::json::object({{"principal", "instance||voice-smoke"},
                                                {"label", "voice smoke"},
                                                {"meta", "scenario=voice mcp"}}));
    Assert(instance_response.status == 200, "chax.create_instance status must be 200");
    const auto instance_json = nlohmann::json::parse(instance_response.body, nullptr, false);
    const auto created_instance_id = instance_json["data"].value("instance_id", "");
    Assert(!created_instance_id.empty(), "chax.create_instance must return instance_id");

    const auto instance_list_response =
        bridge.CallTool("chax.list_instances", nlohmann::json::object({{"limit", 5}}));
    Assert(instance_list_response.status == 200, "chax.list_instances status must be 200");
    const auto instance_list_json = nlohmann::json::parse(instance_list_response.body, nullptr, false);
    Assert(instance_list_json["data"]["items"].is_array(), "chax.list_instances must return items");
    RecordStep(current_step, true, "entity/instance ok");

    current_step = "markdown import export";
    const auto export_md_response =
        bridge.CallTool("chax.export_markdown", nlohmann::json::object({{"checklist", checklist_name}}));
    Assert(export_md_response.status == 200, "chax.export_markdown status must be 200");
    Assert(!export_md_response.body.empty(), "chax.export_markdown should return markdown content");

    const auto import_md_response = bridge.CallTool(
        "chax.import_markdown",
        nlohmann::json::object({{"checklist", checklist_name},
                                {"markdown", export_md_response.body},
                                {"instance_principal", "instance||mcp-test"}}));
    Assert(import_md_response.status == 200, "chax.import_markdown status must be 200");
    RecordStep(current_step, true, "markdown import/export ok");

    server.Stop();
    return true;
  } catch (const std::exception& ex) {
    if (!current_step.empty()) {
      RecordStep(current_step, false, ex.what());
    }
    std::cerr << "MCP bridge test failure: " << ex.what() << std::endl;
    return false;
  }
}

}  // namespace

int main() {
  return RunTests() ? 0 : 1;
}
