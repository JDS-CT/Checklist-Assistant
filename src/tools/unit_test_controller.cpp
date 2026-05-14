#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "nlohmann/json.hpp"
#include "platform/http_client.hpp"

namespace {

struct Options {
  std::string base_url;
  std::string host;
  int port = 8080;
  std::string token;
  std::string admin_user = "admin";
  std::string admin_password;
  std::string client_id = "chax-ui-client";
  std::string client_secret = "chax-ui-secret";
  std::string scope = "checklist:read checklist:write";
  std::string checklist = "Unit Tests";
  std::string section = "markdown_compat_tests";
  std::string procedure = "run markdown-compat test";
  std::string instance_principal = "Reference Implementation Self Tests";
  std::string template_file = "Unit Tests";
  std::string pack = "unit-tests";
  std::string build_dir = "build";
  std::string test_regex = "markdown-compat";
  std::string export_filename = "Unit Tests.export.md";
  bool export_report = false;
  bool export_markdown = false;
  bool export_include_data = false;
  bool replace_checklist = false;
  bool refresh_template = false;
  bool run_all = false;
  bool run_tests = true;
};

struct TestResult {
  int exit_code = 0;
  std::string output;
  double seconds = 0.0;
};

std::string GetEnv(const char* key, const std::string& fallback) {
  if (const char* value = std::getenv(key)) {
    return value;
  }
  return fallback;
}

std::string Trim(const std::string& value) {
  std::size_t first = 0;
  std::size_t last = value.size();
  while (first < value.size() && std::isspace(static_cast<unsigned char>(value[first])) != 0) {
    ++first;
  }
  while (last > first && std::isspace(static_cast<unsigned char>(value[last - 1])) != 0) {
    --last;
  }
  return value.substr(first, last - first);
}

std::string ToLower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  return value;
}

std::string NormalizeChecklistFolder(std::string value) {
  const std::string lowered = ToLower(value);
  if (lowered.size() > 3 && lowered.substr(lowered.size() - 3) == ".md") {
    value.resize(value.size() - 3);
  }
  return Trim(value);
}

int FromHexDigit(char ch) {
  if (ch >= '0' && ch <= '9') return ch - '0';
  if (ch >= 'A' && ch <= 'F') return 10 + (ch - 'A');
  if (ch >= 'a' && ch <= 'f') return 10 + (ch - 'a');
  return -1;
}

std::string UrlDecode(const std::string& value) {
  std::string decoded;
  decoded.reserve(value.size());
  for (std::size_t i = 0; i < value.size(); ++i) {
    const char ch = value[i];
    if (ch == '%' && i + 2 < value.size()) {
      const int hi = FromHexDigit(value[i + 1]);
      const int lo = FromHexDigit(value[i + 2]);
      if (hi >= 0 && lo >= 0) {
        decoded.push_back(static_cast<char>((hi << 4) | lo));
        i += 2;
        continue;
      }
    }
    if (ch == '+') {
      decoded.push_back(' ');
    } else {
      decoded.push_back(ch);
    }
  }
  return decoded;
}

std::optional<std::string> FindHeaderValue(const std::map<std::string, std::string>& headers,
                                           const std::string& key) {
  const std::string target = ToLower(key);
  for (const auto& entry : headers) {
    if (ToLower(entry.first) == target) {
      return entry.second;
    }
  }
  return std::nullopt;
}

std::optional<std::string> ExtractCookie(const std::string& set_cookie,
                                         const std::string& name) {
  const std::string prefix = name + "=";
  const auto start = set_cookie.find(prefix);
  if (start == std::string::npos) {
    return std::nullopt;
  }
  const auto value_start = start + prefix.size();
  const auto end = set_cookie.find(';', value_start);
  return set_cookie.substr(value_start, end == std::string::npos ? std::string::npos : end - value_start);
}

std::optional<std::string> ParseQueryParam(const std::string& url, const std::string& key) {
  const auto query_pos = url.find('?');
  if (query_pos == std::string::npos) {
    return std::nullopt;
  }
  const auto fragment_pos = url.find('#', query_pos);
  const std::string query =
      url.substr(query_pos + 1, fragment_pos == std::string::npos ? std::string::npos
                                                                  : fragment_pos - query_pos - 1);
  std::size_t start = 0;
  while (start <= query.size()) {
    const auto amp = query.find('&', start);
    const std::size_t end = (amp == std::string::npos) ? query.size() : amp;
    const auto pair = query.substr(start, end - start);
    const auto eq = pair.find('=');
    std::string k = (eq == std::string::npos) ? pair : pair.substr(0, eq);
    std::string v = (eq == std::string::npos) ? "" : pair.substr(eq + 1);
    if (UrlDecode(k) == key) {
      return UrlDecode(v);
    }
    if (amp == std::string::npos) break;
    start = amp + 1;
  }
  return std::nullopt;
}

std::string QuoteArg(const std::string& value) {
  std::string escaped;
  escaped.reserve(value.size());
  for (const char ch : value) {
    if (ch == '"') {
      escaped.push_back('\\');
    }
    escaped.push_back(ch);
  }
  return "\"" + escaped + "\"";
}

std::string NowUtc() {
  const auto now = std::chrono::system_clock::now();
  const std::time_t time_val = std::chrono::system_clock::to_time_t(now);
  std::tm utc_tm{};
#if defined(_WIN32)
  gmtime_s(&utc_tm, &time_val);
#else
  gmtime_r(&time_val, &utc_tm);
#endif
  std::ostringstream oss;
  oss << std::put_time(&utc_tm, "%Y-%m-%dT%H:%M:%SZ");
  return oss.str();
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

std::filesystem::path StatsPath() {
  auto root = ResolveRepoRoot();
  const auto chax_dir = root / ".chax";
  std::error_code ec;
  std::filesystem::create_directories(chax_dir, ec);
  return chax_dir / "unit-test-controller.json";
}

struct StatsEntry {
  double seconds = 0.0;
  std::string last_run;
};

struct StatsFile {
  std::map<std::string, StatsEntry> tests;
  StatsEntry all;
};

StatsFile LoadStats(const std::filesystem::path& path) {
  StatsFile stats;
  if (!std::filesystem::exists(path)) {
    return stats;
  }
  std::ifstream input(path);
  nlohmann::json parsed = nlohmann::json::parse(input, nullptr, false);
  if (!parsed.is_object()) {
    return stats;
  }
  if (parsed.contains("tests") && parsed["tests"].is_object()) {
    for (auto it = parsed["tests"].begin(); it != parsed["tests"].end(); ++it) {
      StatsEntry entry;
      entry.seconds = it->value("seconds", 0.0);
      entry.last_run = it->value("last_run", "");
      stats.tests[it.key()] = entry;
    }
  }
  if (parsed.contains("all") && parsed["all"].is_object()) {
    stats.all.seconds = parsed["all"].value("seconds", 0.0);
    stats.all.last_run = parsed["all"].value("last_run", "");
  }
  return stats;
}

void SaveStats(const std::filesystem::path& path, const StatsFile& stats) {
  nlohmann::json output;
  for (const auto& entry : stats.tests) {
    output["tests"][entry.first] = {
        {"seconds", entry.second.seconds},
        {"last_run", entry.second.last_run},
    };
  }
  output["all"] = {{"seconds", stats.all.seconds}, {"last_run", stats.all.last_run}};
  std::ofstream out(path);
  out << output.dump(2);
}

void PrintUsage() {
  std::cout
      << "unit-test-controller [options]\n\n"
      << "Options:\n"
      << "  --base-url <url>              (default: CHAX_BASE_URL)\n"
      << "  --host <host>                 (default: CHAX_HOST or 127.0.0.1)\n"
      << "  --port <port>                 (default: CHAX_PORT or 8080)\n"
      << "  --token <token>               (default: CHAX_TOKEN)\n"
      << "  --admin-user <name>           (default: CHAX_ADMIN_USER or admin)\n"
      << "  --admin-password <pw>         (default: CHAX_ADMIN_PASSWORD)\n"
      << "  --client-id <id>              (default: CHAX_CLIENT_ID or chax-ui-client)\n"
      << "  --client-secret <secret>      (default: CHAX_CLIENT_SECRET or chax-ui-secret)\n"
      << "  --checklist <name>            (default: Unit Tests)\n"
      << "  --section <name>              (default: markdown_compat_tests)\n"
      << "  --procedure <name>            (default: run markdown-compat test)\n"
      << "  --instance-principal <name>   (default: Reference Implementation Self Tests)\n"
      << "  --pack <name>                 (default: unit-tests)\n"
      << "  --template-file <file>        (default: Unit Tests)\n"
      << "  --build-dir <dir>             (default: build)\n"
      << "  --test-regex <regex>          (default: markdown-compat)\n"
      << "  --all                         Run all wired tests in sequence\n"
      << "  --export-report               Export report after updates\n"
      << "  --export-markdown             Export checklist markdown after updates\n"
      << "  --export-include-data         Include result/status/comment in markdown export\n"
      << "  --export-filename <file>      (deprecated; export always writes checklist.md)\n"
      << "  --replace-checklist           Delete checklist before import (template refresh)\n"
      << "  --refresh-template            Import template to add any missing rows\n"
      << "  --skip-tests                  Skip running ctest and PATCH updates\n"
      << "  --help                        Show this help\n";
}

std::optional<int> ParseInt(const std::string& value) {
  try {
    std::size_t idx = 0;
    const int parsed = std::stoi(value, &idx);
    if (idx != value.size()) {
      return std::nullopt;
    }
    return parsed;
  } catch (...) {
    return std::nullopt;
  }
}

Options ParseArgs(int argc, char** argv) {
  Options opts;
  opts.base_url = GetEnv("CHAX_BASE_URL", "");
  opts.host = GetEnv("CHAX_HOST", "127.0.0.1");
  const auto port_env = GetEnv("CHAX_PORT", "");
  if (!port_env.empty()) {
    if (const auto parsed = ParseInt(port_env)) {
      opts.port = *parsed;
    }
  }
  opts.token = GetEnv("CHAX_TOKEN", "");
  opts.admin_user = GetEnv("CHAX_ADMIN_USER", opts.admin_user);
  opts.admin_password = GetEnv("CHAX_ADMIN_PASSWORD", "");
  opts.pack = GetEnv("CHAX_DEFAULT_PACK", opts.pack);
  if (opts.admin_password.empty()) {
    opts.admin_password = GetEnv("ADMIN_PASSWORD", "");
  }
  if (opts.admin_user == "admin") {
    const auto fallback_user = GetEnv("ADMIN_USER", "");
    if (!fallback_user.empty()) {
      opts.admin_user = fallback_user;
    }
  }
  opts.client_id = GetEnv("CHAX_CLIENT_ID", opts.client_id);
  opts.client_secret = GetEnv("CHAX_CLIENT_SECRET", opts.client_secret);

  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "--help" || arg == "-h") {
      PrintUsage();
      std::exit(0);
    }

    std::string key = arg;
    std::string value;
    const auto eq = arg.find('=');
    if (eq != std::string::npos) {
      key = arg.substr(0, eq);
      value = arg.substr(eq + 1);
    }

    auto require_value = [&](const char* name) -> std::string {
      if (!value.empty()) {
        return value;
      }
      if (i + 1 >= argc) {
        throw std::runtime_error(std::string("Missing value for ") + name);
      }
      return argv[++i];
    };

    if (key == "--host") {
      opts.host = require_value("--host");
    } else if (key == "--base-url") {
      opts.base_url = require_value("--base-url");
    } else if (key == "--port") {
      const auto parsed = ParseInt(require_value("--port"));
      if (!parsed || *parsed <= 0) {
        throw std::runtime_error("Invalid --port value.");
      }
      opts.port = *parsed;
    } else if (key == "--token") {
      opts.token = require_value("--token");
    } else if (key == "--admin-user") {
      opts.admin_user = require_value("--admin-user");
    } else if (key == "--admin-password") {
      opts.admin_password = require_value("--admin-password");
    } else if (key == "--client-id") {
      opts.client_id = require_value("--client-id");
    } else if (key == "--client-secret") {
      opts.client_secret = require_value("--client-secret");
    } else if (key == "--checklist") {
      opts.checklist = require_value("--checklist");
    } else if (key == "--section") {
      opts.section = require_value("--section");
    } else if (key == "--procedure") {
      opts.procedure = require_value("--procedure");
    } else if (key == "--instance-principal") {
      opts.instance_principal = require_value("--instance-principal");
    } else if (key == "--pack") {
      opts.pack = require_value("--pack");
    } else if (key == "--template-file") {
      opts.template_file = require_value("--template-file");
    } else if (key == "--build-dir") {
      opts.build_dir = require_value("--build-dir");
    } else if (key == "--test-regex") {
      opts.test_regex = require_value("--test-regex");
    } else if (key == "--all") {
      opts.run_all = true;
    } else if (key == "--export-report") {
      opts.export_report = true;
    } else if (key == "--export-markdown") {
      opts.export_markdown = true;
    } else if (key == "--export-include-data") {
      opts.export_include_data = true;
    } else if (key == "--export-filename") {
      opts.export_filename = require_value("--export-filename");
    } else if (key == "--replace-checklist") {
      opts.replace_checklist = true;
    } else if (key == "--refresh-template") {
      opts.refresh_template = true;
    } else if (key == "--skip-tests") {
      opts.run_tests = false;
    } else {
      throw std::runtime_error("Unknown argument: " + arg);
    }
  }

  return opts;
}

std::string SummarizeOutput(const std::string& output) {
  std::istringstream stream(output);
  std::string line;
  std::vector<std::string> lines;
  while (std::getline(stream, line)) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    const std::string trimmed = Trim(line);
    if (trimmed.empty()) {
      continue;
    }
    lines.push_back(trimmed);
    if (lines.size() == 3) {
      break;
    }
  }
  if (lines.empty()) {
    return "ctest produced no output";
  }
  std::ostringstream summary;
  for (std::size_t i = 0; i < lines.size(); ++i) {
    if (i != 0) {
      summary << " | ";
    }
    summary << lines[i];
  }
  return summary.str();
}

TestResult RunCTest(const Options& opts) {
  const auto start = std::chrono::steady_clock::now();
  const auto now = std::chrono::system_clock::now().time_since_epoch().count();
  const auto output_path =
      std::filesystem::temp_directory_path() / ("chax_unit_test_" + std::to_string(now) + ".log");
  std::filesystem::path build_dir = opts.build_dir;
  if (!std::filesystem::exists(build_dir) && opts.build_dir == "build" &&
      std::filesystem::exists("CMakeCache.txt")) {
    build_dir = ".";
  }
  std::string command = "ctest --test-dir " + QuoteArg(build_dir.string()) + " -R " +
                        QuoteArg(opts.test_regex) + " -V > " + QuoteArg(output_path.string()) +
                        " 2>&1";
  const int raw_code = std::system(command.c_str());
  std::ifstream in(output_path);
  std::ostringstream buffer;
  buffer << in.rdbuf();
  std::error_code ec;
  std::filesystem::remove(output_path, ec);
  TestResult result;
  result.exit_code = raw_code;
  result.output = buffer.str();
  result.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
  return result;
}

std::string AcquireToken(const Options& opts, const std::string& base_url) {
  if (opts.admin_password.empty()) {
    throw std::runtime_error("CHAX_TOKEN or CHAX_ADMIN_PASSWORD is required for API writes.");
  }

  platform::HttpClient client(base_url);
  const auto now = std::chrono::system_clock::now().time_since_epoch().count();
  const std::string state = "unit-test-" + std::to_string(now);
  std::string redirect = base_url;
  if (!redirect.empty() && redirect.back() == '/') {
    redirect.pop_back();
  }
  redirect += "/oauth/callback";

  nlohmann::json login_payload{{"response_type", "code"},
                               {"client_id", opts.client_id},
                               {"redirect_uri", redirect},
                               {"scope", opts.scope},
                               {"state", state},
                               {"username", opts.admin_user},
                               {"password", opts.admin_password},
                               {"action", "login"}};
  const auto login_resp =
      client.Post("/oauth/authorize", login_payload.dump(), {}, "application/json", {});
  if (login_resp.status != 200) {
    throw std::runtime_error("OAuth login failed (status " + std::to_string(login_resp.status) +
                             ").");
  }
  const auto cookie_header = FindHeaderValue(login_resp.headers, "Set-Cookie");
  if (!cookie_header) {
    const std::string body_trimmed = Trim(login_resp.body);
    if (body_trimmed == "ok") {
      throw std::runtime_error(
          "OAuth login missing Set-Cookie header (OAUTH_AUTHORIZE_SIMPLE is enabled). "
          "Provide CHAX_TOKEN or disable OAUTH_AUTHORIZE_SIMPLE.");
    }
    if (login_resp.body.find("Invalid credentials") != std::string::npos) {
      throw std::runtime_error(
          "OAuth login failed (invalid credentials). Ensure CHAX_ADMIN_PASSWORD matches the "
          "server's ADMIN_PASSWORD and restart the server after changing it.");
    }
    throw std::runtime_error(
        "OAuth login missing Set-Cookie header. Ensure CHAX_ADMIN_PASSWORD matches the "
        "server's ADMIN_PASSWORD and restart the server after changing it.");
  }
  const auto session = ExtractCookie(*cookie_header, "chax_session");
  if (!session || session->empty()) {
    throw std::runtime_error("OAuth login missing session cookie.");
  }

  nlohmann::json approve_payload{{"response_type", "code"},
                                 {"client_id", opts.client_id},
                                 {"redirect_uri", redirect},
                                 {"scope", opts.scope},
                                 {"state", state},
                                 {"action", "approve"}};
  std::map<std::string, std::string> approve_headers{{"Cookie", "chax_session=" + *session}};
  const auto approve_resp = client.Post("/oauth/authorize", approve_payload.dump(), {},
                                        "application/json", approve_headers);
  if (approve_resp.status != 302) {
    throw std::runtime_error("OAuth approve failed (status " + std::to_string(approve_resp.status) +
                             ").");
  }
  const auto location = FindHeaderValue(approve_resp.headers, "Location");
  if (!location) {
    throw std::runtime_error("OAuth approve missing Location header.");
  }
  const auto code = ParseQueryParam(*location, "code");
  if (!code || code->empty()) {
    throw std::runtime_error("OAuth approve missing authorization code.");
  }

  nlohmann::json token_payload{{"grant_type", "authorization_code"},
                               {"client_id", opts.client_id},
                               {"client_secret", opts.client_secret},
                               {"code", *code},
                               {"redirect_uri", redirect}};
  const auto token_resp =
      client.Post("/oauth/token", token_payload.dump(), {}, "application/json", {});
  if (token_resp.status != 200) {
    throw std::runtime_error("OAuth token exchange failed (status " +
                             std::to_string(token_resp.status) + ").");
  }
  auto parsed = nlohmann::json::parse(token_resp.body, nullptr, false);
  if (parsed.is_discarded()) {
    throw std::runtime_error("OAuth token response was not valid JSON.");
  }
  const std::string access_token = parsed.value("access_token", "");
  if (access_token.empty()) {
    throw std::runtime_error("OAuth token response missing access_token.");
  }
  return access_token;
}

struct StepUpdate {
  std::string section;
  std::string procedure;
  std::string status;
  std::string message;
};

std::vector<StepUpdate> ParseStepUpdates(const std::string& output) {
  std::vector<StepUpdate> updates;
  std::istringstream stream(output);
  std::string line;
  while (std::getline(stream, line)) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    const auto marker_pos = line.find("CHAX_STEP|");
    if (marker_pos == std::string::npos) {
      continue;
    }
    std::size_t start = marker_pos + std::string("CHAX_STEP|").size();
    std::array<std::string, 4> parts;
    for (std::size_t idx = 0; idx < parts.size(); ++idx) {
      const auto sep = line.find('|', start);
      if (sep == std::string::npos) {
        if (idx < 3) {
          parts[idx] = line.substr(start);
          start = line.size();
        }
        break;
      }
      parts[idx] = line.substr(start, sep - start);
      start = sep + 1;
    }
    const std::string message = (start < line.size()) ? line.substr(start) : std::string{};
    StepUpdate update;
    update.section = parts[0];
    update.procedure = parts[1];
    update.status = parts[2];
    update.message = message;
    if (!update.section.empty() && !update.procedure.empty() && !update.status.empty()) {
      updates.push_back(std::move(update));
    }
  }
  return updates;
}

struct TestPlan {
  std::string name;
  std::string regex;
  std::string section;
  std::string procedure;
};

const std::vector<TestPlan> kPlans = {
    {"markdown-compat", "markdown-compat", "markdown_compat_tests", "run markdown-compat test"},
    {"markdown-relationships", "markdown-relationships", "markdown_relationships_test",
     "run markdown-relationships test"},
    {"report-generation", "report-generation", "report_generation_test",
     "run report-generation test"},
    {"integration-schema", "integration-schema", "integration_schema_test",
     "run integration-schema test"},
    {"http-api", "http-api", "http_api_test", "run http-api test"},
    {"predicate-daemon-exhaustive", "predicate-daemon-exhaustive",
     "predicate_daemon_exhaustive_test", "run predicate-daemon test"},
    {"report-api-flow", "report-api-flow", "report_api_flow_test", "run report-api-flow test"},
    {"oauth-flow", "oauth-flow", "oauth_flow_test", "run oauth-flow test"},
    {"oauth-authorize-ui", "oauth-authorize-ui", "oauth_authorize_ui_test",
     "run oauth-authorize-ui test"},
    {"oauth-store-repro", "oauth-store-repro", "oauth_store_repro_test",
     "run oauth-store-repro test"},
    {"mcp-bridge", "mcp-bridge", "mcp_bridge_test", "run mcp-bridge test"},
    {"mcp-voice-helper", "mcp-voice-helper", "mcp_voice_helper_test", "run mcp-voice test"},
    {"e2e-smoke", "e2e-smoke", "e2e_smoke_test", "run e2e-smoke test"},
    {"server-start-stop", "server-start-stop", "server_start_stop_test",
     "run server-start-stop test"},
};

class ApiClient {
 public:
  ApiClient(std::string base_url, std::string token) : client_(std::move(base_url)) {
    if (!token.empty()) {
      headers_["Authorization"] = "Bearer " + token;
    }
  }

  nlohmann::json Get(const std::string& path,
                     const std::map<std::string, std::string>& query = {}) const {
    return ParseResponse(client_.Get(path, query, headers_));
  }

  nlohmann::json Post(const std::string& path, const nlohmann::json& payload) const {
    return ParseResponse(client_.Post(path, payload.dump(), {}, "application/json", headers_));
  }

  nlohmann::json Patch(const std::string& path, const nlohmann::json& payload) const {
    return ParseResponse(client_.Patch(path, payload.dump(), {}, "application/json", headers_));
  }

  nlohmann::json Delete(const std::string& path) const {
    return ParseResponse(client_.Delete(path, {}, headers_));
  }

 private:
  static std::string ExtractErrorMessage(const nlohmann::json& body) {
    if (body.is_object()) {
      if (body.contains("error") && body["error"].is_object()) {
        return body["error"].value("message", "Request failed");
      }
      if (body.contains("message") && body["message"].is_string()) {
        return body["message"].get<std::string>();
      }
    }
    return "Request failed";
  }

  nlohmann::json ParseResponse(const platform::HttpClientResponse& response) const {
    const auto parsed = nlohmann::json::parse(response.body, nullptr, false);
    if (parsed.is_discarded()) {
      throw std::runtime_error("Invalid JSON response (status " + std::to_string(response.status) +
                               ").");
    }
    if (response.status < 200 || response.status >= 300) {
      throw std::runtime_error("HTTP " + std::to_string(response.status) + ": " +
                               ExtractErrorMessage(parsed));
    }
    if (parsed.contains("ok") && parsed["ok"].is_boolean() && !parsed["ok"].get<bool>()) {
      throw std::runtime_error("API error: " + ExtractErrorMessage(parsed));
    }
    if (parsed.contains("data")) {
      return parsed["data"];
    }
    return parsed;
  }

  platform::HttpClient client_;
  std::map<std::string, std::string> headers_;
};

void EnsureChecklistInstance(const ApiClient& api, const Options& opts) {
  const auto data = api.Get("/api/v1/slugs",
                            {{"checklist", opts.checklist},
                             {"instance_principal", opts.instance_principal},
                             {"limit", "1"}});
  if (data.contains("items") && data["items"].is_array() && !data["items"].empty()) {
    return;
  }
  const std::string template_checklist = NormalizeChecklistFolder(opts.template_file);
  nlohmann::json payload{{"pack", opts.pack},
                         {"checklist", template_checklist},
                         {"instance_principal", opts.instance_principal},
                         {"apply_data", false},
                         {"replace_instance", false}};
  api.Post("/api/v1/workspace/markdown/import", payload);
}

void ImportChecklistTemplate(const ApiClient& api, const Options& opts) {
  const std::string template_checklist = NormalizeChecklistFolder(opts.template_file);
  nlohmann::json payload{{"pack", opts.pack},
                         {"checklist", template_checklist},
                         {"instance_principal", opts.instance_principal},
                         {"apply_data", false},
                         {"replace_instance", false}};
  api.Post("/api/v1/workspace/markdown/import", payload);
}

void ReplaceChecklist(const ApiClient& api, const Options& opts) {
  api.Delete("/api/v1/checklists/" + opts.checklist);
}

std::string ResolveProcedureAddressId(const ApiClient& api, const Options& opts) {
  const auto data = api.Get("/api/v1/slugs",
                            {{"checklist", opts.checklist},
                             {"section", opts.section},
                             {"instance_principal", opts.instance_principal}});
  if (!data.contains("items") || !data["items"].is_array()) {
    throw std::runtime_error("Slug listing did not return items.");
  }
  for (const auto& item : data["items"]) {
    if (!item.contains("procedure") || !item.contains("address_id")) {
      continue;
    }
    if (item["procedure"].get<std::string>() == opts.procedure) {
      return item["address_id"].get<std::string>();
    }
  }
  throw std::runtime_error("Procedure not found: " + opts.procedure + " (section " + opts.section +
                           "). If the template changed, rerun with --replace-checklist.");
}

std::string EnsureInstanceId(const ApiClient& api, const Options& opts) {
  nlohmann::json payload{{"principal", opts.instance_principal}, {"label", ""}, {"meta", ""}};
  const auto data = api.Post("/api/v1/instances", payload);
  if (!data.contains("instance_id") || !data["instance_id"].is_string()) {
    throw std::runtime_error("Instance API did not return instance_id.");
  }
  return data["instance_id"].get<std::string>();
}

std::map<std::string, std::string> ResolveProcedureAddressMap(const ApiClient& api,
                                                              const Options& opts,
                                                              const std::string& section) {
  std::map<std::string, std::string> mapping;
  const auto data =
      api.Get("/api/v1/slugs",
              {{"checklist", opts.checklist},
               {"section", section},
               {"instance_principal", opts.instance_principal}});
  if (!data.contains("items") || !data["items"].is_array()) {
    return mapping;
  }
  for (const auto& item : data["items"]) {
    if (!item.contains("procedure") || !item.contains("address_id")) {
      continue;
    }
    mapping.emplace(item["procedure"].get<std::string>(), item["address_id"].get<std::string>());
  }
  return mapping;
}

void UpdateSlug(const ApiClient& api, const std::string& address_id, const TestResult& result,
                const Options& opts) {
  const std::string status = (result.exit_code == 0) ? "Pass" : "Fail";
  const std::string summary = SummarizeOutput(result.output);
  nlohmann::json payload{{"status", status},
                         {"result", "ctest " + opts.test_regex + " exit=" +
                                        std::to_string(result.exit_code)},
                         {"comment", summary}};
  api.Patch("/api/v1/slugs/" + address_id, payload);
}

void UpdateSlugFromStep(const ApiClient& api, const std::string& address_id,
                        const StepUpdate& step) {
  const std::string status = (step.status == "Pass") ? "Pass" : "Fail";
  const std::string result = (status == "Pass") ? "ok" : "fail";
  nlohmann::json payload{{"status", status}, {"result", result}, {"comment", step.message}};
  api.Patch("/api/v1/slugs/" + address_id, payload);
}

TestResult RunSingleTest(const ApiClient& api, const Options& opts, const TestPlan& plan,
                         StatsFile* stats) {
  Options local = opts;
  local.test_regex = plan.regex;
  local.section = plan.section;
  local.procedure = plan.procedure;

  std::string address_id;
  try {
    address_id = ResolveProcedureAddressId(api, local);
  } catch (const std::exception&) {
    ImportChecklistTemplate(api, local);
    address_id = ResolveProcedureAddressId(api, local);
  }
  const TestResult result = RunCTest(local);
  UpdateSlug(api, address_id, result, local);
  std::cout << "Updated slug " << address_id << " status="
            << (result.exit_code == 0 ? "Pass" : "Fail") << "\n";

  const auto steps = ParseStepUpdates(result.output);
  std::map<std::string, std::map<std::string, std::string>> cache;
  for (const auto& step : steps) {
    auto& mapping = cache[step.section];
    if (mapping.empty()) {
      mapping = ResolveProcedureAddressMap(api, local, step.section);
    }
    const auto it = mapping.find(step.procedure);
    if (it == mapping.end()) {
      std::cerr << "Step update skipped (missing procedure): " << step.section << " / "
                << step.procedure << "\n";
      continue;
    }
    UpdateSlugFromStep(api, it->second, step);
  }

  if (stats) {
    (*stats).tests[plan.name] = StatsEntry{result.seconds, NowUtc()};
  }
  return result;
}

void ExportReport(const ApiClient& api, const Options& opts, const std::string& instance_id) {
  nlohmann::json payload{
      {"checklist", opts.checklist}, {"instance_id", instance_id}, {"pack", opts.pack}};
  const auto data = api.Post("/api/v1/export/report", payload);
  if (data.contains("path")) {
    std::cout << "Report written: " << data["path"].get<std::string>() << "\n";
  }
}

void ExportMarkdown(const ApiClient& api, const Options& opts, const std::string& instance_id) {
  nlohmann::json payload{{"checklist", opts.checklist},
                         {"pack", opts.pack},
                         {"include_data", opts.export_include_data},
                         {"instance_id", instance_id}};
  const auto data = api.Post("/api/v1/workspace/markdown/export", payload);
  if (data.contains("path")) {
    std::cout << "Markdown exported: " << data["path"].get<std::string>() << "\n";
  }
}

}  // namespace

int main(int argc, char** argv) {
  try {
    Options opts = ParseArgs(argc, argv);
    if (argc == 1) {
      const auto stats_path = StatsPath();
      const auto stats = LoadStats(stats_path);
      std::cout << "unit-test-controller: no arguments provided.\n";
      if (stats.all.seconds > 0.0) {
        std::cout << "Last full run: " << stats.all.seconds << "s at " << stats.all.last_run
                  << "\n";
      } else if (!stats.tests.empty()) {
        double sum = 0.0;
        for (const auto& entry : stats.tests) {
          sum += entry.second.seconds;
        }
        if (sum > 0.0) {
          std::cout << "Estimated full run (sum of last tests): " << sum << "s\n";
        }
      } else {
        std::cout << "No prior run stats found.\n";
      }
      std::cout << "Run all wired tests with:\n"
                << "  unit-test-controller.exe --all --export-report --export-markdown --refresh-template\n";
      PrintUsage();
      return 0;
    }

    std::string base_url = opts.base_url;
    if (base_url.empty()) {
      base_url = "http://" + opts.host + ":" + std::to_string(opts.port);
    } else if (base_url.find("://") == std::string::npos) {
      base_url = "http://" + base_url;
    }

    if (opts.token.empty()) {
      opts.token = AcquireToken(opts, base_url);
      std::cout << "Obtained access token via OAuth.\n";
    }

    ApiClient api(base_url, opts.token);

    if (opts.replace_checklist) {
      ReplaceChecklist(api, opts);
    }
    if (opts.refresh_template) {
      ImportChecklistTemplate(api, opts);
    } else {
      EnsureChecklistInstance(api, opts);
    }

    StatsFile stats = LoadStats(StatsPath());
    bool ran_any = false;
    double total_seconds = 0.0;

    if (opts.run_tests) {
      if (opts.run_all) {
        for (const auto& plan : kPlans) {
          const TestResult result = RunSingleTest(api, opts, plan, &stats);
          total_seconds += result.seconds;
          ran_any = true;
        }
        if (ran_any) {
          stats.all.seconds = total_seconds;
          stats.all.last_run = NowUtc();
        }
      } else {
        const TestPlan single{opts.test_regex, opts.test_regex, opts.section, opts.procedure};
        RunSingleTest(api, opts, single, &stats);
        ran_any = true;
      }
    }

    if (opts.export_report || opts.export_markdown) {
      const std::string instance_id = EnsureInstanceId(api, opts);
      if (opts.export_report) {
        ExportReport(api, opts, instance_id);
      }
      if (opts.export_markdown) {
        ExportMarkdown(api, opts, instance_id);
      }
    }

    if (ran_any) {
      SaveStats(StatsPath(), stats);
    }
  } catch (const std::exception& ex) {
    std::cerr << "unit-test-controller error: " << ex.what() << "\n";
    return 1;
  }

  return 0;
}
