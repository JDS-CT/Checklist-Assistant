#include <chrono>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "core/app.hpp"
#include "core/checklist_store.hpp"
#include "core/oauth.hpp"
#include "nlohmann/json.hpp"
#include "platform/http_client.hpp"
#include "platform/http_server.hpp"
#include "platform/system.hpp"

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
  std::cout << "CHAX_STEP|http_api_test|" << procedure << "|"
            << (pass ? "Pass" : "Fail") << "|" << SanitizeMessage(message) << "\n";
}

void RemoveIfExists(const std::filesystem::path& path) {
  std::error_code ec;
  std::filesystem::remove(path, ec);
}

std::filesystem::path ResolveRepoRoot() {
  std::error_code ec;
  std::filesystem::path cwd = std::filesystem::current_path(ec);
  std::filesystem::path candidate = cwd;
  for (int depth = 0; depth <= 4; ++depth) {
    if (std::filesystem::exists(candidate / "CMakeLists.txt") &&
        std::filesystem::exists(candidate / "AGENTS.md")) {
      return candidate;
    }
    if (!candidate.has_parent_path()) break;
    candidate = candidate.parent_path();
  }
  return cwd;
}

void SetEnvValue(const std::string& key, const std::string& value) {
#if defined(_WIN32)
  _putenv_s(key.c_str(), value.c_str());
#else
  setenv(key.c_str(), value.c_str(), 1);
#endif
}

bool CleanArtifactsOnExit() {
  const char* env = std::getenv("CHAX_CLEAN_TEST_ARTIFACTS");
  return env != nullptr && env[0] != '\0';
}

std::filesystem::path ResolveSevenZipForTest() {
  for (const auto &name : {"7z", "7za", "7zz"}) {
    const auto resolved = platform::ResolveExecutableOnPath(name);
    if (!resolved.empty()) {
      return resolved;
    }
  }
#if defined(_WIN32)
  for (const auto &candidate : {std::filesystem::path{"C:\\Program Files\\7-Zip\\7z.exe"},
                                std::filesystem::path{"C:\\Program Files (x86)\\7-Zip\\7z.exe"}}) {
    std::error_code ec;
    if (std::filesystem::exists(candidate, ec)) {
      return candidate;
    }
  }
#endif
  return {};
}

class TestServer {
 public:
  TestServer(std::string db_path, bool seed_demo)
      : db_path_(std::move(db_path)), store_(db_path_), oauth_store_(db_path_), seed_demo_(seed_demo) {
    store_.Initialize(seed_demo);
    oauth_store_.Initialize();
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
    config_.seed_demo_data = seed_demo_;
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
      core::ConfigureServer(server_, store_, oauth_store_, config_);
      configured_ = true;
    }
    const auto issued = oauth_store_.IssueTokens(config_.oauth_client_id, admin_user_,
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
      RemoveIfExists(db_path_);
    }
  }

  const std::string& AccessToken() const { return access_token_; }

 private:
  platform::HttpServer server_;
  std::string db_path_;
  core::ChecklistStore store_;
  core::OAuthStore oauth_store_;
  core::ServerConfig config_;
  std::string client_id_ = "test-client";
  std::string client_secret_ = "test-secret";
  std::string admin_user_ = "admin";
  std::string admin_password_ = "password";
  bool seed_demo_ = false;
  bool configured_ = false;
  std::thread worker_;
  std::string host_;
  int port_ = 0;
  std::string error_;
  std::mutex mutex_;
  std::string access_token_;
};

}  // namespace

int main() {
  std::string current_step;
  try {
    const auto artifacts_root = ResolveRepoRoot() / ".chax" / "test-artifacts" / "http-api";
    {
      std::error_code ec;
      std::filesystem::remove_all(artifacts_root, ec);
      std::filesystem::create_directories(artifacts_root, ec);
    }
    SetEnvValue("CHAX_REPORTS_ROOT", (artifacts_root / "reports").string());
    SetEnvValue("CHAX_REPORT_TEMPLATES_ROOT", (artifacts_root / "templates").string());
    const auto library_root = artifacts_root / "library";
    SetEnvValue("CHAX_CHECKLISTS_ROOT", library_root.string());
    const auto predicate_summary_dir = artifacts_root / "predicate-summaries";
    {
      std::error_code ec;
      std::filesystem::create_directories(predicate_summary_dir, ec);
    }
    const auto predicate_summary_path = predicate_summary_dir / "prefill_predicates.jsonl";

    auto append_predicate_summary = [&](const nlohmann::json& entry) {
      std::ofstream out(predicate_summary_path, std::ios::app);
      if (!out.is_open()) {
        throw std::runtime_error("Failed to open predicate summary file: " +
                                 predicate_summary_path.string());
      }
      out << entry.dump() << "\n";
    };

    const auto db_path =
        (std::filesystem::temp_directory_path() / "chax-http-api-test.db").string();
    RemoveIfExists(db_path);
    TestServer server(db_path, /*seed_demo=*/false);
    constexpr int kPort = 19991;
    server.Start("127.0.0.1", kPort);

    platform::HttpClient client("http://127.0.0.1:" + std::to_string(kPort));
    const std::map<std::string, std::string> auth_headers{
        {"Authorization", "Bearer " + server.AccessToken()}};

    // Create two slugs via the full creation contract.
    current_step = "create slugs";
    nlohmann::json create_payload{
        {"checklist", "api-test-checklist"},
        {"section", "Section A"},
        {"procedure", "Proc A"},
        {"action", "Do something"},
        {"spec", "Expected"},
        {"instructions", "Step 1\nStep 2"},
        {"instance_principal", "instance||http-test"},
    };

    const auto create_response =
        client.Post("/api/v1/slugs", create_payload.dump(), {}, "application/json", auth_headers);
    Assert(create_response.status == 201, "Create slug should return 201");
    const auto create_json =
        nlohmann::json::parse(create_response.body, nullptr, /*allow_exceptions=*/false);
    Assert(create_json.value("ok", false), "Create slug should set ok=true");
    const auto address_id = create_json["data"].value("address_id", "");
    const auto slug_id = create_json["data"].value("slug_id", "");
    const auto instance_id = create_json["data"].value("instance_id", "");
    const auto entity_id = create_json["data"].value("entity_id", "");
    Assert(!address_id.empty(), "Create slug must return address_id");
    Assert(!instance_id.empty(), "Create slug must return instance_id");
    const std::string expected_principal = "user||provider=oauth||username=admin";
    const std::string expected_entity_id = core::ComputeEntityId(expected_principal);
    Assert(entity_id == expected_entity_id, "Create slug must stamp derived entity_id");

    // Allow empty instructions (still a valid slug, but shorter instruction text).
    auto empty_payload = create_payload;
    empty_payload["procedure"] = "Proc Empty";
    empty_payload["instructions"] = "";
    const auto empty_response =
        client.Post("/api/v1/slugs", empty_payload.dump(), {}, "application/json", auth_headers);
    Assert(empty_response.status == 201, "Create slug with empty instructions should return 201");

    // Second slug for relationships.
    create_payload["procedure"] = "Proc B";
    create_payload["section"] = "Section B";
    const auto create_response2 =
        client.Post("/api/v1/slugs", create_payload.dump(), {}, "application/json", auth_headers);
    Assert(create_response2.status == 201, "Create second slug should return 201");
    const auto create_json2 =
        nlohmann::json::parse(create_response2.body, nullptr, /*allow_exceptions=*/false);
    const auto address_id_b = create_json2["data"].value("address_id", "");
    const auto slug_id_b = create_json2["data"].value("slug_id", "");
    Assert(!address_id_b.empty(), "Second slug must return address_id");
    RecordStep(current_step, true, "slugs created");

    // List slugs filtered by checklist and status.
    current_step = "list and history";
    const auto list_response =
        client.Get("/api/v1/slugs", {{"checklist", "api-test-checklist"}}, auth_headers);
    Assert(list_response.status == 200, "List slugs should return 200");
    const auto list_json =
        nlohmann::json::parse(list_response.body, nullptr, /*allow_exceptions=*/false);
    Assert(list_json.value("ok", false), "List slugs should set ok=true");
    const auto items = list_json["data"].value("items", nlohmann::json::array());
    Assert(items.is_array() && !items.empty(), "List slugs should return at least one item");
    // Pagination cursor when limit is set.
    const auto paged = client.Get("/api/v1/slugs", {{"limit", "1"}}, auth_headers);
    const auto paged_json =
        nlohmann::json::parse(paged.body, nullptr, /*allow_exceptions=*/false);
    Assert(paged_json["data"].contains("next_cursor"), "Paged slugs should return next_cursor");

    // Minimal update to exercise history.
    nlohmann::json update_payload{
        {"comment", "updated via http api test"},
        {"status", "Pass"},
    };
    const auto update_response = client.Patch("/api/v1/slugs/" + address_id, update_payload.dump(),
                                              {}, "application/json", auth_headers);
    Assert(update_response.status == 200, "Update slug should return 200");

    // Me endpoint should reflect authenticated principal.
    const auto me_response = client.Get("/api/v1/me", {}, auth_headers);
    Assert(me_response.status == 200, "GET /api/v1/me should return 200");
    const auto me_json = nlohmann::json::parse(me_response.body, nullptr, false);
    const auto me_entity = me_json["data"].value("entity", nlohmann::json::object());
    Assert(me_entity.contains("entity_id"), "/api/v1/me must include entity_id");

    // History endpoint with limit.
    const auto history_response =
        client.Get("/api/v1/history/" + address_id, {{"limit", "1"}}, auth_headers);
    Assert(history_response.status == 200, "History should return 200");
    const auto history_json =
        nlohmann::json::parse(history_response.body, nullptr, /*allow_exceptions=*/false);
    const auto history_items = history_json["data"].value("items", nlohmann::json::array());
    Assert(history_items.is_array() && !history_items.empty(),
           "History should include at least one entry");

    // Mismatched entity principal must be forbidden.
    auto tampered_payload = create_payload;
    tampered_payload["procedure"] = "Proc C";
    tampered_payload["action"] = "Tampered";
    tampered_payload["entity_principal"] = "user||provider=oauth||username=mallory";
    const auto tampered_response =
        client.Post("/api/v1/slugs", tampered_payload.dump(), {}, "application/json", auth_headers);
    Assert(tampered_response.status == 403, "Mismatched entity_principal must be forbidden");
    RecordStep(current_step, true, "list/history ok");

    // Export and import Markdown.
    current_step = "export import md";
    const auto export_md =
        client.Get("/api/v1/export/markdown/api-test-checklist", {}, auth_headers);
    Assert(export_md.status == 200, "Export markdown should return 200");
    Assert(export_md.body.find("# Checklist: api-test-checklist") != std::string::npos,
           "Exported markdown should contain checklist heading");

    const auto import_md = client.Post(
        "/api/v1/import/markdown", export_md.body,
        {{"checklist", "api-test-checklist"}, {"instance_principal", "instance||http-test-2"}},
        "text/markdown", auth_headers);
    Assert(import_md.status == 200, "Import markdown should return 200");
    const auto import_json =
        nlohmann::json::parse(import_md.body, nullptr, /*allow_exceptions=*/false);
    Assert(import_json.value("ok", false), "Import markdown should set ok=true");
    RecordStep(current_step, true, "markdown import/export ok");

    current_step = "template lineage merge";
    const std::string lineage_checklist = "lineage-merge";
    const std::string lineage_section = "Section L";
    const std::string lineage_proc = "Proc L";
    const std::string lineage_action = "Action L";
    const std::string lineage_spec = "Spec L";
    const std::string lineage_instr = "Instr L";
    const auto lineage_slug =
        core::ComputeSlugId(lineage_checklist, lineage_section, lineage_proc, lineage_action,
                            lineage_spec, lineage_instr);
    const auto legacy_slug_a =
        core::ComputeSlugId(lineage_checklist, lineage_section, lineage_proc, lineage_action,
                            "Spec L v0", lineage_instr);
    const auto legacy_slug_b =
        core::ComputeSlugId(lineage_checklist, lineage_section, lineage_proc, lineage_action,
                            "Spec L v1", lineage_instr);
    const auto build_lineage_md = [&](const std::string& legacy_slug) {
      return "# Checklist: " + lineage_checklist + "\n\n" +
             "## Section: " + lineage_section + "\n\n" +
             "### Procedure: " + lineage_proc + "\n" +
             "- Action: " + lineage_action + "\n" +
             "- Spec: " + lineage_spec + "\n" +
             "- Result: \n" +
             "- Status: \n" +
             "- Comment: \n\n" +
             "#### Instructions\n" +
             lineage_instr + "\n" +
             "#### Relationships\n" +
             "- " + legacy_slug + "\n";
    };
    const auto lineage_md_a = build_lineage_md(legacy_slug_a);
    const auto lineage_md_b = build_lineage_md(legacy_slug_b);
    const auto lineage_import_a =
        client.Post("/api/v1/import/markdown", lineage_md_a,
                    {{"checklist", lineage_checklist},
                     {"instance_principal", "template||default"}},
                    "text/markdown", auth_headers);
    Assert(lineage_import_a.status == 200, "First lineage import should return 200");
    const auto lineage_import_b =
        client.Post("/api/v1/import/markdown", lineage_md_b,
                    {{"checklist", lineage_checklist},
                     {"instance_principal", "template||default"}},
                    "text/markdown", auth_headers);
    Assert(lineage_import_b.status == 200, "Second lineage import should return 200");
    const auto lineage_list =
        client.Get("/api/v1/relationships/template", {{"subject_slug_id", lineage_slug}},
                   auth_headers);
    Assert(lineage_list.status == 200, "Lineage relationship list should return 200");
    const auto lineage_json =
        nlohmann::json::parse(lineage_list.body, nullptr, /*allow_exceptions=*/false);
    const auto lineage_items = lineage_json["data"].value("items", nlohmann::json::array());
    bool found_legacy_a = false;
    bool found_legacy_b = false;
    for (const auto& rel : lineage_items) {
      if (rel.value("subject_slug_id", "") != lineage_slug) {
        continue;
      }
      if (rel.value("predicate", "") != "slugPredecessor") {
        continue;
      }
      const auto target = rel.value("target_slug_id", "");
      if (target == legacy_slug_a) {
        found_legacy_a = true;
      } else if (target == legacy_slug_b) {
        found_legacy_b = true;
      }
    }
    Assert(found_legacy_a && found_legacy_b,
           "Template relationships should retain all slugPredecessor entries");
    RecordStep(current_step, true, "lineage merge ok");

    current_step = "template lineage chain";
    const std::string chain_checklist = "lineage-chain";
    const std::string chain_section = "Section C";
    const std::string chain_proc = "Proc C";
    const std::string chain_action = "Action C";
    const std::string chain_instr = "Instr C";
    const std::string chain_spec_v1 = "Spec v1";
    const std::string chain_spec_v2 = "Spec v2";
    const std::string chain_spec_v3 = "Spec v3";
    const auto chain_slug_v1 =
        core::ComputeSlugId(chain_checklist, chain_section, chain_proc, chain_action,
                            chain_spec_v1, chain_instr);
    const auto chain_slug_v2 =
        core::ComputeSlugId(chain_checklist, chain_section, chain_proc, chain_action,
                            chain_spec_v2, chain_instr);
    const auto chain_slug_v3 =
        core::ComputeSlugId(chain_checklist, chain_section, chain_proc, chain_action,
                            chain_spec_v3, chain_instr);
    const auto build_chain_md = [&](const std::string& spec,
                                    const std::vector<std::string>& rels) {
      std::string md =
          "# Checklist: " + chain_checklist + "\n\n" +
          "## Section: " + chain_section + "\n\n" +
          "### Procedure: " + chain_proc + "\n" +
          "- Action: " + chain_action + "\n" +
          "- Spec: " + spec + "\n" +
          "- Result: \n" +
          "- Status: \n" +
          "- Comment: \n\n" +
          "#### Instructions\n" +
          chain_instr + "\n";
      if (!rels.empty()) {
        md += "#### Relationships\n";
        for (const auto& rel : rels) {
          md += "- " + rel + "\n";
        }
      }
      return md;
    };
    const auto chain_md_v1 = build_chain_md(chain_spec_v1, {});
    const auto chain_md_v2 = build_chain_md(chain_spec_v2, {chain_slug_v1});
    const auto chain_md_v3 = build_chain_md(
        chain_spec_v3,
        {chain_slug_v2, std::string{"slugPredecessor "} + chain_slug_v1});

    const auto chain_import_v1 =
        client.Post("/api/v1/import/markdown", chain_md_v1,
                    {{"checklist", chain_checklist},
                     {"instance_principal", "template||default"}},
                    "text/markdown", auth_headers);
    Assert(chain_import_v1.status == 200, "Chain v1 import should return 200");
    const auto chain_import_v2 =
        client.Post("/api/v1/import/markdown", chain_md_v2,
                    {{"checklist", chain_checklist},
                     {"instance_principal", "template||default"}},
                    "text/markdown", auth_headers);
    Assert(chain_import_v2.status == 200, "Chain v2 import should return 200");
    const auto chain_import_v3 =
        client.Post("/api/v1/import/markdown", chain_md_v3,
                    {{"checklist", chain_checklist},
                     {"instance_principal", "template||default"}},
                    "text/markdown", auth_headers);
    Assert(chain_import_v3.status == 200, "Chain v3 import should return 200");

    const auto chain_list =
        client.Get("/api/v1/relationships/template", {{"subject_slug_id", chain_slug_v3}},
                   auth_headers);
    Assert(chain_list.status == 200, "Chain relationship list should return 200");
    const auto chain_json =
        nlohmann::json::parse(chain_list.body, nullptr, /*allow_exceptions=*/false);
    const auto chain_items = chain_json["data"].value("items", nlohmann::json::array());
    bool found_chain_v1 = false;
    bool found_chain_v2 = false;
    for (const auto& rel : chain_items) {
      if (rel.value("subject_slug_id", "") != chain_slug_v3) {
        continue;
      }
      if (rel.value("predicate", "") != "slugPredecessor") {
        continue;
      }
      const auto target = rel.value("target_slug_id", "");
      if (target == chain_slug_v2) {
        found_chain_v2 = true;
      } else if (target == chain_slug_v1) {
        found_chain_v1 = true;
      }
    }
    Assert(found_chain_v1 && found_chain_v2,
           "Chain import should preserve all slugPredecessor entries");
    RecordStep(current_step, true, "lineage chain ok");

    // JSONL import for instance data.
    current_step = "jsonl import";
    const std::string jsonl_update =
        std::string{"{\"slug_id\":\""} + slug_id + "\"," +
        "\"checklist\":\"api-test-checklist\"," +
        "\"section\":\"Section A\"," +
        "\"procedure\":\"Proc A\"," +
        "\"action\":\"Do something\"," +
        "\"spec\":\"Expected\"," +
        "\"instructions\":\"Step 1\\nStep 2\"," +
        "\"result\":\"Imported result\"," +
        "\"status\":\"Pass\"," +
        "\"comment\":\"Imported comment\"}";
    const auto jsonl_import =
        client.Post("/api/v1/import/jsonl", jsonl_update,
                    {{"checklist", "api-test-checklist"}, {"instance_id", instance_id}},
                    "application/x-ndjson", auth_headers);
    Assert(jsonl_import.status == 200, "JSONL import should return 200");
    const auto jsonl_import_json =
        nlohmann::json::parse(jsonl_import.body, nullptr, /*allow_exceptions=*/false);
    Assert(jsonl_import_json.value("ok", false), "JSONL import should set ok=true");
    Assert(jsonl_import_json["data"].value("updated", 0) == 1,
           "JSONL import should update one row");

    const std::string jsonl_section = "Jsonl Section";
    const std::string jsonl_procedure = "Jsonl Proc";
    const std::string jsonl_action = "Jsonl Action";
    const std::string jsonl_spec = "Jsonl Spec";
    const std::string jsonl_instructions = "Jsonl Instructions";
    const auto jsonl_slug_id =
        core::ComputeSlugId("api-test-checklist", jsonl_section, jsonl_procedure, jsonl_action,
                            jsonl_spec, jsonl_instructions);
    const std::string jsonl_missing =
        std::string{"{\"slug_id\":\""} + jsonl_slug_id + "\"," +
        "\"checklist\":\"api-test-checklist\"," +
        "\"section\":\"" + jsonl_section + "\"," +
        "\"procedure\":\"" + jsonl_procedure + "\"," +
        "\"action\":\"" + jsonl_action + "\"," +
        "\"spec\":\"" + jsonl_spec + "\"," +
        "\"instructions\":\"" + jsonl_instructions + "\"," +
        "\"result\":\"Imported new\",\"status\":\"Pass\",\"comment\":\"Created\"}";
    const auto jsonl_missing_resp =
        client.Post("/api/v1/import/jsonl", jsonl_missing,
                    {{"checklist", "api-test-checklist"}, {"instance_id", instance_id}},
                    "application/x-ndjson", auth_headers);
    Assert(jsonl_missing_resp.status == 409, "JSONL import should report missing rows");
    const auto jsonl_missing_json =
        nlohmann::json::parse(jsonl_missing_resp.body, nullptr, /*allow_exceptions=*/false);
    Assert(jsonl_missing_json["error"].value("code", "") == "MISSING_SLUGS",
           "JSONL missing rows should return MISSING_SLUGS");

    const auto jsonl_allow_resp =
        client.Post("/api/v1/import/jsonl", jsonl_missing,
                    {{"checklist", "api-test-checklist"},
                     {"instance_id", instance_id},
                     {"allow_new", "1"}},
                    "application/x-ndjson", auth_headers);
    Assert(jsonl_allow_resp.status == 200, "JSONL import with allow_new should return 200");
    const std::string jsonl_address = core::ComposeAddressId(jsonl_slug_id, instance_id);
    const auto jsonl_fetch =
        client.Get("/api/v1/slugs/" + jsonl_address, {}, auth_headers);
    Assert(jsonl_fetch.status == 200, "JSONL import should insert missing rows");

    const std::string jsonl_unknown_status =
        std::string{"{\"slug_id\":\""} + slug_id + "\"," +
        "\"checklist\":\"api-test-checklist\"," +
        "\"section\":\"Section A\"," +
        "\"procedure\":\"Proc A\"," +
        "\"action\":\"Do something\"," +
        "\"spec\":\"Expected\"," +
        "\"instructions\":\"Step 1\\nStep 2\"," +
        "\"result\":\"Imported result 2\"," +
        "\"status\":\"Pending\"," +
        "\"comment\":\"Imported comment 2\"}";
    const auto jsonl_unknown_resp =
        client.Post("/api/v1/import/jsonl", jsonl_unknown_status,
                    {{"checklist", "api-test-checklist"}, {"instance_id", instance_id}},
                    "application/x-ndjson", auth_headers);
    Assert(jsonl_unknown_resp.status == 200, "JSONL import should accept unknown status values");
    const auto jsonl_unknown_json =
        nlohmann::json::parse(jsonl_unknown_resp.body, nullptr, /*allow_exceptions=*/false);
    Assert(jsonl_unknown_json.value("ok", false), "JSONL import should set ok=true");
    const auto jsonl_unknown_warn = jsonl_unknown_json.value("warnings", nlohmann::json::array());
    bool status_warning = false;
    for (const auto& warn : jsonl_unknown_warn) {
      if (warn.value("code", "") == "STATUS_UNKNOWN") {
        status_warning = true;
        break;
      }
    }
    Assert(status_warning, "JSONL import should warn on unknown status values");

    nlohmann::json successor_payload{
        {"checklist", "api-test-checklist"},
        {"section", "Section A"},
        {"procedure", "Proc A Updated"},
        {"action", "Do something"},
        {"spec", "Expected v2"},
        {"instructions", "Step 1\nStep 2"},
        {"instance_principal", "instance||http-test-mapped"},
    };
    const auto successor_resp =
        client.Post("/api/v1/slugs", successor_payload.dump(), {}, "application/json", auth_headers);
    Assert(successor_resp.status == 201, "Create successor slug should return 201");
    const auto successor_json =
        nlohmann::json::parse(successor_resp.body, nullptr, /*allow_exceptions=*/false);
    const auto successor_slug_id = successor_json["data"].value("slug_id", "");
    const auto successor_instance_id = successor_json["data"].value("instance_id", "");
    Assert(!successor_slug_id.empty(), "Successor slug must return slug_id");
    Assert(!successor_instance_id.empty(), "Successor slug must return instance_id");

    nlohmann::json successor_rel{
        {"subject_slug_id", slug_id},
        {"predicate", "slugSuccessor"},
        {"target_slug_id", successor_slug_id},
    };
    const auto successor_rel_resp =
        client.Post("/api/v1/relationships/template", successor_rel.dump(), {}, "application/json", auth_headers);
    Assert(successor_rel_resp.status == 201, "Create slugSuccessor relationship should return 201");

    const std::string jsonl_lineage =
        std::string{"{\"slug_id\":\""} + slug_id + "\"," +
        "\"checklist\":\"api-test-checklist\"," +
        "\"result\":\"Mapped result\"," +
        "\"status\":\"Pass\"," +
        "\"comment\":\"Mapped comment\"}";
    const auto jsonl_lineage_resp =
        client.Post("/api/v1/import/jsonl", jsonl_lineage,
                    {{"checklist", "api-test-checklist"}, {"instance_id", successor_instance_id}},
                    "application/x-ndjson", auth_headers);
    Assert(jsonl_lineage_resp.status == 200, "JSONL import should map lineage to latest slug");
    const auto jsonl_lineage_json =
        nlohmann::json::parse(jsonl_lineage_resp.body, nullptr, /*allow_exceptions=*/false);
    const auto jsonl_lineage_warn =
        jsonl_lineage_json.value("warnings", nlohmann::json::array());
    bool lineage_warning = false;
    for (const auto& warn : jsonl_lineage_warn) {
      if (warn.value("code", "") == "SLUG_LINEAGE_ALIAS") {
        lineage_warning = true;
        break;
      }
    }
    Assert(lineage_warning, "JSONL import should warn when lineage alias is applied");
    const std::string mapped_address =
        core::ComposeAddressId(successor_slug_id, successor_instance_id);
    const auto mapped_fetch =
        client.Get("/api/v1/slugs/" + mapped_address, {}, auth_headers);
    Assert(mapped_fetch.status == 200, "Mapped successor slug should be updated");
    RecordStep(current_step, true, "jsonl import ok");

    // Workspace Markdown list/import/export (file-based authoring workflow).
    current_step = "workspace markdown";
    const std::string ws_checklist = "ws-md-checklist";
    const std::string ws_pack = "test-pack";
    const auto md_templates_root = library_root / ws_pack / ws_checklist;
    {
      std::error_code ec;
      std::filesystem::create_directories(md_templates_root, ec);
    }
    const std::string ws_section = "section1";
    const std::string ws_proc_a = "proc1";
    const std::string ws_proc_b = "proc2";
    const std::string ws_action_a = "action1";
    const std::string ws_spec_a = "spec1";
    const std::string ws_instr_a = "instructions1";
    const std::string ws_action_b = "action2";
    const std::string ws_spec_b = std::string("spec2 ") + "\xE2\x82\xAC";
    const std::string ws_instr_b = "instructions2";
    const auto ws_slug_a =
        core::ComputeSlugId(ws_checklist, ws_section, ws_proc_a, ws_action_a, ws_spec_a, ws_instr_a);
    const auto ws_slug_b =
        core::ComputeSlugId(ws_checklist, ws_section, ws_proc_b, ws_action_b, ws_spec_b, ws_instr_b);
    const auto ws_template_instance_id = core::ComputeInstanceId("template||default");
    const auto ws_target_address =
        core::ComposeAddressId(ws_slug_b, ws_template_instance_id);
    const std::string ws_section_proc_target = "(" + ws_section + ", " + ws_proc_b + ")";

    const std::string ws_markdown =
        "# Checklist: " + ws_checklist + "\n\n" +
        "## Section: " + ws_section + "\n\n" +
        "### Procedure: " + ws_proc_a + "\n" +
        "- Action: " + ws_action_a + "\n" +
        "- Spec: " + ws_spec_a + "\n" +
        "- Result: \n" +
        "- Status: \n" +
        "- Comment: \n\n" +
        "#### Instructions\n" +
        ws_instr_a + "\n" +
        "#### Relationships\n" +
        "- " + ws_slug_a + "\n" +
        "- passPropagateValidatedPass " + ws_target_address + "\n" +
        "- passVerifyImpliedPass " + ws_slug_b + "\n" +
        "- passRequiresVerified " + ws_section_proc_target + "\n\n" +
        "### Procedure: " + ws_proc_b + "\n" +
        "- Action: " + ws_action_b + "\n" +
        "- Spec: " + ws_spec_b + "\n" +
        "- Result: \n" +
        "- Status: \n" +
        "- Comment: \n\n" +
        "#### Instructions\n" +
        ws_instr_b + "\n";

    const auto ws_file = md_templates_root / "checklist.md";
    {
      std::ofstream out(ws_file, std::ios::binary | std::ios::trunc);
      Assert(static_cast<bool>(out), "Workspace markdown file should be writable");
      out << ws_markdown;
      out.close();
    }

    const auto ws_list = client.Get("/api/v1/workspace/markdown/templates", {}, auth_headers);
    Assert(ws_list.status == 200, "Workspace markdown templates list should return 200");
    const auto ws_list_json =
        nlohmann::json::parse(ws_list.body, nullptr, /*allow_exceptions=*/false);
    Assert(ws_list_json.value("ok", false), "Workspace markdown list should set ok=true");
    const auto ws_items = ws_list_json["data"].value("items", nlohmann::json::array());
    bool found_ws_file = false;
    for (const auto& item : ws_items) {
      if (item.value("pack", "") == ws_pack && item.value("checklist", "") == ws_checklist) {
        found_ws_file = true;
        Assert(item.value("valid", false), "Workspace markdown file should validate");
        Assert(item.value("parsed_checklist", "") == ws_checklist,
               "Workspace markdown file checklist must match heading");
        Assert(item.value("template_relationships", 0) == 3,
               "Workspace markdown file should include three template relationships");
        const auto warnings = item.value("warnings", nlohmann::json::array());
        Assert(warnings.is_array() && !warnings.empty(),
               "Workspace markdown list should include unicode warnings for unsupported characters");
        Assert(warnings[0].value("code", "") == "UNSUPPORTED_UNICODE",
               "Workspace markdown warnings should report unsupported unicode");
        break;
      }
    }
    Assert(found_ws_file, "Workspace markdown list must include the test file");

    nlohmann::json ws_import_payload{
        {"pack", ws_pack},
        {"checklist", ws_checklist},
        {"instance_principal", "template||default"},
        {"apply_data", false},
        {"replace_instance", true},
    };
    const auto ws_import = client.Post("/api/v1/workspace/markdown/import", ws_import_payload.dump(),
                                       {}, "application/json", auth_headers);
    Assert(ws_import.status == 200, "Workspace markdown import should return 200");
    const auto ws_import_json =
        nlohmann::json::parse(ws_import.body, nullptr, /*allow_exceptions=*/false);
    Assert(ws_import_json.value("ok", false), "Workspace markdown import should set ok=true");
    const std::string ws_instance_id = ws_import_json["data"].value("instance_id", "");
    Assert(!ws_instance_id.empty(), "Workspace markdown import must return instance_id");

    nlohmann::json ws_apply_payload{
        {"pack", ws_pack},
        {"checklist", ws_checklist},
        {"instance_principal", "template||default"},
        {"apply_data", true},
        {"apply_instance_principal", "template||default"},
        {"replace_instance", false},
    };
    const auto ws_apply = client.Post("/api/v1/workspace/markdown/import", ws_apply_payload.dump(),
                                      {}, "application/json", auth_headers);
    Assert(ws_apply.status == 200, "Workspace markdown apply_data should return 200");
    const auto ws_apply_json =
        nlohmann::json::parse(ws_apply.body, nullptr, /*allow_exceptions=*/false);
    Assert(ws_apply_json.value("ok", false), "Workspace markdown apply_data should set ok=true");
    Assert(ws_apply_json["data"].value("apply_instance_id", "") == ws_instance_id,
           "Workspace markdown apply_data must report apply_instance_id");

    // Template relationship should be created for ws_slug_a -> ws_slug_b.
    const auto trel_ws_list =
        client.Get("/api/v1/relationships/template", {{"subject_slug_id", ws_slug_a}}, auth_headers);
    Assert(trel_ws_list.status == 200, "Template relationship list should return 200");
    const auto trel_ws_json =
        nlohmann::json::parse(trel_ws_list.body, nullptr, /*allow_exceptions=*/false);
    const auto trel_ws_items = trel_ws_json["data"].value("items", nlohmann::json::array());
    bool found_ws_rel_address = false;
    bool found_ws_rel_slug = false;
    bool found_ws_rel_tuple = false;
    for (const auto& rel : trel_ws_items) {
      if (rel.value("subject_slug_id", "") == ws_slug_a &&
          rel.value("target_slug_id", "") == ws_slug_b) {
        const auto predicate = rel.value("predicate", "");
        if (predicate == "passPropagateValidatedPass") found_ws_rel_address = true;
        if (predicate == "passVerifyImpliedPass") found_ws_rel_slug = true;
        if (predicate == "passRequiresVerified") found_ws_rel_tuple = true;
      }
    }
    Assert(found_ws_rel_address, "Workspace markdown import must create the address-id target relationship");
    Assert(found_ws_rel_slug, "Workspace markdown import must create the slug-id target relationship");
    Assert(found_ws_rel_tuple, "Workspace markdown import must create the section/procedure target relationship");

    // Derived address relationship should be created in the template/root instance.
    const auto ws_addr_a = core::ComposeAddressId(ws_slug_a, ws_instance_id);
    const auto ws_addr_b = core::ComposeAddressId(ws_slug_b, ws_instance_id);
    const auto arel_ws_graph =
        client.Get("/api/v1/relationships/address/" + ws_addr_a, {}, auth_headers);
    Assert(arel_ws_graph.status == 200, "Address relationship graph should return 200");
    const auto arel_ws_json =
        nlohmann::json::parse(arel_ws_graph.body, nullptr, /*allow_exceptions=*/false);
    const auto outgoing = arel_ws_json["data"].value("outgoing", nlohmann::json::array());
    bool found_outgoing = false;
    for (const auto& edge : outgoing) {
      if (edge.value("predicate", "") == "passPropagateValidatedPass" &&
          edge.value("target", "") == ws_addr_b) {
        found_outgoing = true;
        break;
      }
    }
    Assert(found_outgoing, "Workspace markdown import must derive an outgoing address relationship");

    current_step = "graph projection";
    const auto graph_response =
        client.Get("/api/v1/visualizations/graph",
                   {{"checklist", ws_checklist}, {"instance_id", ws_instance_id}, {"pack", ws_pack}},
                   auth_headers);
    Assert(graph_response.status == 200, "Graph projection should return 200");
    const auto graph_response_json =
        nlohmann::json::parse(graph_response.body, nullptr, /*allow_exceptions=*/false);
    Assert(graph_response_json.value("ok", false), "Graph projection should set ok=true");
    const auto graph_data = graph_response_json["data"];
    Assert(graph_data.value("schema", "") == "chax-graph-view-v1",
           "Graph projection should report the versioned schema");
    Assert(graph_data.value("nodes", nlohmann::json::array()).size() == 2,
           "Graph projection should contain selected checklist rows");
    bool graph_has_order_edge = false;
    bool graph_has_relationship_edge = false;
    for (const auto& edge : graph_data.value("edges", nlohmann::json::array())) {
      if (edge.value("kind", "") == "checklistOrder") {
        graph_has_order_edge = true;
      }
      if (edge.value("kind", "") == "relationship" &&
          edge.value("predicate", "") == "passPropagateValidatedPass") {
        graph_has_relationship_edge = true;
      }
    }
    Assert(graph_has_order_edge, "Graph projection should derive checklist order edges");
    Assert(graph_has_relationship_edge, "Graph projection should preserve relationship edges");

    nlohmann::json graph_export_payload{
        {"checklist", ws_checklist},
        {"pack", ws_pack},
        {"instance_id", ws_instance_id},
    };
    const auto graph_export = client.Post("/api/v1/workspace/visualizations/export",
                                          graph_export_payload.dump(), {}, "application/json",
                                          auth_headers);
    Assert(graph_export.status == 201, "Graph export should return 201");
    const auto graph_export_json =
        nlohmann::json::parse(graph_export.body, nullptr, /*allow_exceptions=*/false);
    Assert(graph_export_json.value("ok", false), "Graph export should set ok=true");
    const auto graph_export_root = md_templates_root / "visualizations" / ws_instance_id;
    Assert(std::filesystem::exists(graph_export_root / "graph.json"),
           "Graph export should write JSON under the checklist visualizations folder");
    Assert(std::filesystem::exists(graph_export_root / "section-flow.dot"),
           "Graph export should write DOT under the checklist visualizations folder");
    Assert(std::filesystem::exists(graph_export_root / "section-flow.mmd"),
           "Graph export should write Mermaid under the checklist visualizations folder");
    RecordStep(current_step, true, "graph view and asset-pack exports ok");

    current_step = "workspace markdown";
    nlohmann::json ws_export_payload{
        {"checklist", ws_checklist},
        {"pack", ws_pack},
        {"include_data", false},
        {"instance_id", ws_instance_id},
    };
    const auto ws_export = client.Post("/api/v1/workspace/markdown/export", ws_export_payload.dump(),
                                       {}, "application/json", auth_headers);
    Assert(ws_export.status == 201, "Workspace markdown export should return 201");
    const auto ws_export_path = md_templates_root / "checklist.md";
    Assert(std::filesystem::exists(ws_export_path),
           "Workspace markdown export should write the target file");
    {
      std::ifstream in(ws_export_path, std::ios::binary);
      Assert(static_cast<bool>(in), "Workspace markdown export file should be readable");
      std::ostringstream oss;
      oss << in.rdbuf();
      const std::string md = oss.str();
      Assert(md.find("#### Relationships") != std::string::npos,
             "Workspace markdown export must include Relationships sections");
      Assert(md.find("- " + ws_slug_a + "\n") != std::string::npos,
             "Workspace markdown export must include slug_id source identity lines");
      Assert(md.find("passPropagateValidatedPass " + ws_slug_b) != std::string::npos,
             "Workspace markdown export must include slug-based relationship edges");
    }

    const std::string dup_checklist = "ws-duplicate-owned-checklist";
    const std::string dup_pack_a = "owner-a";
    const std::string dup_pack_b = "owner-b";
    const auto dup_root_a = library_root / dup_pack_a / dup_checklist;
    const auto dup_root_b = library_root / dup_pack_b / dup_checklist;
    {
      std::error_code ec;
      std::filesystem::create_directories(dup_root_a, ec);
      std::filesystem::create_directories(dup_root_b, ec);
    }
    const std::string dup_common_proc = "Common procedure";
    const std::string dup_common_action = "Shared row";
    const std::string dup_common_spec = "Shared spec";
    const std::string dup_common_instructions = "Shared instructions";
    const std::string dup_alpha_proc = "Alpha procedure";
    const std::string dup_beta_proc = "Beta procedure";
    const auto dup_common_slug = core::ComputeSlugId(dup_checklist, "main", dup_common_proc, dup_common_action,
                                                     dup_common_spec, dup_common_instructions);
    const auto dup_alpha_slug =
        core::ComputeSlugId(dup_checklist, "main", dup_alpha_proc, "Alpha only", "Alpha spec", "Alpha instructions");
    const auto dup_beta_slug =
        core::ComputeSlugId(dup_checklist, "main", dup_beta_proc, "Beta only", "Beta spec", "Beta instructions");
    const auto build_dup_markdown = [&](const std::string &proc, const std::string &action, const std::string &spec,
                                        const std::string &instructions, const std::string &slug_id) {
      return "# Checklist: " + dup_checklist + "\n\n" + "## Section: main\n\n" + "### Procedure: " + dup_common_proc +
             "\n" + "- Action: " + dup_common_action + "\n" + "- Spec: " + dup_common_spec + "\n" + "- Result: \n" +
             "- Status: \n" + "- Comment: \n\n" + "#### Instructions\n" + dup_common_instructions + "\n\n" +
             "#### Relationships\n" + "- " + dup_common_slug + "\n\n" + "---\n\n" + "### Procedure: " + proc + "\n" +
             "- Action: " + action + "\n" + "- Spec: " + spec + "\n" + "- Result: \n" + "- Status: \n" +
             "- Comment: \n\n" + "#### Instructions\n" + instructions + "\n\n" + "#### Relationships\n" + "- " +
             slug_id + "\n";
    };
    {
      std::ofstream out(dup_root_a / "checklist.md", std::ios::binary | std::ios::trunc);
      out << build_dup_markdown(dup_alpha_proc, "Alpha only", "Alpha spec", "Alpha instructions", dup_alpha_slug);
    }
    {
      std::ofstream out(dup_root_b / "checklist.md", std::ios::binary | std::ios::trunc);
      out << build_dup_markdown(dup_beta_proc, "Beta only", "Beta spec", "Beta instructions", dup_beta_slug);
    }

    for (const auto &pack_name : {dup_pack_a, dup_pack_b}) {
      nlohmann::json import_payload{
          {"pack", pack_name},
          {"checklist", dup_checklist},
          {"instance_principal", "template||default"},
          {"replace_instance", true},
      };
      const auto import_response =
          client.Post("/api/v1/workspace/markdown/import", import_payload.dump(), {}, "application/json", auth_headers);
      Assert(import_response.status == 200, "Duplicate-owner import should return 200");
    }

    const auto dup_a_list = client.Get("/api/v1/slugs",
                                       {{"checklist", dup_checklist},
                                        {"instance_id", ws_template_instance_id},
                                        {"pack", dup_pack_a},
                                        {"checklist_dir", dup_checklist}},
                                       auth_headers);
    const auto dup_b_list = client.Get("/api/v1/slugs",
                                       {{"checklist", dup_checklist},
                                        {"instance_id", ws_template_instance_id},
                                        {"pack", dup_pack_b},
                                        {"checklist_dir", dup_checklist}},
                                       auth_headers);
    Assert(dup_a_list.status == 200 && dup_b_list.status == 200,
           "Duplicate-owner filtered slug lists should return 200");
    const auto dup_a_json = nlohmann::json::parse(dup_a_list.body, nullptr, /*allow_exceptions=*/false);
    const auto dup_b_json = nlohmann::json::parse(dup_b_list.body, nullptr, /*allow_exceptions=*/false);
    const auto dup_a_items = dup_a_json["data"].value("items", nlohmann::json::array());
    const auto dup_b_items = dup_b_json["data"].value("items", nlohmann::json::array());
    Assert(dup_a_items.size() == 2 && dup_b_items.size() == 2,
           "Duplicate-owner filters should preserve shared rows and isolate owned rows");
    bool dup_a_has_alpha = false;
    bool dup_a_has_beta = false;
    bool dup_b_has_alpha = false;
    bool dup_b_has_beta = false;
    for (const auto &item : dup_a_items) {
      dup_a_has_alpha = dup_a_has_alpha || item.value("slug_id", "") == dup_alpha_slug;
      dup_a_has_beta = dup_a_has_beta || item.value("slug_id", "") == dup_beta_slug;
    }
    for (const auto &item : dup_b_items) {
      dup_b_has_alpha = dup_b_has_alpha || item.value("slug_id", "") == dup_alpha_slug;
      dup_b_has_beta = dup_b_has_beta || item.value("slug_id", "") == dup_beta_slug;
    }
    Assert(dup_a_has_alpha && !dup_a_has_beta && dup_b_has_beta && !dup_b_has_alpha,
           "Duplicate-owner filters should not leak pack-specific rows");

    nlohmann::json dup_ambiguous_export{
        {"checklist", dup_checklist},
        {"include_data", false},
        {"instance_id", ws_template_instance_id},
    };
    const auto dup_ambiguous_response = client.Post("/api/v1/workspace/markdown/export", dup_ambiguous_export.dump(),
                                                    {}, "application/json", auth_headers);
    Assert(dup_ambiguous_response.status == 400, "Duplicate-owner export without pack should reveal ambiguity");
    Assert(dup_ambiguous_response.body.find("AMBIGUOUS_CHECKLIST_OWNERSHIP") != std::string::npos,
           "Duplicate-owner export should report ownership ambiguity");

    for (const auto &pack_name : {dup_pack_a, dup_pack_b}) {
      nlohmann::json export_payload{
          {"checklist", dup_checklist},
          {"pack", pack_name},
          {"checklist_dir", dup_checklist},
          {"include_data", false},
          {"instance_id", ws_template_instance_id},
      };
      const auto export_response =
          client.Post("/api/v1/workspace/markdown/export", export_payload.dump(), {}, "application/json", auth_headers);
      Assert(export_response.status == 201, "Duplicate-owner explicit export should return 201");
    }
    {
      std::ifstream in(dup_root_a / "checklist.md", std::ios::binary);
      std::ostringstream oss;
      oss << in.rdbuf();
      const std::string md = oss.str();
      Assert(md.find("Alpha only") != std::string::npos && md.find("Beta only") == std::string::npos,
             "Pack A export should include only Pack A-specific rows");
    }
    {
      std::ifstream in(dup_root_b / "checklist.md", std::ios::binary);
      std::ostringstream oss;
      oss << in.rdbuf();
      const std::string md = oss.str();
      Assert(md.find("Beta only") != std::string::npos && md.find("Alpha only") == std::string::npos,
             "Pack B export should include only Pack B-specific rows");
    }
    RecordStep(current_step, true, "workspace markdown ok");

    current_step = "asset pack archive";
    if (ResolveSevenZipForTest().empty()) {
      RecordStep(current_step, true, "skipped because 7-Zip is not available");
    } else {
      const auto archive_assets_dir = md_templates_root / "data";
      {
        std::error_code ec;
        std::filesystem::create_directories(archive_assets_dir, ec);
      }
      {
        std::ofstream out(archive_assets_dir / "lookup.csv", std::ios::binary | std::ios::trunc);
        out << "key,value\ntransport,ok\n";
      }
      const auto archive_output_dir = artifacts_root / "asset-archives";
      {
        std::error_code ec;
        std::filesystem::create_directories(archive_output_dir, ec);
      }
      const auto chk_archive = archive_output_dir / "ws-md-checklist.chk";
      const auto seven_z_archive = archive_output_dir / "ws-md-checklist.7z";
      const auto zip_archive = archive_output_dir / "ws-md-checklist.zip";

      const auto export_asset_pack = [&](const std::filesystem::path &output_path) {
        nlohmann::json payload{
            {"pack", ws_pack},
            {"checklist", ws_checklist},
            {"output_path", output_path.string()},
        };
        const auto response =
            client.Post("/api/v1/workspace/asset-pack/export", payload.dump(), {}, "application/json", auth_headers);
        Assert(response.status == 201, "Asset pack archive export should return 201");
        Assert(std::filesystem::exists(output_path), "Asset pack archive export should create the archive");
      };
      export_asset_pack(chk_archive);
      export_asset_pack(seven_z_archive);
      export_asset_pack(zip_archive);

      const auto import_asset_pack = [&](const std::filesystem::path &archive_path, const std::string &pack_name,
                                         const std::string &checklist_dir) {
        nlohmann::json payload{
            {"archive_path", archive_path.string()},
            {"pack", pack_name},
            {"checklist_dir", checklist_dir},
            {"replace_files", true},
            {"replace_instance", true},
        };
        const auto response =
            client.Post("/api/v1/workspace/asset-pack/import", payload.dump(), {}, "application/json", auth_headers);
        Assert(response.status == 201, "Asset pack archive import should return 201, got " +
                                           std::to_string(response.status) + ": " + response.body);
        const auto parsed = nlohmann::json::parse(response.body, nullptr, /*allow_exceptions=*/false);
        Assert(parsed.value("ok", false), "Asset pack archive import should set ok=true");
        Assert(parsed["data"].value("imported", 0) == 1,
               "Asset pack archive import should report one imported checklist");
        const auto target_root = library_root / pack_name / checklist_dir;
        Assert(std::filesystem::exists(target_root / "checklist.md"),
               "Asset pack archive import should restore checklist.md");
        Assert(std::filesystem::exists(target_root / "data" / "lookup.csv"),
               "Asset pack archive import should restore sidecar data assets");

        const auto imported_slugs = client.Get("/api/v1/slugs",
                                               {{"checklist", ws_checklist},
                                                {"instance_id", ws_template_instance_id},
                                                {"pack", pack_name},
                                                {"checklist_dir", checklist_dir}},
                                               auth_headers);
        Assert(imported_slugs.status == 200, "Imported asset pack slug query should return 200");
        const auto imported_json = nlohmann::json::parse(imported_slugs.body, nullptr, /*allow_exceptions=*/false);
        const auto imported_items = imported_json["data"].value("items", nlohmann::json::array());
        bool has_slug_a = false;
        bool has_slug_b = false;
        for (const auto &item : imported_items) {
          has_slug_a = has_slug_a || item.value("slug_id", "") == ws_slug_a;
          has_slug_b = has_slug_b || item.value("slug_id", "") == ws_slug_b;
        }
        Assert(has_slug_a && has_slug_b,
               "Asset pack archive import should load restored checklist rows into the database");
      };
      import_asset_pack(chk_archive, "archive-pack", "archive-ws-md-checklist");
      import_asset_pack(zip_archive, "zip-pack", "zip-ws-md-checklist");
      RecordStep(current_step, true, "asset pack archive export/import ok");
    }

    // Predicate governance catalog should expose seeded predicates.
    current_step = "relationships api";
    const auto predicates_list =
        client.Get("/api/v1/predicates", {{"limit", "50"}}, auth_headers);
    Assert(predicates_list.status == 200, "Predicates list should return 200");
    const auto predicates_json =
        nlohmann::json::parse(predicates_list.body, nullptr, /*allow_exceptions=*/false);
    Assert(predicates_json.value("ok", false), "Predicates list should set ok=true");
    const auto predicate_items =
        predicates_json["data"].value("items", nlohmann::json::array());
    bool found_pass_propagate = false;
    for (const auto& item : predicate_items) {
      if (item.value("name", "") == "passPropagateValidatedPass") {
        found_pass_propagate = true;
        break;
      }
    }
    Assert(found_pass_propagate, "Predicates list must include seeded passPropagateValidatedPass");

    // Predicate upsert should trim/validate and be visible in subsequent listings.
    nlohmann::json predicate_create_payload{
        {"name", "Blocks "},
        {"kind", "extension"},
        {"status", "active"},
        {"description", "Smoke predicate for relationship tests."},
        {"meta", nlohmann::json::object()},
    };
    const auto predicate_create =
        client.Post("/api/v1/predicates", predicate_create_payload.dump(), {}, "application/json",
                    auth_headers);
    Assert(predicate_create.status == 201, "Predicate create should return 201");
    const auto predicate_create_json =
        nlohmann::json::parse(predicate_create.body, nullptr, /*allow_exceptions=*/false);
    Assert(predicate_create_json.value("ok", false), "Predicate create should set ok=true");
    Assert(predicate_create_json["data"].value("name", "") == "Blocks",
           "Predicate create should trim and preserve case");

    const auto predicates_list2 =
        client.Get("/api/v1/predicates", {{"limit", "250"}}, auth_headers);
    Assert(predicates_list2.status == 200, "Predicates list after create should return 200");
    const auto predicates_json2 =
        nlohmann::json::parse(predicates_list2.body, nullptr, /*allow_exceptions=*/false);
    const auto predicate_items2 =
        predicates_json2["data"].value("items", nlohmann::json::array());
    bool found_blocks = false;
    for (const auto& item : predicate_items2) {
      if (item.value("name", "") == "Blocks") {
        found_blocks = true;
        break;
      }
    }
    Assert(found_blocks, "Predicates list must include upserted blocks predicate");

    // Template relationship create/list.
    nlohmann::json trel_payload{
        {"subject_slug_id", slug_id},
        {"predicate", "passVerifyValidatedPass"},
        {"target_slug_id", slug_id_b},
    };
    const auto trel_response =
        client.Post("/api/v1/relationships/template", trel_payload.dump(), {}, "application/json",
                    auth_headers);
    Assert(trel_response.status == 201, "Template relationship create should return 201");
    const auto trel_list = client.Get("/api/v1/relationships/template",
                                      {{"subject_slug_id", slug_id}, {"limit", "1"}},
                                      auth_headers);
    const auto trel_list_json =
        nlohmann::json::parse(trel_list.body, nullptr, /*allow_exceptions=*/false);
    Assert(trel_list_json.value("ok", false), "Template relationship list should set ok=true");

    // Address relationship create/list.
    nlohmann::json arel_payload{
        {"subject_address_id", address_id},
        {"predicate", "passPropagateValidatedPass"},
        {"target_address_id", address_id_b},
    };
    const auto arel_response =
        client.Post("/api/v1/relationships/address", arel_payload.dump(), {}, "application/json",
                    auth_headers);
    Assert(arel_response.status == 201, "Address relationship create should return 201");
    nlohmann::json arel_payload2{
        {"subject_address_id", address_id},
        {"predicate", "failPropagateValidatedFail"},
        {"target_address_id", address_id_b},
    };
    const auto arel_response2 =
        client.Post("/api/v1/relationships/address", arel_payload2.dump(), {}, "application/json",
                    auth_headers);
    Assert(arel_response2.status == 201, "Second address relationship create should return 201");
    const auto arel_list = client.Get("/api/v1/relationships/address", {{"limit", "1"}},
                                      auth_headers);
    const auto arel_list_json =
        nlohmann::json::parse(arel_list.body, nullptr, /*allow_exceptions=*/false);
    Assert(arel_list_json.value("ok", false), "Address relationship list should set ok=true");

    // Relationship daemon: passPropagateValidatedPass should set target to Pass and annotate comment.
    nlohmann::json target_prep{{"status", "Fail"}, {"comment", "prep for propagate"}};
    const auto target_prep_resp = client.Patch("/api/v1/slugs/" + address_id_b, target_prep.dump(),
                                               {}, "application/json", auth_headers);
    Assert(target_prep_resp.status == 200, "Target prep update should return 200");

    nlohmann::json source_fail{{"status", "Fail"}, {"comment", "source set fail"}};
    const auto source_fail_resp = client.Patch("/api/v1/slugs/" + address_id, source_fail.dump(),
                                               {}, "application/json", auth_headers);
    Assert(source_fail_resp.status == 200, "Source fail update should return 200");

    nlohmann::json source_pass{{"status", "Pass"}, {"comment", "source set pass"}};
    const auto source_pass_resp = client.Patch("/api/v1/slugs/" + address_id, source_pass.dump(),
                                               {}, "application/json", auth_headers);
    Assert(source_pass_resp.status == 200, "Source pass update should return 200");

    const auto target_after = client.Get("/api/v1/slugs/" + address_id_b, {}, auth_headers);
    Assert(target_after.status == 200, "Target fetch after propagate should return 200");
    const auto target_after_json =
        nlohmann::json::parse(target_after.body, nullptr, /*allow_exceptions=*/false);
    Assert(target_after_json.value("ok", false), "Target fetch after propagate should set ok=true");
    Assert(target_after_json["data"].value("status", "") == "Pass",
           "Target status should be set to Pass by relationship daemon");
    const std::string expected_comment =
        "Filled per relationship passPropagateValidatedPass " + address_id;
    Assert(target_after_json["data"].value("comment", "") == expected_comment,
           "Target comment must include predicate token and source address_id");

    // Second sweep: once the daemon wrote the target, it should still update it again when the
    // subject status changes (no permanent lockout).
    nlohmann::json source_fail2{{"status", "Fail"}, {"comment", "source set fail again"}};
    const auto source_fail_resp2 =
        client.Patch("/api/v1/slugs/" + address_id, source_fail2.dump(), {}, "application/json",
                     auth_headers);
    Assert(source_fail_resp2.status == 200, "Source fail update should return 200 (again)");

    const auto target_after2 = client.Get("/api/v1/slugs/" + address_id_b, {}, auth_headers);
    Assert(target_after2.status == 200, "Target fetch after second propagate should return 200");
    const auto target_after_json2 =
        nlohmann::json::parse(target_after2.body, nullptr, /*allow_exceptions=*/false);
    Assert(target_after_json2.value("ok", false),
           "Target fetch after second propagate should set ok=true");
    Assert(target_after_json2["data"].value("status", "") == "Fail",
           "Target status should be set to Fail by relationship daemon");
    const std::string expected_comment2 =
        "Filled per relationship failPropagateValidatedFail " + address_id;
    Assert(target_after_json2["data"].value("comment", "") == expected_comment2,
           "Target comment must include predicate token and source address_id (second sweep)");
    RecordStep(current_step, true, "relationships api ok");

    // Prefill predicates using slot-based tokens and CSV lookup.
    current_step = "prefill predicate";
    const auto repo_root = ResolveRepoRoot();
    const auto source_prefill_csv = repo_root / "tests" / "fixtures" / "csvFillChecklist" / "data" /
                                    "csvFillChecklist.csv";
    Assert(std::filesystem::exists(source_prefill_csv),
           "Prefill CSV dataset must exist for test");
    const auto prefill_dir = library_root / "examples" / "csvFillChecklist" / "data";
    {
      std::error_code ec;
      std::filesystem::create_directories(prefill_dir, ec);
    }
    const auto prefill_csv = prefill_dir / "csvFillChecklist.csv";
    {
      std::error_code ec;
      std::filesystem::copy_file(source_prefill_csv, prefill_csv,
                                 std::filesystem::copy_options::overwrite_existing, ec);
    }
    Assert(std::filesystem::exists(prefill_csv), "Prefill CSV dataset copy must exist for test");

    const std::string prefill_checklist = "csvFillChecklist";
    const std::string prefill_section = "csvFillTestSection";
    const std::string prefill_instance = "instance||prefill-test";

    auto make_prefill_payload = [&](const std::string& procedure,
                                    const std::string& action,
                                    const std::string& spec,
                                    const std::string& instructions) {
      return nlohmann::json{{"checklist", prefill_checklist},
                            {"section", prefill_section},
                            {"procedure", procedure},
                            {"action", action},
                            {"spec", spec},
                            {"instructions", instructions},
                            {"instance_principal", prefill_instance}};
    };

    const std::string slug_id_1 =
        core::ComputeSlugId(prefill_checklist, prefill_section, "csvFillTestProcedure1",
                            "csvFillTestAction1", "csvFillTestSpec1",
                            "csvFillTestInstructions1");
    const std::string slug_id_2 =
        core::ComputeSlugId(prefill_checklist, prefill_section, "csvFillTestProcedure2",
                            "csvFillTestAction2", "csvFillTestSpec2",
                            "csvFillTestInstructions2");
    const std::string slug_id_3 =
        core::ComputeSlugId(prefill_checklist, prefill_section, "csvFillTestProcedure3",
                            "csvFillTestAction3", "csvFillTestSpec3",
                            "csvFillTestInstructions3");
    const std::string slug_id_4 =
        core::ComputeSlugId(prefill_checklist, prefill_section, "csvFillTestProcedure4",
                            "csvFillTestAction4", "csvFillTestSpec4",
                            "csvFillTestInstructions4");
    const std::string slug_id_5 =
        core::ComputeSlugId(prefill_checklist, prefill_section, "csvFillTestProcedure5",
                            "csvFillTestAction5", "csvFillTestSpec5",
                            "csvFillTestInstructions5");
    Assert(slug_id_1 == "7KWKZ8613XAB2517", "Prefill slug 1 must match CSV header");
    Assert(slug_id_2 == "X4AC2JY7HEDD1KFD", "Prefill slug 2 must match CSV header");
    Assert(slug_id_3 == "NYX0W2AJXJB8BCMT", "Prefill slug 3 must match CSV header");
    Assert(slug_id_4 == "CH57CSDH3PAMDTWC", "Prefill slug 4 must match CSV header");
    Assert(slug_id_5 == "TH80MXC38K45WRDB", "Prefill slug 5 must match CSV header");

    const auto prefill_resp1 =
        client.Post("/api/v1/slugs",
                    make_prefill_payload("csvFillTestProcedure1", "csvFillTestAction1",
                                         "csvFillTestSpec1", "csvFillTestInstructions1")
                        .dump(),
                    {}, "application/json", auth_headers);
    Assert(prefill_resp1.status == 201, "Prefill slug 1 create should return 201");
    const auto prefill_json1 =
        nlohmann::json::parse(prefill_resp1.body, nullptr, /*allow_exceptions=*/false);
    Assert(prefill_json1.value("ok", false), "Prefill slug 1 create should set ok=true");
    Assert(prefill_json1["data"].value("slug_id", "") == slug_id_1,
           "Prefill slug 1 id must match expected slug_id");
    const std::string prefill_addr1 = prefill_json1["data"].value("address_id", "");

    const auto prefill_resp2 =
        client.Post("/api/v1/slugs",
                    make_prefill_payload("csvFillTestProcedure2", "csvFillTestAction2",
                                         "csvFillTestSpec2", "csvFillTestInstructions2")
                        .dump(),
                    {}, "application/json", auth_headers);
    Assert(prefill_resp2.status == 201, "Prefill slug 2 create should return 201");
    const auto prefill_json2 =
        nlohmann::json::parse(prefill_resp2.body, nullptr, /*allow_exceptions=*/false);
    const std::string prefill_addr2 = prefill_json2["data"].value("address_id", "");

    const auto prefill_resp3 =
        client.Post("/api/v1/slugs",
                    make_prefill_payload("csvFillTestProcedure3", "csvFillTestAction3",
                                         "csvFillTestSpec3", "csvFillTestInstructions3")
                        .dump(),
                    {}, "application/json", auth_headers);
    Assert(prefill_resp3.status == 201, "Prefill slug 3 create should return 201");
    const auto prefill_json3 =
        nlohmann::json::parse(prefill_resp3.body, nullptr, /*allow_exceptions=*/false);
    const std::string prefill_addr3 = prefill_json3["data"].value("address_id", "");

    const auto prefill_resp4 =
        client.Post("/api/v1/slugs",
                    make_prefill_payload("csvFillTestProcedure4", "csvFillTestAction4",
                                         "csvFillTestSpec4", "csvFillTestInstructions4")
                        .dump(),
                    {}, "application/json", auth_headers);
    Assert(prefill_resp4.status == 201, "Prefill slug 4 create should return 201");
    const auto prefill_json4 =
        nlohmann::json::parse(prefill_resp4.body, nullptr, /*allow_exceptions=*/false);
    const std::string prefill_addr4 = prefill_json4["data"].value("address_id", "");

    const auto prefill_resp5 =
        client.Post("/api/v1/slugs",
                    make_prefill_payload("csvFillTestProcedure5", "csvFillTestAction5",
                                         "csvFillTestSpec5", "csvFillTestInstructions5")
                        .dump(),
                    {}, "application/json", auth_headers);
    Assert(prefill_resp5.status == 201, "Prefill slug 5 create should return 201");
    const auto prefill_json5 =
        nlohmann::json::parse(prefill_resp5.body, nullptr, /*allow_exceptions=*/false);
    const std::string prefill_addr5 = prefill_json5["data"].value("address_id", "");

    nlohmann::json prefill_rel_1{
        {"subject_address_id", prefill_addr1},
        {"predicate", "ResultSearchPrefillResult"},
        {"target_address_id", prefill_addr2},
    };
    const auto prefill_rel_resp1 = client.Post("/api/v1/relationships/address",
                                               prefill_rel_1.dump(), {}, "application/json",
                                               auth_headers);
    Assert(prefill_rel_resp1.status == 201, "Prefill relationship 1 should return 201");

    nlohmann::json prefill_rel_2{
        {"subject_address_id", prefill_addr1},
        {"predicate", "ResultSearchPrefillResult"},
        {"target_address_id", prefill_addr3},
    };
    const auto prefill_rel_resp2 = client.Post("/api/v1/relationships/address",
                                               prefill_rel_2.dump(), {}, "application/json",
                                               auth_headers);
    Assert(prefill_rel_resp2.status == 201, "Prefill relationship 2 should return 201");

    nlohmann::json prefill_rel_3{
        {"subject_address_id", prefill_addr1},
        {"predicate", "ResultSearchPrefillResult"},
        {"target_address_id", prefill_addr4},
    };
    const auto prefill_rel_resp3 = client.Post("/api/v1/relationships/address",
                                               prefill_rel_3.dump(), {}, "application/json",
                                               auth_headers);
    Assert(prefill_rel_resp3.status == 201, "Prefill relationship 3 should return 201");

    nlohmann::json prefill_rel_4{
        {"subject_address_id", prefill_addr1},
        {"predicate", "ResultSearchPrefillComment"},
        {"target_address_id", prefill_addr5},
    };
    const auto prefill_rel_resp4 = client.Post("/api/v1/relationships/address",
                                               prefill_rel_4.dump(), {}, "application/json",
                                               auth_headers);
    Assert(prefill_rel_resp4.status == 201, "Prefill relationship 4 should return 201");
    append_predicate_summary(
        {{"event", "prefill_setup"},
         {"checklist", prefill_checklist},
         {"instance", prefill_instance},
         {"dataset", prefill_csv.string()},
         {"subject_address_id", prefill_addr1},
         {"subject_slug_id", slug_id_1},
         {"targets",
          nlohmann::json::array(
              {{{"address_id", prefill_addr2},
                {"slug_id", slug_id_2},
                {"predicate", "ResultSearchPrefillResult"}},
               {{"address_id", prefill_addr3},
                {"slug_id", slug_id_3},
                {"predicate", "ResultSearchPrefillResult"}},
               {{"address_id", prefill_addr4},
                {"slug_id", slug_id_4},
                {"predicate", "ResultSearchPrefillResult"}},
               {{"address_id", prefill_addr5},
                {"slug_id", slug_id_5},
                {"predicate", "ResultSearchPrefillComment"}}})}});

    auto fetch_slug = [&](const std::string& address_id) {
      const auto response = client.Get("/api/v1/slugs/" + address_id, {}, auth_headers);
      Assert(response.status == 200, "Prefill slug fetch should return 200");
      return nlohmann::json::parse(response.body, nullptr, /*allow_exceptions=*/false);
    };

    auto clear_result = [&](const std::string& address_id) {
      nlohmann::json clear_payload{{"result", ""}};
      const auto response = client.Patch("/api/v1/slugs/" + address_id, clear_payload.dump(), {},
                                         "application/json", auth_headers);
      Assert(response.status == 200, "Prefill target result clear should return 200");
    };

    auto clear_comment = [&](const std::string& address_id) {
      nlohmann::json clear_payload{{"comment", ""}};
      const auto response = client.Patch("/api/v1/slugs/" + address_id, clear_payload.dump(), {},
                                         "application/json", auth_headers);
      Assert(response.status == 200, "Prefill target comment clear should return 200");
    };

    nlohmann::json prefill_source_update{{"result", "csvFillTestResult111"}};
    const auto prefill_source_resp =
        client.Patch("/api/v1/slugs/" + prefill_addr1, prefill_source_update.dump(), {},
                     "application/json", auth_headers);
    Assert(prefill_source_resp.status == 200, "Prefill source update should return 200");

    const auto prefill_target2_json = fetch_slug(prefill_addr2);
    Assert(prefill_target2_json["data"].value("result", "") == "csvFillTestResult222",
           "Prefill target 2 result should match CSV lookup");

    const auto prefill_target3_json = fetch_slug(prefill_addr3);
    Assert(prefill_target3_json["data"].value("result", "") == "csvFillTestResult333",
           "Prefill target 3 result should match CSV lookup");

    const auto prefill_target4_json = fetch_slug(prefill_addr4);
    Assert(prefill_target4_json["data"].value("result", "") == "csvFillTestResult444",
           "Prefill target 4 result should match CSV lookup");

    const auto prefill_target5_json = fetch_slug(prefill_addr5);
    Assert(prefill_target5_json["data"].value("comment", "") == "csvFillTestComment555",
           "Prefill target 5 comment should match CSV lookup");
    append_predicate_summary(
        {{"event", "prefill_update"},
         {"checklist", prefill_checklist},
         {"instance", prefill_instance},
         {"dataset", prefill_csv.string()},
         {"subject_address_id", prefill_addr1},
         {"subject_value", "csvFillTestResult111"},
         {"targets",
          nlohmann::json::array(
              {{{"address_id", prefill_addr2},
                {"field", "result"},
                {"value", prefill_target2_json["data"].value("result", "")}},
               {{"address_id", prefill_addr3},
                {"field", "result"},
                {"value", prefill_target3_json["data"].value("result", "")}},
               {{"address_id", prefill_addr4},
                {"field", "result"},
                {"value", prefill_target4_json["data"].value("result", "")}},
               {{"address_id", prefill_addr5},
                {"field", "comment"},
                {"value", prefill_target5_json["data"].value("comment", "")}}})}});

    clear_result(prefill_addr2);
    clear_result(prefill_addr3);
    clear_result(prefill_addr4);
    clear_comment(prefill_addr5);

    nlohmann::json prefill_source_update2{{"result", "csvFillTestResult11"}};
    const auto prefill_source_resp2 =
        client.Patch("/api/v1/slugs/" + prefill_addr1, prefill_source_update2.dump(), {},
                     "application/json", auth_headers);
    Assert(prefill_source_resp2.status == 200, "Prefill source update 2 should return 200");

    const auto prefill_target2_json2 = fetch_slug(prefill_addr2);
    Assert(prefill_target2_json2["data"].value("result", "") == "csvFillTestResult22",
           "Prefill target 2 second result should match CSV lookup");

    const auto prefill_target3_json2 = fetch_slug(prefill_addr3);
    Assert(prefill_target3_json2["data"].value("result", "") == "csvFillTestResult33",
           "Prefill target 3 second result should match CSV lookup");

    const auto prefill_target4_json2 = fetch_slug(prefill_addr4);
    Assert(prefill_target4_json2["data"].value("result", "") == "csvFillTestResult44",
           "Prefill target 4 second result should match CSV lookup");

    const auto prefill_target5_json2 = fetch_slug(prefill_addr5);
    Assert(prefill_target5_json2["data"].value("comment", "") == "csvFillTestComment55",
           "Prefill target 5 second comment should match CSV lookup");
    append_predicate_summary(
        {{"event", "prefill_update"},
         {"checklist", prefill_checklist},
         {"instance", prefill_instance},
         {"dataset", prefill_csv.string()},
         {"subject_address_id", prefill_addr1},
         {"subject_value", "csvFillTestResult11"},
         {"targets",
          nlohmann::json::array(
              {{{"address_id", prefill_addr2},
                {"field", "result"},
                {"value", prefill_target2_json2["data"].value("result", "")}},
               {{"address_id", prefill_addr3},
                {"field", "result"},
                {"value", prefill_target3_json2["data"].value("result", "")}},
               {{"address_id", prefill_addr4},
                {"field", "result"},
                {"value", prefill_target4_json2["data"].value("result", "")}},
               {{"address_id", prefill_addr5},
                {"field", "comment"},
                {"value", prefill_target5_json2["data"].value("comment", "")}}})}});

    clear_result(prefill_addr2);
    clear_result(prefill_addr3);
    clear_result(prefill_addr4);
    clear_comment(prefill_addr5);

    nlohmann::json prefill_source_update3{{"result", "csvFillTestResult999"}};
    const auto prefill_source_resp3 =
        client.Patch("/api/v1/slugs/" + prefill_addr1, prefill_source_update3.dump(), {},
                     "application/json", auth_headers);
    Assert(prefill_source_resp3.status == 200, "Prefill source update 3 should return 200");

    const auto prefill_target2_json3 = fetch_slug(prefill_addr2);
    Assert(prefill_target2_json3["data"].value("result", "").empty(),
           "Prefill target 2 result should remain empty when no match");

    const auto prefill_target3_json3 = fetch_slug(prefill_addr3);
    Assert(prefill_target3_json3["data"].value("result", "").empty(),
           "Prefill target 3 result should remain empty when no match");

    const auto prefill_target4_json3 = fetch_slug(prefill_addr4);
    Assert(prefill_target4_json3["data"].value("result", "").empty(),
           "Prefill target 4 result should remain empty when no match");

    const auto prefill_target5_json3 = fetch_slug(prefill_addr5);
    Assert(prefill_target5_json3["data"].value("comment", "").empty(),
           "Prefill target 5 comment should remain empty when no match");
    append_predicate_summary(
        {{"event", "prefill_update"},
         {"checklist", prefill_checklist},
         {"instance", prefill_instance},
         {"dataset", prefill_csv.string()},
         {"subject_address_id", prefill_addr1},
         {"subject_value", "csvFillTestResult999"},
         {"targets",
          nlohmann::json::array(
              {{{"address_id", prefill_addr2},
                {"field", "result"},
                {"value", prefill_target2_json3["data"].value("result", "")}},
               {{"address_id", prefill_addr3},
                {"field", "result"},
                {"value", prefill_target3_json3["data"].value("result", "")}},
               {{"address_id", prefill_addr4},
                {"field", "result"},
                {"value", prefill_target4_json3["data"].value("result", "")}},
               {{"address_id", prefill_addr5},
                {"field", "comment"},
                {"value", prefill_target5_json3["data"].value("comment", "")}}})}});
    RecordStep(current_step, true, "prefill predicate ok");

    // Entity and instance catalog endpoints.
    current_step = "entity and instance";
    nlohmann::json entity_payload{
        {"principal", "user||tester"}, {"kind", "user"}, {"display_name", "Tester"}};
    const auto entity_resp =
        client.Post("/api/v1/entities", entity_payload.dump(), {}, "application/json", auth_headers);
    Assert(entity_resp.status == 200, "Entity create should return 200");
    const auto entities_list = client.Get("/api/v1/entities", {{"limit", "1"}}, auth_headers);
    Assert(entities_list.status == 200, "Entity list should return 200");

    nlohmann::json instance_payload{
        {"principal", "instance||http-test-catalog"}, {"label", "Catalog"}, {"meta", "{}"}};
    const auto instance_resp = client.Post("/api/v1/instances", instance_payload.dump(), {},
                                           "application/json", auth_headers);
    Assert(instance_resp.status == 200, "Instance create should return 200");
    const auto instances_list =
        client.Get("/api/v1/instances", {{"limit", "1"}}, auth_headers);
    Assert(instances_list.status == 200, "Instance list should return 200");
    RecordStep(current_step, true, "entity/instance ok");

    // Workspace scripts launch logs stay with the checklist asset pack.
    current_step = "workspace scripts";
    const std::string scripts_pack = "test-pack";
    const std::string scripts_checklist = "script-log-checklist";
    const auto script_checklist_root = library_root / scripts_pack / scripts_checklist;
    const auto script_bundle_root = script_checklist_root / "scripts";
    std::filesystem::create_directories(script_bundle_root);
#if defined(_WIN32)
    const auto script_path = script_bundle_root / "quick_exit.cmd";
    {
      std::ofstream script_file(script_path, std::ios::binary | std::ios::trunc);
      script_file << "@echo off\r\n";
      script_file << "echo script started\r\n";
      script_file << "exit /b 0\r\n";
      Assert(script_file.good(), "Workspace test script should be written");
    }
#else
    const auto script_path = script_bundle_root / "quick_exit.sh";
    {
      std::ofstream script_file(script_path, std::ios::binary | std::ios::trunc);
      script_file << "#!/usr/bin/env sh\n";
      script_file << "echo script started\n";
      script_file << "exit 0\n";
      Assert(script_file.good(), "Workspace test script should be written");
    }
    {
      std::error_code ec;
      std::filesystem::permissions(
          script_path,
          std::filesystem::perms::owner_exec | std::filesystem::perms::group_exec |
              std::filesystem::perms::others_exec,
          std::filesystem::perm_options::add, ec);
      Assert(!ec, "Workspace test script should be executable");
    }
#endif
    {
      nlohmann::json manifest{
          {"scripts",
           nlohmann::json::array(
               {{{"id", "quick_exit"},
                 {"label", "Quick Exit"},
                 {"description", "Minimal script for log path testing."},
                 {"path", script_path.filename().string()},
                 {"enabled", true}}})}};
      std::ofstream manifest_file(script_bundle_root / "scripts.json",
                                  std::ios::binary | std::ios::trunc);
      manifest_file << manifest.dump(2);
      Assert(manifest_file.good(), "Workspace scripts manifest should be written");
    }
    const auto scripts_list_resp =
        client.Get("/api/v1/workspace/scripts",
                   {{"pack", scripts_pack}, {"checklist", scripts_checklist}}, auth_headers);
    Assert(scripts_list_resp.status == 200, "Workspace scripts list should return 200");
    const auto scripts_list_json =
        nlohmann::json::parse(scripts_list_resp.body, nullptr, /*allow_exceptions=*/false);
    Assert(scripts_list_json.value("ok", false), "Workspace scripts list should set ok=true");
    const auto scripts_items = scripts_list_json["data"].value("items", nlohmann::json::array());
    Assert(scripts_items.is_array() && scripts_items.size() == 1,
           "Workspace scripts list should return the test script");

    nlohmann::json run_script_payload{{"pack", scripts_pack},
                                      {"checklist", scripts_checklist},
                                      {"script_id", "quick_exit"}};
    const auto run_script_resp =
        client.Post("/api/v1/workspace/scripts/run", run_script_payload.dump(), {},
                    "application/json", auth_headers);
    Assert(run_script_resp.status == 202, "Workspace script run should return 202");
    const auto run_script_json =
        nlohmann::json::parse(run_script_resp.body, nullptr, /*allow_exceptions=*/false);
    Assert(run_script_json.value("ok", false), "Workspace script run should set ok=true");
    const auto stdout_log = std::filesystem::path(run_script_json["data"].value("stdout_log", ""));
    const auto stderr_log = std::filesystem::path(run_script_json["data"].value("stderr_log", ""));
    const auto expected_logs_root = (script_checklist_root / "logs").lexically_normal();
    Assert(!stdout_log.empty(), "Workspace script run should return stdout_log");
    Assert(!stderr_log.empty(), "Workspace script run should return stderr_log");
    Assert(std::filesystem::exists(expected_logs_root),
           "Workspace script run should create the checklist-local logs directory");
    Assert(stdout_log.parent_path().lexically_normal() == expected_logs_root,
           "Workspace script stdout log should live under checklist-local logs");
    Assert(stderr_log.parent_path().lexically_normal() == expected_logs_root,
           "Workspace script stderr log should live under checklist-local logs");
    RecordStep(current_step, true, "workspace scripts ok");

    // Evaluation endpoints.
    current_step = "evaluation api";
    const auto eval_slug = client.Get("/api/v1/evaluate/slug/" + address_id, {}, auth_headers);
    Assert(eval_slug.status == 200, "Evaluate slug should return 200");
    const auto eval_slug_json =
        nlohmann::json::parse(eval_slug.body, nullptr, /*allow_exceptions=*/false);
    Assert(eval_slug_json.value("ok", false), "Evaluate slug should set ok=true");
    Assert(eval_slug_json["data"].contains("verify"),
           "Evaluate slug should include verify diagnostics");
    Assert(eval_slug_json["data"]["verify"].is_array(),
           "Evaluate slug verify diagnostics should be an array");
    nlohmann::json graph_payload{{"root_address_ids", nlohmann::json::array({address_id})}};
    const auto eval_graph =
        client.Post("/api/v1/evaluate/graph", graph_payload.dump(), {}, "application/json",
                    auth_headers);
    Assert(eval_graph.status == 200, "Evaluate graph should return 200");
    const auto eval_graph_json =
        nlohmann::json::parse(eval_graph.body, nullptr, /*allow_exceptions=*/false);
    Assert(eval_graph_json.value("ok", false), "Evaluate graph should set ok=true");
    const auto eval_nodes = eval_graph_json["data"].value("nodes", nlohmann::json::array());
    Assert(!eval_nodes.empty(), "Evaluate graph should return at least one node");
    Assert(eval_nodes[0].contains("verify"),
           "Evaluate graph node should include verify diagnostics");
    RecordStep(current_step, true, "evaluation ok");

    // Report export (LaTeX).
    current_step = "report export";
    nlohmann::json report_payload{{"checklist", "api-test-checklist"},
                                  {"instance_id", instance_id}};
    const auto report_resp =
        client.Post("/api/v1/export/report", report_payload.dump(), {}, "application/json",
                    auth_headers);
    Assert(report_resp.status == 201, "Report export should return 201");
    const auto report_json =
        nlohmann::json::parse(report_resp.body, nullptr, /*allow_exceptions=*/false);
    Assert(report_json.value("ok", false), "Report export should set ok=true");
    const auto report_path = report_json["data"].value("path", "");
    const auto report_dir = report_json["data"].value("directory", "");
    Assert(!report_path.empty(), "Report export should return a path");
    Assert(!report_dir.empty(), "Report export should return a directory");
    Assert(std::filesystem::exists(report_path), "Report file should exist on disk");
    Assert(std::filesystem::exists(report_dir), "Report directory should exist on disk");
    RecordStep(current_step, true, "report export ok");
    std::error_code ec;
    if (CleanArtifactsOnExit()) {
      std::filesystem::remove_all(artifacts_root, ec);
    }

    server.Stop();
    return 0;
  } catch (const std::exception& ex) {
    if (!current_step.empty()) {
      RecordStep(current_step, false, ex.what());
    }
    std::cerr << "http_api_test failure: " << ex.what() << std::endl;
    return 1;
  }
}
