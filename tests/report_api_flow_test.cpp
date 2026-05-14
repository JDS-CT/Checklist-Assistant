#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
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

namespace {

bool KeepArtifacts() {
  const char* env = std::getenv("CHAX_KEEP_TEST_ARTIFACTS");
  return env != nullptr && env[0] != '\0';
}

bool CleanArtifactsOnExit() {
  const char* env = std::getenv("CHAX_CLEAN_TEST_ARTIFACTS");
  return env != nullptr && env[0] != '\0';
}

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
  std::cout << "CHAX_STEP|report_api_flow_test|" << procedure << "|"
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

std::string ReadFile(const std::filesystem::path& path) {
  std::ifstream file(path);
  std::stringstream buffer;
  buffer << file.rdbuf();
  return buffer.str();
}

class TestServer {
 public:
  TestServer(std::string db_path, bool seed_demo)
      : db_path_(std::move(db_path)),
        store_(std::make_unique<core::ChecklistStore>(db_path_)),
        oauth_store_(std::make_unique<core::OAuthStore>(db_path_)),
        seed_demo_(seed_demo) {
    store_->Initialize(seed_demo);
    oauth_store_->Initialize();
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
      core::ConfigureServer(server_, *store_, *oauth_store_, config_);
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
    oauth_store_.reset();
    store_.reset();
    if (!error_.empty()) {
      throw std::runtime_error(error_);
    }
    if (!db_path_.empty() && !KeepArtifacts()) {
      RemoveIfExists(db_path_);
    }
  }

  const std::string& AccessToken() const { return access_token_; }

 private:
  platform::HttpServer server_;
  std::string db_path_;
  std::unique_ptr<core::ChecklistStore> store_;
  std::unique_ptr<core::OAuthStore> oauth_store_;
  core::ServerConfig config_;
  std::string client_id_ = "report-client";
  std::string client_secret_ = "report-secret";
  std::string admin_user_ = "admin";
  std::string admin_password_ = "password";
  bool seed_demo_ = false;
  bool configured_ = false;
  std::string access_token_;
  std::thread worker_;
  std::string host_;
  int port_ = 0;
  std::string error_;
  std::mutex mutex_;
};

std::string RandomSuffix() {
  const auto now = std::chrono::steady_clock::now().time_since_epoch();
  const auto nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
  return std::to_string(static_cast<unsigned long long>(nanos));
}

}  // namespace

int main() {
  std::string current_step;
  try {
    const auto artifacts_root = ResolveRepoRoot() / ".chax" / "test-artifacts" / "report-api-flow";
    {
      std::error_code ec;
      std::filesystem::remove_all(artifacts_root, ec);
      std::filesystem::create_directories(artifacts_root, ec);
    }
    const auto reports_root = artifacts_root / "reports";
    const auto templates_root =
        ResolveRepoRoot() / "tests" / "fixtures" / "formFillDemo" / "templates";
    SetEnvValue("CHAX_REPORTS_ROOT", reports_root.string());
    SetEnvValue("CHAX_REPORT_TEMPLATES_ROOT", templates_root.string());

    const auto db_path =
        (std::filesystem::temp_directory_path() / "chax-report-api-flow.db").string();
    RemoveIfExists(db_path);
    TestServer server(db_path, /*seed_demo=*/false);
    constexpr int kPort = 19992;
    server.Start("127.0.0.1", kPort);

    platform::HttpClient client("http://127.0.0.1:" + std::to_string(kPort));
    const std::map<std::string, std::string> auth_headers{
        {"Authorization", "Bearer " + server.AccessToken()}};

    const std::string suffix = RandomSuffix();
    const std::string checklist = "report-api-" + suffix;
    const std::string instance_principal = "instance||report-api-" + suffix;

    auto create_slug = [&](const std::string& checklist_name,
                           const std::string& instance_principal_name,
                           const std::string& section, const std::string& procedure,
                           const std::string& spec, const std::string& result,
                           const std::string& status, const std::string& comment) {
      nlohmann::json payload{
          {"checklist", checklist_name},
          {"section", section},
          {"procedure", procedure},
          {"action", procedure},
          {"spec", spec},
          {"instructions", "Auto generated test row"},
          {"instance_principal", instance_principal_name},
          {"result", result},
          {"status", status},
          {"comment", comment},
      };
      const auto response =
          client.Post("/api/v1/slugs", payload.dump(), {}, "application/json", auth_headers);
      Assert(response.status == 201, "Slug creation should return 201");
      const auto json =
          nlohmann::json::parse(response.body, nullptr, /*allow_exceptions=*/false);
      Assert(json.value("ok", false), "Slug creation should set ok=true");
      return std::make_pair(json["data"].value("address_id", ""),
                            json["data"].value("instance_id", ""));
    };

    current_step = "create slugs";
    const auto [address1, instance_id] =
        create_slug(checklist, instance_principal, "Alpha", "Inspect & Clean", "Spec_1", "Ready",
                    "Pass", "Keep tidy");
    Assert(!instance_id.empty(), "instance_id must be returned");
    const auto [address2, instance_id_2] =
        create_slug(checklist, instance_principal, "Beta", "Tighten", "Spec B", "Tight %1", "Fail",
                    "");
    Assert(instance_id_2 == instance_id, "All slugs should share the instance");
    const auto [address3, instance_id_3] =
        create_slug(checklist, instance_principal, "Gamma", "Skip NA", "Spec G", "None", "NA",
                    "Skip row");
    Assert(instance_id_3 == instance_id, "All slugs should share the instance");
    const auto [address4, instance_id_4] =
        create_slug(checklist, instance_principal, "Delta", "Unknown row", "Spec U", "Pending",
                    "Unknown", "");
    Assert(instance_id_4 == instance_id, "All slugs should share the instance");
    (void)address1;
    (void)address2;
    (void)address3;
    (void)address4;
    RecordStep(current_step, true, "slugs created");

    auto read_jsonl_rows = [&](const std::string& jsonl_path) {
      Assert(!jsonl_path.empty(), "JSONL path should be returned");
      Assert(std::filesystem::exists(jsonl_path), "JSONL snapshot should exist");
      const std::string jsonl_body = ReadFile(jsonl_path);
      Assert(!jsonl_body.empty(), "JSONL snapshot should not be empty");
      std::vector<nlohmann::json> rows;
      std::istringstream input(jsonl_body);
      std::string line;
      while (std::getline(input, line)) {
        if (line.empty()) {
          continue;
        }
        auto row = nlohmann::json::parse(line, nullptr, /*allow_exceptions=*/false);
        Assert(row.is_object(), "JSONL rows should be objects");
        rows.push_back(std::move(row));
      }
      Assert(!rows.empty(), "JSONL snapshot should contain rows");
      return rows;
    };
    auto export_report_jsonl_rows = [&](const nlohmann::json& payload) {
      const auto response = client.Post("/api/v1/export/report", payload.dump(), {},
                                        "application/json", auth_headers);
      Assert(response.status == 201, "Report export should return 201");
      const auto json =
          nlohmann::json::parse(response.body, nullptr, /*allow_exceptions=*/false);
      Assert(json.value("ok", false), "Report export should set ok=true");
      const auto jsonl_path = json["data"].value("jsonl_path", "");
      return read_jsonl_rows(jsonl_path);
    };
    auto contains_status = [](const std::vector<nlohmann::json>& rows, const std::string& status) {
      for (const auto& row : rows) {
        if (row.value("status", "") == status) {
          return true;
        }
      }
      return false;
    };

    // Export the report via API.
    current_step = "export report";
    nlohmann::json report_payload{{"checklist", checklist}, {"instance_id", instance_id}};
    const auto report_resp = client.Post("/api/v1/export/report", report_payload.dump(), {},
                                         "application/json", auth_headers);
    Assert(report_resp.status == 201, "Report export should return 201");
    const auto report_json =
        nlohmann::json::parse(report_resp.body, nullptr, /*allow_exceptions=*/false);
    Assert(report_json.value("ok", false), "Report export should set ok=true");
    const auto report_path = report_json["data"].value("path", "");
    const auto report_dir = report_json["data"].value("directory", "");
    const auto jsonl_path = report_json["data"].value("jsonl_path", "");
    Assert(!report_path.empty(), "Report path should be returned");
    Assert(!report_dir.empty(), "Report directory should be returned");
    Assert(std::filesystem::exists(report_path), "Report file should exist");
    const auto jsonl_rows = read_jsonl_rows(jsonl_path);
    const auto& jsonl_first = jsonl_rows.front();
    Assert(jsonl_first.contains("address_id"), "JSONL rows should include address_id");
    Assert(jsonl_first.contains("checklist"), "JSONL rows should include checklist");
    Assert(!jsonl_first.contains("instance_id"), "JSONL report should omit instance_id");
    Assert(!jsonl_first.contains("slug_id"), "JSONL report should omit slug_id");
    Assert(jsonl_rows.size() == 2, "Report JSONL should omit Unknown/NA rows");
    Assert(!contains_status(jsonl_rows, "NA"), "Report JSONL should omit NA rows");
    Assert(!contains_status(jsonl_rows, "Unknown"), "Report JSONL should omit Unknown rows");
    RecordStep(current_step, true, "report exported");

    current_step = "validate report";
    const std::string content = ReadFile(report_path);
    Assert(content.find("Inspect \\& Clean") != std::string::npos,
           "Pass row should appear in report");
    Assert(content.find("Tighten") != std::string::npos,
           "Fail row should appear in report");
    Assert(content.find("Skip NA") == std::string::npos,
           "NA rows should be filtered out");
    Assert(content.find("Keep tidy") != std::string::npos,
           "Comments should be present when provided");
    RecordStep(current_step, true, "report content ok");

    current_step = "export html report";
    nlohmann::json html_report_payload{{"checklist", checklist},
                                       {"instance_id", instance_id},
                                       {"format", "html"}};
    const auto html_report_resp = client.Post("/api/v1/export/report", html_report_payload.dump(),
                                              {}, "application/json", auth_headers);
    Assert(html_report_resp.status == 201, "HTML report export should return 201");
    const auto html_report_json =
        nlohmann::json::parse(html_report_resp.body, nullptr, /*allow_exceptions=*/false);
    Assert(html_report_json.value("ok", false), "HTML report export should set ok=true");
    Assert(html_report_json["data"].value("format", "") == "html",
           "HTML report export should report format=html");
    const auto html_report_path = html_report_json["data"].value("path", "");
    const auto html_report_dir = html_report_json["data"].value("directory", "");
    Assert(!html_report_path.empty(), "HTML report path should be returned");
    Assert(!html_report_dir.empty(), "HTML report directory should be returned");
    Assert(std::filesystem::exists(html_report_path), "HTML report file should exist");
    Assert(html_report_path.find(".html") != std::string::npos,
           "HTML report path should end in .html");
    const std::string html_content = ReadFile(html_report_path);
    Assert(html_content.find("<!doctype html>") != std::string::npos,
           "HTML report should emit an HTML document");
    Assert(html_content.find("Inspect &amp; Clean") != std::string::npos,
           "HTML report should escape row content");
    Assert(html_content.find("Skip NA") == std::string::npos,
           "HTML report should filter NA rows from AutoTables");
    RecordStep(current_step, true, "html report ok");

    current_step = "export report minimal jsonl";
    nlohmann::json minimal_payload{{"checklist", checklist},
                                   {"instance_id", instance_id},
                                   {"jsonl_mode", "minimal"}};
    const auto minimal_rows = export_report_jsonl_rows(minimal_payload);
    const auto& minimal_row = minimal_rows.front();
    Assert(minimal_row.contains("address_id"), "Minimal JSONL should include address_id");
    Assert(minimal_row.contains("result"), "Minimal JSONL should include result");
    Assert(minimal_row.contains("status"), "Minimal JSONL should include status");
    Assert(minimal_row.contains("comment"), "Minimal JSONL should include comment");
    Assert(!minimal_row.contains("checklist"), "Minimal JSONL should omit checklist");
    Assert(!minimal_row.contains("section"), "Minimal JSONL should omit section");
    Assert(!minimal_row.contains("procedure"), "Minimal JSONL should omit procedure");
    Assert(!minimal_row.contains("action"), "Minimal JSONL should omit action");
    Assert(!minimal_row.contains("spec"), "Minimal JSONL should omit spec");
    Assert(!minimal_row.contains("timestamp"), "Minimal JSONL should omit timestamp");
    Assert(!minimal_row.contains("address_order"), "Minimal JSONL should omit address_order");
    Assert(!minimal_row.contains("instructions"), "Minimal JSONL should omit instructions");
    Assert(!minimal_row.contains("relationships"), "Minimal JSONL should omit relationships");
    Assert(!minimal_row.contains("instance_id"), "Minimal JSONL should omit instance_id");
    Assert(!minimal_row.contains("slug_id"), "Minimal JSONL should omit slug_id by default");
    Assert(minimal_rows.size() == 2, "Minimal JSONL should omit Unknown/NA rows");
    Assert(!contains_status(minimal_rows, "NA"), "Minimal JSONL should omit NA rows");
    Assert(!contains_status(minimal_rows, "Unknown"), "Minimal JSONL should omit Unknown rows");
    RecordStep(current_step, true, "minimal jsonl ok");

    current_step = "export report full jsonl";
    nlohmann::json full_payload{{"checklist", checklist},
                                {"instance_id", instance_id},
                                {"jsonl_mode", "full"}};
    const auto full_rows = export_report_jsonl_rows(full_payload);
    const auto& full_row = full_rows.front();
    Assert(full_row.contains("address_id"), "Full JSONL should include address_id");
    Assert(full_row.contains("address_order"), "Full JSONL should include address_order");
    Assert(full_row.contains("checklist"), "Full JSONL should include checklist");
    Assert(full_row.contains("section"), "Full JSONL should include section");
    Assert(full_row.contains("procedure"), "Full JSONL should include procedure");
    Assert(full_row.contains("action"), "Full JSONL should include action");
    Assert(full_row.contains("spec"), "Full JSONL should include spec");
    Assert(full_row.contains("result"), "Full JSONL should include result");
    Assert(full_row.contains("status"), "Full JSONL should include status");
    Assert(full_row.contains("comment"), "Full JSONL should include comment");
    Assert(full_row.contains("timestamp"), "Full JSONL should include timestamp");
    Assert(full_row.contains("instructions"), "Full JSONL should include instructions");
    Assert(full_row.contains("relationships"), "Full JSONL should include relationships");
    Assert(!full_row.contains("instance_id"), "Full JSONL should omit instance_id");
    Assert(!full_row.contains("slug_id"), "Full JSONL should omit slug_id by default");
    Assert(full_rows.size() == 4, "Full JSONL should include Unknown/NA rows");
    Assert(contains_status(full_rows, "NA"), "Full JSONL should include NA rows");
    Assert(contains_status(full_rows, "Unknown"), "Full JSONL should include Unknown rows");
    RecordStep(current_step, true, "full jsonl ok");

    current_step = "export report slug-only jsonl";
    nlohmann::json slug_only_payload{{"checklist", checklist},
                                     {"instance_id", instance_id},
                                     {"jsonl_mode", "minimal"},
                                     {"jsonl_slug_only", true}};
    const auto slug_only_rows = export_report_jsonl_rows(slug_only_payload);
    const auto& slug_only_row = slug_only_rows.front();
    Assert(!slug_only_row.contains("address_id"), "Slug-only JSONL should omit address_id");
    Assert(slug_only_row.contains("slug_id"), "Slug-only JSONL should include slug_id");
    Assert(slug_only_row.contains("result"), "Slug-only JSONL should include result");
    Assert(slug_only_row.contains("status"), "Slug-only JSONL should include status");
    Assert(slug_only_row.contains("comment"), "Slug-only JSONL should include comment");
    Assert(!slug_only_row.contains("instance_id"), "Slug-only JSONL should omit instance_id");
    Assert(slug_only_rows.size() == 2, "Slug-only JSONL should omit Unknown/NA rows");
    Assert(!contains_status(slug_only_rows, "NA"), "Slug-only JSONL should omit NA rows");
    Assert(!contains_status(slug_only_rows, "Unknown"), "Slug-only JSONL should omit Unknown rows");
    RecordStep(current_step, true, "slug-only jsonl ok");

    current_step = "fillable report";
    const std::string fillable_checklist = "formFillDemo";
    const std::string fillable_instance_principal = "instance||formFillDemo";
    const auto fdf_template_path = templates_root / "report.fdf";
    const auto pdf_template_path = templates_root / "report.pdf";
    Assert(std::filesystem::exists(fdf_template_path), "Fillable FDF template should exist");
    Assert(std::filesystem::exists(pdf_template_path), "Fillable PDF template should exist");

    const auto [fillable_addr, fillable_instance_id] =
        create_slug(fillable_checklist, fillable_instance_principal, "Alpha", "Fillable Row",
                    "Spec F", "Ready", "Pass", "");
    Assert(!fillable_instance_id.empty(), "fillable instance_id must be returned");
    (void)fillable_addr;

    nlohmann::json fillable_payload{{"checklist", fillable_checklist},
                                    {"instance_id", fillable_instance_id}};
    const auto fillable_resp = client.Post("/api/v1/export/report", fillable_payload.dump(), {},
                                           "application/json", auth_headers);
    Assert(fillable_resp.status == 201, "Fillable report export should return 201");
    const auto fillable_json =
        nlohmann::json::parse(fillable_resp.body, nullptr, /*allow_exceptions=*/false);
    Assert(fillable_json.value("ok", false), "Fillable report export should set ok=true");
    Assert(fillable_json["data"].contains("fillable"),
           "Fillable export should include fillable metadata");
    const auto fillable_meta = fillable_json["data"]["fillable"];
    const auto fillable_dir = fillable_meta.value("directory", "");
    const auto fdf_path = fillable_meta.value("fdf_path", "");
    const auto jsonl_path_fillable = fillable_meta.value("jsonl_path", "");
    const auto pdf_copy_path = fillable_meta.value("pdf_copy_path", "");
    Assert(!fillable_dir.empty(), "Fillable output directory should be returned");
    Assert(fillable_dir.find(fillable_instance_id) != std::string::npos,
           "Fillable output directory should include instance_id");
    Assert(fdf_path.find(fillable_checklist + ".fdf") != std::string::npos,
           "Fillable FDF path should be reported");
    Assert(std::filesystem::exists(fdf_path), "Fillable FDF output should exist");
    Assert(std::filesystem::exists(jsonl_path_fillable), "Fillable JSONL output should exist");
    Assert(std::filesystem::exists(pdf_copy_path), "Fillable PDF copy should exist");

    const std::string fillable_fdf_body = ReadFile(fdf_path);
    Assert(fillable_fdf_body.find(fillable_checklist) != std::string::npos,
           "Fillable FDF should include checklist value");
    Assert(fillable_fdf_body.find(fillable_instance_id) != std::string::npos,
           "Fillable FDF should include instance_id value");
    RecordStep(current_step, true, "fillable report ok");

    if (KeepArtifacts()) {
      std::cout << "Artifacts preserved at: " << artifacts_root
                << "\nReport dir: " << report_dir
                << "\nDB: " << db_path << "\nChecklist: " << checklist
                << "\nInstance ID: " << instance_id << std::endl;
    }

    if (CleanArtifactsOnExit()) {
      std::error_code ec;
      std::filesystem::remove_all(artifacts_root, ec);
    }

    server.Stop();
    return 0;
  } catch (const std::exception& ex) {
    if (!current_step.empty()) {
      RecordStep(current_step, false, ex.what());
    }
    std::cerr << "report_api_flow_test failure: " << ex.what() << std::endl;
    return 1;
  }
}
