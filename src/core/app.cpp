#include "core/app.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "core/asset_pack.hpp"
#include "core/checklist_markdown.hpp"
#include "core/checklist_store.hpp"
#include "core/graph_projection.hpp"
#include "core/logging.hpp"
#include "core/oauth.hpp"
#include "core/report_generator.hpp"
#include "nlohmann/json.hpp"
#include "platform/http_server.hpp"
#include "platform/system.hpp"

namespace core {
namespace {

using core::logging::LogInfo;
using core::logging::LogDebug;
using core::logging::LogWarn;
using core::logging::LogError;
using core::logging::IsDebugEnabled;
using nlohmann::json;

struct DemoCommand {
  std::string_view method;
  std::string_view path;
  std::string_view description;
};

const std::vector<DemoCommand> kCommandCatalog = {
    {"GET", "/api/v1/commands", "List every API command exposed by the server."},
    {"GET", "/api/v1/health", "Report server readiness, uptime, and version."},
    {"GET", "/api/v1/hello", "Send a greeting back. Optional query parameter 'name'."},
    {"POST", "/api/v1/echo", "Echo the provided payload for integration smoke tests."},
    {"GET", "/api/v1/checklists", "List available checklists in the runtime store."},
    {"DELETE", "/api/v1/checklists/<checklist>", "Delete a checklist, its instances, and history."},
    {"DELETE", "/api/v1/checklists/<checklist>/instances/<instance_id>",
     "Delete a single checklist instance (rows + history)."},
    {"POST", "/api/v1/slugs", "Create or upsert a slug via the full creation contract."},
    {"GET", "/api/v1/slugs/<address_id>", "Return a single checklist slug by Address ID."},
    {"GET", "/api/v1/slugs", "Return slugs with optional filters (e.g., checklist)."},
    {"GET", "/api/v1/relationships/address/<address_id>",
     "Return incoming and outgoing relationships for a slug by Address ID."},
    {"PATCH", "/api/v1/slugs/<address_id>", "Apply a minimal state update to a single slug."},
    {"POST", "/api/v1/slugs/bulk-update", "Apply minimal state updates to multiple slugs in one call."},
    {"GET", "/api/v1/history/<address_id>", "Return history snapshots for a slug address."},
    {"GET", "/api/v1/export/json", "Export all slugs as a JSON array."},
    {"GET", "/api/v1/export/jsonl", "Export all slugs as JSON Lines."},
    {"GET", "/api/v1/visualizations/graph",
     "Return a read-only graph projection for one checklist instance."},
    {"GET", "/api/v1/visualizations/workbench",
     "Return the relationship workbench projection for one checklist instance and asset package."},
    {"POST", "/api/v1/import/jsonl",
     "Import JSON Lines into a checklist instance (update existing rows, optionally add missing)."},
    {"GET", "/api/v1/export/markdown/<checklist>", "Export a checklist as canonical Markdown for authors."},
    {"POST", "/api/v1/export/report",
     "Generate a LaTeX or HTML report for a checklist instance; LaTeX exports may also emit fillable FDF output."},
    {"POST", "/api/v1/import/markdown?checklist=<name>",
     "Import Markdown for a checklist and replace its runtime state."},
    {"GET", "/api/v1/workspace/markdown/templates",
     "List Markdown checklist templates under the checklists asset packs with parse/validation status."},
    {"POST", "/api/v1/workspace/markdown/import",
     "Import a Markdown checklist template from the checklists asset packs."},
    {"POST", "/api/v1/workspace/markdown/export",
     "Export a checklist (template or instance data) into checklists/<pack>/<checklist>/checklist.md."},
    {"POST", "/api/v1/workspace/visualizations/export",
     "Export deterministic graph and relationship-workbench views under a checklist asset pack's visualizations folder."},
    {"POST", "/api/v1/workspace/asset-pack/export",
     "Archive checklists/<pack>/<checklist>/ as a transportable .chk/.7z/.zip asset pack."},
    {"POST", "/api/v1/workspace/asset-pack/import",
     "Restore a .chk/.7z/.zip asset pack and import its checklist.md rows into the runtime store."},
    {"GET", "/api/v1/local/settings", "Read local-only portal settings, including extra checklist asset roots."},
    {"POST", "/api/v1/local/settings", "Update local-only portal settings after loopback/origin/path validation."},
    {"GET", "/api/v1/workspace/scripts",
     "List runnable checklist-local scripts under checklists/<pack>/<checklist>/scripts."},
    {"POST", "/api/v1/workspace/scripts/run",
     "Launch a checklist-local script from checklists/<pack>/<checklist>/scripts."},
    {"POST", "/api/v1/relationships/template", "Create a template-level relationship triple (slug_id to slug_id)."},
    {"GET", "/api/v1/relationships/template", "List template-level relationships with optional filters and cursor."},
    {"POST", "/api/v1/relationships/address",
     "Create an address-level relationship triple (address_id to address_id)."},
    {"GET", "/api/v1/relationships/address", "List address-level relationships with optional filters and cursor."},
    {"POST", "/api/v1/entities", "Create or upsert an entity in the catalog."},
    {"GET", "/api/v1/entities", "List entities from the catalog."},
    {"POST", "/api/v1/instances", "Create or upsert an instance principal in the catalog."},
    {"GET", "/api/v1/instances", "List instances from the catalog."},
    {"POST", "/api/v1/predicates", "Create or upsert a predicate governance record."},
    {"GET", "/api/v1/predicates", "List registered relationship predicates."},
    {"GET", "/api/v1/evaluate/slug/<address_id>", "Read-only evaluation for a single slug (effective_status + flags)."},
    {"POST", "/api/v1/evaluate/graph", "Read-only evaluation for a set of address_ids (graph view)."},
};

const auto kServerStart = std::chrono::steady_clock::now();

void ApplyCors(platform::HttpResponse& response) {
  response.headers["Access-Control-Allow-Origin"] = "*";
  response.headers["Access-Control-Allow-Methods"] = "GET,POST,PATCH,DELETE,OPTIONS";
  response.headers["Access-Control-Allow-Headers"] = "Content-Type,Authorization";
}

platform::HttpResponse JsonResponse(const json& body, int status = 200) {
  platform::HttpResponse response;
  response.status = status;
  response.content_type = "application/json";
  response.body = body.dump();
  ApplyCors(response);
  return response;
}

json EnvelopeOk(const json& data, const json& warnings = json::array()) {
  json envelope;
  envelope["ok"] = true;
  envelope["data"] = data;
  if (!warnings.empty()) {
    envelope["warnings"] = warnings;
  }
  return envelope;
}

std::string FormatCodepoint(std::uint32_t codepoint) {
  std::ostringstream out;
  out << "U+" << std::uppercase << std::hex << std::setfill('0');
  if (codepoint <= 0xFFFF) {
    out << std::setw(4) << codepoint;
  } else {
    out << std::setw(6) << codepoint;
  }
  return out.str();
}

std::string EncodeUtf8(std::uint32_t codepoint) {
  std::string out;
  if (codepoint <= 0x7F) {
    out.push_back(static_cast<char>(codepoint));
    return out;
  }
  if (codepoint <= 0x7FF) {
    out.push_back(static_cast<char>(0xC0 | ((codepoint >> 6) & 0x1F)));
    out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    return out;
  }
  if (codepoint <= 0xFFFF) {
    out.push_back(static_cast<char>(0xE0 | ((codepoint >> 12) & 0x0F)));
    out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    return out;
  }
  out.push_back(static_cast<char>(0xF0 | ((codepoint >> 18) & 0x07)));
  out.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F)));
  out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
  out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
  return out;
}

json BuildUnicodeWarnings(const core::markdown::ParsedChecklist& parsed) {
  if (parsed.unicode_warnings.empty()) {
    return json::array();
  }
  json examples = json::array();
  const std::size_t limit = 12;
  std::size_t count = 0;
  for (const auto& entry : parsed.unicode_warnings) {
    if (count >= limit) break;
    examples.push_back({{"codepoint", FormatCodepoint(entry.codepoint)},
                        {"character", EncodeUtf8(entry.codepoint)},
                        {"line", entry.line},
                        {"column", entry.column},
                        {"count", entry.count}});
    ++count;
  }
  json warning;
  warning["code"] = "UNSUPPORTED_UNICODE";
  warning["message"] =
      "Markdown contains Unicode characters that are not supported by the LaTeX report renderer.";
  warning["details"] = {{"unique", parsed.unicode_warnings.size()}, {"examples", examples}};
  return json::array({warning});
}

struct JsonlImportItem {
  std::size_t line = 0;
  ChecklistSlug slug;
  std::string original_slug_id;
  std::string original_address_id;
  std::string mapped_address_id;
  bool has_result = false;
  bool has_status = false;
  bool has_comment = false;
  bool has_timestamp = false;
  bool has_instructions = false;
  bool has_address_order = false;
  bool has_template_fields = false;
  bool slug_id_mismatch = false;
  std::string expected_slug_id;
  bool has_address_id = false;
  bool address_instance_match = false;
  bool lineage_mapped = false;
  bool lineage_used_address = false;
  bool exists = false;
  ChecklistSlug existing;
};

std::optional<std::string> ExtractSlugIdFromAddress(const std::string& address,
                                                    std::string* instance_out) {
  if (core::IsValidBase32Id(address, 32)) {
    if (instance_out) {
      *instance_out = address.substr(16, 16);
    }
    return address.substr(0, 16);
  }
  if (address.size() == 34 && address.find("||") == 16) {
    const std::string slug = address.substr(0, 16);
    const std::string inst = address.substr(18, 16);
    if (core::IsValidBase32Id(slug, 16) && core::IsValidBase32Id(inst, 16)) {
      if (instance_out) {
        *instance_out = inst;
      }
      return slug;
    }
  }
  return std::nullopt;
}

std::optional<std::string> ReadOptionalString(const json& payload, const char* key,
                                              bool* present_out) {
  const auto it = payload.find(key);
  if (it == payload.end()) {
    if (present_out) {
      *present_out = false;
    }
    return std::nullopt;
  }
  if (present_out) {
    *present_out = true;
  }
  if (it->is_null()) {
    return std::string{};
  }
  if (!it->is_string()) {
    throw std::invalid_argument(std::string{"Field '"} + key +
                                "' must be a string or null when provided.");
  }
  return it->get<std::string>();
}

platform::HttpResponse OkResponse(const json& data, int status = 200,
                                  const json& warnings = json::array()) {
  return JsonResponse(EnvelopeOk(data, warnings), status);
}

platform::HttpResponse TextResponse(const std::string& body, const std::string& content_type,
                                    int status = 200) {
  platform::HttpResponse response;
  response.status = status;
  response.content_type = content_type;
  response.body = body;
  ApplyCors(response);
  return response;
}

platform::HttpResponse ErrorResponse(const std::string& code, const std::string& message,
                                     const json& details = json::object(), int status = 400) {
  json envelope;
  envelope["ok"] = false;
  envelope["error"] = {{"code", code}, {"message", message}, {"details", details}};
  return JsonResponse(envelope, status);
}

std::string GetQueryParam(const platform::HttpRequest& request, const std::string& key,
                          const std::string& fallback) {
  if (const auto it = request.query_params.find(key); it != request.query_params.end()) {
    return it->second;
  }
  return fallback;
}

std::string ToLowerCopy(std::string value);
std::string TrimString(const std::string& value);
std::optional<std::string> ReadUtf8FileLimited(const std::filesystem::path& path,
                                               std::size_t max_bytes,
                                               std::string* error_out);

std::optional<int> ParseLimit(const std::string& value) {
  if (value.empty()) return std::nullopt;
  try {
    const int parsed = std::stoi(value);
    if (parsed > 0) {
      return parsed;
    }
  } catch (...) {
  }
  return std::nullopt;
}

std::optional<int> ParseCursor(const std::string& value) {
  if (value.empty()) return std::nullopt;
  try {
    const int parsed = std::stoi(value);
    if (parsed >= 0) {
      return parsed;
    }
  } catch (...) {
  }
  return std::nullopt;
}

bool IsValidPredicateToken(const std::string& predicate) {
  constexpr std::size_t kMaxLen = 128;
  if (predicate.empty() || predicate.size() > kMaxLen) return false;

  const auto is_ascii_alpha = [](unsigned char ch) {
    return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z');
  };
  const auto is_ascii_digit = [](unsigned char ch) { return (ch >= '0' && ch <= '9'); };

  const unsigned char first = static_cast<unsigned char>(predicate.front());
  if (!is_ascii_alpha(first)) return false;

  for (const unsigned char ch : predicate) {
    if (ch >= 0x80) return false;
    if (is_ascii_alpha(ch) || is_ascii_digit(ch) || ch == '_') continue;
    return false;
  }
  return true;
}

std::string CanonicalizePredicate(const std::string& raw) {
  const std::string trimmed = TrimString(raw);
  if (!IsValidPredicateToken(trimmed)) {
    throw std::invalid_argument("Predicate token must match [A-Za-z][A-Za-z0-9_]{0,127} (ASCII, case-sensitive).");
  }
  return trimmed;
}

std::filesystem::path GetLibraryRoot() {
  return core::ResolveLibraryRoot();
}

std::filesystem::path GetLocalSettingsPath() {
  return std::filesystem::path{".chax"} / "local_settings.json";
}

std::string GetDefaultLibraryPack() {
  if (const char* env = std::getenv("CHAX_DEFAULT_PACK")) {
    if (env[0] != '\0') {
      return env;
    }
  }
  return "chax";
}

struct ChecklistWorkspaceRoot {
  std::string source_name;
  std::filesystem::path root;
  bool primary = false;
};

struct ChecklistRootResolution {
  std::string source_name;
  std::filesystem::path library_root;
  std::string pack;
  std::string checklist_dir;
  std::filesystem::path root;
  bool defaulted = false;
};

struct WorkspaceMarkdownImportOptions {
  std::string template_principal = "template||default";
  std::string apply_instance_principal;
  std::string apply_instance_id_payload;
  bool apply_data = false;
  bool replace_instance = true;
};

std::filesystem::path NormalizeChecklistRootCandidate(const std::string& raw_path) {
  std::filesystem::path path = TrimString(raw_path);
  if (path.empty()) {
    return {};
  }
  std::error_code ec;
  if (!path.is_absolute()) {
    path = std::filesystem::current_path(ec) / path;
  }
  if (ec) {
    return {};
  }
  const std::filesystem::path checklists_child = path / "checklists";
  if (std::filesystem::exists(checklists_child, ec) &&
      std::filesystem::is_directory(checklists_child, ec)) {
    path = checklists_child;
  }
  const auto canonical = std::filesystem::weakly_canonical(path, ec);
  if (!ec) {
    path = canonical;
  }
  if (!std::filesystem::exists(path, ec) || !std::filesystem::is_directory(path, ec)) {
    return {};
  }
  return path;
}

std::vector<ChecklistWorkspaceRoot> LoadChecklistWorkspaceRoots() {
  std::vector<ChecklistWorkspaceRoot> roots;
  std::unordered_set<std::string> names;
  std::unordered_set<std::string> paths;

  auto add_root = [&](std::string source_name, const std::filesystem::path& root, bool primary) {
    source_name = TrimString(source_name);
    if (source_name.empty()) {
      source_name = primary ? "public" : "extra";
    }
    if (!core::IsSafePackToken(source_name)) {
      source_name = primary ? "public" : "extra";
    }
    std::string base = source_name;
    int suffix = 2;
    while (names.find(source_name) != names.end()) {
      source_name = base + "_" + std::to_string(suffix++);
    }

    std::error_code ec;
    auto canonical = std::filesystem::weakly_canonical(root, ec);
    if (ec) {
      canonical = std::filesystem::absolute(root, ec);
    }
    if (ec) {
      canonical = root;
    }
    const std::string key = canonical.string();
    if (paths.find(key) != paths.end()) {
      return;
    }
    names.insert(source_name);
    paths.insert(key);
    roots.push_back(ChecklistWorkspaceRoot{source_name, canonical, primary});
  };

  add_root("public", GetLibraryRoot(), true);

  std::string error;
  const auto raw = ReadUtf8FileLimited(GetLocalSettingsPath(), 256 * 1024, &error);
  if (!raw || raw->empty()) {
    return roots;
  }
  const auto settings = json::parse(*raw, nullptr, false);
  if (settings.is_discarded() || !settings.is_object()) {
    LogWarn("Ignoring invalid local settings JSON at " + GetLocalSettingsPath().string());
    return roots;
  }
  const auto it = settings.find("checklist_paths");
  if (it == settings.end() || !it->is_array()) {
    return roots;
  }
  int index = 1;
  for (const auto& entry : *it) {
    std::string name = "extra_" + std::to_string(index++);
    std::string raw_path;
    if (entry.is_string()) {
      raw_path = entry.get<std::string>();
    } else if (entry.is_object()) {
      name = entry.value("name", name);
      raw_path = entry.value("path", "");
    } else {
      continue;
    }
    const auto normalized = NormalizeChecklistRootCandidate(raw_path);
    if (normalized.empty()) {
      LogWarn("Ignoring invalid checklist path from local settings: " + raw_path);
      continue;
    }
    add_root(name, normalized, false);
  }
  return roots;
}

json ChecklistWorkspaceRootsToJson(const std::vector<ChecklistWorkspaceRoot>& roots) {
  json items = json::array();
  for (const auto& root : roots) {
    items.push_back({{"name", root.source_name},
                     {"path", root.root.string()},
                     {"primary", root.primary}});
  }
  return items;
}

std::optional<ChecklistRootResolution> ResolveChecklistRoot(
    const std::filesystem::path& library_root,
    const std::string& checklist,
    const std::string& pack,
    bool allow_create,
    std::string* error_out,
    json* error_details) {
  const std::string checklist_trimmed = TrimString(checklist);
  if (checklist_trimmed.empty()) {
    if (error_out) {
      *error_out = "checklist is required.";
    }
    return std::nullopt;
  }
  const std::string pack_trimmed = TrimString(pack);
  if (!pack_trimmed.empty()) {
    if (!core::IsSafePackToken(pack_trimmed) ||
        !core::IsSafeChecklistToken(checklist_trimmed)) {
      if (error_out) {
        *error_out = "pack and checklist must be plain folder names.";
      }
      return std::nullopt;
    }
    const std::filesystem::path root = library_root / pack_trimmed / checklist_trimmed;
    std::error_code ec;
    if (!allow_create && !std::filesystem::exists(root, ec)) {
      if (error_out) {
        *error_out = "Checklist not found in the requested pack.";
      }
      if (error_details) {
        *error_details = json{{"pack", pack_trimmed}, {"checklist", checklist_trimmed}};
      }
      return std::nullopt;
    }
    return ChecklistRootResolution{"", library_root, pack_trimmed, checklist_trimmed, root, false};
  }

  const auto matches = core::FindChecklistRoots(library_root, checklist_trimmed);
  if (matches.size() == 1) {
    return ChecklistRootResolution{
        "", library_root, matches.front().pack, matches.front().checklist, matches.front().root, false};
  }
  if (matches.size() > 1) {
    if (error_out) {
      *error_out = "Checklist exists in multiple packs; specify pack.";
    }
    if (error_details) {
      json packs = json::array();
      for (const auto& match : matches) {
        packs.push_back(match.pack);
      }
      *error_details = json{{"checklist", checklist_trimmed}, {"packs", packs}};
    }
    return std::nullopt;
  }
  if (!allow_create) {
    if (error_out) {
      *error_out = "Checklist not found in checklists.";
    }
    if (error_details) {
      *error_details = json{{"checklist", checklist_trimmed}};
    }
    return std::nullopt;
  }
  const std::string default_pack = GetDefaultLibraryPack();
  if (!core::IsSafePackToken(default_pack) ||
      !core::IsSafeChecklistToken(checklist_trimmed)) {
    if (error_out) {
      *error_out = "Default pack name is invalid.";
    }
    return std::nullopt;
  }
  return ChecklistRootResolution{
      "", library_root, default_pack, checklist_trimmed, library_root / default_pack / checklist_trimmed, true};
}

std::optional<ChecklistRootResolution> ResolveChecklistRoot(
    const std::vector<ChecklistWorkspaceRoot>& roots,
    const std::string& checklist,
    const std::string& pack,
    const std::string& source_name,
    bool allow_create,
    std::string* error_out,
    json* error_details) {
  const std::string checklist_trimmed = TrimString(checklist);
  const std::string pack_trimmed = TrimString(pack);
  const std::string source_trimmed = TrimString(source_name);
  if (checklist_trimmed.empty()) {
    if (error_out) {
      *error_out = "checklist is required.";
    }
    return std::nullopt;
  }
  if (!source_trimmed.empty() && !core::IsSafePackToken(source_trimmed)) {
    if (error_out) {
      *error_out = "source_name must be a plain token.";
    }
    return std::nullopt;
  }

  std::vector<ChecklistWorkspaceRoot> candidates;
  for (const auto& root : roots) {
    if (source_trimmed.empty() || root.source_name == source_trimmed) {
      candidates.push_back(root);
    }
  }
  if (candidates.empty()) {
    if (error_out) {
      *error_out = "Checklist source not found.";
    }
    if (error_details) {
      *error_details = json{{"source_name", source_trimmed}};
    }
    return std::nullopt;
  }

  std::vector<ChecklistRootResolution> matches;
  for (const auto& candidate : candidates) {
    std::string local_error;
    json local_details = json::object();
    const auto resolved = ResolveChecklistRoot(candidate.root, checklist_trimmed, pack_trimmed,
                                               false, &local_error, &local_details);
    if (resolved) {
      matches.push_back(ChecklistRootResolution{candidate.source_name, candidate.root, resolved->pack,
                                                resolved->checklist_dir, resolved->root, false});
    }
  }
  if (matches.size() == 1) {
    return matches.front();
  }
  if (matches.size() > 1) {
    if (error_out) {
      *error_out = "Checklist exists in multiple sources; specify source_name and pack.";
    }
    if (error_details) {
      json sources = json::array();
      for (const auto& match : matches) {
        sources.push_back({{"source_name", match.source_name},
                           {"source_path", match.library_root.string()},
                           {"pack", match.pack}});
      }
      *error_details = json{{"checklist", checklist_trimmed}, {"matches", sources}};
    }
    return std::nullopt;
  }
  if (!allow_create) {
    if (error_out) {
      *error_out = "Checklist not found in configured checklist sources.";
    }
    if (error_details) {
      *error_details = json{{"checklist", checklist_trimmed},
                            {"source_name", source_trimmed.empty() ? json(nullptr)
                                                                   : json(source_trimmed)}};
    }
    return std::nullopt;
  }

  const ChecklistWorkspaceRoot* target = nullptr;
  if (!source_trimmed.empty()) {
    target = &candidates.front();
  } else {
    for (const auto& candidate : roots) {
      if (candidate.primary) {
        target = &candidate;
        break;
      }
    }
  }
  if (target == nullptr && !roots.empty()) {
    target = &roots.front();
  }
  if (target == nullptr) {
    if (error_out) {
      *error_out = "No checklist sources are configured.";
    }
    return std::nullopt;
  }
  std::string local_error;
  json local_details = json::object();
  const auto resolved = ResolveChecklistRoot(target->root, checklist_trimmed, pack_trimmed, true,
                                             &local_error, &local_details);
  if (!resolved) {
    if (error_out) {
      *error_out = local_error;
    }
    if (error_details) {
      *error_details = local_details;
    }
    return std::nullopt;
  }
  return ChecklistRootResolution{target->source_name,     target->root,   resolved->pack,
                                 resolved->checklist_dir, resolved->root, resolved->defaulted};
}

core::ChecklistOwnership OwnershipFromResolution(const ChecklistRootResolution &resolution,
                                                 const std::string &checklist) {
  core::ChecklistOwnership ownership;
  ownership.source_name = resolution.source_name;
  ownership.source_path = resolution.library_root.string();
  ownership.pack = resolution.pack;
  ownership.checklist_dir = resolution.checklist_dir;
  ownership.checklist = checklist;
  return ownership;
}

json OwnershipsToJson(const std::vector<core::ChecklistOwnership> &ownerships) {
  json matches = json::array();
  for (const auto &ownership : ownerships) {
    matches.push_back({{"source_name", ownership.source_name},
                       {"source_path", ownership.source_path},
                       {"pack", ownership.pack},
                       {"checklist_dir", ownership.checklist_dir},
                       {"checklist", ownership.checklist}});
  }
  return matches;
}

struct WorkspaceScriptEntry {
  std::string id;
  std::string label;
  std::string description;
  std::string rel_path;
  std::filesystem::path absolute_path;
  std::string command;
  std::vector<std::string> args;
  std::vector<std::string> start_args;
  std::vector<std::string> stop_args;
  bool enabled = true;
  bool valid = true;
  bool auto_discovered = false;
  std::string error;
};

struct WorkspaceScriptsCatalog {
  std::string pack;
  std::string checklist;
  std::filesystem::path checklist_root;
  std::filesystem::path scripts_root;
  std::filesystem::path manifest_path;
  bool valid = false;
  std::string error;
  std::vector<WorkspaceScriptEntry> entries;
};

bool IsSafeScriptIdToken(const std::string& value) {
  if (value.empty() || value.size() > 128) {
    return false;
  }
  for (unsigned char ch : value) {
    const bool alpha = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z');
    const bool digit = (ch >= '0' && ch <= '9');
    if (alpha || digit || ch == '_' || ch == '-' || ch == '.') {
      continue;
    }
    return false;
  }
  return true;
}

std::string SanitizeTokenForFileName(const std::string& value) {
  std::string out;
  out.reserve(value.size());
  for (unsigned char ch : value) {
    const bool alpha = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z');
    const bool digit = (ch >= '0' && ch <= '9');
    if (alpha || digit || ch == '_' || ch == '-' || ch == '.') {
      out.push_back(static_cast<char>(ch));
    } else {
      out.push_back('_');
    }
  }
  if (out.empty()) {
    out = "script";
  }
  return out;
}

std::string UtcTimestampForFileName() {
  const std::time_t now = std::time(nullptr);
  std::tm utc{};
#if defined(_WIN32)
  gmtime_s(&utc, &now);
#else
  gmtime_r(&now, &utc);
#endif
  char buffer[32]{};
  std::strftime(buffer, sizeof(buffer), "%Y%m%dT%H%M%SZ", &utc);
  return buffer;
}

bool IsSupportedAssetArchiveExtension(const std::filesystem::path &archive_path) {
  const std::string ext = ToLowerCopy(archive_path.extension().string());
  return ext == ".chk" || ext == ".7z" || ext == ".zip";
}

std::string AssetArchiveType(const std::filesystem::path &archive_path) {
  const std::string ext = ToLowerCopy(archive_path.extension().string());
  if (ext == ".zip") {
    return "zip";
  }
  return "7z";
}

std::filesystem::path ResolveSevenZipExecutable() {
  if (const char *env = std::getenv("CHAX_7Z_PATH")) {
    if (env[0] != '\0') {
      std::error_code ec;
      std::filesystem::path configured = env;
      if (std::filesystem::exists(configured, ec)) {
        return configured;
      }
    }
  }
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

bool RunSevenZip(const std::vector<std::string> &args, const std::filesystem::path &working_directory,
                 std::string *error) {
  const auto executable = ResolveSevenZipExecutable();
  if (executable.empty()) {
    if (error) {
      *error = "7-Zip executable not found. Install 7-Zip, add 7z to PATH, or set CHAX_7Z_PATH.";
    }
    return false;
  }
  int exit_code = -1;
  std::string run_error;
  if (!platform::RunProcess(executable, args, working_directory, {}, {}, &exit_code, &run_error)) {
    if (error) {
      *error = run_error.empty() ? "Failed to run 7-Zip." : run_error;
    }
    return false;
  }
  if (exit_code != 0) {
    if (error) {
      *error = "7-Zip exited with code " + std::to_string(exit_code) + ".";
    }
    return false;
  }
  return true;
}

std::filesystem::path MakeAssetArchiveTempDir() {
  std::error_code ec;
  auto root = std::filesystem::temp_directory_path(ec);
  if (ec || root.empty()) {
    root = std::filesystem::current_path(ec);
  }
  return root / ("chax-asset-pack-" + std::to_string(platform::CurrentProcessId()) + "-" + UtcTimestampForFileName());
}

struct ScopedTempDirectory {
  explicit ScopedTempDirectory(std::filesystem::path path_in) : path(std::move(path_in)) {}
  ~ScopedTempDirectory() {
    if (!path.empty()) {
      std::error_code ec;
      std::filesystem::remove_all(path, ec);
    }
  }
  std::filesystem::path path;
};

struct StagedAssetChecklist {
  std::filesystem::path source_root;
  std::string pack;
  std::string checklist_dir;
};

std::vector<StagedAssetChecklist> FindStagedAssetChecklists(const std::filesystem::path &staging_root,
                                                            const std::string &pack_override,
                                                            const std::string &checklist_dir_override,
                                                            const std::string &default_pack) {
  std::vector<StagedAssetChecklist> items;
  std::error_code ec;
  if (!std::filesystem::exists(staging_root, ec)) {
    return items;
  }
  for (const auto &entry : std::filesystem::recursive_directory_iterator(staging_root, ec)) {
    if (ec) {
      break;
    }
    if (!entry.is_regular_file(ec) || entry.path().filename() != "checklist.md") {
      continue;
    }
    StagedAssetChecklist item;
    item.source_root = entry.path().parent_path();
    item.checklist_dir = checklist_dir_override.empty() ? item.source_root.filename().string() : checklist_dir_override;
    if (item.source_root == staging_root && checklist_dir_override.empty()) {
      item.checklist_dir.clear();
    }
    if (!pack_override.empty()) {
      item.pack = pack_override;
    } else {
      const auto pack_root = item.source_root.parent_path();
      item.pack = pack_root != staging_root && !pack_root.empty() ? pack_root.filename().string() : default_pack;
    }
    items.push_back(std::move(item));
  }
  return items;
}

bool CopyChecklistAssetFolder(const std::filesystem::path &source_root, const std::filesystem::path &target_root,
                              bool replace_files, std::string *error) {
  std::error_code ec;
  if (!std::filesystem::exists(source_root, ec) || !std::filesystem::is_directory(source_root, ec)) {
    if (error) {
      *error = "Extracted checklist folder is missing: " + source_root.string();
    }
    return false;
  }
  if (std::filesystem::exists(target_root, ec)) {
    if (!replace_files) {
      if (error) {
        *error = "Target checklist folder already exists; set replace_files=true to overwrite it.";
      }
      return false;
    }
    std::filesystem::remove_all(target_root, ec);
    if (ec) {
      if (error) {
        *error = "Failed to remove existing checklist folder: " + ec.message();
      }
      return false;
    }
  }
  std::filesystem::create_directories(target_root.parent_path(), ec);
  if (ec) {
    if (error) {
      *error = "Failed to create target pack folder: " + ec.message();
    }
    return false;
  }
  std::filesystem::copy(source_root, target_root,
                        std::filesystem::copy_options::recursive | std::filesystem::copy_options::overwrite_existing,
                        ec);
  if (ec) {
    if (error) {
      *error = "Failed to copy checklist asset folder: " + ec.message();
    }
    return false;
  }
  return true;
}

bool IsRunnableScriptExtension(const std::string& ext_lower) {
  static const std::unordered_set<std::string> kExts = {
      ".ps1", ".cmd", ".bat", ".py", ".exe", ".com", ".sh"};
  return kExts.find(ext_lower) != kExts.end();
}

std::filesystem::path CanonicalOrAbsolutePath(const std::filesystem::path& path) {
  std::error_code ec;
  const std::filesystem::path canonical = std::filesystem::weakly_canonical(path, ec);
  if (!ec) {
    return canonical;
  }
  ec.clear();
  const std::filesystem::path absolute = std::filesystem::absolute(path, ec);
  if (!ec) {
    return absolute.lexically_normal();
  }
  return path.lexically_normal();
}

bool IsPathWithinRoot(const std::filesystem::path& root, const std::filesystem::path& candidate) {
  const std::filesystem::path normalized_root = CanonicalOrAbsolutePath(root);
  const std::filesystem::path normalized_candidate = CanonicalOrAbsolutePath(candidate);

  auto root_it = normalized_root.begin();
  auto candidate_it = normalized_candidate.begin();
  for (; root_it != normalized_root.end() && candidate_it != normalized_candidate.end();
       ++root_it, ++candidate_it) {
    std::string root_part = root_it->string();
    std::string candidate_part = candidate_it->string();
#if defined(_WIN32)
    root_part = ToLowerCopy(root_part);
    candidate_part = ToLowerCopy(candidate_part);
#endif
    if (root_part != candidate_part) {
      return false;
    }
  }
  return root_it == normalized_root.end();
}

std::optional<std::filesystem::path> ResolvePathWithinRoot(
    const std::filesystem::path& root,
    const std::string& relative_path,
    std::string* error_out) {
  const std::string trimmed = TrimString(relative_path);
  if (trimmed.empty()) {
    if (error_out) {
      *error_out = "Path cannot be empty.";
    }
    return std::nullopt;
  }
  const std::filesystem::path rel = std::filesystem::path(trimmed);
  if (rel.is_absolute()) {
    if (error_out) {
      *error_out = "Path must be relative to scripts root.";
    }
    return std::nullopt;
  }
  const std::filesystem::path candidate = (root / rel).lexically_normal();
  if (!IsPathWithinRoot(root, candidate)) {
    if (error_out) {
      *error_out = "Path escapes scripts root.";
    }
    return std::nullopt;
  }
  return candidate;
}

std::optional<std::filesystem::path> ResolveFirstExecutableOnPath(
    std::initializer_list<const char*> names) {
  for (const char* name : names) {
    if (name == nullptr || name[0] == '\0') {
      continue;
    }
    const auto resolved = platform::ResolveExecutableOnPath(name);
    if (!resolved.empty()) {
      return resolved;
    }
  }
  return std::nullopt;
}

json WorkspaceScriptEntryToJson(const WorkspaceScriptEntry& entry) {
  json item{{"id", entry.id},
            {"label", entry.label},
            {"description", entry.description},
            {"rel_path", entry.rel_path},
            {"path", entry.absolute_path.string()},
            {"command", entry.command},
            {"args", entry.args},
            {"start_args", entry.start_args},
            {"stop_args", entry.stop_args},
            {"enabled", entry.enabled},
            {"valid", entry.valid},
            {"auto_discovered", entry.auto_discovered}};
  if (!entry.error.empty()) {
    item["error"] = entry.error;
  } else {
    item["error"] = nullptr;
  }
  return item;
}

std::optional<WorkspaceScriptsCatalog> LoadWorkspaceScriptsCatalog(
    const std::filesystem::path& library_root,
    const std::string& checklist,
    const std::string& pack,
    std::string* error_out,
    json* error_details) {
  std::string resolve_error;
  json resolve_details = json::object();
  const auto resolved =
      ResolveChecklistRoot(library_root, checklist, pack, false, &resolve_error, &resolve_details);
  if (!resolved) {
    if (error_out) {
      *error_out = resolve_error.empty() ? "Invalid pack or checklist." : resolve_error;
    }
    if (error_details) {
      *error_details = resolve_details;
    }
    return std::nullopt;
  }

  WorkspaceScriptsCatalog catalog;
  catalog.pack = resolved->pack;
  catalog.checklist = checklist;
  catalog.checklist_root = resolved->root;
  catalog.scripts_root = resolved->root / "scripts";
  catalog.manifest_path = catalog.scripts_root / "scripts.json";

  std::error_code ec;
  if (!std::filesystem::exists(catalog.scripts_root, ec) ||
      !std::filesystem::is_directory(catalog.scripts_root, ec)) {
    catalog.valid = false;
    catalog.error =
        "Scripts folder not found. Expected checklists/<pack>/<checklist>/scripts.";
    return catalog;
  }

  if (std::filesystem::exists(catalog.manifest_path, ec)) {
    std::string read_error;
    const auto manifest_raw = ReadUtf8FileLimited(catalog.manifest_path, 1024 * 1024, &read_error);
    if (!manifest_raw) {
      catalog.valid = false;
      catalog.error = read_error;
      return catalog;
    }
    const auto manifest = json::parse(*manifest_raw, nullptr, false);
    if (manifest.is_discarded() || !manifest.is_object()) {
      catalog.valid = false;
      catalog.error = "scripts.json must be a valid JSON object.";
      return catalog;
    }
    if (!manifest.contains("scripts") || !manifest["scripts"].is_array()) {
      catalog.valid = false;
      catalog.error = "scripts.json must contain a 'scripts' array.";
      return catalog;
    }

    std::unordered_set<std::string> seen_ids;
    for (std::size_t i = 0; i < manifest["scripts"].size(); ++i) {
      WorkspaceScriptEntry entry;
      const auto& item = manifest["scripts"][i];
      if (!item.is_object()) {
        entry.valid = false;
        entry.error = "Manifest entry is not an object.";
        entry.id = "entry_" + std::to_string(i + 1);
        entry.label = entry.id;
        catalog.entries.push_back(entry);
        continue;
      }
      entry.auto_discovered = false;
      entry.enabled = item.value("enabled", true);
      entry.label = item.value("label", "");
      entry.description = item.value("description", "");
      entry.command = item.value("command", "");
      auto parse_args_field = [&](const char* field_name, std::vector<std::string>* out) {
        if (!item.contains(field_name)) {
          return;
        }
        if (!item[field_name].is_array()) {
          entry.valid = false;
          if (entry.error.empty()) {
            entry.error = std::string("Field '") + field_name +
                          "' must be an array of strings when provided.";
          }
          return;
        }
        for (const auto& arg : item[field_name]) {
          if (!arg.is_string()) {
            entry.valid = false;
            if (entry.error.empty()) {
              entry.error = std::string("Field '") + field_name +
                            "' must be an array of strings when provided.";
            }
            return;
          }
          out->push_back(arg.get<std::string>());
        }
      };
      parse_args_field("args", &entry.args);
      parse_args_field("start_args", &entry.start_args);
      parse_args_field("stop_args", &entry.stop_args);

      const std::string configured_path = item.value("path", "");
      if (configured_path.empty()) {
        entry.valid = false;
        if (entry.error.empty()) {
          entry.error = "Field 'path' is required.";
        }
      } else {
        std::string path_error;
        const auto resolved_path =
            ResolvePathWithinRoot(catalog.scripts_root, configured_path, &path_error);
        if (!resolved_path) {
          entry.valid = false;
          if (entry.error.empty()) {
            entry.error = path_error;
          }
        } else {
          entry.absolute_path = *resolved_path;
          std::error_code file_ec;
          if (!std::filesystem::exists(entry.absolute_path, file_ec) ||
              !std::filesystem::is_regular_file(entry.absolute_path, file_ec)) {
            entry.valid = false;
            if (entry.error.empty()) {
              entry.error = "Script file does not exist: " + entry.absolute_path.string();
            }
          }
          std::error_code rel_ec;
          const auto rel = std::filesystem::relative(entry.absolute_path, catalog.scripts_root, rel_ec);
          entry.rel_path = rel_ec ? entry.absolute_path.filename().string()
                                  : rel.generic_string();
        }
      }

      std::string id = item.value("id", "");
      if (id.empty()) {
        id = entry.absolute_path.empty() ? ("entry_" + std::to_string(i + 1))
                                         : SanitizeTokenForFileName(entry.absolute_path.stem().string());
      }
      if (!IsSafeScriptIdToken(id)) {
        entry.valid = false;
        if (entry.error.empty()) {
          entry.error = "Field 'id' contains unsupported characters.";
        }
        id = "entry_" + std::to_string(i + 1);
      }
      entry.id = id;
      if (entry.label.empty()) {
        entry.label = entry.id;
      }

      if (seen_ids.find(entry.id) != seen_ids.end()) {
        entry.valid = false;
        entry.error = "Duplicate script id in scripts.json: " + entry.id;
      }
      seen_ids.insert(entry.id);
      catalog.entries.push_back(entry);
    }

    catalog.valid = true;
    for (const auto& entry : catalog.entries) {
      if (!entry.valid) {
        catalog.valid = false;
        if (catalog.error.empty()) {
          catalog.error = "One or more scripts in scripts.json are invalid.";
        }
      }
    }
    return catalog;
  }

  std::vector<WorkspaceScriptEntry> discovered;
  for (const auto& fs_entry : std::filesystem::directory_iterator(catalog.scripts_root, ec)) {
    if (ec || !fs_entry.is_regular_file()) {
      continue;
    }
    const std::string ext = ToLowerCopy(fs_entry.path().extension().string());
    if (!IsRunnableScriptExtension(ext)) {
      continue;
    }
    WorkspaceScriptEntry entry;
    entry.auto_discovered = true;
    entry.absolute_path = fs_entry.path();
    entry.rel_path = fs_entry.path().filename().string();
    entry.id = SanitizeTokenForFileName(fs_entry.path().stem().string());
    entry.label = fs_entry.path().filename().string();
    entry.enabled = true;
    entry.valid = true;
    discovered.push_back(entry);
  }
  std::sort(discovered.begin(), discovered.end(), [](const WorkspaceScriptEntry& a,
                                                     const WorkspaceScriptEntry& b) {
    return a.label < b.label;
  });

  std::unordered_map<std::string, int> ids;
  for (auto& entry : discovered) {
    int& count = ids[entry.id];
    ++count;
    if (count > 1) {
      entry.id += "_" + std::to_string(count);
    }
  }

  catalog.entries = std::move(discovered);
  catalog.valid = true;
  return catalog;
}

bool ResolveWorkspaceScriptCommand(const WorkspaceScriptEntry& entry,
                                   const std::filesystem::path& scripts_root,
                                   std::filesystem::path* executable_out,
                                   std::vector<std::string>* args_out,
                                   std::string* error_out) {
  if (!executable_out || !args_out) {
    if (error_out) {
      *error_out = "Internal error: missing command outputs.";
    }
    return false;
  }
  args_out->clear();
  if (!entry.valid || entry.absolute_path.empty()) {
    if (error_out) {
      *error_out = entry.error.empty() ? "Script entry is invalid." : entry.error;
    }
    return false;
  }

  auto resolve_named_runner =
      [](std::initializer_list<const char*> names, std::string* error) -> std::optional<std::filesystem::path> {
    const auto resolved = ResolveFirstExecutableOnPath(names);
    if (!resolved && error) {
      std::ostringstream out;
      bool first = true;
      for (const char* name : names) {
        if (!first) out << ", ";
        out << name;
        first = false;
      }
      *error = "Runner not found on PATH: " + out.str();
    }
    return resolved;
  };

  if (!entry.command.empty()) {
    const std::filesystem::path command_path_candidate = std::filesystem::path(entry.command);
    std::filesystem::path command_path;
    if (command_path_candidate.is_absolute()) {
      std::error_code ec;
      if (!std::filesystem::exists(command_path_candidate, ec)) {
        if (error_out) {
          *error_out = "Configured command does not exist: " + command_path_candidate.string();
        }
        return false;
      }
      command_path = command_path_candidate;
    } else if (command_path_candidate.has_parent_path()) {
      std::string path_error;
      const auto resolved =
          ResolvePathWithinRoot(scripts_root, entry.command, &path_error);
      if (!resolved) {
        if (error_out) {
          *error_out = path_error;
        }
        return false;
      }
      std::error_code ec;
      if (!std::filesystem::exists(*resolved, ec)) {
        if (error_out) {
          *error_out = "Configured command does not exist: " + resolved->string();
        }
        return false;
      }
      command_path = *resolved;
    } else {
      command_path = platform::ResolveExecutableOnPath(entry.command);
      if (command_path.empty()) {
        if (error_out) {
          *error_out = "Configured command not found on PATH: " + entry.command;
        }
        return false;
      }
    }

    *executable_out = command_path;
    args_out->push_back(entry.absolute_path.string());
    args_out->insert(args_out->end(), entry.args.begin(), entry.args.end());
    return true;
  }

  const std::string ext = ToLowerCopy(entry.absolute_path.extension().string());
  if (ext == ".exe" || ext == ".com") {
    *executable_out = entry.absolute_path;
    *args_out = entry.args;
    return true;
  }
  if (ext == ".ps1") {
    std::string runner_error;
    const auto runner =
        resolve_named_runner({"pwsh", "pwsh.exe", "powershell", "powershell.exe"}, &runner_error);
    if (!runner) {
      if (error_out) {
        *error_out = runner_error;
      }
      return false;
    }
    *executable_out = *runner;
    *args_out = {"-ExecutionPolicy", "Bypass", "-File", entry.absolute_path.string()};
    args_out->insert(args_out->end(), entry.args.begin(), entry.args.end());
    return true;
  }
  if (ext == ".cmd" || ext == ".bat") {
    std::string runner_error;
    const auto runner = resolve_named_runner({"cmd", "cmd.exe"}, &runner_error);
    if (!runner) {
      if (error_out) {
        *error_out = runner_error;
      }
      return false;
    }
    *executable_out = *runner;
    *args_out = {"/c", entry.absolute_path.string()};
    args_out->insert(args_out->end(), entry.args.begin(), entry.args.end());
    return true;
  }
  if (ext == ".py") {
    std::string runner_error;
    const auto runner = resolve_named_runner({"python", "python3"}, &runner_error);
    if (!runner) {
      if (error_out) {
        *error_out = runner_error;
      }
      return false;
    }
    *executable_out = *runner;
    *args_out = {entry.absolute_path.string()};
    args_out->insert(args_out->end(), entry.args.begin(), entry.args.end());
    return true;
  }
  if (ext == ".sh") {
    std::string runner_error;
    const auto runner = resolve_named_runner({"sh", "bash"}, &runner_error);
    if (!runner) {
      if (error_out) {
        *error_out = runner_error;
      }
      return false;
    }
    *executable_out = *runner;
    *args_out = {entry.absolute_path.string()};
    args_out->insert(args_out->end(), entry.args.begin(), entry.args.end());
    return true;
  }

  if (error_out) {
    *error_out = "Unsupported script extension: " + ext;
  }
  return false;
}

std::optional<std::vector<std::string>> ParseStringArrayField(const json& payload,
                                                              const char* key,
                                                              std::string* error_out) {
  const auto it = payload.find(key);
  if (it == payload.end()) {
    return std::nullopt;
  }
  if (!it->is_array()) {
    if (error_out) {
      *error_out = std::string("Field '") + key + "' must be an array of strings.";
    }
    return std::nullopt;
  }
  std::vector<std::string> values;
  values.reserve(it->size());
  for (const auto& node : *it) {
    if (!node.is_string()) {
      if (error_out) {
        *error_out = std::string("Field '") + key + "' must be an array of strings.";
      }
      return std::nullopt;
    }
    values.push_back(node.get<std::string>());
  }
  return values;
}

std::optional<std::string> ReadUtf8FileLimited(const std::filesystem::path& path,
                                               std::size_t max_bytes,
                                               std::string* error_out) {
  std::error_code ec;
  const auto size = std::filesystem::file_size(path, ec);
  if (ec) {
    if (error_out) *error_out = "Failed to stat file: " + path.string();
    return std::nullopt;
  }
  if (size > max_bytes) {
    if (error_out) *error_out = "File too large (max " + std::to_string(max_bytes) + " bytes).";
    return std::nullopt;
  }
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    if (error_out) *error_out = "Failed to open file: " + path.string();
    return std::nullopt;
  }
  std::string content;
  content.resize(static_cast<std::size_t>(size));
  in.read(content.data(), static_cast<std::streamsize>(content.size()));
  if (!in && !in.eof()) {
    if (error_out) *error_out = "Failed to read file: " + path.string();
    return std::nullopt;
  }
  return content;
}

bool WriteUtf8File(const std::filesystem::path& path, const std::string& content,
                   std::string* error_out) {
  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) {
    if (error_out) *error_out = "Failed to open file for write: " + path.string();
    return false;
  }
  out.write(content.data(), static_cast<std::streamsize>(content.size()));
  if (!out) {
    if (error_out) *error_out = "Failed to write file: " + path.string();
    return false;
  }
  return true;
}

json RelationshipsToJson(const RelationshipGraph& graph) {
  json outgoing = json::array();
  for (const auto& edge : graph.outgoing) {
    outgoing.push_back({{"predicate", edge.predicate}, {"target", edge.target}});
  }
  json incoming = json::array();
  for (const auto& edge : graph.incoming) {
    incoming.push_back({{"predicate", edge.predicate}, {"source", edge.target}});
  }
  return {{"outgoing", outgoing}, {"incoming", incoming}};
}

bool IsPrefillPredicateToken(const std::string& predicate) {
  return predicate.find("SearchPrefill") != std::string::npos;
}

bool HasPrefillRelationships(const RelationshipGraph& graph) {
  for (const auto& edge : graph.outgoing) {
    if (IsPrefillPredicateToken(edge.predicate)) {
      return true;
    }
  }
  return false;
}

json PrefillDatasetStatusToJson(const PrefillDatasetStatus& status) {
  json payload;
  payload["mode"] = status.mode;
  if (!status.path.empty()) {
    payload["path"] = status.path;
  }
  if (!status.matched_slug_id.empty()) {
    payload["matched_slug_id"] = status.matched_slug_id;
  }
  return payload;
}

json PredicateWarning(const std::string& predicate, const std::string& status) {
  return {{"code", "UNKNOWN_PREDICATE"},
          {"message", "Predicate not registered as active."},
          {"details", {{"predicate", predicate}, {"status", status}}}};
}

json VerifyEvaluationToJson(const VerifyEvaluation& evaluation) {
  return {
      {"predicate", evaluation.predicate},
      {"target_address_id", evaluation.target_address_id},
      {"predicate_bool", evaluation.predicate_bool},
      {"reason_code", evaluation.reason_code},
      {"reason", evaluation.reason},
      {"gate_applied", evaluation.gate_applied},
      {"gate_mode", evaluation.gate_mode.empty() ? json(nullptr) : json(evaluation.gate_mode)},
      {"contributor_count", evaluation.contributor_count},
      {"contributor_true_count", evaluation.contributor_true_count},
      {"would_write", evaluation.would_write},
      {"write_decision", evaluation.write_decision},
  };
}

constexpr std::int64_t kImportOrderGap = 1000;
constexpr std::int64_t kImportSectionStride = 10000000;
constexpr std::string_view kSlugSuccessorPredicate = "slugSuccessor";
constexpr std::string_view kSlugPredecessorPredicate = "slugPredecessor";

json SlugToJson(const ChecklistSlug& slug) {
  json relationships = json::array();
  for (const auto& edge : slug.relationships) {
    relationships.push_back({{"predicate", edge.predicate}, {"target", edge.target}});
  }

  return {{"address_id", slug.address_id},
          {"address_order", slug.address_order},
          {"slug_id", slug.slug_id},
          {"instance_id", slug.instance_id},
          {"instance_principal", slug.instance_principal},
          {"checklist", slug.checklist},
          {"section", slug.section},
          {"procedure", slug.procedure},
          {"action", slug.action},
          {"spec", slug.spec},
          {"result", slug.result},
          {"status", StatusToString(slug.status)},
          {"comment", slug.comment},
          {"timestamp", slug.timestamp},
          {"entity_id", slug.entity_id},
          {"instructions", slug.instructions},
          {"relationships", relationships}};
}

json ChecklistGraphToJson(const ChecklistGraph& graph) {
  json nodes = json::array();
  std::unordered_set<std::string> sections;
  for (const auto& node : graph.nodes) {
    sections.insert(node.section);
    nodes.push_back({
        {"address_id", node.address_id},
        {"slug_id", node.slug_id},
        {"address_order", node.address_order},
        {"section", node.section},
        {"procedure", node.procedure},
        {"action", node.action},
        {"spec", node.spec},
        {"result", node.result},
        {"status", node.status},
        {"comment", node.comment},
        {"instructions", node.instructions},
        {"spec_kind", node.spec_kind},
        {"visual_shape", node.visual_shape},
        {"incoming_relationship_count", node.incoming_relationship_count},
        {"outgoing_relationship_count", node.outgoing_relationship_count},
    });
  }
  json edges = json::array();
  std::size_t relationship_count = 0;
  std::size_t external_relationship_count = 0;
  for (const auto& edge : graph.edges) {
    if (edge.kind == "relationship") {
      ++relationship_count;
      if (edge.is_external) {
        ++external_relationship_count;
      }
    }
    edges.push_back({
        {"source_address_id", edge.source_address_id},
        {"target_address_id", edge.target_address_id},
        {"predicate", edge.predicate},
        {"kind", edge.kind},
        {"is_lineage", edge.is_lineage},
        {"is_external", edge.is_external},
        {"external_category", edge.external_category},
    });
  }
  return {
      {"schema", "chax-graph-view-v1"},
      {"checklist", graph.checklist},
      {"instance_id", graph.instance_id},
      {"nodes", nodes},
      {"edges", edges},
      {"warnings", graph.warnings},
      {"summary",
       {{"sections", sections.size()},
        {"nodes", graph.nodes.size()},
        {"relationships", relationship_count},
        {"external_relationships", external_relationship_count}}},
  };
}

void AssignImportOrder(std::vector<ChecklistSlug>& slugs) {
  std::unordered_map<std::string, std::int64_t> section_index;
  std::unordered_map<std::string, std::int64_t> row_counts;
  std::int64_t next_section_index = 0;

  for (auto& slug : slugs) {
    auto it = section_index.find(slug.section);
    if (it == section_index.end()) {
      section_index[slug.section] = next_section_index++;
      row_counts[slug.section] = 0;
      it = section_index.find(slug.section);
    }
    std::int64_t& row_index = row_counts[slug.section];
    slug.address_order =
        it->second * kImportSectionStride + (row_index + 1) * kImportOrderGap;
    ++row_index;
  }
}

std::vector<TemplateRelationship> MergeTemplateRelationshipsWithLineage(
    const ChecklistStore& store,
    const std::string& checklist,
    const std::vector<std::string>& subject_slug_ids,
    const std::vector<TemplateRelationship>& incoming) {
  if (checklist.empty()) {
    return incoming;
  }

  std::unordered_set<std::string> subjects;
  subjects.reserve(subject_slug_ids.size());
  for (const auto& subject : subject_slug_ids) {
    if (!subject.empty()) {
      subjects.insert(subject);
    }
  }

  std::unordered_set<std::string> seen;
  seen.reserve(incoming.size());
  std::vector<TemplateRelationship> merged;
  merged.reserve(incoming.size());

  std::unordered_map<std::string, std::vector<std::string>> direct_predecessors;
  for (const auto& rel : incoming) {
    if (rel.predicate == kSlugPredecessorPredicate &&
        subjects.find(rel.subject_slug_id) != subjects.end()) {
      direct_predecessors[rel.subject_slug_id].push_back(rel.target_slug_id);
      continue;
    }
    if (rel.predicate == kSlugSuccessorPredicate &&
        subjects.find(rel.target_slug_id) != subjects.end()) {
      direct_predecessors[rel.target_slug_id].push_back(rel.subject_slug_id);
    }
  }

  const auto add_rel = [&](const TemplateRelationship& rel) {
    if (rel.subject_slug_id.empty() || rel.predicate.empty() || rel.target_slug_id.empty()) {
      return;
    }
    if (!subjects.empty() && subjects.find(rel.subject_slug_id) == subjects.end()) {
      return;
    }
    std::string key;
    key.reserve(rel.subject_slug_id.size() + rel.predicate.size() + rel.target_slug_id.size() + 2);
    key.append(rel.subject_slug_id);
    key.push_back('\x1f');
    key.append(rel.predicate);
    key.push_back('\x1f');
    key.append(rel.target_slug_id);
    if (seen.insert(key).second) {
      merged.push_back(rel);
    }
  };

  for (const auto& rel : incoming) {
    add_rel(rel);
  }

  if (subjects.empty()) {
    return merged;
  }

  const auto existing = store.GetTemplateRelationshipsForChecklist(checklist);
  for (const auto& rel : existing) {
    if (rel.predicate != kSlugPredecessorPredicate && rel.predicate != kSlugSuccessorPredicate) {
      continue;
    }
    add_rel(rel);
  }

  if (!direct_predecessors.empty()) {
    std::unordered_map<std::string, std::vector<std::string>> predecessor_cache;
    predecessor_cache.reserve(direct_predecessors.size());

    const auto load_predecessors = [&](const std::string& slug_id)
        -> std::vector<std::string> {
      auto it = predecessor_cache.find(slug_id);
      if (it != predecessor_cache.end()) {
        return it->second;
      }
      std::vector<std::string> preds;
      const auto rels = store.ListTemplateRelationships(slug_id, std::nullopt, std::nullopt,
                                                        std::nullopt, std::nullopt);
      for (const auto& rel : rels) {
        if (rel.predicate == kSlugPredecessorPredicate) {
          preds.push_back(rel.target_slug_id);
        }
      }
      auto inserted = predecessor_cache.emplace(slug_id, std::move(preds));
      return inserted.first->second;
    };

    for (const auto& entry : direct_predecessors) {
      const std::string& subject = entry.first;
      std::unordered_set<std::string> visited;
      for (const auto& direct : entry.second) {
        if (direct.empty() || direct == subject) {
          continue;
        }
        if (!visited.insert(direct).second) {
          continue;
        }
        std::vector<std::string> stack;
        stack.push_back(direct);
        while (!stack.empty()) {
          const std::string current = stack.back();
          stack.pop_back();
          const auto preds = load_predecessors(current);
          for (const auto& pred : preds) {
            if (pred.empty() || pred == subject) {
              continue;
            }
            if (!visited.insert(pred).second) {
              continue;
            }
            add_rel(TemplateRelationship{subject, std::string{kSlugPredecessorPredicate}, pred});
            stack.push_back(pred);
          }
        }
      }
    }
  }

  return merged;
}

struct SlugLineageAliases {
  std::unordered_map<std::string, std::string> aliases;
  json warnings = json::array();
};

SlugLineageAliases BuildSlugLineageAliases(
    const std::vector<ChecklistSlug>& slugs,
    const std::vector<TemplateRelationship>& relationships) {
  SlugLineageAliases result;
  std::unordered_set<std::string> present;
  present.reserve(slugs.size());
  for (const auto& slug : slugs) {
    present.insert(slug.slug_id);
  }

  std::unordered_map<std::string, std::vector<std::string>> successors;
  std::unordered_set<std::string> lineage_nodes;
  successors.reserve(relationships.size());
  lineage_nodes.reserve(relationships.size() * 2);
  for (const auto& rel : relationships) {
    std::string subject;
    std::string target;
    if (rel.predicate == kSlugSuccessorPredicate) {
      subject = rel.subject_slug_id;
      target = rel.target_slug_id;
    } else if (rel.predicate == kSlugPredecessorPredicate) {
      subject = rel.target_slug_id;
      target = rel.subject_slug_id;
    } else {
      continue;
    }
    lineage_nodes.insert(subject);
    lineage_nodes.insert(target);
    auto& targets = successors[subject];
    if (std::find(targets.begin(), targets.end(), target) == targets.end()) {
      targets.push_back(target);
    }
  }
  if (lineage_nodes.empty()) {
    return result;
  }

  std::unordered_set<std::string> warned;
  const auto add_warning = [&](const std::string& code, const std::string& slug_id,
                               const std::string& message, const json& details) {
    const std::string key = code + "|" + slug_id;
    if (warned.insert(key).second) {
      result.warnings.push_back({{"code", code}, {"message", message}, {"details", details}});
    }
  };

  const auto resolve_latest = [&](const std::string& start) -> std::optional<std::string> {
    std::unordered_set<std::string> seen;
    std::string current = start;
    while (true) {
      if (!seen.insert(current).second) {
        add_warning("LINEAGE_CYCLE", start,
                    "Slug lineage contains a cycle; cannot resolve latest slug.",
                    json{{"slug_id", start}, {"cycle_at", current}});
        return std::nullopt;
      }
      const auto it = successors.find(current);
      if (it == successors.end() || it->second.empty()) {
        return current;
      }
      if (it->second.size() > 1) {
        add_warning("LINEAGE_BRANCH", start,
                    "Slug lineage has multiple successors; cannot resolve latest slug.",
                    json{{"slug_id", start}, {"successors", it->second}});
        return std::nullopt;
      }
      current = it->second.front();
    }
  };

  for (const auto& slug_id : lineage_nodes) {
    const auto latest = resolve_latest(slug_id);
    if (!latest || *latest == slug_id) {
      continue;
    }
    if (present.find(*latest) == present.end()) {
      add_warning("LINEAGE_MISSING_LATEST", slug_id,
                  "Latest slug not found in instance; no alias created.",
                  json{{"slug_id", slug_id}, {"latest_slug_id", *latest}});
      continue;
    }
    result.aliases.emplace(slug_id, *latest);
  }
  return result;
}

std::optional<std::string> ResolveTemplateTargetForInstance(
    const TemplateRelationship& relationship, const SlugLineageAliases& lineage) {
  const auto it = lineage.aliases.find(relationship.target_slug_id);
  if (it == lineage.aliases.end()) {
    return relationship.target_slug_id;
  }

  // A lineage declaration remains template metadata.  Its legacy target was not an
  // address-level edge before resolution, and resolving it would create a self-edge.
  if (relationship.predicate == kSlugPredecessorPredicate ||
      relationship.predicate == kSlugSuccessorPredicate) {
    return std::nullopt;
  }
  return it->second;
}

struct AddressLineageAliases {
  std::unordered_map<std::string, std::string> aliases;
  json warnings = json::array();
};

AddressLineageAliases BuildAddressLineageAliases(
    const std::vector<ChecklistSlug>& slugs,
    const std::vector<AddressRelationship>& relationships,
    const std::string& instance_id) {
  AddressLineageAliases result;
  std::unordered_set<std::string> present;
  present.reserve(slugs.size());
  for (const auto& slug : slugs) {
    if (!slug.address_id.empty()) {
      present.insert(slug.address_id);
    }
  }

  std::unordered_map<std::string, std::vector<std::string>> successors;
  std::unordered_set<std::string> lineage_nodes;
  successors.reserve(relationships.size());
  lineage_nodes.reserve(relationships.size() * 2);
  for (const auto& rel : relationships) {
    std::string subject;
    std::string target;
    if (rel.predicate == kSlugSuccessorPredicate) {
      subject = rel.subject_address_id;
      target = rel.target_address_id;
    } else if (rel.predicate == kSlugPredecessorPredicate) {
      subject = rel.target_address_id;
      target = rel.subject_address_id;
    } else {
      continue;
    }
    if (subject.size() != 32 || target.size() != 32) {
      continue;
    }
    if (!instance_id.empty()) {
      if (subject.substr(16, 16) != instance_id || target.substr(16, 16) != instance_id) {
        continue;
      }
    }
    lineage_nodes.insert(subject);
    lineage_nodes.insert(target);
    auto& targets = successors[subject];
    if (std::find(targets.begin(), targets.end(), target) == targets.end()) {
      targets.push_back(target);
    }
  }
  if (lineage_nodes.empty()) {
    return result;
  }

  std::unordered_set<std::string> warned;
  const auto add_warning = [&](const std::string& code, const std::string& address_id,
                               const std::string& message, const json& details) {
    const std::string key = code + "|" + address_id;
    if (warned.insert(key).second) {
      result.warnings.push_back({{"code", code}, {"message", message}, {"details", details}});
    }
  };

  const auto resolve_latest = [&](const std::string& start) -> std::optional<std::string> {
    std::unordered_set<std::string> seen;
    std::string current = start;
    while (true) {
      if (!seen.insert(current).second) {
        add_warning("LINEAGE_CYCLE", start,
                    "Address lineage contains a cycle; cannot resolve latest address.",
                    json{{"address_id", start}, {"cycle_at", current}});
        return std::nullopt;
      }
      const auto it = successors.find(current);
      if (it == successors.end() || it->second.empty()) {
        return current;
      }
      if (it->second.size() > 1) {
        add_warning("LINEAGE_BRANCH", start,
                    "Address lineage has multiple successors; cannot resolve latest address.",
                    json{{"address_id", start}, {"successors", it->second}});
        return std::nullopt;
      }
      current = it->second.front();
    }
  };

  for (const auto& address_id : lineage_nodes) {
    const auto latest = resolve_latest(address_id);
    if (!latest || *latest == address_id) {
      continue;
    }
    if (present.find(*latest) == present.end()) {
      add_warning("LINEAGE_MISSING_LATEST", address_id,
                  "Latest address not found in instance; no alias created.",
                  json{{"address_id", address_id}, {"latest_address_id", *latest}});
      continue;
    }
    result.aliases.emplace(address_id, *latest);
  }
  return result;
}

SlugUpdate ParseUpdatePayload(const json& payload) {
  if (!payload.is_object()) {
    throw std::invalid_argument("Payload must be a JSON object.");
  }
  SlugUpdate update;

  if (const auto result_it = payload.find("result"); result_it != payload.end() &&
                                                     (result_it->is_string() || result_it->is_null())) {
    update.result = result_it->is_null() ? std::string{} : result_it->get<std::string>();
  }

  if (const auto status_it = payload.find("status"); status_it != payload.end()) {
    if (!status_it->is_string()) {
      throw std::invalid_argument("Field 'status' must be a string when provided.");
    }
    auto trim = [](std::string value) {
      std::size_t start = 0;
      std::size_t end = value.size();
      while (start < value.size() &&
             std::isspace(static_cast<unsigned char>(value[start])) != 0) {
        ++start;
      }
      while (end > start && std::isspace(static_cast<unsigned char>(value[end - 1])) != 0) {
        --end;
      }
      return value.substr(start, end - start);
    };
    const std::string raw_status = status_it->get<std::string>();
    const std::string trimmed_status = trim(raw_status);
    if (!trimmed_status.empty()) {
      auto to_lower = [](std::string value) {
        for (auto& ch : value) {
          ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        }
        return value;
      };
      if (to_lower(trimmed_status) == "unknown") {
        // Treat Unknown as unset so result/comment writes are not blocked.
      } else {
      const auto status = ParseStatus(trimmed_status);
      if (status == ChecklistStatus::kUnknown) {
        throw std::invalid_argument("Status must be Pass, Fail, NA, or Other.");
      }
      update.status = status;
      }
    }
  }

  if (const auto comment_it = payload.find("comment");
      comment_it != payload.end() && (comment_it->is_string() || comment_it->is_null())) {
    update.comment = comment_it->is_null() ? std::string{} : comment_it->get<std::string>();
  }

  if (const auto ts_it = payload.find("timestamp"); ts_it != payload.end()) {
    if (!ts_it->is_string()) {
      throw std::invalid_argument("Field 'timestamp' must be a string when provided.");
    }
    update.timestamp = ts_it->get<std::string>();
  }

  if (const auto entity_it = payload.find("entity_id"); entity_it != payload.end()) {
    if (!entity_it->is_string()) {
      throw std::invalid_argument("Field 'entity_id' must be a string when provided.");
    }
    update.entity_id_override = entity_it->get<std::string>();
  }
  if (const auto entity_pr_it = payload.find("entity_principal"); entity_pr_it != payload.end()) {
    if (!entity_pr_it->is_string()) {
      throw std::invalid_argument("Field 'entity_principal' must be a string when provided.");
    }
    update.entity_principal_override = entity_pr_it->get<std::string>();
    update.entity_id_override = core::ComputeEntityId(update.entity_principal_override.value());
  }

  return update;
}

std::vector<SlugUpdate> ParseBulkPayload(const json& payload) {
  if (!payload.is_array()) {
    throw std::invalid_argument("Bulk payload must be a JSON array.");
  }
  std::vector<SlugUpdate> updates;
  for (const auto& item : payload) {
    updates.push_back(ParseUpdatePayload(item));
  }
  return updates;
}

std::string ToLowerCopy(std::string value) {
  for (auto& ch : value) {
    ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  }
  return value;
}

std::string TrimString(const std::string& value) {
  std::size_t start = 0;
  std::size_t end = value.size();
  while (start < value.size() &&
         std::isspace(static_cast<unsigned char>(value[start])) != 0) {
    ++start;
  }
  while (end > start && std::isspace(static_cast<unsigned char>(value[end - 1])) != 0) {
    --end;
  }
  return value.substr(start, end - start);
}

std::string CollapseWhitespace(const std::string& value) {
  std::string output;
  output.reserve(value.size());
  bool in_space = false;
  for (unsigned char ch : value) {
    if (std::isspace(ch) != 0) {
      if (!in_space) {
        output.push_back(' ');
        in_space = true;
      }
    } else {
      output.push_back(static_cast<char>(ch));
      in_space = false;
    }
  }
  return output;
}

std::string NormalizePrincipalField(const std::string& value) {
  return ToLowerCopy(CollapseWhitespace(TrimString(value)));
}

std::string CanonicalizePrincipalString(const std::string& value) {
  return NormalizePrincipalField(value);
}

enum class AuthKind { kGuest = 0, kUser, kService };

struct AuthContext {
  AuthKind kind = AuthKind::kGuest;
  std::string subject;
  std::string provider;
  std::string client_id;
  std::string scope;
};

thread_local AuthContext g_auth_context;

AuthContext CurrentAuthContext() { return g_auth_context; }

void SetAuthContext(const AuthContext& ctx) { g_auth_context = ctx; }

std::string GetHeaderValue(const platform::HttpRequest& request, const std::string& key) {
  const auto target = ToLowerCopy(key);
  for (const auto& entry : request.headers) {
    if (ToLowerCopy(entry.first) == target) {
      return entry.second;
    }
  }
  return {};
}

bool IsLoopbackAddress(const std::string& address) {
  if (address == "127.0.0.1" || address == "::1" || address == "localhost") {
    return true;
  }
  if (address.rfind("127.", 0) == 0) {
    return true;
  }
  if (address.rfind("::ffff:127.", 0) == 0) {
    return true;
  }
  return false;
}

std::unordered_map<std::string, std::string> ParseCookies(const platform::HttpRequest& request) {
  std::unordered_map<std::string, std::string> cookies;
  const std::string header = GetHeaderValue(request, "Cookie");
  std::size_t start = 0;
  while (start < header.size()) {
    const auto sep = header.find(';', start);
    const std::size_t end = (sep == std::string::npos) ? header.size() : sep;
    const auto pair = header.substr(start, end - start);
    const auto eq = pair.find('=');
    if (eq != std::string::npos) {
      auto name = pair.substr(0, eq);
      auto value = pair.substr(eq + 1);
      while (!name.empty() && std::isspace(static_cast<unsigned char>(name.front())) != 0) {
        name.erase(name.begin());
      }
      while (!name.empty() && std::isspace(static_cast<unsigned char>(name.back())) != 0) {
        name.pop_back();
      }
      cookies[name] = value;
    }
    if (sep == std::string::npos) {
      break;
    }
    start = sep + 1;
  }
  return cookies;
}

std::string UrlDecode(const std::string& value) {
  std::string decoded;
  decoded.reserve(value.size());
  for (std::size_t i = 0; i < value.size(); ++i) {
    if (value[i] == '%' && i + 2 < value.size()) {
      const auto hex = value.substr(i + 1, 2);
      try {
        const auto byte = static_cast<char>(std::stoi(hex, nullptr, 16));
        decoded.push_back(byte);
        i += 2;
        continue;
      } catch (...) {
      }
    } else if (value[i] == '+') {
      decoded.push_back(' ');
      continue;
    }
    decoded.push_back(value[i]);
  }
  return decoded;
}

std::string UrlEncode(const std::string& value) {
  static constexpr char kHex[] = "0123456789ABCDEF";
  std::string encoded;
  for (unsigned char ch : value) {
    if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ||
        ch == '-' || ch == '_' || ch == '.' || ch == '~') {
      encoded.push_back(static_cast<char>(ch));
    } else if (ch == ' ') {
      encoded.push_back('+');
    } else {
      encoded.push_back('%');
      encoded.push_back(kHex[(ch >> 4) & 0x0F]);
      encoded.push_back(kHex[ch & 0x0F]);
    }
  }
  return encoded;
}

std::optional<bool> ParseEnvBool(const char* value) {
  if (!value) {
    return std::nullopt;
  }
  const std::string lowered = ToLowerCopy(value);
  if (lowered == "1" || lowered == "true" || lowered == "yes" || lowered == "on") {
    return true;
  }
  if (lowered == "0" || lowered == "false" || lowered == "no" || lowered == "off") {
    return false;
  }
  return std::nullopt;
}

AuthContext MakeGuestContext(const ServerConfig& config) {
  AuthContext ctx;
  ctx.kind = AuthKind::kGuest;
  ctx.subject = config.guest_name;
  ctx.provider = config.guest_provider;
  return ctx;
}

std::string BuildEntityPrincipalFromContext(const AuthContext& ctx, const ServerConfig& config) {
  std::vector<std::pair<std::string, std::string>> fields;
  std::string prefix = "guest";
  if (ctx.kind == AuthKind::kUser) {
    prefix = "user";
    const std::string username = ctx.subject.empty() ? "unknown" : ctx.subject;
    fields.emplace_back("provider", config.auth_provider);
    fields.emplace_back("username", username);
  } else if (ctx.kind == AuthKind::kService) {
    prefix = "service";
    const std::string name = !ctx.subject.empty() ? ctx.subject
                                                  : (!ctx.client_id.empty() ? ctx.client_id : "unknown");
    fields.emplace_back("name", name);
    fields.emplace_back("provider", config.auth_provider);
  } else {
    fields.emplace_back("provider", config.guest_provider);
    if (!config.guest_name.empty()) {
      fields.emplace_back("name", config.guest_name);
    }
  }

  std::sort(fields.begin(), fields.end(),
            [](const auto& a, const auto& b) { return a.first < b.first; });

  std::ostringstream oss;
  oss << prefix;
  for (const auto& field : fields) {
    oss << "||" << field.first << "=" << NormalizePrincipalField(field.second);
  }
  return oss.str();
}

struct EntityResolution {
  std::string principal;
  std::string entity_id;
};

std::optional<platform::HttpResponse> ResolveEntityForRequest(
    const std::optional<std::string>& requested_principal, const ServerConfig& config,
    EntityResolution* out) {
  const std::string derived_principal = BuildEntityPrincipalFromContext(CurrentAuthContext(), config);
  if (requested_principal) {
    if (CanonicalizePrincipalString(*requested_principal) !=
        CanonicalizePrincipalString(derived_principal)) {
      return ErrorResponse("FORBIDDEN",
                           "entity_principal does not match the authenticated identity.", {}, 403);
    }
  }
  out->principal = derived_principal;
  out->entity_id = ComputeEntityId(out->principal);
  return std::nullopt;
}

std::unordered_map<std::string, std::string> ParseFormUrlEncoded(const std::string& body) {
  std::unordered_map<std::string, std::string> params;
  std::size_t start = 0;
  while (start <= body.size()) {
    const auto amp = body.find('&', start);
    const std::size_t end = (amp == std::string::npos) ? body.size() : amp;
    const auto pair = body.substr(start, end - start);
    const auto eq = pair.find('=');
    if (eq != std::string::npos) {
      const auto key = UrlDecode(pair.substr(0, eq));
      const auto value = UrlDecode(pair.substr(eq + 1));
      params[key] = value;
    } else if (!pair.empty()) {
      params[UrlDecode(pair)] = "";
    }
    if (amp == std::string::npos) break;
    start = amp + 1;
  }
  return params;
}

std::unordered_map<std::string, std::string> ParseRequestParams(
    const platform::HttpRequest& request) {
  const std::string content_type = ToLowerCopy(GetHeaderValue(request, "Content-Type"));
  if (content_type.find("application/json") != std::string::npos) {
    std::unordered_map<std::string, std::string> params;
    auto parsed = json::parse(request.body, nullptr, /*allow_exceptions=*/false);
    if (!parsed.is_object()) {
      return params;
    }
    for (auto it = parsed.begin(); it != parsed.end(); ++it) {
      if (it->is_string()) {
        params[it.key()] = it->get<std::string>();
      } else if (it->is_number_integer()) {
        params[it.key()] = std::to_string(it->get<int>());
      }
    }
    return params;
  }
  return ParseFormUrlEncoded(request.body);
}

std::string HtmlEscape(const std::string& value) {
  std::string escaped;
  escaped.reserve(value.size());
  for (char ch : value) {
    switch (ch) {
      case '&':
        escaped.append("&amp;");
        break;
      case '<':
        escaped.append("&lt;");
        break;
      case '>':
        escaped.append("&gt;");
        break;
      case '"':
        escaped.append("&quot;");
        break;
      case '\'':
        escaped.append("&#39;");
        break;
      default:
        escaped.push_back(ch);
    }
  }
  return escaped;
}

bool HasScope(const std::string& scope_list, const std::string& required_scope) {
  std::size_t start = 0;
  while (start <= scope_list.size()) {
    const auto space = scope_list.find(' ', start);
    const std::size_t end = (space == std::string::npos) ? scope_list.size() : space;
    if (scope_list.substr(start, end - start) == required_scope) {
      return true;
    }
    if (space == std::string::npos) break;
    start = space + 1;
  }
  return false;
}

bool IsHttpsUrl(const std::string& url) { return url.rfind("https://", 0) == 0; }

constexpr std::size_t kMaxAuthorizeParamLength = 4096;

struct UrlParts {
  std::string scheme;
  std::string host;
  std::string port;
  std::string path;
};

std::optional<UrlParts> ParseUrlParts(const std::string& url) {
  const auto scheme_end = url.find("://");
  if (scheme_end == std::string::npos || scheme_end == 0) {
    return std::nullopt;
  }
  UrlParts parts;
  parts.scheme = ToLowerCopy(url.substr(0, scheme_end));
  const std::size_t authority_start = scheme_end + 3;
  const auto path_pos = url.find('/', authority_start);
  const std::string authority =
      url.substr(authority_start, path_pos == std::string::npos ? std::string::npos : path_pos - authority_start);
  if (authority.empty()) {
    return std::nullopt;
  }
  const auto colon_pos = authority.find(':');
  if (colon_pos == std::string::npos) {
    parts.host = ToLowerCopy(authority);
  } else {
    parts.host = ToLowerCopy(authority.substr(0, colon_pos));
    parts.port = authority.substr(colon_pos + 1);
  }
  parts.path = (path_pos == std::string::npos) ? "/" : url.substr(path_pos);
  return parts;
}

std::string NormalizeLoopbackHost(const std::string& host) {
  const auto lower = ToLowerCopy(host);
  if (lower == "localhost" || lower == "127.0.0.1") {
    return "127.0.0.1";
  }
  return lower;
}

std::string NormalizePort(const std::string& scheme, const std::string& port) {
  if (!port.empty()) {
    return port;
  }
  if (scheme == "https") return "443";
  if (scheme == "http") return "80";
  return port;
}

bool IsLoopbackRedirect(const std::string& uri) {
  const auto parts = ParseUrlParts(uri);
  if (!parts) {
    return false;
  }
  return NormalizeLoopbackHost(parts->host) == "127.0.0.1";
}

bool RedirectUrisMatch(const std::string& requested, const std::string& allowed) {
  const auto requested_parts = ParseUrlParts(requested);
  const auto allowed_parts = ParseUrlParts(allowed);
  if (!requested_parts || !allowed_parts) {
    return false;
  }
  if (requested_parts->scheme != allowed_parts->scheme) {
    return false;
  }
  if (NormalizeLoopbackHost(requested_parts->host) != NormalizeLoopbackHost(allowed_parts->host)) {
    return false;
  }
  if (NormalizePort(requested_parts->scheme, requested_parts->port) !=
      NormalizePort(allowed_parts->scheme, allowed_parts->port)) {
    return false;
  }
  return requested_parts->path == allowed_parts->path;
}

bool IsLoopbackOrigin(const std::string& origin) {
  if (origin.empty()) {
    return true;
  }
  const auto parts = ParseUrlParts(origin);
  if (!parts) {
    return false;
  }
  if (parts->scheme != "http" && parts->scheme != "https") {
    return false;
  }
  return NormalizeLoopbackHost(parts->host) == "127.0.0.1" ||
         IsLoopbackAddress(parts->host);
}

bool IsLocalSettingsRequestAllowed(const platform::HttpRequest& request) {
  if (!IsLoopbackAddress(request.remote_address)) {
    return false;
  }
  const std::string origin = GetHeaderValue(request, "Origin");
  if (!IsLoopbackOrigin(origin)) {
    return false;
  }
  const std::string referer = GetHeaderValue(request, "Referer");
  return IsLoopbackOrigin(referer);
}

std::string FormatRedirectSamples(const std::vector<std::string>& allowed_redirects,
                                  std::size_t max_samples = 3) {
  std::ostringstream oss;
  oss << "[";
  for (std::size_t i = 0; i < allowed_redirects.size() && i < max_samples; ++i) {
    if (i > 0) oss << ", ";
    oss << allowed_redirects[i];
  }
  if (allowed_redirects.size() > max_samples) {
    oss << ", ...";
  }
  oss << "]";
  return oss.str();
}

class SessionManager {
 public:
  explicit SessionManager(std::chrono::seconds ttl) : ttl_(ttl) {}

  std::string Create(const std::string& user) {
    const auto id = GenerateTokenString(32);
    const auto expires = std::chrono::system_clock::now() + ttl_;
    std::lock_guard<std::mutex> lock(mutex_);
    sessions_[id] = {user, expires};
    return id;
  }

  std::optional<std::string> Validate(const std::string& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = sessions_.find(id);
    if (it == sessions_.end()) {
      return std::nullopt;
    }
    if (it->second.expires_at <= std::chrono::system_clock::now()) {
      sessions_.erase(it);
      return std::nullopt;
    }
    return it->second.user_id;
  }

 private:
  struct SessionInfo {
    std::string user_id;
    std::chrono::system_clock::time_point expires_at;
  };

  std::unordered_map<std::string, SessionInfo> sessions_;
  std::mutex mutex_;
  std::chrono::seconds ttl_;
};

class TokenRateLimiter {
 public:
  TokenRateLimiter(int max_requests, std::chrono::seconds window)
      : max_requests_(max_requests), window_(window) {}

  bool Allow(const std::string& key) {
    if (max_requests_ <= 0 || window_.count() <= 0) {
      return true;
    }
    const auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(mutex_);
    auto& bucket = buckets_[key];
    while (!bucket.empty() && (now - bucket.front()) > window_) {
      bucket.pop_front();
    }
    if (static_cast<int>(bucket.size()) >= max_requests_) {
      return false;
    }
    bucket.push_back(now);
    return true;
  }

 private:
  std::unordered_map<std::string, std::deque<std::chrono::steady_clock::time_point>> buckets_;
  std::mutex mutex_;
  int max_requests_;
  std::chrono::seconds window_;
};

constexpr char kScopeRead[] = "checklist:read";
constexpr char kScopeWrite[] = "checklist:write";

ChecklistSlug ParseCreatePayload(const json& payload) {
  if (!payload.is_object()) {
    throw std::invalid_argument("Payload must be a JSON object.");
  }
  ChecklistSlug slug;
  auto require_string = [&](const char* key) -> std::string {
    const auto it = payload.find(key);
    if (it == payload.end() || !it->is_string()) {
      throw std::invalid_argument(std::string{"Field '"} + key + "' is required and must be a string.");
    }
    return it->get<std::string>();
  };

  slug.checklist = require_string("checklist");
  slug.section = require_string("section");
  slug.procedure = require_string("procedure");
  slug.action = require_string("action");
  slug.spec = require_string("spec");
  slug.instructions = require_string("instructions");

    const std::string instance_principal =
        payload.contains("instance_principal") && payload["instance_principal"].is_string()
            ? payload["instance_principal"].get<std::string>()
            : "instance||default";
    slug.instance_principal = instance_principal;

    if (const auto it = payload.find("address_order"); it != payload.end() && it->is_number_integer()) {
      slug.address_order = it->get<std::int64_t>();
    }

  if (const auto it = payload.find("result"); it != payload.end() && it->is_string()) {
    slug.result = it->get<std::string>();
  }
  if (const auto it = payload.find("status"); it != payload.end() && it->is_string()) {
    slug.status = ParseStatus(it->get<std::string>());
  } else {
    slug.status = ChecklistStatus::kUnknown;
  }
  if (const auto it = payload.find("comment"); it != payload.end() && it->is_string()) {
    slug.comment = it->get<std::string>();
  }
    if (const auto it = payload.find("timestamp"); it != payload.end() && it->is_string()) {
      slug.timestamp = it->get<std::string>();
    }
    if (const auto it = payload.find("entity_id"); it != payload.end() && it->is_string()) {
      slug.entity_id = it->get<std::string>();
    }
    if (const auto it = payload.find("entity_principal"); it != payload.end() && it->is_string()) {
      slug.entity_principal = it->get<std::string>();
      if (slug.entity_id.empty()) {
        slug.entity_id = core::ComputeEntityId(slug.entity_principal);
      }
    }

  slug.slug_id = core::ComputeSlugId(slug.checklist, slug.section, slug.procedure, slug.action,
                                     slug.spec, slug.instructions);
  slug.instance_id = core::ComputeInstanceId(instance_principal);
  slug.address_id = core::ComposeAddressId(slug.slug_id, slug.instance_id);

  if (const auto it = payload.find("relationships"); it != payload.end() && it->is_array()) {
    for (const auto& rel : *it) {
      if (!rel.is_object()) {
        continue;
      }
      RelationshipEdge edge;
      edge.predicate = rel.value("predicate", "");
      edge.target = rel.value("target_address_id", "");
      if (edge.predicate.empty() || edge.target.empty()) {
        continue;
      }
      slug.relationships.push_back(edge);
    }
  }

  return slug;
}

platform::HttpResponse HandleCorsPreflight(const platform::HttpRequest&) {
  platform::HttpResponse response;
  response.status = 204;
  response.content_type = "text/plain";
  ApplyCors(response);
  return response;
}

}  // namespace

void ConfigureServer(platform::HttpServer& server, ChecklistStore& store, OAuthStore& oauth_store,
                     const ServerConfig& config) {
  const std::string base_url =
      config.base_url.empty()
          ? ("http://" + config.host + ":" + std::to_string(config.port))
          : config.base_url;
  const bool secure_cookies = IsHttpsUrl(base_url);
  const bool simple_authorize = config.simple_authorize;
  LogInfo(std::string{"Simple authorize mode: "} + (simple_authorize ? "enabled" : "disabled"));
  SetEntitySalt(config.entity_salt);

  OAuthClientConfig client_config;
  client_config.client_id = config.oauth_client_id;
  client_config.secret_hash = HashSecret(config.oauth_client_secret);
  client_config.redirect_uris = config.oauth_redirect_uris;
  client_config.allowed_scopes = config.oauth_scopes;
  LogInfo("OAuth client " + client_config.client_id + " redirects=" +
          std::to_string(client_config.redirect_uris.size()));
  oauth_store.UpsertClient(client_config);

  if (config.oauth_client_generated) {
    LogWarn("Generated dev OAuth client credentials: client_id=" + config.oauth_client_id +
            " client_secret=" + config.oauth_client_secret +
            " (set OAUTH_CLIENT_ID/OAUTH_CLIENT_SECRET to persist).");
  }
  if (config.admin_password_generated) {
    LogWarn("Generated dev admin password for /oauth/authorize login: user=" + config.admin_user +
            " password=" + config.admin_password_plain +
            " (set ADMIN_USER/ADMIN_PASSWORD to persist).");
  }

  auto sessions = std::make_shared<SessionManager>(std::chrono::minutes(30));
  auto token_limiter =
      std::make_shared<TokenRateLimiter>(config.token_rate_limit, config.token_rate_window);

  const AuthContext guest_context = MakeGuestContext(config);

  auto require_bearer =
      [&oauth_store, &config, guest_context](const platform::HttpRequest& request,
                                             const std::string& required_scope)
      -> std::optional<platform::HttpResponse> {
    SetAuthContext(guest_context);
    if (config.localhost_noauth && IsLoopbackAddress(request.remote_address)) {
      if (IsDebugEnabled()) {
        LogDebug("Bypassing auth for loopback request from " + request.remote_address);
      }
      return std::nullopt;
    }
    const std::string header = GetHeaderValue(request, "Authorization");
    if (header.empty()) {
      auto resp = ErrorResponse("UNAUTHORIZED", "Missing Authorization bearer token.", {}, 401);
      resp.headers["WWW-Authenticate"] = "Bearer";
      return resp;
    }
    const auto lower = ToLowerCopy(header);
    const std::string prefix = "bearer ";
    if (lower.rfind(prefix, 0) != 0 || header.size() <= prefix.size()) {
      auto resp = ErrorResponse("UNAUTHORIZED", "Authorization header must use Bearer.", {}, 401);
      resp.headers["WWW-Authenticate"] = "Bearer error=\"invalid_request\"";
      return resp;
    }
    const std::string token = header.substr(prefix.size());
    const auto meta = oauth_store.FindAccessToken(token);
    if (!meta) {
      auto resp = ErrorResponse("UNAUTHORIZED", "Invalid or expired access token.", {}, 401);
      resp.headers["WWW-Authenticate"] = "Bearer error=\"invalid_token\"";
      return resp;
    }
    if (!HasScope(meta->scope, required_scope)) {
      return ErrorResponse("FORBIDDEN", "Token missing required scope " + required_scope, {}, 403);
    }
    AuthContext ctx;
    ctx.client_id = meta->client_id;
    ctx.scope = meta->scope;
    ctx.provider = config.auth_provider;
    if (!meta->user_id.empty()) {
      ctx.kind = AuthKind::kUser;
      ctx.subject = meta->user_id;
    } else {
      ctx.kind = AuthKind::kService;
      ctx.subject = meta->client_id;
    }
    SetAuthContext(ctx);
    return std::nullopt;
  };

  auto with_auth = [&](platform::HttpMethod method, platform::HttpHandler handler) {
    const std::string required_scope =
        (method == platform::HttpMethod::kGet) ? kScopeRead : kScopeWrite;
    return [require_bearer, required_scope,
            handler = std::move(handler)](const platform::HttpRequest& request) {
      const auto auth_error = require_bearer(request, required_scope);
      if (auth_error) {
        return *auth_error;
      }
      return handler(request);
    };
  };

  auto add_authed = [&](platform::HttpMethod method, const std::string& path,
                        platform::HttpHandler handler) {
    server.AddHandler(method, path, with_auth(method, std::move(handler)));
  };

  const auto join_scopes = [](const std::vector<std::string>& scopes) {
    std::ostringstream oss;
    for (std::size_t i = 0; i < scopes.size(); ++i) {
      if (i > 0) oss << ' ';
      oss << scopes[i];
    }
    return oss.str();
  };

  auto gather_params = [&](const platform::HttpRequest& request) {
    std::unordered_map<std::string, std::string> params = ParseRequestParams(request);
    for (const auto& qp : request.query_params) {
      params.emplace(qp.first, qp.second);
    }
    return params;
  };

  auto resolve_entity = [&](const std::optional<std::string>& requested, EntityResolution& out)
      -> std::optional<platform::HttpResponse> {
    return ResolveEntityForRequest(requested, config, &out);
  };

  auto is_scope_subset = [&](const std::string& requested,
                             const std::vector<std::string>& allowed) {
    std::unordered_set<std::string> allowed_set(allowed.begin(), allowed.end());
    std::size_t start = 0;
    while (start <= requested.size()) {
      const auto space = requested.find(' ', start);
      const std::size_t end = (space == std::string::npos) ? requested.size() : space;
      const auto token = requested.substr(start, end - start);
      if (!token.empty() && allowed_set.find(token) == allowed_set.end()) {
        return false;
      }
      if (space == std::string::npos) break;
      start = space + 1;
    }
    return true;
  };

  struct AuthorizeRequest {
    std::string client_id;
    std::string redirect_uri;
    std::string scope;
    std::string state;
  };

  auto validate_authorize = [&, base_url](const std::unordered_map<std::string, std::string>& params,
                                          const std::string& request_host,
                                          const std::string& forwarded_proto)
      -> std::pair<std::optional<AuthorizeRequest>, std::optional<platform::HttpResponse>> {
    auto fail = [&](const std::string& code, const std::string& message, int status = 400,
                    bool logged = false) {
      if (!logged) {
        LogWarn("OAuth authorize rejected code=" + code + " message=" + message);
      }
      return std::make_pair(std::optional<AuthorizeRequest>{},
                            std::optional<platform::HttpResponse>{ErrorResponse(code, message, {}, status)});
    };

    const auto response_type_it = params.find("response_type");
    const std::string response_type =
        (response_type_it != params.end()) ? response_type_it->second : "";
    if (response_type != "code") {
      return fail("INVALID_REQUEST", "response_type must be 'code'");
    }

    const auto client_it = params.find("client_id");
    if (client_it == params.end() || client_it->second.empty()) {
      return fail("INVALID_CLIENT", "client_id is required", 400);
    }
    const std::string client_id = client_it->second;
    if (client_id.size() > kMaxAuthorizeParamLength) {
      return fail("INVALID_CLIENT", "client_id is too long", 400);
    }
    if (IsDebugEnabled()) {
      LogDebug("Authorize validating client_id=" + client_id);
    }
    const auto client = oauth_store.GetClient(client_id);
    if (!client) {
      return fail("INVALID_CLIENT", "Unknown client_id", 400);
    }
    if (IsDebugEnabled()) {
      LogDebug("Authorize validation client=" + client->client_id + " redirects=" +
               std::to_string(client->redirect_uris.size()) + " scopes=" +
               std::to_string(client->allowed_scopes.size()));
    }

    const auto redirect_it = params.find("redirect_uri");
    if (redirect_it == params.end() || redirect_it->second.empty()) {
      return fail("INVALID_REQUEST", "redirect_uri is required", 400);
    }
    const std::string redirect_uri = redirect_it->second;
    if (redirect_uri.size() > kMaxAuthorizeParamLength) {
      return fail("INVALID_REQUEST", "redirect_uri is too long", 400);
    }
    const auto parsed_redirect = ParseUrlParts(redirect_uri);
    if (!parsed_redirect) {
      return fail("INVALID_REDIRECT", "redirect_uri must be an absolute http(s) URL", 400);
    }
    if (IsDebugEnabled()) {
      LogDebug("Authorize requested redirect=" + redirect_uri);
    }
    const std::size_t base_length = base_url.size();
    if (base_length > kMaxAuthorizeParamLength) {
      LogError("Authorize base_url length=" + std::to_string(base_length) + " exceeds limit");
      return fail("INVALID_REDIRECT", "Server base_url is invalid.", 500);
    }
    if (IsDebugEnabled()) {
      LogDebug("Authorize base_url length=" + std::to_string(base_length));
    }
    if (IsDebugEnabled()) {
      const std::size_t client_first_len =
          client->redirect_uris.empty() ? 0 : client->redirect_uris.front().size();
      LogDebug("Authorize client redirect_uris size=" + std::to_string(client->redirect_uris.size()) +
               " first_length=" + std::to_string(client_first_len));
    }

    if (IsDebugEnabled()) {
      LogDebug("Authorize preparing allowed redirect list");
    }
    std::vector<std::string> allowed_redirects = client->redirect_uris;
    if (IsDebugEnabled()) {
      LogDebug("Authorize initial allowed redirect copy size=" +
               std::to_string(allowed_redirects.size()));
    }
    if (config.serve_ui) {
      const std::string ui_redirect = base_url + "/ui/oauth_callback.html";
      if (IsDebugEnabled()) {
        LogDebug("Authorize ui redirect candidate length=" + std::to_string(ui_redirect.size()) +
                 " value=" + ui_redirect);
      }
      const bool already_allowed =
          std::find(allowed_redirects.begin(), allowed_redirects.end(), ui_redirect) !=
          allowed_redirects.end();
      if (IsDebugEnabled()) {
        LogDebug(std::string{"Authorize ui redirect already_present="} + (already_allowed ? "true" : "false"));
      }
      if (!already_allowed) {
        allowed_redirects.push_back(ui_redirect);
      }
      if (IsDebugEnabled()) {
        LogDebug("Authorize allowlist after adding ui redirect size=" +
                 std::to_string(allowed_redirects.size()));
      }
      if (!request_host.empty()) {
        const auto base_parts = ParseUrlParts(base_url);
        std::string scheme = base_parts ? base_parts->scheme : std::string{"http"};
        if (!forwarded_proto.empty()) {
          scheme = ToLowerCopy(forwarded_proto);
        }
        std::string host_only = request_host;
        const auto colon_pos = request_host.find(':');
        if (colon_pos != std::string::npos) {
          host_only = request_host.substr(0, colon_pos);
        }
        std::string port_part;
        if (colon_pos != std::string::npos) {
          port_part = request_host.substr(colon_pos + 1);
        }
        const std::string normalized_host = NormalizeLoopbackHost(host_only);
        const std::string normalized_port = NormalizePort(scheme, port_part);
        std::string host_with_port = normalized_host;
        if (!normalized_port.empty() &&
            !((scheme == "http" && normalized_port == "80") ||
              (scheme == "https" && normalized_port == "443"))) {
          host_with_port += ":" + normalized_port;
        }
        const std::string request_ui_redirect =
            scheme + "://" + host_with_port + "/ui/oauth_callback.html";
        if (std::find(allowed_redirects.begin(), allowed_redirects.end(), request_ui_redirect) ==
            allowed_redirects.end()) {
          allowed_redirects.push_back(request_ui_redirect);
          if (IsDebugEnabled()) {
            LogDebug("Authorize added request host ui redirect: " + request_ui_redirect);
          }
        }
      }
    }
    if (IsDebugEnabled()) {
      const std::size_t first_len = allowed_redirects.empty() ? 0 : allowed_redirects.front().size();
      LogDebug("Authorize allowed redirects count=" + std::to_string(allowed_redirects.size()) +
               " first_length=" + std::to_string(first_len));
    }

    const std::string allowed_samples = FormatRedirectSamples(allowed_redirects);
    if (IsDebugEnabled()) {
      LogDebug("Authorize allowed redirect samples ready");
    }
    bool redirect_allowed = false;
    std::string matched_redirect;
    for (const auto& allowed : allowed_redirects) {
      if (RedirectUrisMatch(redirect_uri, allowed)) {
        redirect_allowed = true;
        matched_redirect = allowed;
        break;
      }
    }
    if (!redirect_allowed) {
      LogWarn("OAuth authorize redirect denied client_id=" + client_id +
              " redirect_uri=" + redirect_uri +
              " allowed_count=" + std::to_string(allowed_redirects.size()) +
              " samples=" + allowed_samples);
      return fail("INVALID_REDIRECT", "redirect_uri is not allowlisted", 400, true);
    }
    if (!IsHttpsUrl(redirect_uri) && !IsLoopbackRedirect(redirect_uri)) {
      LogWarn("OAuth authorize redirect denied client_id=" + client_id +
              " redirect_uri=" + redirect_uri + " reason=insecure_non_loopback" +
              " allowed_count=" + std::to_string(allowed_redirects.size()) +
              " samples=" + allowed_samples);
      return fail("INVALID_REDIRECT", "redirect_uri must use https unless localhost", 400, true);
    }
    LogInfo("OAuth authorize redirect accepted client_id=" + client_id +
            " redirect_uri=" + redirect_uri + " matched=" + matched_redirect +
            " allowed_count=" + std::to_string(allowed_redirects.size()) +
            " samples=" + allowed_samples);

    std::string scope = params.count("scope") ? params.at("scope") : "";
    if (scope.size() > kMaxAuthorizeParamLength) {
      return fail("INVALID_SCOPE", "Requested scopes are too long.", 400);
    }
    if (scope.empty()) {
      scope = join_scopes(client->allowed_scopes);
    }
    if (!is_scope_subset(scope, client->allowed_scopes)) {
      return fail("INVALID_SCOPE", "Requested scopes are not allowed for this client.", 400);
    }
    if (IsDebugEnabled()) {
      LogDebug("Authorize scope accepted: " + scope);
    }

    const std::string state = params.count("state") ? params.at("state") : "";
    if (state.empty()) {
      return fail("INVALID_REQUEST", "state is required", 400);
    }
    if (state.size() > kMaxAuthorizeParamLength) {
      return fail("INVALID_REQUEST", "state is too long", 400);
    }

    AuthorizeRequest auth{client_id, redirect_uri, scope, state};
    LogInfo("OAuth authorize validated client_id=" + auth.client_id + " redirect=" + auth.redirect_uri +
            " scope=" + auth.scope);
    return {auth, std::nullopt};
  };

  auto render_login_page = [&](const AuthorizeRequest& auth, const std::string& message) {
    std::ostringstream page;
    page << "<!doctype html><html lang=\"en\"><head><meta charset=\"UTF-8\">";
    page << "<title>Checklist Assistant OAuth</title>";
    page << "<link rel=\"stylesheet\" href=\"/ui/theme.css\">";
    page << "<style>";
    page << "body{margin:0;min-height:100vh;display:flex;align-items:center;justify-content:center;";
    page << "padding:24px;background:var(--page-gradient);color:var(--text);font-family:var(--font-sans);}";
    page << ".card{width:min(520px,100%);background:var(--panel);border:1px solid var(--border);";
    page << "border-radius:16px;padding:20px;box-shadow:var(--shadow);}";
    page << "h2{margin:0 0 12px 0;font-size:20px;}";
    page << ".meta{color:var(--muted);font-size:12px;margin-bottom:12px;}";
    page << ".error{color:var(--bad);margin:6px 0 10px 0;}";
    page << "label{display:block;font-size:13px;color:var(--muted);margin:10px 0 4px;}";
    page << "input{width:100%;padding:10px;border:1px solid var(--border);border-radius:10px;";
    page << "background:var(--field-bg);color:var(--text);font-family:inherit;}";
    page << "button{margin-top:14px;padding:10px 14px;border:none;border-radius:10px;";
    page << "background:var(--accent);color:var(--accent-ink);font-weight:700;cursor:pointer;}";
    page << "</style></head><body><div class=\"card\">";
    page << "<h2>Checklist Assistant OAuth</h2>";
    if (!message.empty()) {
      page << "<div class=\"error\">" << HtmlEscape(message) << "</div>";
    }
    page << "<div class=\"meta\">Client: " << HtmlEscape(auth.client_id) << "</div>";
    page << "<form method=\"post\" action=\"/oauth/authorize\">";
    page << "<input type=\"hidden\" name=\"response_type\" value=\"code\"/>";
    page << "<input type=\"hidden\" name=\"client_id\" value=\"" << HtmlEscape(auth.client_id) << "\"/>";
    page << "<input type=\"hidden\" name=\"redirect_uri\" value=\"" << HtmlEscape(auth.redirect_uri) << "\"/>";
    page << "<input type=\"hidden\" name=\"scope\" value=\"" << HtmlEscape(auth.scope) << "\"/>";
    page << "<input type=\"hidden\" name=\"state\" value=\"" << HtmlEscape(auth.state) << "\"/>";
    page << "<label>Username <input name=\"username\" autocomplete=\"username\" /></label><br/>";
    page << "<label>Password <input type=\"password\" name=\"password\" autocomplete=\"current-password\" /></label><br/>";
    page << "<button type=\"submit\" name=\"action\" value=\"login\">Login</button>";
    page << "</form>";
    page << "</div></body></html>";
    platform::HttpResponse response;
    response.status = 200;
    response.content_type = "text/html; charset=utf-8";
    response.body = page.str();
    response.headers["Connection"] = "close";
    ApplyCors(response);
    return response;
  };

  auto render_consent_page = [&](const AuthorizeRequest& auth, const std::string& user) {
    std::ostringstream page;
    page << "<!doctype html><html lang=\"en\"><head><meta charset=\"UTF-8\">";
    page << "<title>Authorize access</title>";
    page << "<link rel=\"stylesheet\" href=\"/ui/theme.css\">";
    page << "<style>";
    page << "body{margin:0;min-height:100vh;display:flex;align-items:center;justify-content:center;";
    page << "padding:24px;background:var(--page-gradient);color:var(--text);font-family:var(--font-sans);}";
    page << ".card{width:min(560px,100%);background:var(--panel);border:1px solid var(--border);";
    page << "border-radius:16px;padding:20px;box-shadow:var(--shadow);}";
    page << "h2{margin:0 0 12px 0;font-size:20px;}";
    page << ".meta{color:var(--muted);font-size:12px;margin:0 0 8px 0;}";
    page << ".warning{color:var(--warn);font-size:12px;margin:6px 0 10px 0;}";
    page << ".actions{display:flex;gap:10px;flex-wrap:wrap;margin-top:14px;}";
    page << "button{padding:10px 14px;border:none;border-radius:10px;";
    page << "background:var(--accent);color:var(--accent-ink);font-weight:700;cursor:pointer;}";
    page << "button.secondary{background:var(--accent-soft);color:var(--text);border:1px solid var(--border);}";
    page << "</style></head><body><div class=\"card\">";
    page << "<h2>Authorize access</h2>";
    page << "<div class=\"meta\">Signed in as " << HtmlEscape(user) << "</div>";
    page << "<div class=\"meta\">Client: " << HtmlEscape(auth.client_id) << "</div>";
    page << "<div class=\"meta\">Requested scopes: " << HtmlEscape(auth.scope) << "</div>";
    if (HasScope(auth.scope, kScopeWrite)) {
      page << "<div class=\"warning\"><strong>Write access</strong> to checklists will be granted.</div>";
    }
    page << "<form method=\"post\" action=\"/oauth/authorize\">";
    page << "<input type=\"hidden\" name=\"response_type\" value=\"code\"/>";
    page << "<input type=\"hidden\" name=\"client_id\" value=\"" << HtmlEscape(auth.client_id) << "\"/>";
    page << "<input type=\"hidden\" name=\"redirect_uri\" value=\"" << HtmlEscape(auth.redirect_uri) << "\"/>";
    page << "<input type=\"hidden\" name=\"scope\" value=\"" << HtmlEscape(auth.scope) << "\"/>";
    page << "<input type=\"hidden\" name=\"state\" value=\"" << HtmlEscape(auth.state) << "\"/>";
    page << "<div class=\"actions\">";
    page << "<button type=\"submit\" name=\"action\" value=\"approve\">Allow</button>";
    page << "<button class=\"secondary\" type=\"submit\" name=\"action\" value=\"deny\">Deny</button>";
    page << "</div>";
    page << "</form>";
    page << "</div></body></html>";
    platform::HttpResponse response;
    response.status = 200;
    response.content_type = "text/html; charset=utf-8";
    response.body = page.str();
    response.headers["Connection"] = "close";
    ApplyCors(response);
    return response;
  };

  auto handle_authorize = [sessions, gather_params, validate_authorize, render_login_page,
                           render_consent_page, simple_authorize, &oauth_store, &config](
                              const platform::HttpRequest& request) {
    static std::mutex authorize_mutex;
    try {
      std::lock_guard<std::mutex> lock(authorize_mutex);
      LogDebug(std::string{"Handling GET /oauth/authorize simple="} +
               (simple_authorize ? "true" : "false"));
      if (simple_authorize) {
        const auto params = gather_params(request);
        const std::string request_host = GetHeaderValue(request, "Host");
        const std::string forwarded_proto = GetHeaderValue(request, "X-Forwarded-Proto");
        const auto validation = validate_authorize(params, request_host, forwarded_proto);
        if (validation.second) {
          auto resp = *validation.second;
          resp.headers["Connection"] = "close";
          return resp;
        }
        const auto auth = *validation.first;
        const auto code = oauth_store.IssueAuthorizationCode(auth.client_id, config.admin_user,
                                                             auth.redirect_uri, auth.scope,
                                                             auth.state, config.auth_code_ttl);
        LogInfo("OAuth simple authorize issued code client_id=" + auth.client_id +
                " state=" + auth.state);
        const std::string redirect =
            auth.redirect_uri + "?code=" + UrlEncode(code) + "&state=" + UrlEncode(auth.state);
        platform::HttpResponse resp;
        resp.status = 302;
        resp.content_type = "text/plain";
        resp.body = "Redirecting";
        resp.headers["Location"] = redirect;
        resp.headers["Connection"] = "close";
        ApplyCors(resp);
        return resp;
      }
      const auto params = gather_params(request);
      const std::string request_host = GetHeaderValue(request, "Host");
      const std::string forwarded_proto = GetHeaderValue(request, "X-Forwarded-Proto");
      LogDebug("Authorize params parsed");
      const auto validation = validate_authorize(params, request_host, forwarded_proto);
      if (validation.second) {
        LogDebug("Authorize validation failed, returning error");
        auto resp = *validation.second;
        resp.headers["Connection"] = "close";
        return resp;
      }
      const auto auth = *validation.first;
      const auto cookies = ParseCookies(request);
      const auto session_it = cookies.find("chax_session");
      const auto user =
          (session_it != cookies.end()) ? sessions->Validate(session_it->second) : std::nullopt;
      if (!user) {
        LogDebug("Authorize returning login page");
        return render_login_page(auth, "");
      }
      LogDebug("Authorize returning consent page");
      return render_consent_page(auth, *user);
    } catch (const std::exception& ex) {
      LogError("GET /oauth/authorize exception: " + std::string(ex.what()) +
               " client_id=" + GetQueryParam(request, "client_id", "") +
               " redirect_uri=" + GetQueryParam(request, "redirect_uri", "") +
               " query_params=" + std::to_string(request.query_params.size()) +
               " body_bytes=" + std::to_string(request.body.size()));
      return ErrorResponse("INTERNAL_ERROR", "Authorization request failed.", {}, 500);
    } catch (...) {
      LogError("GET /oauth/authorize unknown exception query_params=" +
               std::to_string(request.query_params.size()) + " body_bytes=" +
               std::to_string(request.body.size()));
      return ErrorResponse("INTERNAL_ERROR", "Authorization request failed.", {}, 500);
    }
  };

  auto handle_authorize_post =
      [sessions, gather_params, validate_authorize, render_login_page, render_consent_page,
       &config, secure_cookies, simple_authorize, &oauth_store](
          const platform::HttpRequest& request) {
    static std::mutex authorize_mutex;
    try {
      std::lock_guard<std::mutex> lock(authorize_mutex);
      LogDebug("Handling POST /oauth/authorize");
      if (simple_authorize) {
        const auto params = gather_params(request);
        const std::string request_host = GetHeaderValue(request, "Host");
        const std::string forwarded_proto = GetHeaderValue(request, "X-Forwarded-Proto");
        const auto validation = validate_authorize(params, request_host, forwarded_proto);
        if (validation.second) {
          auto resp = *validation.second;
          resp.headers["Connection"] = "close";
          return resp;
        }
        const auto auth = *validation.first;
        const auto code = oauth_store.IssueAuthorizationCode(auth.client_id, config.admin_user,
                                                             auth.redirect_uri, auth.scope,
                                                             auth.state, config.auth_code_ttl);
        LogInfo("OAuth simple authorize issued code client_id=" + auth.client_id +
                " state=" + auth.state);
        const std::string redirect =
            auth.redirect_uri + "?code=" + UrlEncode(code) + "&state=" + UrlEncode(auth.state);
        platform::HttpResponse resp;
        resp.status = 302;
        resp.content_type = "text/plain";
        resp.body = "Redirecting";
        resp.headers["Location"] = redirect;
        resp.headers["Connection"] = "close";
        ApplyCors(resp);
        return resp;
      }
      const auto params = gather_params(request);
      const std::string request_host = GetHeaderValue(request, "Host");
      const std::string forwarded_proto = GetHeaderValue(request, "X-Forwarded-Proto");
      LogDebug("Authorize POST params parsed");
      const auto validation = validate_authorize(params, request_host, forwarded_proto);
      if (validation.second) {
        LogDebug("Authorize POST validation failed");
        auto resp = *validation.second;
        resp.headers["Connection"] = "close";
        return resp;
      }
      const auto auth = *validation.first;
      const std::string action = params.count("action") ? params.at("action") : "";

      if (action == "login") {
        const std::string username = params.count("username") ? params.at("username") : "";
        const std::string password = params.count("password") ? params.at("password") : "";
        if (username != config.admin_user ||
            !ConstantTimeEquals(HashSecret(password), config.admin_password_hash)) {
          return render_login_page(auth, "Invalid credentials.");
        }
        const std::string session_id = sessions->Create(username);
        auto response = render_consent_page(auth, username);
        std::string cookie = "chax_session=" + session_id + "; Path=/; HttpOnly; SameSite=Lax";
        if (secure_cookies) {
          cookie += "; Secure";
        }
        response.headers["Set-Cookie"] = cookie;
        LogDebug("Authorize POST login accepted, returning consent");
        return response;
      }

      const auto cookies = ParseCookies(request);
      const auto session_it = cookies.find("chax_session");
      const auto user =
          (session_it != cookies.end()) ? sessions->Validate(session_it->second) : std::nullopt;
      if (!user) {
        LogDebug("Authorize POST missing session, returning login");
        return render_login_page(auth, "Session expired. Please sign in again.");
      }

      if (action == "approve") {
        const auto code = oauth_store.IssueAuthorizationCode(auth.client_id, *user, auth.redirect_uri,
                                                             auth.scope, auth.state,
                                                             config.auth_code_ttl);
        LogInfo("OAuth authorization code issued client_id=" + auth.client_id +
                " state=" + auth.state);
        platform::HttpResponse response;
        response.status = 302;
        response.headers["Location"] =
            auth.redirect_uri + "?code=" + UrlEncode(code) + "&state=" + UrlEncode(auth.state);
        response.headers["Connection"] = "close";
        ApplyCors(response);
        LogDebug("Authorize POST approve redirecting");
        return response;
      }

      if (action == "deny") {
        platform::HttpResponse response;
        response.status = 302;
        response.headers["Location"] =
            auth.redirect_uri + "?error=access_denied&state=" + UrlEncode(auth.state);
        response.headers["Connection"] = "close";
        ApplyCors(response);
        LogDebug("Authorize POST deny redirecting");
        return response;
      }

      LogDebug("Authorize POST unsupported action");
      return ErrorResponse("INVALID_REQUEST", "Unsupported authorize action.", {}, 400);
    } catch (const std::exception& ex) {
      LogError("POST /oauth/authorize exception: " + std::string(ex.what()) +
               " client_id=" + GetQueryParam(request, "client_id", "") +
               " query_params=" + std::to_string(request.query_params.size()) +
               " body_bytes=" + std::to_string(request.body.size()));
      return ErrorResponse("INTERNAL_ERROR", "Authorization request failed.", {}, 500);
    } catch (...) {
      LogError("POST /oauth/authorize unknown exception query_params=" +
               std::to_string(request.query_params.size()) + " body_bytes=" +
               std::to_string(request.body.size()));
      return ErrorResponse("INTERNAL_ERROR", "Authorization request failed.", {}, 500);
    }
  };

  auto handle_token = [gather_params, &config, &oauth_store, token_limiter](
                          const platform::HttpRequest& request) {
    const auto params = gather_params(request);
    const std::string grant_type = params.count("grant_type") ? params.at("grant_type") : "";
    if (grant_type.empty()) {
      return ErrorResponse("invalid_request", "grant_type is required.", {}, 400);
    }

    const std::string client_id = params.count("client_id") ? params.at("client_id") : "";
    const std::string client_secret =
        params.count("client_secret") ? params.at("client_secret") : "";
    if (client_id.empty() || client_secret.empty()) {
      auto resp = ErrorResponse("invalid_client", "client_id and client_secret are required.", {}, 401);
      resp.headers["WWW-Authenticate"] = "Bearer error=\"invalid_client\"";
      return resp;
    }

    if (!token_limiter->Allow(client_id)) {
      auto resp = ErrorResponse("RATE_LIMITED", "Too many token requests for this client.", {}, 429);
      resp.headers["Retry-After"] = std::to_string(config.token_rate_window.count());
      return resp;
    }

    const auto client = oauth_store.GetClient(client_id);
    if (!client) {
      auto resp = ErrorResponse("invalid_client", "Unknown client_id.", {}, 401);
      resp.headers["WWW-Authenticate"] = "Bearer error=\"invalid_client\"";
      return resp;
    }
    if (!ConstantTimeEquals(HashSecret(client_secret), client->secret_hash)) {
      auto resp = ErrorResponse("invalid_client", "Invalid client credentials.", {}, 401);
      resp.headers["WWW-Authenticate"] = "Bearer error=\"invalid_client\"";
      return resp;
    }

    if (grant_type == "authorization_code") {
      const std::string code = params.count("code") ? params.at("code") : "";
      const std::string redirect_uri =
          params.count("redirect_uri") ? params.at("redirect_uri") : "";
      if (code.empty() || redirect_uri.empty()) {
        return ErrorResponse("invalid_request", "code and redirect_uri are required.", {}, 400);
      }
      const auto auth = oauth_store.ConsumeAuthorizationCode(code, client_id, redirect_uri);
      if (!auth) {
        return ErrorResponse("invalid_grant", "Authorization code is invalid or expired.", {}, 400);
      }
      const auto tokens =
          oauth_store.IssueTokens(client_id, auth->user_id, auth->scope, config.access_token_ttl,
                                  config.issue_refresh_tokens ? std::optional{config.refresh_token_ttl}
                                                               : std::optional<std::chrono::seconds>{});
      LogInfo("OAuth token issued via authorization_code client_id=" + client_id +
              " scope=" + tokens.scope);
      json body{{"access_token", tokens.access_token},
                {"token_type", "bearer"},
                {"expires_in", tokens.access_expires_in.count()},
                {"scope", tokens.scope}};
      if (tokens.refresh_token) {
        body["refresh_token"] = *tokens.refresh_token;
      }
      return JsonResponse(body);
    }

    if (grant_type == "refresh_token") {
      const std::string refresh = params.count("refresh_token") ? params.at("refresh_token") : "";
      if (refresh.empty()) {
        return ErrorResponse("invalid_request", "refresh_token is required.", {}, 400);
      }
      const auto rotated =
          oauth_store.RotateRefreshToken(refresh, config.access_token_ttl,
                                         config.issue_refresh_tokens
                                             ? std::optional{config.refresh_token_ttl}
                                             : std::optional<std::chrono::seconds>{});
      if (!rotated) {
        return ErrorResponse("invalid_grant", "Refresh token is invalid or expired.", {}, 400);
      }
      LogInfo("OAuth token refreshed client_id=" + client_id + " scope=" + rotated->scope);
      json body{{"access_token", rotated->access_token},
                {"token_type", "bearer"},
                {"expires_in", rotated->access_expires_in.count()},
                {"scope", rotated->scope}};
      if (rotated->refresh_token) {
        body["refresh_token"] = *rotated->refresh_token;
      }
      return JsonResponse(body);
    }

    return ErrorResponse("unsupported_grant_type", "Unsupported grant_type.", {}, 400);
  };

  auto handle_oauth_callback = [](const platform::HttpRequest&) {
    platform::HttpResponse response;
    response.status = 200;
    response.content_type = "text/html; charset=utf-8";
    response.body =
        "<!doctype html><html lang=\"en\"><head><meta charset=\"UTF-8\">"
        "<title>OAuth Complete</title></head>"
        "<body><p>OAuth flow complete. You may close this tab.</p></body></html>";
    response.headers["Connection"] = "close";
    ApplyCors(response);
    return response;
  };

  auto handle_favicon = [](const platform::HttpRequest&) {
    platform::HttpResponse response;
    response.status = 204;
    response.content_type = "image/x-icon";
    response.headers["Connection"] = "close";
    ApplyCors(response);
    return response;
  };

  auto handle_commands = [](const platform::HttpRequest&) {
    json commands = json::array();
    for (const auto& cmd : kCommandCatalog) {
      commands.push_back(
          {{"method", cmd.method}, {"path", cmd.path}, {"description", cmd.description}});
    }
    LogInfo("GET /api/v1/commands");
    return OkResponse(json{{"commands", commands}});
  };

  auto handle_health = [&store](const platform::HttpRequest&) {
    const auto now = std::chrono::steady_clock::now();
    const auto uptime_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(now - kServerStart).count();
    json payload{{"status", "ok"},
                 {"uptime_ms", uptime_ms},
                 {"version", "0.2.0"},
                 {"checklists", store.ListChecklists()}};
    LogInfo("GET /api/v1/health");
    return OkResponse(payload);
  };

  auto handle_shutdown = [&server, &config](const platform::HttpRequest& request) {
    if (config.shutdown_token.empty()) {
      return ErrorResponse("FORBIDDEN", "Shutdown token not configured.", {}, 403);
    }
    if (!IsLoopbackAddress(request.remote_address)) {
      return ErrorResponse("FORBIDDEN", "Shutdown allowed from loopback only.", {}, 403);
    }
    const auto token = GetHeaderValue(request, "X-CHAX-SHUTDOWN-TOKEN");
    if (token != config.shutdown_token) {
      return ErrorResponse("FORBIDDEN", "Invalid shutdown token.", {}, 403);
    }
    LogWarn("Shutdown requested from " + request.remote_address);
    std::thread([&server]() {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      server.Stop();
    }).detach();
    return OkResponse(json{{"stopping", true}});
  };

  auto handle_me = [&config](const platform::HttpRequest&) {
    const AuthContext ctx = CurrentAuthContext();
    EntityResolution resolution;
    ResolveEntityForRequest(std::nullopt, config, &resolution);
    std::vector<std::string> scopes;
    {
      std::istringstream iss(ctx.scope);
      std::string token;
      while (iss >> token) {
        scopes.push_back(token);
      }
    }
    json auth = {{"provider", config.auth_provider}, {"scopes", scopes}};
    if (ctx.kind == AuthKind::kUser) {
      auth["username"] = ctx.subject;
      auth["kind"] = "user";
    } else if (ctx.kind == AuthKind::kService) {
      auth["service"] = ctx.subject;
      auth["kind"] = "service";
    } else {
      auth["kind"] = "guest";
    }
    return OkResponse(json{{"auth", auth},
                           {"entity",
                            {{"entity_id", resolution.entity_id},
                             {"entity_principal", resolution.principal}}}});
  };

  auto handle_hello = [](const platform::HttpRequest& request) {
    const std::string name = GetQueryParam(request, "name", "world");
    LogInfo("GET /api/v1/hello name=" + name);
    const std::string message = "Hello, " + name + "!";
    return OkResponse(json{{"message", message}});
  };

  auto handle_echo = [](const platform::HttpRequest& request) {
    LogInfo("POST /api/v1/echo bytes=" + std::to_string(request.body.size()));
    return OkResponse(json{{"received", request.body}});
  };

  auto handle_checklists = [&store](const platform::HttpRequest&) {
    const auto names = store.ListChecklists();
    LogInfo("GET /api/v1/checklists");
    return OkResponse(json{{"checklists", names}});
  };

  auto handle_delete_checklist = [&store](const platform::HttpRequest& request) {
    const std::string checklist =
        request.path_params.size() > 0 ? request.path_params[0] : "";
    LogInfo("DELETE /api/v1/checklists/" + checklist);
    if (checklist.empty()) {
      return ErrorResponse("INVALID_FIELD", "Checklist name is required.", {}, 400);
    }
    try {
      int removed = 0;
      store.DeleteChecklist(checklist, &removed);
      return OkResponse(json{{"checklist", checklist}, {"deleted", removed}});
    } catch (const std::exception& ex) {
      return ErrorResponse("INTERNAL_ERROR", ex.what(), {}, 500);
    }
  };

  auto handle_delete_instance = [&store](const platform::HttpRequest& request) {
    const std::string checklist =
        request.path_params.size() > 0 ? request.path_params[0] : "";
    const std::string instance_id =
        request.path_params.size() > 1 ? request.path_params[1] : "";
    LogInfo("DELETE /api/v1/checklists/" + checklist + "/instances/" + instance_id);
    if (checklist.empty() || instance_id.empty()) {
      return ErrorResponse("INVALID_FIELD", "Checklist and instance_id are required.", {}, 400);
    }
    try {
      int removed = 0;
      store.DeleteInstance(checklist, instance_id, &removed);
      return OkResponse(json{{"checklist", checklist}, {"instance_id", instance_id}, {"deleted", removed}});
    } catch (const std::exception& ex) {
      return ErrorResponse("INTERNAL_ERROR", ex.what(), {}, 500);
    }
  };

  auto handle_create_slug = [&store, resolve_entity, &config](const platform::HttpRequest& request) {
    const auto payload = json::parse(request.body, nullptr, false);
    if (payload.is_discarded()) {
      return ErrorResponse("INVALID_JSON", "Invalid JSON payload.", {}, 400);
    }
    try {
      auto slug = ParseCreatePayload(payload);
      EntityResolution resolution;
      const auto requested = slug.entity_principal.empty()
                                 ? std::optional<std::string>{}
                                 : std::make_optional(slug.entity_principal);
      if (const auto err = resolve_entity(requested, resolution)) {
        return *err;
      }
      slug.entity_principal = resolution.principal;
      slug.entity_id = resolution.entity_id;
      store.UpsertSlug(slug);
      if (payload.contains("pack") && payload["pack"].is_string() && !payload.value("pack", "").empty()) {
        core::ChecklistOwnership ownership;
        ownership.source_name = payload.value("source_name", "");
        ownership.source_path = payload.value("source_path", "");
        ownership.pack = payload.value("pack", "");
        ownership.checklist_dir = payload.value("checklist_dir", slug.checklist);
        ownership.checklist = slug.checklist;
        store.UpsertOwnership(slug, ownership);
      }
      LogInfo("POST /api/v1/slugs address_id=" + slug.address_id);
      return OkResponse(SlugToJson(store.GetSlugOrThrow(slug.address_id)), 201);
    } catch (const std::exception& ex) {
      return ErrorResponse("INVALID_FIELD", ex.what(), {}, 400);
    }
  };

  auto handle_slug = [&store](const platform::HttpRequest& request) {
    if (request.path_params.empty()) {
      return ErrorResponse("INVALID_FIELD", "Missing address_id path parameter.", {}, 400);
    }
    const std::string address_id = request.path_params.front();
    LogInfo("GET /api/v1/slugs/" + address_id);
    try {
      const auto slug = store.GetSlugOrThrow(address_id);
      return OkResponse(SlugToJson(slug));
    } catch (const std::exception& ex) {
      return ErrorResponse("NOT_FOUND", ex.what(), json{{"address_id", address_id}}, 404);
    }
  };

  auto handle_checklist = [&store](const platform::HttpRequest& request) {
    if (request.path_params.empty()) {
      return ErrorResponse("INVALID_FIELD", "Missing checklist path parameter.", {}, 400);
    }
    const std::string checklist = request.path_params.front();
    const std::string source_name = GetQueryParam(request, "source_name", "");
    const std::string pack = GetQueryParam(request, "pack", "");
    const std::string checklist_dir = GetQueryParam(request, "checklist_dir", "");
    LogInfo("GET /api/v1/slugs?checklist=" + checklist);
    std::optional<core::ChecklistOwnership> ownership_filter;
    if (!source_name.empty() || !pack.empty() || !checklist_dir.empty()) {
      core::ChecklistOwnership ownership;
      ownership.source_name = source_name;
      ownership.pack = pack;
      ownership.checklist_dir = checklist_dir;
      ownership.checklist = checklist;
      ownership_filter = ownership;
    }
    const auto slugs = store.GetSlugsForChecklist(checklist, ownership_filter);
    json payload = json::array();
    for (const auto& slug : slugs) {
      payload.push_back(SlugToJson(slug));
    }
    return OkResponse(json{{"items", payload}});
  };

  auto handle_relationships = [&store](const platform::HttpRequest& request) {
    if (request.path_params.empty()) {
      return ErrorResponse("INVALID_FIELD", "Missing address_id path parameter.", {}, 400);
    }
    const std::string address_id = request.path_params.front();
    LogInfo("GET /api/v1/relationships/address/" + address_id);
    const auto graph = store.GetRelationships(address_id);
    json payload = RelationshipsToJson(graph);
    payload["address_id"] = address_id;
    if (HasPrefillRelationships(graph)) {
      if (const auto status = store.GetPrefillDatasetStatus(address_id)) {
        payload["prefill_dataset"] = PrefillDatasetStatusToJson(*status);
      }
    }
    return OkResponse(payload);
  };

  auto handle_relationships_template_create = [&store](const platform::HttpRequest& request) {
    json warnings = json::array();
    try {
      const auto body = json::parse(request.body);
      const std::string subject = body.value("subject_slug_id", "");
      std::string predicate;
      const std::string target = body.value("target_slug_id", "");
      if (!IsValidBase32Id(subject, 16) || !IsValidBase32Id(target, 16)) {
        return ErrorResponse("INVALID_FIELD", "subject_slug_id and target_slug_id must be 16-char Base32.",
                             {}, 400);
      }
      if (!store.HasSlugById(subject) || !store.HasSlugById(target)) {
        return ErrorResponse("NOT_FOUND", "Subject or target slug_id not found.", {}, 404);
      }
      try {
        predicate = CanonicalizePredicate(body.value("predicate", ""));
      } catch (const std::exception& ex) {
        return ErrorResponse("INVALID_FIELD", ex.what(), {}, 400);
      }
      if (const auto record = store.GetPredicate(predicate)) {
        if (ToLowerCopy(record->status) != "active") {
          warnings.push_back(PredicateWarning(predicate, record->status));
        }
      } else {
        store.EnsurePredicate(predicate, "extension", "unreviewed");
        warnings.push_back(PredicateWarning(predicate, "unreviewed"));
      }
      store.InsertTemplateRelationship(TemplateRelationship{subject, predicate, target});
      LogInfo("POST /api/v1/relationships/template " + subject + "->" + target);
      return OkResponse(json{{"subject_slug_id", subject},
                             {"predicate", predicate},
                             {"target_slug_id", target}},
                        201, warnings);
    } catch (const std::exception& ex) {
      return ErrorResponse("INVALID_JSON", ex.what(), {}, 400);
    }
  };

  auto handle_relationships_template_list = [&store](const platform::HttpRequest& request) {
    const std::string subject = GetQueryParam(request, "subject_slug_id", "");
    const std::string target = GetQueryParam(request, "target_slug_id", "");
    const std::string predicate = GetQueryParam(request, "predicate", "");
    const std::string checklist = GetQueryParam(request, "checklist", "");
    const auto limit = ParseLimit(GetQueryParam(request, "limit", ""));
    const auto cursor = ParseCursor(GetQueryParam(request, "cursor", ""));

    std::vector<TemplateRelationship> rels;
    if (!checklist.empty()) {
      rels = store.GetTemplateRelationshipsForChecklist(checklist);
      if (!subject.empty() || !target.empty() || !predicate.empty()) {
        std::vector<TemplateRelationship> filtered;
        filtered.reserve(rels.size());
        for (const auto& rel : rels) {
          if (!subject.empty() && rel.subject_slug_id != subject) continue;
          if (!target.empty() && rel.target_slug_id != target) continue;
          if (!predicate.empty() && rel.predicate != predicate) continue;
          filtered.push_back(rel);
        }
        rels.swap(filtered);
      }
      std::sort(rels.begin(), rels.end(),
                [](const TemplateRelationship& a, const TemplateRelationship& b) {
                  if (a.subject_slug_id != b.subject_slug_id) {
                    return a.subject_slug_id < b.subject_slug_id;
                  }
                  if (a.predicate != b.predicate) {
                    return a.predicate < b.predicate;
                  }
                  return a.target_slug_id < b.target_slug_id;
                });
      if (cursor && *cursor > 0) {
        const auto offset = static_cast<std::size_t>(*cursor);
        if (offset >= rels.size()) {
          rels.clear();
        } else {
          rels.erase(rels.begin(), rels.begin() + static_cast<std::ptrdiff_t>(offset));
        }
      }
      if (limit && *limit > 0 && rels.size() > static_cast<std::size_t>(*limit)) {
        rels.resize(static_cast<std::size_t>(*limit));
      }
    } else {
      rels = store.ListTemplateRelationships(subject.empty() ? std::nullopt : std::make_optional(subject),
                                             target.empty() ? std::nullopt : std::make_optional(target),
                                             predicate.empty() ? std::nullopt : std::make_optional(predicate),
                                             limit, cursor);
    }
    json items = json::array();
    for (const auto& rel : rels) {
      items.push_back({{"subject_slug_id", rel.subject_slug_id},
                       {"predicate", rel.predicate},
                       {"target_slug_id", rel.target_slug_id}});
    }
    std::optional<int> next_cursor;
    if (limit && rels.size() == static_cast<std::size_t>(*limit)) {
      next_cursor = cursor.value_or(0) + static_cast<int>(rels.size());
    }
    return OkResponse(json{{"items", items},
                           {"next_cursor", next_cursor ? json(*next_cursor) : json(nullptr)}});
  };

  auto handle_relationships_address_create = [&store](const platform::HttpRequest& request) {
    json warnings = json::array();
    try {
      const auto body = json::parse(request.body);
      const std::string subject = body.value("subject_address_id", "");
      std::string predicate;
      const std::string target = body.value("target_address_id", "");
      if (!IsValidBase32Id(subject, 32) || !IsValidBase32Id(target, 32)) {
        return ErrorResponse("INVALID_FIELD", "subject_address_id and target_address_id must be 32-char Base32.",
                             {}, 400);
      }
      store.GetSlugOrThrow(subject);
      store.GetSlugOrThrow(target);
      try {
        predicate = CanonicalizePredicate(body.value("predicate", ""));
      } catch (const std::exception& ex) {
        return ErrorResponse("INVALID_FIELD", ex.what(), {}, 400);
      }
      if (const auto record = store.GetPredicate(predicate)) {
        if (ToLowerCopy(record->status) != "active") {
          warnings.push_back(PredicateWarning(predicate, record->status));
        }
      } else {
        store.EnsurePredicate(predicate, "extension", "unreviewed");
        warnings.push_back(PredicateWarning(predicate, "unreviewed"));
      }
      store.InsertAddressRelationship(subject, RelationshipEdge{predicate, target});
      LogInfo("POST /api/v1/relationships/address " + subject + "->" + target);
      return OkResponse(json{{"subject_address_id", subject},
                             {"predicate", predicate},
                             {"target_address_id", target}},
                        201, warnings);
    } catch (const std::exception& ex) {
      return ErrorResponse("INVALID_JSON", ex.what(), {}, 400);
    }
  };

  auto handle_relationships_address_list = [&store](const platform::HttpRequest& request) {
    const std::string subject = GetQueryParam(request, "subject_address_id", "");
    const std::string target = GetQueryParam(request, "target_address_id", "");
    const std::string predicate = GetQueryParam(request, "predicate", "");
    const auto limit = ParseLimit(GetQueryParam(request, "limit", ""));
    const auto cursor = ParseCursor(GetQueryParam(request, "cursor", ""));

    auto rels = store.ListAddressRelationships(subject.empty() ? std::nullopt : std::make_optional(subject),
                                               target.empty() ? std::nullopt : std::make_optional(target),
                                               predicate.empty() ? std::nullopt : std::make_optional(predicate),
                                               limit, cursor);
    json items = json::array();
    for (const auto& rel : rels) {
      items.push_back({{"subject_address_id", rel.subject_address_id},
                       {"predicate", rel.predicate},
                       {"target_address_id", rel.target_address_id}});
    }
    std::optional<int> next_cursor;
    if (limit && rels.size() == static_cast<std::size_t>(*limit)) {
      next_cursor = cursor.value_or(0) + static_cast<int>(rels.size());
    }
    return OkResponse(json{{"items", items},
                           {"next_cursor", next_cursor ? json(*next_cursor) : json(nullptr)}});
  };

  auto handle_predicates_list = [&store](const platform::HttpRequest& request) {
    const auto limit = ParseLimit(GetQueryParam(request, "limit", ""));
    const auto cursor = ParseCursor(GetQueryParam(request, "cursor", ""));
    const auto preds = store.ListPredicates(limit, cursor);
    json items = json::array();
    for (const auto& pred : preds) {
      items.push_back({{"name", pred.name},
                       {"kind", pred.kind},
                       {"status", pred.status},
                       {"description", pred.description},
                       {"meta", pred.meta.empty() ? json(nullptr) : json(pred.meta)}});
    }
    std::optional<int> next_cursor;
    if (limit && preds.size() == static_cast<std::size_t>(*limit)) {
      next_cursor = cursor.value_or(0) + static_cast<int>(preds.size());
    }
    return OkResponse(
        json{{"items", items}, {"next_cursor", next_cursor ? json(*next_cursor) : json(nullptr)}});
  };

  auto handle_predicates_create = [&store](const platform::HttpRequest& request) {
    try {
      const auto body = json::parse(request.body);
      std::string name;
      try {
        name = CanonicalizePredicate(body.value("name", ""));
      } catch (const std::exception& ex) {
        return ErrorResponse("INVALID_FIELD", ex.what(), {}, 400);
      }
      const std::string kind = body.value("kind", "extension");
      const std::string status = body.value("status", "active");
      const std::string description = body.value("description", "");
      std::string meta;
      if (const auto it = body.find("meta"); it != body.end() && !it->is_null()) {
        meta = it->is_string() ? it->get<std::string>() : it->dump();
      }
      store.UpsertPredicate(core::PredicateRecord{name, kind, status, description, meta});
      return OkResponse(json{{"name", name},
                             {"kind", kind},
                             {"status", status},
                             {"description", description},
                             {"meta", meta.empty() ? json(nullptr) : json(meta)}},
                        201);
    } catch (const std::exception& ex) {
      return ErrorResponse("INVALID_JSON", ex.what(), {}, 400);
    }
  };

  auto handle_entities_create = [&store](const platform::HttpRequest& request) {
    try {
      const auto body = json::parse(request.body);
      const std::string principal = body.value("principal", "");
      const std::string kind = body.value("kind", "user");
      const std::string display_name = body.value("display_name", "");
      if (principal.empty()) {
        return ErrorResponse("INVALID_FIELD", "Entity principal is required.", {}, 400);
      }
      const auto entity_id = store.EnsureEntityRecord(principal, kind, display_name);
      return OkResponse(json{{"entity_id", entity_id},
                             {"principal", principal},
                             {"kind", kind},
                             {"display_name", display_name}});
    } catch (const std::exception& ex) {
      return ErrorResponse("INVALID_JSON", ex.what(), {}, 400);
    }
  };

  auto handle_entities_list = [&store](const platform::HttpRequest& request) {
    const auto limit = ParseLimit(GetQueryParam(request, "limit", ""));
    const auto cursor = ParseCursor(GetQueryParam(request, "cursor", ""));
    const auto entities = store.ListEntities(limit, cursor);
    json items = json::array();
    for (const auto& ent : entities) {
      items.push_back({{"entity_id", ent.first}, {"principal", ent.second}});
    }
    std::optional<int> next_cursor;
    if (limit && entities.size() == static_cast<std::size_t>(*limit)) {
      next_cursor = cursor.value_or(0) + static_cast<int>(entities.size());
    }
    return OkResponse(json{{"items", items},
                           {"next_cursor", next_cursor ? json(*next_cursor) : json(nullptr)}});
  };

  auto handle_instances_create = [&store](const platform::HttpRequest& request) {
    try {
      const auto body = json::parse(request.body);
      const std::string principal = body.value("principal", "");
      const std::string label = body.value("label", "");
      const std::string meta = body.value("meta", "");
      if (principal.empty()) {
        return ErrorResponse("INVALID_FIELD", "Instance principal is required.", {}, 400);
      }
      const auto instance_id = store.EnsureInstanceRecord(principal, label, meta);
      return OkResponse(json{{"instance_id", instance_id}, {"principal", principal}, {"label", label}});
    } catch (const std::exception& ex) {
      return ErrorResponse("INVALID_JSON", ex.what(), {}, 400);
    }
  };

  auto handle_instances_list = [&store](const platform::HttpRequest& request) {
    const auto limit = ParseLimit(GetQueryParam(request, "limit", ""));
    const auto cursor = ParseCursor(GetQueryParam(request, "cursor", ""));
    const auto instances = store.ListInstances(limit, cursor);
    json items = json::array();
    for (const auto& inst : instances) {
      items.push_back({{"instance_id", inst.first}, {"principal", inst.second}});
    }
    std::optional<int> next_cursor;
    if (limit && instances.size() == static_cast<std::size_t>(*limit)) {
      next_cursor = cursor.value_or(0) + static_cast<int>(instances.size());
    }
    return OkResponse(json{{"items", items},
                           {"next_cursor", next_cursor ? json(*next_cursor) : json(nullptr)}});
  };

  auto handle_evaluate_slug = [&store](const platform::HttpRequest& request) {
    if (request.path_params.empty()) {
      return ErrorResponse("INVALID_FIELD", "Missing address_id path parameter.", {}, 400);
    }
    const std::string address_id = request.path_params.front();
    try {
      const auto slug = store.GetSlugOrThrow(address_id);
      const auto verify_evaluations = store.EvaluateVerifyRelationships(address_id);
      json verify = json::array();
      json flags = json::array();
      for (const auto& item : verify_evaluations) {
        verify.push_back(VerifyEvaluationToJson(item));
        if (item.predicate_bool == "indeterminate") {
          flags.push_back({{"code", "VERIFY_INDETERMINATE"},
                           {"details", {{"predicate", item.predicate},
                                        {"target_address_id", item.target_address_id},
                                        {"reason_code", item.reason_code},
                                        {"reason", item.reason}}}});
        }
        if (!item.would_write) {
          flags.push_back({{"code", "VERIFY_NO_WRITE"},
                           {"details", {{"predicate", item.predicate},
                                        {"target_address_id", item.target_address_id},
                                        {"write_decision", item.write_decision}}}});
        }
      }
      json data{{"address_id", address_id},
                {"effective_status", StatusToString(slug.status)},
                {"flags", flags},
                {"verify", verify}};
      return OkResponse(data);
    } catch (const std::exception& ex) {
      return ErrorResponse("NOT_FOUND", ex.what(), json{{"address_id", address_id}}, 404);
    }
  };

  auto handle_evaluate_graph = [&store](const platform::HttpRequest& request) {
    try {
      const auto body = json::parse(request.body);
      if (!body.contains("root_address_ids") || !body["root_address_ids"].is_array()) {
        return ErrorResponse("INVALID_FIELD", "root_address_ids array is required.", {}, 400);
      }
      json nodes = json::array();
      for (const auto& id_val : body["root_address_ids"]) {
        const std::string addr = id_val.get<std::string>();
        try {
          const auto slug = store.GetSlugOrThrow(addr);
          const auto verify_evaluations = store.EvaluateVerifyRelationships(addr);
          json verify = json::array();
          json flags = json::array();
          for (const auto& item : verify_evaluations) {
            verify.push_back(VerifyEvaluationToJson(item));
            if (item.predicate_bool == "indeterminate") {
              flags.push_back(
                  {{"code", "VERIFY_INDETERMINATE"},
                   {"details", {{"predicate", item.predicate},
                                {"target_address_id", item.target_address_id},
                                {"reason_code", item.reason_code},
                                {"reason", item.reason}}}});
            }
            if (!item.would_write) {
              flags.push_back({{"code", "VERIFY_NO_WRITE"},
                               {"details", {{"predicate", item.predicate},
                                            {"target_address_id", item.target_address_id},
                                            {"write_decision", item.write_decision}}}});
            }
          }
          nodes.push_back({{"address_id", addr},
                           {"effective_status", StatusToString(slug.status)},
                           {"flags", flags},
                           {"verify", verify}});
        } catch (...) {
          nodes.push_back({{"address_id", addr},
                           {"effective_status", "Unknown"},
                           {"verify", json::array()},
                            {"flags", json::array({json{{"code", "NOT_FOUND"}, {"details", {{"address_id", addr}}}}})}});
        }
      }
      return OkResponse(json{{"nodes", nodes}});
    } catch (const std::exception& ex) {
      return ErrorResponse("INVALID_JSON", ex.what(), {}, 400);
    }
  };

  auto handle_slugs = [&store](const platform::HttpRequest& request) {
    const std::string checklist = GetQueryParam(request, "checklist", "");
    const std::string source_name = GetQueryParam(request, "source_name", "");
    const std::string pack = GetQueryParam(request, "pack", "");
    const std::string checklist_dir = GetQueryParam(request, "checklist_dir", "");
    const std::string section = GetQueryParam(request, "section", "");
    const std::string instance_id_param = GetQueryParam(request, "instance_id", "");
    const std::string instance_principal = GetQueryParam(request, "instance_principal", "");
    const std::string status_filter = GetQueryParam(request, "status", "");
    const std::string limit_param = GetQueryParam(request, "limit", "");
    const std::string cursor_param = GetQueryParam(request, "cursor", "");

    std::optional<ChecklistStatus> status_value;
    if (!status_filter.empty()) {
      auto parsed_status = ParseStatus(status_filter);
      if (parsed_status == ChecklistStatus::kUnknown) {
        return ErrorResponse("INVALID_FIELD", "Status filter must be Pass, Fail, NA, or Other.", {},
                             400);
      }
      status_value = parsed_status;
    }

    auto limit = ParseLimit(limit_param);
    if (!limit_param.empty() && !limit) {
      return ErrorResponse("INVALID_FIELD", "Limit must be a positive integer.", {}, 400);
    }
    auto cursor = ParseCursor(cursor_param);
    if (!cursor_param.empty() && !cursor) {
      return ErrorResponse("INVALID_FIELD", "Cursor must be a non-negative integer.", {}, 400);
    }

    std::optional<std::string> instance_filter;
    if (!instance_id_param.empty()) {
      instance_filter = instance_id_param;
    } else if (!instance_principal.empty()) {
      instance_filter = core::ComputeInstanceId(instance_principal);
    }

    std::optional<core::ChecklistOwnership> ownership_filter;
    if (!source_name.empty() || !pack.empty() || !checklist_dir.empty()) {
      core::ChecklistOwnership ownership;
      ownership.source_name = source_name;
      ownership.pack = pack;
      ownership.checklist_dir = checklist_dir;
      ownership.checklist = checklist;
      ownership_filter = ownership;
    }

    std::vector<ChecklistSlug> slugs = store.QuerySlugs(
        checklist.empty() ? std::nullopt : std::make_optional(checklist), instance_filter,
        section.empty() ? std::nullopt : std::make_optional(section), status_value, limit, cursor, ownership_filter);
    json items = json::array();
    for (const auto& slug : slugs) {
      items.push_back(SlugToJson(slug));
    }
    std::optional<int> next_cursor;
    if (limit && slugs.size() == static_cast<std::size_t>(*limit)) {
      next_cursor = cursor.value_or(0) + static_cast<int>(slugs.size());
    }
    LogInfo("GET /api/v1/slugs" + (checklist.empty() ? "" : " checklist=" + checklist) +
            (instance_filter && !instance_filter->empty() ? " instance_id=" + *instance_filter
                                                          : ""));
    return OkResponse(json{{"items", items},
                           {"next_cursor", next_cursor ? json(*next_cursor) : json(nullptr)}});
  };

  auto handle_visualization_graph = [&store](const platform::HttpRequest& request) {
    const std::string checklist = GetQueryParam(request, "checklist", "");
    const std::string instance_id = GetQueryParam(request, "instance_id", "");
    if (checklist.empty() || instance_id.empty()) {
      return ErrorResponse("INVALID_FIELD", "checklist and instance_id are required.", {}, 400);
    }
    if (!core::IsValidBase32Id(instance_id, 16)) {
      return ErrorResponse("INVALID_FIELD", "instance_id must be a 16-char Base32 token.",
                           json{{"instance_id", instance_id}}, 400);
    }

    const std::string source_name = GetQueryParam(request, "source_name", "");
    const std::string pack = GetQueryParam(request, "pack", "");
    const std::string checklist_dir = GetQueryParam(request, "checklist_dir", "");
    std::optional<core::ChecklistOwnership> ownership_filter;
    if (!source_name.empty() || !pack.empty() || !checklist_dir.empty()) {
      ownership_filter = core::ChecklistOwnership{
          source_name, "", pack, checklist_dir, checklist};
    }
    const auto slugs = store.QuerySlugs(checklist, instance_id, std::nullopt, std::nullopt,
                                        std::nullopt, std::nullopt, ownership_filter);
    if (slugs.empty()) {
      return ErrorResponse("NOT_FOUND", "No slugs found for checklist/instance.",
                           json{{"checklist", checklist}, {"instance_id", instance_id}}, 404);
    }
    const auto graph = BuildChecklistGraph(slugs);
    LogInfo("GET /api/v1/visualizations/graph checklist=" + checklist +
            " instance=" + instance_id);
    return OkResponse(ChecklistGraphToJson(graph));
  };

  auto handle_visualization_workbench = [&store](const platform::HttpRequest& request) {
    const std::string checklist = GetQueryParam(request, "checklist", "");
    const std::string instance_id = GetQueryParam(request, "instance_id", "");
    if (checklist.empty() || instance_id.empty()) {
      return ErrorResponse("INVALID_FIELD", "checklist and instance_id are required.", {}, 400);
    }
    if (!core::IsValidBase32Id(instance_id, 16)) {
      return ErrorResponse("INVALID_FIELD", "instance_id must be a 16-char Base32 token.",
                           json{{"instance_id", instance_id}}, 400);
    }

    std::string source_name = GetQueryParam(request, "source_name", "");
    std::string pack = GetQueryParam(request, "pack", "");
    std::string checklist_dir = GetQueryParam(request, "checklist_dir", "");
    if (source_name.empty() && pack.empty() && checklist_dir.empty()) {
      const auto ownerships = store.ListOwnershipsForInstance(checklist, instance_id);
      if (ownerships.size() == 1) {
        source_name = ownerships.front().source_name;
        pack = ownerships.front().pack;
        checklist_dir = ownerships.front().checklist_dir;
      } else if (ownerships.size() > 1) {
        return ErrorResponse("AMBIGUOUS_CHECKLIST_OWNERSHIP",
                             "Checklist instance has multiple source/pack owners; specify source_name and pack.",
                             json{{"checklist", checklist},
                                  {"instance_id", instance_id},
                                  {"matches", OwnershipsToJson(ownerships)}},
                             400);
      }
    }
    if (checklist_dir.empty()) {
      checklist_dir = checklist;
    }

    const auto workspace_roots = LoadChecklistWorkspaceRoots();
    std::string resolve_error;
    json resolve_details = json::object();
    const auto resolved_root = ResolveChecklistRoot(workspace_roots, checklist_dir, pack, source_name,
                                                     false, &resolve_error, &resolve_details);
    if (!resolved_root) {
      return ErrorResponse("INVALID_FIELD",
                           resolve_error.empty() ? "Invalid pack or checklist." : resolve_error,
                           resolve_details, 400);
    }

    json warnings = json::array();
    const auto ownership = OwnershipFromResolution(*resolved_root, checklist);
    auto slugs = store.QuerySlugs(checklist, instance_id, std::nullopt, std::nullopt,
                                  std::nullopt, std::nullopt, ownership);
    if (slugs.empty()) {
      slugs = store.QuerySlugs(checklist, instance_id, std::nullopt, std::nullopt,
                               std::nullopt, std::nullopt);
      if (!slugs.empty()) {
        warnings.push_back(
            {{"code", "OWNERSHIP_FILTER_EMPTY"},
             {"message",
              "No rows had persisted ownership for the requested source/pack; analyzed legacy unowned rows."},
             {"details",
              {{"source_name", resolved_root->source_name},
               {"pack", resolved_root->pack},
               {"checklist_dir", resolved_root->checklist_dir}}}});
      }
    }
    if (slugs.empty()) {
      return ErrorResponse("NOT_FOUND", "No slugs found for checklist/instance.",
                           json{{"checklist", checklist}, {"instance_id", instance_id}}, 404);
    }
    const auto graph = BuildChecklistGraph(slugs);
    const auto workbench = BuildRelationshipWorkbench(graph, resolved_root->root);
    LogInfo("GET /api/v1/visualizations/workbench source=" + resolved_root->source_name +
            " pack=" + resolved_root->pack + " checklist=" + checklist +
            " instance=" + instance_id);
    return OkResponse(workbench, 200, warnings);
  };

  auto handle_history = [&store](const platform::HttpRequest& request) {
    if (request.path_params.empty()) {
      return ErrorResponse("INVALID_FIELD", "Missing address_id path parameter.", {}, 400);
    }
    const std::string address_id = request.path_params.front();
    std::optional<int> limit;
    const std::string limit_param = GetQueryParam(request, "limit", "");
    const std::string cursor_param = GetQueryParam(request, "cursor", "");
    if (!limit_param.empty()) {
      limit = ParseLimit(limit_param);
      if (!limit) {
        return ErrorResponse("INVALID_FIELD", "Limit must be a positive integer.", {}, 400);
      }
    }
    auto cursor = ParseCursor(cursor_param);
    if (!cursor_param.empty() && !cursor) {
      return ErrorResponse("INVALID_FIELD", "Cursor must be a non-negative integer.", {}, 400);
    }
    LogInfo("GET /api/v1/history/" + address_id);
    try {
      const auto entries = store.GetHistory(address_id, limit, cursor);
      json items = json::array();
      for (const auto& entry : entries) {
        items.push_back({{"address_id", entry.address_id},
                         {"timestamp", entry.timestamp},
                         {"result", entry.result},
                         {"status", StatusToString(entry.status)},
                         {"comment", entry.comment},
                         {"entity_id", entry.entity_id}});
      }
      std::optional<int> next_cursor;
      if (limit && entries.size() == static_cast<std::size_t>(*limit)) {
        next_cursor = cursor.value_or(0) + static_cast<int>(entries.size());
      }
      return OkResponse(json{{"items", items},
                             {"next_cursor", next_cursor ? json(*next_cursor) : json(nullptr)}});
    } catch (const std::exception& ex) {
      return ErrorResponse("NOT_FOUND", ex.what(), json{{"address_id", address_id}}, 404);
    }
  };

  auto handle_update = [&store, resolve_entity, &config](const platform::HttpRequest& request) {
    if (request.path_params.empty()) {
      return ErrorResponse("INVALID_FIELD", "Missing address_id path parameter.", {}, 400);
    }
    const auto payload = json::parse(request.body, nullptr, false);
    if (payload.is_discarded()) {
      return ErrorResponse("INVALID_JSON", "Invalid JSON payload.", {}, 400);
    }
    try {
      auto update = ParseUpdatePayload(payload);
      update.address_id = request.path_params.front();
      EntityResolution resolution;
      const auto requested = update.entity_principal_override;
      if (const auto err = resolve_entity(requested, resolution)) {
        return *err;
      }
      update.entity_principal_override = resolution.principal;
      update.entity_id_override = resolution.entity_id;
      store.ApplyUpdate(update);
      const auto updated = store.GetSlugOrThrow(update.address_id);
      LogInfo("PATCH /api/v1/slugs/" + update.address_id);
      return OkResponse(SlugToJson(updated));
    } catch (const std::exception& ex) {
      return ErrorResponse("INVALID_FIELD", ex.what(), {}, 400);
    }
  };

  auto handle_update_bulk = [&store, resolve_entity, &config](const platform::HttpRequest& request) {
    const auto payload = json::parse(request.body, nullptr, false);
    if (payload.is_discarded()) {
      return ErrorResponse("INVALID_JSON", "Invalid JSON payload.", {}, 400);
    }
    try {
      if (!payload.is_object() || !payload.contains("updates")) {
        return ErrorResponse("INVALID_FIELD", "Missing 'updates' array.", {}, 400);
      }
      auto updates = ParseBulkPayload(payload.at("updates"));
      EntityResolution resolution;
      for (auto& update : updates) {
        const auto requested = update.entity_principal_override;
        if (const auto err = resolve_entity(requested, resolution)) {
          return *err;
        }
        update.entity_principal_override = resolution.principal;
        update.entity_id_override = resolution.entity_id;
      }
      store.ApplyBulkUpdates(updates);
      json updated = json::array();
      for (const auto& update : updates) {
        updated.push_back(SlugToJson(store.GetSlugOrThrow(update.address_id)));
      }
      LogInfo("POST /api/v1/slugs/bulk-update count=" + std::to_string(updates.size()));
      return OkResponse(json{{"results", updated}});
    } catch (const std::exception& ex) {
      return ErrorResponse("INVALID_FIELD", ex.what(), {}, 400);
    }
  };

  auto handle_export_json = [&store](const platform::HttpRequest&) {
    const auto slugs = store.ExportAllSlugs();
    json payload = json::array();
    for (const auto& slug : slugs) {
      payload.push_back(SlugToJson(slug));
    }
    LogInfo("GET /api/v1/export/json");
    return OkResponse(payload);
  };

  auto handle_export_jsonl = [&store](const platform::HttpRequest&) {
    const auto slugs = store.ExportAllSlugs();
    std::ostringstream stream;
    for (std::size_t i = 0; i < slugs.size(); ++i) {
      stream << SlugToJson(slugs[i]).dump();
      if (i + 1 < slugs.size()) {
        stream << "\n";
      }
    }
    LogInfo("GET /api/v1/export/jsonl");
    return TextResponse(stream.str(), "application/json", 200);
  };

  auto handle_import_jsonl = [&store, resolve_entity](const platform::HttpRequest& request) {
    const std::string checklist_param = GetQueryParam(request, "checklist", "");
    const std::string instance_id_param = GetQueryParam(request, "instance_id", "");
    const std::string instance_principal =
        GetQueryParam(request, "instance_principal", "instance||default");
    const bool allow_new = GetQueryParam(request, "allow_new", "") == "1";
    const std::string resolve_lineage_param = GetQueryParam(request, "resolve_lineage", "");
    const std::string resolve_lineage_lower = ToLowerCopy(resolve_lineage_param);
    const bool resolve_lineage =
        resolve_lineage_param.empty() ||
        (resolve_lineage_lower != "0" && resolve_lineage_lower != "false");

    if (request.body.empty()) {
      return ErrorResponse("INVALID_FIELD", "Request body must contain JSONL content.", {}, 400);
    }

    std::string instance_id = instance_id_param;
    if (!instance_id.empty()) {
      if (!core::IsValidBase32Id(instance_id, 16)) {
        return ErrorResponse("INVALID_FIELD", "instance_id must be a 16-char Base32 token.",
                             json{{"instance_id", instance_id}}, 400);
      }
    } else {
      instance_id = core::ComputeInstanceId(instance_principal);
    }

    EntityResolution resolution;
    if (const auto err = resolve_entity(std::nullopt, resolution)) {
      return *err;
    }

    std::vector<JsonlImportItem> items;
    std::vector<json> missing_items;
    json warnings = json::array();
    std::string target_checklist = checklist_param;
    std::istringstream stream(request.body);
    std::string line;
    std::size_t line_number = 0;
    while (std::getline(stream, line)) {
      ++line_number;
      if (TrimString(line).empty()) {
        continue;
      }
      const auto parsed = json::parse(line, nullptr, false);
      if (parsed.is_discarded() || !parsed.is_object()) {
        return ErrorResponse(
            "INVALID_JSONL", "Failed to parse JSONL line as an object.",
            json{{"line", line_number}, {"snippet", line.substr(0, 160)}}, 400);
      }

      JsonlImportItem item;
      item.line = line_number;
      try {
        bool present = false;
        if (const auto checklist = ReadOptionalString(parsed, "checklist", &present)) {
          item.slug.checklist = *checklist;
        }
        if (!target_checklist.empty() && !item.slug.checklist.empty() &&
            item.slug.checklist != target_checklist) {
          return ErrorResponse(
              "CHECKLIST_MISMATCH", "JSONL checklist does not match target checklist.",
              json{{"line", line_number},
                   {"expected", target_checklist},
                   {"found", item.slug.checklist}},
              400);
        }
        if (target_checklist.empty() && !item.slug.checklist.empty()) {
          target_checklist = item.slug.checklist;
        }
        if (!target_checklist.empty() && item.slug.checklist.empty()) {
          item.slug.checklist = target_checklist;
        }

        if (const auto section = ReadOptionalString(parsed, "section", &present)) {
          item.slug.section = *section;
        }
        if (const auto procedure = ReadOptionalString(parsed, "procedure", &present)) {
          item.slug.procedure = *procedure;
        }
        if (const auto action = ReadOptionalString(parsed, "action", &present)) {
          item.slug.action = *action;
        }
        if (const auto spec = ReadOptionalString(parsed, "spec", &present)) {
          item.slug.spec = *spec;
        }
        if (const auto instructions = ReadOptionalString(parsed, "instructions", &present)) {
          item.has_instructions = true;
          item.slug.instructions = *instructions;
        }

        item.has_template_fields =
            !item.slug.checklist.empty() && !item.slug.section.empty() &&
            !item.slug.procedure.empty() && !item.slug.action.empty() && !item.slug.spec.empty();

        if (const auto result = ReadOptionalString(parsed, "result", &present)) {
          item.has_result = true;
          item.slug.result = *result;
        }
        if (const auto comment = ReadOptionalString(parsed, "comment", &present)) {
          item.has_comment = true;
          item.slug.comment = *comment;
        }
        if (const auto timestamp = ReadOptionalString(parsed, "timestamp", &present)) {
          item.has_timestamp = true;
          item.slug.timestamp = *timestamp;
        }

        if (const auto status = ReadOptionalString(parsed, "status", &present)) {
          item.has_status = true;
          if (!status->empty()) {
            item.slug.status = ParseStatus(*status);
            if (item.slug.status == ChecklistStatus::kUnknown) {
              item.has_status = false;
              warnings.push_back(
                  {{"code", "STATUS_UNKNOWN"},
                   {"message", "Unrecognized status; leaving existing status unchanged."},
                   {"details", {{"line", line_number}, {"status", *status}}}});
            }
          } else {
            item.slug.status = ChecklistStatus::kUnknown;
          }
        }

        if (const auto it = parsed.find("address_order"); it != parsed.end()) {
          if (!it->is_number_integer()) {
            return ErrorResponse("INVALID_FIELD", "address_order must be an integer when provided.",
                                 json{{"line", line_number}}, 400);
          }
          item.has_address_order = true;
          item.slug.address_order = it->get<std::int64_t>();
        }

        if (const auto slug_id = ReadOptionalString(parsed, "slug_id", &present)) {
          item.slug.slug_id = *slug_id;
        }
        if (const auto address = ReadOptionalString(parsed, "address_id", &present)) {
          if (!address->empty()) {
            std::string address_instance;
            const auto derived = ExtractSlugIdFromAddress(*address, &address_instance);
            if (!derived) {
              return ErrorResponse("INVALID_FIELD", "address_id must be a 32-char Base32 token.",
                                   json{{"line", line_number}, {"address_id", *address}}, 400);
            }
            item.has_address_id = true;
            item.original_address_id = *derived + address_instance;
            if (!address_instance.empty() && address_instance == instance_id) {
              item.address_instance_match = true;
            }
            if (item.slug.slug_id.empty()) {
              item.slug.slug_id = *derived;
            }
            if (!address_instance.empty() && address_instance != instance_id) {
              warnings.push_back({{"code", "ADDRESS_INSTANCE_MISMATCH"},
                                  {"message",
                                   "Address instance_id does not match import target; using slug_id."},
                                  {"details",
                                   {{"line", line_number},
                                    {"address_id", *address},
                                    {"instance_id", address_instance},
                                    {"target_instance_id", instance_id}}}});
            }
          }
        }
        if (item.slug.slug_id.empty()) {
          if (!item.has_template_fields) {
            return ErrorResponse(
                "INVALID_FIELD", "slug_id is required when template fields are missing.",
                json{{"line", line_number}}, 400);
          }
          const std::string expected = core::ComputeSlugId(
              item.slug.checklist, item.slug.section, item.slug.procedure, item.slug.action,
              item.slug.spec, item.slug.instructions);
          item.slug.slug_id = expected;
        } else if (item.has_template_fields && item.has_instructions) {
          item.expected_slug_id = core::ComputeSlugId(
              item.slug.checklist, item.slug.section, item.slug.procedure, item.slug.action,
              item.slug.spec, item.slug.instructions);
          if (!item.expected_slug_id.empty() && item.expected_slug_id != item.slug.slug_id) {
            item.slug_id_mismatch = true;
            warnings.push_back({{"code", "SLUG_ID_MISMATCH"},
                                {"message", "slug_id does not match computed template hash."},
                                {"details",
                                 {{"line", line_number},
                                  {"slug_id", item.slug.slug_id},
                                  {"expected_slug_id", item.expected_slug_id}}}});
          }
        }

        if (!core::IsValidBase32Id(item.slug.slug_id, 16)) {
          return ErrorResponse("INVALID_FIELD", "slug_id must be a 16-char Base32 token.",
                               json{{"line", line_number}, {"slug_id", item.slug.slug_id}}, 400);
        }
        item.original_slug_id = item.slug.slug_id;
      } catch (const std::exception& ex) {
        return ErrorResponse("INVALID_FIELD", ex.what(), json{{"line", line_number}}, 400);
      }

      if (item.slug.checklist.empty()) {
        return ErrorResponse("INVALID_FIELD", "checklist is required for JSONL import.",
                             json{{"line", line_number}}, 400);
      }
      items.push_back(std::move(item));
    }

    if (items.empty()) {
      return ErrorResponse("INVALID_JSONL", "JSONL payload contained no data.", {}, 400);
    }
    if (target_checklist.empty()) {
      return ErrorResponse("INVALID_FIELD", "checklist is required for JSONL import.", {}, 400);
    }

    std::unordered_map<std::string, std::string> slug_aliases;
    std::unordered_map<std::string, std::string> address_aliases;
    if (resolve_lineage) {
      auto instance_slugs = store.QuerySlugs(target_checklist, instance_id, std::nullopt,
                                             std::nullopt, std::nullopt, std::nullopt);
      if (!instance_slugs.empty()) {
        const auto template_relationships =
            store.GetTemplateRelationshipsForChecklist(target_checklist);
        auto lineage = BuildSlugLineageAliases(instance_slugs, template_relationships);
        slug_aliases = std::move(lineage.aliases);
        for (const auto& warn : lineage.warnings) {
          warnings.push_back(warn);
        }
        const auto address_relationships = store.ListAddressRelationships(
            std::nullopt, std::nullopt, std::make_optional(std::string{kSlugSuccessorPredicate}),
            std::nullopt, std::nullopt);
        auto address_lineage =
            BuildAddressLineageAliases(instance_slugs, address_relationships, instance_id);
        address_aliases = std::move(address_lineage.aliases);
        for (const auto& warn : address_lineage.warnings) {
          warnings.push_back(warn);
        }
      }
    }

    for (auto& item : items) {
      if (resolve_lineage) {
        if (item.has_address_id && item.address_instance_match &&
            !item.original_address_id.empty()) {
          const auto it = address_aliases.find(item.original_address_id);
          if (it != address_aliases.end() && it->second != item.original_address_id) {
            item.mapped_address_id = it->second;
            item.slug.slug_id = it->second.substr(0, 16);
            item.lineage_mapped = true;
            item.lineage_used_address = true;
            warnings.push_back(
                {{"code", "ADDRESS_LINEAGE_ALIAS"},
                 {"message", "Mapped JSONL address_id to latest successor."},
                 {"details",
                  {{"line", item.line},
                   {"address_id", item.original_address_id},
                   {"latest_address_id", item.mapped_address_id},
                   {"slug_id", item.original_slug_id},
                   {"latest_slug_id", item.slug.slug_id}}}});
          }
        }
        if (!item.lineage_mapped) {
          const auto it = slug_aliases.find(item.slug.slug_id);
          if (it != slug_aliases.end() && it->second != item.slug.slug_id) {
            item.slug.slug_id = it->second;
            item.lineage_mapped = true;
            item.lineage_used_address = false;
            warnings.push_back(
                {{"code", "SLUG_LINEAGE_ALIAS"},
                 {"message", "Mapped JSONL slug_id to latest successor."},
                 {"details",
                  {{"line", item.line},
                   {"slug_id", item.original_slug_id},
                   {"latest_slug_id", item.slug.slug_id}}}});
          }
        }
      }

      const std::string address_id = core::ComposeAddressId(item.slug.slug_id, instance_id);
      try {
        item.existing = store.GetSlugOrThrow(address_id);
        item.exists = true;
      } catch (const std::exception&) {
        item.exists = false;
        json missing{{"line", item.line}, {"slug_id", item.slug.slug_id}};
        if (item.lineage_mapped) {
          missing["original_slug_id"] = item.original_slug_id;
          if (item.lineage_used_address) {
            missing["address_id"] = item.original_address_id;
            missing["latest_address_id"] = item.mapped_address_id;
          }
        }
        missing_items.push_back(std::move(missing));
      }
    }

    if (!missing_items.empty() && !allow_new) {
      json details{{"missing", missing_items}, {"count", missing_items.size()}};
      return ErrorResponse("MISSING_SLUGS",
                           "JSONL contains rows not present in the target instance.",
                           details, 409);
    }

    std::size_t updated = 0;
    std::size_t created = 0;
    for (auto& item : items) {
      ChecklistSlug slug;
      if (item.exists) {
        slug = item.existing;
        if (item.has_result) slug.result = item.slug.result;
        if (item.has_status) slug.status = item.slug.status;
        if (item.has_comment) slug.comment = item.slug.comment;
        if (item.has_timestamp) slug.timestamp = item.slug.timestamp;
        if (item.has_address_order) slug.address_order = item.slug.address_order;
      } else {
        if (!item.has_template_fields) {
          return ErrorResponse("INVALID_FIELD",
                               "Template fields are required to insert a new row.",
                               json{{"line", item.line}, {"slug_id", item.slug.slug_id}}, 400);
        }
        slug = item.slug;
      }
      slug.checklist = target_checklist;
      slug.instance_id = instance_id;
      slug.instance_principal = instance_principal;
      slug.entity_id = resolution.entity_id;
      slug.entity_principal = resolution.principal;
      store.UpsertSlug(slug);
      if (item.exists) {
        ++updated;
      } else {
        ++created;
      }
    }

    if (created > 0) {
      for (const auto& missing : missing_items) {
        warnings.push_back({{"code", "MISSING_SLUG"},
                            {"message", "Row did not exist and was inserted."},
                            {"details", missing}});
      }
    }

    json data{{"checklist", target_checklist},
              {"instance_id", instance_id},
              {"updated", updated},
              {"created", created},
              {"rows", items.size()}};
    LogInfo("POST /api/v1/import/jsonl checklist=" + target_checklist +
            " instance_id=" + instance_id + " rows=" + std::to_string(items.size()));
    return OkResponse(data, 200, warnings);
  };

  auto handle_export_markdown = [&store](const platform::HttpRequest& request) {
    if (request.path_params.empty()) {
      return ErrorResponse("INVALID_FIELD", "Missing checklist path parameter.", {}, 400);
    }
    const std::string checklist = request.path_params.front();
    LogInfo("GET /api/v1/export/markdown/" + checklist);
    const auto slugs = store.GetSlugsForChecklist(checklist);
    if (slugs.empty()) {
      return ErrorResponse("NOT_FOUND", "Checklist not found: " + checklist,
                           json{{"checklist", checklist}}, 404);
    }
    try {
      const auto template_relationships = store.GetTemplateRelationshipsForChecklist(checklist);
      const auto markdown = core::markdown::ExportChecklistMarkdown(
          checklist, slugs, template_relationships, core::markdown::RelationshipExportMode::kTemplate,
          core::markdown::RelationshipIdentityFormat::kId);
      return TextResponse(markdown, "text/markdown; charset=utf-8", 200);
    } catch (const std::exception& ex) {
      return ErrorResponse("INVALID_FIELD", ex.what(), {}, 400);
    }
  };

  auto handle_export_report = [&store](const platform::HttpRequest& request) {
    const auto payload = json::parse(request.body, nullptr, false);
    if (payload.is_discarded()) {
      return ErrorResponse("INVALID_JSON", "Invalid JSON payload.", {}, 400);
    }

    const std::string checklist = payload.value("checklist", "");
    const std::string pack = payload.value("pack", "");
    const std::string source_name = payload.value("source_name", "");
    const std::string checklist_dir_payload = payload.value("checklist_dir", "");
    const std::string instance_id = payload.value("instance_id", "");
    const bool use_latest_slug_lineage = payload.value("use_latest_slug_lineage", true);
    const std::string format_raw = payload.value("format", "tex");
    std::string format = ToLowerCopy(format_raw);
    if (format.empty() || format == "latex") {
      format = "tex";
    } else if (format == "htm") {
      format = "html";
    }
    if (format != "tex" && format != "html") {
      return ErrorResponse("INVALID_FIELD", "format must be one of: tex, html.",
                           json{{"format", format_raw}}, 400);
    }

    const std::string jsonl_mode_raw = payload.value("jsonl_mode", "report");
    const std::string jsonl_mode = ToLowerCopy(jsonl_mode_raw);
    ReportJsonlOptions jsonl_options = ReportJsonlOptions::Report();
    if (jsonl_mode == "full") {
      jsonl_options = ReportJsonlOptions::Full();
    } else if (jsonl_mode == "minimal") {
      jsonl_options = ReportJsonlOptions::Minimal();
    } else if (!jsonl_mode.empty() && jsonl_mode != "report") {
      return ErrorResponse("INVALID_FIELD", "jsonl_mode must be one of: report, minimal, full.",
                           json{{"jsonl_mode", jsonl_mode_raw}}, 400);
    }

    const bool include_principals = payload.value(
        "jsonl_include_principals",
        jsonl_options.include_instance_principal || jsonl_options.include_entity_principal);
    jsonl_options.include_instance_principal = include_principals;
    jsonl_options.include_entity_principal = include_principals;
    const bool jsonl_slug_only = payload.value("jsonl_slug_only", false);
    if (jsonl_slug_only) {
      jsonl_options.include_address_id = false;
      jsonl_options.include_slug_id = true;
    }

    if (checklist.empty() || instance_id.empty()) {
      return ErrorResponse("INVALID_FIELD", "checklist and instance_id are required.", {}, 400);
    }

    try {
      const auto workspace_roots = LoadChecklistWorkspaceRoots();
      const std::filesystem::path library_root =
          workspace_roots.empty() ? GetLibraryRoot() : workspace_roots.front().root;
      {
        std::error_code ec;
        std::filesystem::create_directories(library_root, ec);
      }
      std::string effective_source_name = source_name;
      std::string effective_pack = pack;
      std::string effective_checklist_dir = checklist_dir_payload.empty() ? checklist : checklist_dir_payload;
      if (effective_source_name.empty() && effective_pack.empty() && checklist_dir_payload.empty()) {
        const auto ownerships = store.ListOwnershipsForInstance(checklist, instance_id);
        if (ownerships.size() == 1) {
          effective_source_name = ownerships.front().source_name;
          effective_pack = ownerships.front().pack;
          effective_checklist_dir = ownerships.front().checklist_dir;
        } else if (ownerships.size() > 1) {
          return ErrorResponse(
              "AMBIGUOUS_CHECKLIST_OWNERSHIP",
              "Checklist instance has multiple source/pack owners; specify source_name and pack.",
              json{{"checklist", checklist}, {"instance_id", instance_id}, {"matches", OwnershipsToJson(ownerships)}},
              400);
        }
      }
      std::string resolve_error;
      json resolve_details = json::object();
      const auto resolved_root = ResolveChecklistRoot(workspace_roots, effective_checklist_dir, effective_pack,
                                                      effective_source_name, true, &resolve_error, &resolve_details);
      if (!resolved_root) {
        return ErrorResponse("INVALID_FIELD",
                             resolve_error.empty() ? "Invalid pack or checklist." : resolve_error,
                             resolve_details, 400);
      }

      json warnings = json::array();
      const auto ownership = OwnershipFromResolution(*resolved_root, checklist);
      auto slugs =
          store.QuerySlugs(checklist, instance_id, std::nullopt, std::nullopt, std::nullopt, std::nullopt, ownership);
      if (slugs.empty()) {
        slugs = store.QuerySlugs(checklist, instance_id, std::nullopt, std::nullopt, std::nullopt, std::nullopt);
        if (!slugs.empty()) {
          warnings.push_back(
              {{"code", "OWNERSHIP_FILTER_EMPTY"},
               {"message",
                "No rows had persisted ownership for the requested source/pack; exported legacy unowned rows."},
               {"details",
                {{"source_name", resolved_root->source_name},
                 {"pack", resolved_root->pack},
                 {"checklist_dir", resolved_root->checklist_dir}}}});
        }
      }
      if (slugs.empty()) {
        return ErrorResponse("NOT_FOUND", "No slugs found for checklist/instance.",
                             json{{"checklist", checklist}, {"instance_id", instance_id}}, 404);
      }

      std::string instance_principal = slugs.front().instance_principal;
      if (instance_principal.empty()) {
        instance_principal = "instance||" + instance_id;
      }

      std::filesystem::path reports_root = resolved_root->root / "reports";
      std::filesystem::path templates_root = resolved_root->root / "templates";
      if (const char* env = std::getenv("CHAX_REPORTS_ROOT")) {
        reports_root = std::filesystem::path(env);
      }
      if (const char* env = std::getenv("CHAX_REPORT_TEMPLATES_ROOT")) {
        templates_root = std::filesystem::path(env);
      }

      const TexReportConfig config{reports_root, templates_root};
      std::unordered_map<std::string, std::string> slug_aliases;
      if (use_latest_slug_lineage) {
        const auto template_relationships = store.GetTemplateRelationshipsForChecklist(checklist, ownership);
        auto lineage = BuildSlugLineageAliases(slugs, template_relationships);
        slug_aliases = std::move(lineage.aliases);
        for (auto &warning : lineage.warnings) {
          warnings.push_back(std::move(warning));
        }
      }

      const auto append_alias_warnings = [&](const auto& report_result) {
        if (report_result.alias_hits.empty()) {
          return;
        }
        for (const auto& hit : report_result.alias_hits) {
          warnings.push_back(
              {{"code", "LEGACY_TEMPLATE_SLUG"},
               {"message", "Report template referenced a legacy slug_id; substituted latest slug."},
               {"details",
                {{"legacy_slug_id", hit.first},
                 {"latest_slug_id", hit.second},
                 {"template_path", report_result.template_path.string()}}}});
        }
      };

      const auto build_base_data = [&](const auto &report_result) {
        return json{
            {"format", format},
            {"path", report_result.output_path.string()},
            {"directory", report_result.output_dir.string()},
            {"jsonl_path", report_result.jsonl_path.empty() ? json(nullptr) : json(report_result.jsonl_path.string())},
            {"jsonl_written", report_result.jsonl_written},
            {"images_manifest_path", report_result.images_manifest_path.empty()
                                         ? json(nullptr)
                                         : json(report_result.images_manifest_path.string())},
            {"image_count", report_result.image_count},
            {"source_name", resolved_root->source_name},
            {"source_path", resolved_root->library_root.string()},
            {"pack", resolved_root->pack},
            {"checklist_dir", resolved_root->checklist_dir},
            {"template_used", report_result.template_used},
            {"template_path", report_result.template_used ? json(report_result.template_path.string()) : json(nullptr)},
            {"generated_at", report_result.generated_at},
            {"row_count", report_result.row_count}};
      };

      json data;
      if (format == "html") {
        core::TemplateContext report_context;
        const auto report =
            GenerateHtmlReport(config, checklist, instance_id, instance_principal, slugs,
                               slug_aliases, jsonl_options, &report_context);
        append_alias_warnings(report);
        data = build_base_data(report);
      } else {
        core::TemplateContext report_context;
        const auto report =
            GenerateTexReport(config, checklist, instance_id, instance_principal, slugs,
                              slug_aliases, jsonl_options, &report_context);
        append_alias_warnings(report);
        const auto fillable = GenerateFillableReport(config, checklist, instance_id,
                                                     report.generated_at, report_context, slugs,
                                                     jsonl_options);
        if (fillable.template_used && !fillable.fdf_written) {
          json details{{"template_path", fillable.fdf_template_path.string()},
                       {"error", fillable.error}};
          warnings.push_back({{"code", "FILLABLE_EXPORT_FAILED"},
                              {"message", "Fillable report export did not complete."},
                              {"details", details}});
        }
        if (fillable.template_used && fillable.pdf_template_path.empty()) {
          warnings.push_back(
              {{"code", "FILLABLE_PDF_TEMPLATE_MISSING"},
               {"message", "Fillable report template PDF not found."},
               {"details", {{"template_path", fillable.fdf_template_path.string()}}}});
        } else if (fillable.template_used && !fillable.pdf_template_path.empty() &&
                   !fillable.pdf_copied) {
          warnings.push_back(
              {{"code", "FILLABLE_PDF_COPY_FAILED"},
               {"message", "Fillable report template PDF could not be copied."},
               {"details", {{"template_path", fillable.pdf_template_path.string()}}}});
        }
        if (fillable.template_used && !fillable.jsonl_written) {
          json details{{"directory", fillable.output_dir.string()},
                       {"error", fillable.error}};
          warnings.push_back({{"code", "FILLABLE_JSONL_FAILED"},
                              {"message", "Fillable report JSONL snapshot failed."},
                              {"details", details}});
        }

        data = build_base_data(report);
        if (fillable.template_used) {
          data["fillable"] = json{
              {"directory", fillable.output_dir.string()},
              {"fdf_path", fillable.fdf_output_path.empty()
                               ? json(nullptr)
                               : json(fillable.fdf_output_path.string())},
              {"jsonl_path", fillable.jsonl_path.empty()
                                ? json(nullptr)
                                : json(fillable.jsonl_path.string())},
              {"template_path", fillable.fdf_template_path.string()},
              {"pdf_template_path", fillable.pdf_template_path.empty()
                                        ? json(nullptr)
                                        : json(fillable.pdf_template_path.string())},
              {"pdf_copy_path", fillable.pdf_copy_path.empty()
                                    ? json(nullptr)
                                    : json(fillable.pdf_copy_path.string())},
              {"fdf_written", fillable.fdf_written},
              {"jsonl_written", fillable.jsonl_written},
              {"pdf_copied", fillable.pdf_copied},
              {"error", fillable.error.empty() ? json(nullptr) : json(fillable.error)},
          };
        }
      }

      if (resolved_root->defaulted) {
        warnings.push_back({{"code", "DEFAULT_PACK"},
                            {"message", "Report exported using the default asset pack."},
                            {"details", {{"pack", resolved_root->pack},
                                         {"source_name", resolved_root->source_name},
                                         {"checklist", checklist}}}});
      }

      LogInfo("POST /api/v1/export/report format=" + format + " checklist=" + checklist +
              " source=" + resolved_root->source_name + " instance=" + instance_id +
              " -> " + data.value("path", ""));
      return OkResponse(data, 201, warnings);
    } catch (const std::exception &ex) {
      return ErrorResponse("INTERNAL_ERROR", ex.what(), {}, 500);
    }
  };

  auto handle_import_markdown =
      [&store, resolve_entity, &config](const platform::HttpRequest& request) {
    const std::string checklist = GetQueryParam(request, "checklist", "");
    const std::string instance_principal =
        GetQueryParam(request, "instance_principal", "template||default");
    const bool apply_data = GetQueryParam(request, "apply_data", "") == "1";
    if (request.body.empty()) {
      return ErrorResponse("INVALID_FIELD", "Request body must contain Markdown content.", {}, 400);
    }
    try {
      auto parsed = core::markdown::ParseChecklistMarkdown(checklist, request.body);
      if (!checklist.empty() && parsed.checklist != checklist) {
        return ErrorResponse("INVALID_FIELD", "Checklist heading does not match query parameter.",
                             json{{"expected", checklist}, {"found", parsed.checklist}}, 400);
      }
      AssignImportOrder(parsed.slugs);
      const std::string instance_id = core::ComputeInstanceId(instance_principal);
      const bool update_template_fields =
          instance_id == core::ComputeInstanceId("template||default");
      EntityResolution resolution;
      if (const auto err = resolve_entity(std::nullopt, resolution)) {
        return *err;
      }
      json warnings = BuildUnicodeWarnings(parsed);
      std::size_t created = 0;
      for (auto slug : parsed.slugs) {
        slug.instance_id = instance_id;
        slug.instance_principal = instance_principal;
        slug.address_id = core::ComposeAddressId(slug.slug_id, slug.instance_id);
        slug.entity_principal = resolution.principal;
        slug.entity_id = resolution.entity_id;
        if (store.CreateSlugIfMissing(slug)) {
          ++created;
        }
        if (update_template_fields) {
          store.UpdateTemplateFieldsForSlug(slug);
        }
      }

      std::vector<std::string> template_subjects;
      template_subjects.reserve(parsed.slugs.size());
      for (const auto& slug : parsed.slugs) {
        if (!slug.slug_id.empty()) {
          template_subjects.push_back(slug.slug_id);
        }
      }
      const auto merged_template_relationships = MergeTemplateRelationshipsWithLineage(
          store, parsed.checklist, template_subjects, parsed.template_relationships);
      store.ReplaceTemplateRelationshipsForSubjects(template_subjects,
                                                    merged_template_relationships);
      const auto lineage_aliases =
          BuildSlugLineageAliases(parsed.slugs, merged_template_relationships);
      for (const auto& warning : lineage_aliases.warnings) {
        warnings.push_back(warning);
      }

      std::unordered_map<std::string, std::vector<core::RelationshipEdge>> address_edges;
      address_edges.reserve(parsed.slugs.size() + parsed.address_relationships.size());
      for (const auto& slug : parsed.slugs) {
        const std::string subject_address = core::ComposeAddressId(slug.slug_id, instance_id);
        address_edges.emplace(subject_address, std::vector<core::RelationshipEdge>{});
      }
      for (const auto& rel : merged_template_relationships) {
        const std::string subject_address = core::ComposeAddressId(rel.subject_slug_id, instance_id);
        const auto target_slug_id = ResolveTemplateTargetForInstance(rel, lineage_aliases);
        if (!target_slug_id) {
          continue;
        }
        const std::string target_address = core::ComposeAddressId(*target_slug_id, instance_id);
        if (address_edges.find(subject_address) == address_edges.end()) {
          warnings.push_back(
              {{"code", "MISSING_SUBJECT"},
               {"message", "Subject slug not imported; skipping derived edge."},
               {"details", {{"slug_id", rel.subject_slug_id}}}});
          continue;
        }
        if (!store.HasSlugForInstance(rel.subject_slug_id, instance_id)) {
          warnings.push_back({{"code", "MISSING_SUBJECT"},
                              {"message", "Subject slug does not exist for instance."},
                              {"details", {{"slug_id", rel.subject_slug_id}}}});
          continue;
        }
        if (!store.HasSlugForInstance(*target_slug_id, instance_id)) {
          warnings.push_back({{"code", "MISSING_TARGET"},
                              {"message", "Target slug does not exist for instance."},
                              {"details", {{"slug_id", rel.target_slug_id}}}});
          continue;
        }
        address_edges[subject_address].push_back(
            core::RelationshipEdge{rel.predicate, target_address});
      }

      const auto split_address = [](const std::string& address_id)
          -> std::optional<std::pair<std::string, std::string>> {
        if (!core::IsValidBase32Id(address_id, 32)) {
          return std::nullopt;
        }
        return std::make_pair(address_id.substr(0, 16), address_id.substr(16, 16));
      };
      for (const auto& rel : parsed.address_relationships) {
        const auto subject_split = split_address(rel.subject_address_id);
        const auto target_split = split_address(rel.target_address_id);
        if (!subject_split || !target_split) {
          warnings.push_back({{"code", "INVALID_ADDRESS"},
                              {"message", "Address relationship has invalid address_id."},
                              {"details",
                               {{"subject_address_id", rel.subject_address_id},
                                {"target_address_id", rel.target_address_id}}}});
          continue;
        }
        if (!store.HasSlugForInstance(subject_split->first, subject_split->second)) {
          warnings.push_back({{"code", "MISSING_SUBJECT"},
                              {"message", "Address relationship subject does not exist."},
                              {"details", {{"address_id", rel.subject_address_id}}}});
          continue;
        }
        if (!store.HasSlugForInstance(target_split->first, target_split->second)) {
          warnings.push_back({{"code", "MISSING_TARGET"},
                              {"message", "Address relationship target does not exist."},
                              {"details", {{"address_id", rel.target_address_id}}}});
          continue;
        }
        address_edges[rel.subject_address_id].push_back(
            core::RelationshipEdge{rel.predicate, rel.target_address_id});
      }

      for (auto& entry : address_edges) {
        auto& edges = entry.second;
        std::sort(edges.begin(), edges.end(), [](const core::RelationshipEdge& a,
                                                 const core::RelationshipEdge& b) {
          if (a.predicate != b.predicate) return a.predicate < b.predicate;
          return a.target < b.target;
        });
        edges.erase(std::unique(edges.begin(), edges.end(),
                                [](const core::RelationshipEdge& a,
                                   const core::RelationshipEdge& b) {
                                  return a.predicate == b.predicate && a.target == b.target;
                                }),
                    edges.end());
      }
      for (const auto& entry : address_edges) {
        store.ReplaceRelationships(entry.first, entry.second);
      }

      if (apply_data) {
        std::vector<core::SlugUpdate> updates;
        updates.reserve(parsed.slugs.size());
        for (const auto& slug : parsed.slugs) {
          core::SlugUpdate update;
          update.address_id = core::ComposeAddressId(slug.slug_id, instance_id);
          update.result = slug.result;
          update.status = slug.status;
          update.comment = slug.comment;
          update.entity_principal_override = resolution.principal;
          update.entity_id_override = resolution.entity_id;
          updates.push_back(update);
        }
        store.ApplyBulkUpdates(updates);
      }

      LogInfo("POST /api/v1/import/markdown checklist=" + parsed.checklist +
              " instance=" + instance_id);
      json data{{"checklist", parsed.checklist},
                {"instance_id", instance_id},
                {"imported", parsed.slugs.size()},
                {"created", created}};
      return OkResponse(data, 200, warnings);
    } catch (const std::exception& ex) {
      return ErrorResponse("INVALID_FIELD", ex.what(), {}, 400);
    }
  };

  auto handle_local_settings_get = [](const platform::HttpRequest& request) {
    if (!IsLocalSettingsRequestAllowed(request)) {
      return ErrorResponse("FORBIDDEN", "Local settings are only editable from loopback origins.",
                           json{{"remote_address", request.remote_address}}, 403);
    }

    json settings = json::object();
    settings["checklist_paths"] = json::array();
    std::string error;
    const auto raw = ReadUtf8FileLimited(GetLocalSettingsPath(), 256 * 1024, &error);
    if (raw && !raw->empty()) {
      const auto parsed = json::parse(*raw, nullptr, false);
      if (!parsed.is_discarded() && parsed.is_object()) {
        settings = parsed;
      }
    }
    if (!settings.contains("checklist_paths") || !settings["checklist_paths"].is_array()) {
      settings["checklist_paths"] = json::array();
    }

    const auto workspace_roots = LoadChecklistWorkspaceRoots();
    return OkResponse(json{{"settings_path", GetLocalSettingsPath().string()},
                           {"checklist_paths", settings["checklist_paths"]},
                           {"effective_roots", ChecklistWorkspaceRootsToJson(workspace_roots)}},
                      200);
  };

  auto handle_local_settings_post = [](const platform::HttpRequest& request) {
    if (!IsLocalSettingsRequestAllowed(request)) {
      return ErrorResponse("FORBIDDEN", "Local settings are only editable from loopback origins.",
                           json{{"remote_address", request.remote_address}}, 403);
    }
    const auto payload = json::parse(request.body, nullptr, false);
    if (payload.is_discarded() || !payload.is_object()) {
      return ErrorResponse("INVALID_JSON", "Invalid JSON payload.", {}, 400);
    }
    const auto paths_it = payload.find("checklist_paths");
    if (paths_it == payload.end() || !paths_it->is_array()) {
      return ErrorResponse("INVALID_FIELD", "checklist_paths array is required.", {}, 400);
    }

    const auto primary_root = NormalizeChecklistRootCandidate(GetLibraryRoot().string());
    const std::string primary_key = primary_root.empty() ? "" : primary_root.string();
    std::unordered_set<std::string> seen_paths;
    if (!primary_key.empty()) {
      seen_paths.insert(primary_key);
    }

    json checklist_paths = json::array();
    int index = 1;
    for (const auto& entry : *paths_it) {
      std::string name = "extra_" + std::to_string(index);
      std::string raw_path;
      if (entry.is_string()) {
        raw_path = entry.get<std::string>();
      } else if (entry.is_object()) {
        name = entry.value("name", name);
        raw_path = entry.value("path", "");
      } else {
        return ErrorResponse("INVALID_FIELD", "Each checklist_paths entry must be a string or object.",
                             json{{"index", index}}, 400);
      }
      ++index;
      name = TrimString(name);
      raw_path = TrimString(raw_path);
      if (name.empty() || !core::IsSafePackToken(name)) {
        return ErrorResponse("INVALID_FIELD",
                             "checklist_paths name must be a plain source token.",
                             json{{"name", name}}, 400);
      }
      if (raw_path.empty()) {
        continue;
      }
      const auto normalized = NormalizeChecklistRootCandidate(raw_path);
      if (normalized.empty()) {
        return ErrorResponse("INVALID_FIELD",
                             "Checklist path must exist and either be a checklists folder or contain one.",
                             json{{"path", raw_path}}, 400);
      }
      const std::string path_key = normalized.string();
      if (seen_paths.find(path_key) != seen_paths.end()) {
        return ErrorResponse("INVALID_FIELD", "Duplicate checklist path.",
                             json{{"path", path_key}}, 400);
      }
      seen_paths.insert(path_key);
      checklist_paths.push_back({{"name", name}, {"path", path_key}});
    }

    json settings = payload;
    settings["checklist_paths"] = checklist_paths;
    std::string error;
    if (!WriteUtf8File(GetLocalSettingsPath(), settings.dump(2) + "\n", &error)) {
      return ErrorResponse("INTERNAL_ERROR", error, {}, 500);
    }

    const auto workspace_roots = LoadChecklistWorkspaceRoots();
    return OkResponse(json{{"settings_path", GetLocalSettingsPath().string()},
                           {"checklist_paths", checklist_paths},
                           {"effective_roots", ChecklistWorkspaceRootsToJson(workspace_roots)}},
                      200);
  };

  auto handle_workspace_markdown_templates = [](const platform::HttpRequest&) {
    const auto workspace_roots = LoadChecklistWorkspaceRoots();

    json items = json::array();
    std::unordered_map<std::string, int> checklist_counts;
    for (const auto& workspace_root : workspace_roots) {
      if (workspace_root.primary) {
        std::error_code ec;
        std::filesystem::create_directories(workspace_root.root, ec);
      }
      for (const auto& entry : core::ListChecklistMarkdown(workspace_root.root)) {
        const auto path = entry.root / "checklist.md";
        json item;
        item["source_name"] = workspace_root.source_name;
        item["source_path"] = workspace_root.root.string();
        item["source_primary"] = workspace_root.primary;
        item["pack"] = entry.pack;
        item["checklist"] = entry.checklist;
        item["filename"] = "checklist.md";
        item["rel_path"] = entry.pack + "/" + entry.checklist + "/checklist.md";
        item["path"] = path.string();

        std::error_code sz_ec;
        const auto bytes = std::filesystem::file_size(path, sz_ec);
        item["bytes"] = sz_ec ? 0 : static_cast<std::uintmax_t>(bytes);

        std::string error;
        const auto content = ReadUtf8FileLimited(path, 2 * 1024 * 1024, &error);
        if (!content) {
          item["valid"] = false;
          item["error"] = error;
          checklist_counts[item.value("checklist", "")]++;
          items.push_back(item);
          continue;
        }
        try {
          const auto parsed = core::markdown::ParseChecklistMarkdown("", *content);
          const auto unicode_warnings = BuildUnicodeWarnings(parsed);
          item["valid"] = true;
          item["parsed_checklist"] = parsed.checklist;
          item["slugs"] = parsed.slugs.size();
          item["template_relationships"] = parsed.template_relationships.size();
          if (!unicode_warnings.empty()) {
            item["warnings"] = unicode_warnings;
          }
          item["error"] = nullptr;
        } catch (const std::exception& ex) {
          item["valid"] = false;
          item["error"] = ex.what();
        }
        const std::string checklist_key =
            item.value("parsed_checklist", item.value("checklist", ""));
        if (!checklist_key.empty()) {
          checklist_counts[checklist_key]++;
        }
        items.push_back(item);
      }
    }

    for (auto& item : items) {
      const std::string checklist_key =
          item.value("parsed_checklist", item.value("checklist", ""));
      const auto count_it = checklist_counts.find(checklist_key);
      if (count_it != checklist_counts.end() && count_it->second > 1) {
        item["duplicate_checklist_id"] = true;
      }
    }

    LogInfo("GET /api/v1/workspace/markdown/templates roots=" +
            std::to_string(workspace_roots.size()) + " count=" + std::to_string(items.size()));
    const std::string primary_root =
        workspace_roots.empty() ? GetLibraryRoot().string() : workspace_roots.front().root.string();
    json data{{"templates_root", primary_root},
              {"template_roots", ChecklistWorkspaceRootsToJson(workspace_roots)},
              {"items", items}};
    return OkResponse(data, 200);
  };

  auto handle_workspace_scripts_list = [](const platform::HttpRequest& request) {
    const std::string checklist = TrimString(GetQueryParam(request, "checklist", ""));
    const std::string pack = TrimString(GetQueryParam(request, "pack", ""));
    const std::string source_name = TrimString(GetQueryParam(request, "source_name", ""));
    if (checklist.empty()) {
      return ErrorResponse("INVALID_FIELD", "checklist is required.", {}, 400);
    }

    const auto workspace_roots = LoadChecklistWorkspaceRoots();
    std::string resolve_error;
    json resolve_details = json::object();
    const auto resolved_root = ResolveChecklistRoot(workspace_roots, checklist, pack, source_name,
                                                    false, &resolve_error, &resolve_details);
    if (!resolved_root) {
      return ErrorResponse("INVALID_FIELD",
                           resolve_error.empty() ? "Invalid pack or checklist." : resolve_error,
                           resolve_details, 400);
    }
    const auto catalog =
        LoadWorkspaceScriptsCatalog(resolved_root->library_root, checklist, resolved_root->pack,
                                    &resolve_error, &resolve_details);
    if (!catalog) {
      return ErrorResponse("INVALID_FIELD",
                           resolve_error.empty() ? "Invalid pack or checklist." : resolve_error,
                           resolve_details, 400);
    }

    json items = json::array();
    for (const auto& entry : catalog->entries) {
      items.push_back(WorkspaceScriptEntryToJson(entry));
    }
    LogInfo("GET /api/v1/workspace/scripts checklist=" + checklist + " pack=" + catalog->pack +
            " source=" + resolved_root->source_name + " count=" + std::to_string(items.size()));
    json data{{"templates_root", resolved_root->library_root.string()},
              {"source_name", resolved_root->source_name},
              {"source_path", resolved_root->library_root.string()},
              {"pack", catalog->pack},
              {"checklist", catalog->checklist},
              {"checklist_root", catalog->checklist_root.string()},
              {"scripts_root", catalog->scripts_root.string()},
              {"manifest_path", catalog->manifest_path.string()},
              {"valid", catalog->valid},
              {"error", catalog->error.empty() ? json(nullptr) : json(catalog->error)},
              {"items", items}};
    return OkResponse(data, 200);
  };

  auto handle_workspace_scripts_run = [&config](const platform::HttpRequest& request) {
    const auto payload = json::parse(request.body, nullptr, false);
    if (payload.is_discarded() || !payload.is_object()) {
      return ErrorResponse("INVALID_JSON", "Invalid JSON payload.", {}, 400);
    }

    if (!payload.contains("checklist") || !payload["checklist"].is_string()) {
      return ErrorResponse("INVALID_FIELD", "checklist is required.", {}, 400);
    }
    if (!payload.contains("script_id") || !payload["script_id"].is_string()) {
      return ErrorResponse("INVALID_FIELD", "script_id is required.", {}, 400);
    }
    const std::string checklist = TrimString(payload.value("checklist", ""));
    const std::string pack = TrimString(payload.value("pack", ""));
    const std::string source_name = TrimString(payload.value("source_name", ""));
    const std::string script_id = TrimString(payload.value("script_id", ""));
    if (checklist.empty() || script_id.empty()) {
      return ErrorResponse("INVALID_FIELD", "checklist and script_id are required.", {}, 400);
    }
    const bool dry_run = payload.value("dry_run", false);

    std::vector<std::string> request_args;
    if (payload.contains("args")) {
      std::string parse_error;
      const auto parsed = ParseStringArrayField(payload, "args", &parse_error);
      if (!parsed && !parse_error.empty()) {
        return ErrorResponse("INVALID_FIELD", parse_error, {}, 400);
      }
      if (parsed) {
        request_args = *parsed;
      }
    }
    for (const auto& arg : request_args) {
      if (arg.size() > 4096) {
        return ErrorResponse("INVALID_FIELD", "Script argument exceeds 4096 characters.", {}, 400);
      }
    }

    auto read_optional_string = [&payload](const char* key) -> std::optional<std::string> {
      const auto it = payload.find(key);
      if (it == payload.end() || it->is_null()) {
        return std::nullopt;
      }
      if (!it->is_string()) {
        throw std::invalid_argument(std::string("Field '") + key + "' must be a string when provided.");
      }
      return TrimString(it->get<std::string>());
    };

    std::optional<std::string> instance_id;
    std::optional<std::string> instance_principal;
    try {
      instance_id = read_optional_string("instance_id");
      instance_principal = read_optional_string("instance_principal");
    } catch (const std::exception& ex) {
      return ErrorResponse("INVALID_FIELD", ex.what(), {}, 400);
    }

    const auto workspace_roots = LoadChecklistWorkspaceRoots();
    std::string resolve_error;
    json resolve_details = json::object();
    const auto resolved_root = ResolveChecklistRoot(workspace_roots, checklist, pack, source_name,
                                                    false, &resolve_error, &resolve_details);
    if (!resolved_root) {
      return ErrorResponse("INVALID_FIELD",
                           resolve_error.empty() ? "Invalid pack or checklist." : resolve_error,
                           resolve_details, 400);
    }
    const auto catalog =
        LoadWorkspaceScriptsCatalog(resolved_root->library_root, checklist, resolved_root->pack,
                                    &resolve_error, &resolve_details);
    if (!catalog) {
      return ErrorResponse("INVALID_FIELD",
                           resolve_error.empty() ? "Invalid pack or checklist." : resolve_error,
                           resolve_details, 400);
    }
    if (!catalog->valid) {
      return ErrorResponse("INVALID_FIELD",
                           catalog->error.empty() ? "Scripts folder is not ready." : catalog->error,
                           json{{"checklist", checklist},
                                {"pack", catalog->pack},
                                {"scripts_root", catalog->scripts_root.string()},
                                {"manifest_path", catalog->manifest_path.string()}},
                           400);
    }

    const auto script_it =
        std::find_if(catalog->entries.begin(), catalog->entries.end(),
                     [&script_id](const WorkspaceScriptEntry& entry) { return entry.id == script_id; });
    if (script_it == catalog->entries.end()) {
      return ErrorResponse("NOT_FOUND", "Script id not found.",
                           json{{"script_id", script_id}, {"checklist", checklist}}, 404);
    }
    if (!script_it->enabled) {
      return ErrorResponse("INVALID_FIELD", "Script is disabled in scripts.json.",
                           json{{"script_id", script_id}}, 400);
    }
    if (!script_it->valid) {
      return ErrorResponse("INVALID_FIELD",
                           script_it->error.empty() ? "Script entry is invalid." : script_it->error,
                           json{{"script_id", script_id}}, 400);
    }

    std::filesystem::path executable;
    std::vector<std::string> args;
    std::string command_error;
    if (!ResolveWorkspaceScriptCommand(*script_it, catalog->scripts_root, &executable, &args,
                                       &command_error)) {
      return ErrorResponse("INVALID_FIELD",
                           command_error.empty() ? "Unable to resolve script command."
                                                 : command_error,
                           json{{"script_id", script_id}}, 400);
    }
    args.insert(args.end(), request_args.begin(), request_args.end());

    const std::string base_url = config.base_url.empty()
                                     ? ("http://" + config.host + ":" + std::to_string(config.port))
                                     : config.base_url;
    std::vector<std::pair<std::string, std::string>> env_overrides = {
        {"CHAX_HOST", config.host},
        {"CHAX_PORT", std::to_string(config.port)},
        {"CHAX_BASE_URL", base_url},
        {"CHAX_SOURCE_NAME", resolved_root->source_name},
        {"CHAX_CHECKLISTS_ROOT", resolved_root->library_root.string()},
        {"CHAX_CHECKLIST", checklist},
        {"CHAX_PACK", catalog->pack},
        {"CHAX_SCRIPT_ID", script_id},
    };
    if (instance_id && !instance_id->empty()) {
      env_overrides.push_back({"CHAX_INSTANCE_ID", *instance_id});
    }
    if (instance_principal && !instance_principal->empty()) {
      env_overrides.push_back({"CHAX_INSTANCE_PRINCIPAL", *instance_principal});
    }

    std::filesystem::path stdout_log;
    std::filesystem::path stderr_log;
    if (!dry_run) {
      std::filesystem::path logs_root = catalog->checklist_root / "logs";
      std::error_code ec;
      std::filesystem::create_directories(logs_root, ec);
      if (ec) {
        return ErrorResponse("INTERNAL_ERROR", "Failed to create script log directory.",
                             json{{"path", logs_root.string()}, {"message", ec.message()}}, 500);
      }
      const std::string run_token = SanitizeTokenForFileName(catalog->pack) + "-" +
                                    SanitizeTokenForFileName(checklist) + "-" +
                                    SanitizeTokenForFileName(script_id) + "-" +
                                    UtcTimestampForFileName();
      stdout_log = logs_root / ("script-" + run_token + ".out.log");
      stderr_log = logs_root / ("script-" + run_token + ".err.log");
    }

    json data{{"source_name", resolved_root->source_name},
              {"source_path", resolved_root->library_root.string()},
              {"pack", catalog->pack},
              {"checklist", checklist},
              {"script_id", script_id},
              {"script", WorkspaceScriptEntryToJson(*script_it)},
              {"executable", executable.string()},
              {"args", args},
              {"working_directory", catalog->scripts_root.string()},
              {"dry_run", dry_run},
              {"env_context",
               {{"instance_id", instance_id ? json(*instance_id) : json(nullptr)},
                {"instance_principal", instance_principal ? json(*instance_principal)
                                                           : json(nullptr)}}},
              {"stdout_log", stdout_log.empty() ? json(nullptr) : json(stdout_log.string())},
              {"stderr_log", stderr_log.empty() ? json(nullptr) : json(stderr_log.string())}};

    if (dry_run) {
      LogInfo("POST /api/v1/workspace/scripts/run dry_run checklist=" + checklist +
              " source=" + resolved_root->source_name + " script_id=" + script_id);
      return OkResponse(data, 200);
    }

    int pid = -1;
    std::string launch_error;
    const bool launched = platform::LaunchDetachedWithEnv(
        executable, args, catalog->scripts_root, stdout_log, stderr_log, env_overrides, &pid,
        &launch_error);
    if (!launched) {
      return ErrorResponse("INTERNAL_ERROR",
                           launch_error.empty() ? "Failed to launch script." : launch_error,
                           json{{"script_id", script_id},
                                {"executable", executable.string()},
                                {"working_directory", catalog->scripts_root.string()}},
                           500);
    }
    data["pid"] = pid;
    data["status"] = "launched";
    LogInfo("POST /api/v1/workspace/scripts/run checklist=" + checklist +
            " source=" + resolved_root->source_name + " script_id=" + script_id +
            " pid=" + std::to_string(pid));
    return OkResponse(data, 202);
  };

  auto import_workspace_markdown_file = [&store,
                                         resolve_entity](const ChecklistRootResolution &resolved_root,
                                                         const WorkspaceMarkdownImportOptions &options, json *data_out,
                                                         json *warnings_out) -> std::optional<platform::HttpResponse> {
    const auto resolved = resolved_root.root / "checklist.md";
    std::string error;
    const auto content = ReadUtf8FileLimited(resolved, 2 * 1024 * 1024, &error);
    if (!content) {
      return ErrorResponse("NOT_FOUND", error,
                           json{{"source_name", resolved_root.source_name},
                                {"pack", resolved_root.pack},
                                {"checklist", resolved_root.checklist_dir}},
                           404);
    }

    try {
      auto parsed = core::markdown::ParseChecklistMarkdown("", *content);
      const std::string instance_id = core::ComputeInstanceId(options.template_principal);
      AssignImportOrder(parsed.slugs);
      const auto ownership = OwnershipFromResolution(resolved_root, parsed.checklist);

      EntityResolution resolution;
      if (const auto err = resolve_entity(std::nullopt, resolution)) {
        return err;
      }

      json warnings = BuildUnicodeWarnings(parsed);
      if (options.replace_instance) {
        store.DeleteOwnedInstance(parsed.checklist, instance_id, ownership);
      }

      for (auto slug : parsed.slugs) {
        slug.instance_id = instance_id;
        slug.instance_principal = options.template_principal;
        slug.address_id = core::ComposeAddressId(slug.slug_id, slug.instance_id);
        slug.entity_principal = resolution.principal;
        slug.entity_id = resolution.entity_id;
        store.UpsertSlug(slug);
        store.UpsertOwnership(slug, ownership);
      }

      std::vector<std::string> subjects;
      subjects.reserve(parsed.slugs.size());
      for (const auto &slug : parsed.slugs) {
        if (!slug.slug_id.empty())
          subjects.push_back(slug.slug_id);
      }
      const auto merged_template_relationships =
          MergeTemplateRelationshipsWithLineage(store, parsed.checklist, subjects, parsed.template_relationships);
      store.ReplaceTemplateRelationshipsForSubjects(subjects, merged_template_relationships);
      const auto lineage_aliases =
          BuildSlugLineageAliases(parsed.slugs, merged_template_relationships);
      for (const auto& warning : lineage_aliases.warnings) {
        warnings.push_back(warning);
      }

      std::unordered_map<std::string, std::vector<core::RelationshipEdge>> address_edges;
      address_edges.reserve(parsed.slugs.size() + parsed.address_relationships.size());
      for (const auto &slug : parsed.slugs) {
        const std::string subject_address = core::ComposeAddressId(slug.slug_id, instance_id);
        address_edges.emplace(subject_address, std::vector<core::RelationshipEdge>{});
      }
      for (const auto &rel : merged_template_relationships) {
        const std::string subject_address = core::ComposeAddressId(rel.subject_slug_id, instance_id);
        const auto target_slug_id = ResolveTemplateTargetForInstance(rel, lineage_aliases);
        if (!target_slug_id) {
          continue;
        }
        const std::string target_address = core::ComposeAddressId(*target_slug_id, instance_id);
        if (address_edges.find(subject_address) == address_edges.end()) {
          warnings.push_back({{"code", "MISSING_SUBJECT"},
                              {"message", "Subject slug not imported; skipping derived edge."},
                              {"details", {{"slug_id", rel.subject_slug_id}}}});
          continue;
        }
        if (!store.HasSlugForInstance(rel.subject_slug_id, instance_id)) {
          warnings.push_back({{"code", "MISSING_SUBJECT"},
                              {"message", "Subject slug does not exist for instance."},
                              {"details", {{"slug_id", rel.subject_slug_id}}}});
          continue;
        }
        if (!store.HasSlugForInstance(*target_slug_id, instance_id)) {
          warnings.push_back({{"code", "MISSING_TARGET"},
                              {"message", "Target slug does not exist for instance."},
                              {"details", {{"slug_id", rel.target_slug_id}}}});
          continue;
        }
        address_edges[subject_address].push_back(core::RelationshipEdge{rel.predicate, target_address});
      }

      const auto split_address =
          [](const std::string &address_id) -> std::optional<std::pair<std::string, std::string>> {
        if (!core::IsValidBase32Id(address_id, 32)) {
          return std::nullopt;
        }
        return std::make_pair(address_id.substr(0, 16), address_id.substr(16, 16));
      };
      for (const auto &rel : parsed.address_relationships) {
        const auto subject_split = split_address(rel.subject_address_id);
        const auto target_split = split_address(rel.target_address_id);
        if (!subject_split || !target_split) {
          warnings.push_back(
              {{"code", "INVALID_ADDRESS"},
               {"message", "Address relationship has invalid address_id."},
               {"details",
                {{"subject_address_id", rel.subject_address_id}, {"target_address_id", rel.target_address_id}}}});
          continue;
        }
        if (!store.HasSlugForInstance(subject_split->first, subject_split->second)) {
          warnings.push_back({{"code", "MISSING_SUBJECT"},
                              {"message", "Address relationship subject does not exist."},
                              {"details", {{"address_id", rel.subject_address_id}}}});
          continue;
        }
        if (!store.HasSlugForInstance(target_split->first, target_split->second)) {
          warnings.push_back({{"code", "MISSING_TARGET"},
                              {"message", "Address relationship target does not exist."},
                              {"details", {{"address_id", rel.target_address_id}}}});
          continue;
        }
        address_edges[rel.subject_address_id].push_back(core::RelationshipEdge{rel.predicate, rel.target_address_id});
      }

      for (auto &entry : address_edges) {
        auto &edges = entry.second;
        std::sort(edges.begin(), edges.end(), [](const core::RelationshipEdge &a, const core::RelationshipEdge &b) {
          if (a.predicate != b.predicate)
            return a.predicate < b.predicate;
          return a.target < b.target;
        });
        edges.erase(std::unique(edges.begin(), edges.end(),
                                [](const core::RelationshipEdge &a, const core::RelationshipEdge &b) {
                                  return a.predicate == b.predicate && a.target == b.target;
                                }),
                    edges.end());
      }
      for (const auto &entry : address_edges) {
        store.ReplaceRelationships(entry.first, entry.second);
      }

      std::string apply_instance_id;
      if (options.apply_data) {
        apply_instance_id = instance_id;
        if (!options.apply_instance_id_payload.empty()) {
          apply_instance_id = options.apply_instance_id_payload;
        } else if (!options.apply_instance_principal.empty()) {
          apply_instance_id = core::ComputeInstanceId(options.apply_instance_principal);
        }
        std::vector<core::SlugUpdate> updates;
        updates.reserve(parsed.slugs.size());
        for (const auto &slug : parsed.slugs) {
          core::SlugUpdate update;
          update.address_id = core::ComposeAddressId(slug.slug_id, apply_instance_id);
          update.result = slug.result;
          update.status = slug.status;
          update.comment = slug.comment;
          update.entity_principal_override = resolution.principal;
          update.entity_id_override = resolution.entity_id;
          updates.push_back(update);
        }
        store.ApplyBulkUpdates(updates);
        for (auto slug : parsed.slugs) {
          if (!store.HasSlugForInstance(slug.slug_id, apply_instance_id)) {
            continue;
          }
          slug.instance_id = apply_instance_id;
          slug.address_id = core::ComposeAddressId(slug.slug_id, apply_instance_id);
          store.UpsertOwnership(slug, ownership);
        }
      }

      std::string log_line = "workspace markdown import source=" + resolved_root.source_name +
                             " pack=" + resolved_root.pack + " checklist_dir=" + resolved_root.checklist_dir +
                             " checklist=" + parsed.checklist + " instance=" + instance_id;
      if (options.apply_data) {
        log_line += " apply_instance=" + apply_instance_id;
      }
      LogInfo(log_line);
      json data{{"source_name", resolved_root.source_name},
                {"source_path", resolved_root.library_root.string()},
                {"pack", resolved_root.pack},
                {"checklist_dir", resolved_root.checklist_dir},
                {"checklist", parsed.checklist},
                {"instance_id", instance_id},
                {"imported", parsed.slugs.size()},
                {"template_relationships", parsed.template_relationships.size()}};
      if (options.apply_data) {
        data["apply_instance_id"] = apply_instance_id;
      }
      *data_out = std::move(data);
      *warnings_out = std::move(warnings);
      return std::nullopt;
    } catch (const std::exception &ex) {
      return ErrorResponse("INVALID_FIELD", ex.what(), {}, 400);
    }
  };

  auto handle_workspace_markdown_import =
      [&store, resolve_entity](const platform::HttpRequest& request) {
        const auto payload = json::parse(request.body, nullptr, false);
        if (payload.is_discarded()) {
          return ErrorResponse("INVALID_JSON", "Invalid JSON payload.", {}, 400);
        }
        const std::string pack = payload.value("pack", "");
        const std::string source_name = payload.value("source_name", "");
        const std::string checklist_dir = payload.value("checklist", "");
        const std::string template_principal = payload.value("instance_principal", "template||default");
        const std::string apply_instance_principal = payload.value("apply_instance_principal", "");
        const std::string apply_instance_id_payload = payload.value("apply_instance_id", "");
        const bool apply_data = payload.value("apply_data", false);
        const bool replace_instance = payload.value("replace_instance", true);

        if (pack.empty() || checklist_dir.empty()) {
          return ErrorResponse("INVALID_FIELD", "pack and checklist are required.",
                               json{{"pack", pack}, {"checklist", checklist_dir}}, 400);
        }
        const auto workspace_roots = LoadChecklistWorkspaceRoots();
        std::string resolve_error;
        json resolve_details = json::object();
        const auto resolved_root = ResolveChecklistRoot(workspace_roots, checklist_dir, pack,
                                                        source_name, false, &resolve_error,
                                                        &resolve_details);
        if (!resolved_root) {
          return ErrorResponse("INVALID_FIELD",
                               resolve_error.empty() ? "Invalid pack or checklist." : resolve_error,
                               resolve_details, 400);
        }
        const auto resolved = resolved_root->root / "checklist.md";
        std::string error;
        const auto content = ReadUtf8FileLimited(resolved, 2 * 1024 * 1024, &error);
        if (!content) {
          return ErrorResponse("NOT_FOUND", error,
                               json{{"source_name", resolved_root->source_name},
                                    {"pack", resolved_root->pack},
                                    {"checklist", checklist_dir}},
                               404);
        }

        try {
          auto parsed = core::markdown::ParseChecklistMarkdown("", *content);
          const std::string instance_id = core::ComputeInstanceId(template_principal);
          AssignImportOrder(parsed.slugs);
          const auto ownership = OwnershipFromResolution(*resolved_root, parsed.checklist);

          EntityResolution resolution;
          if (const auto err = resolve_entity(std::nullopt, resolution)) {
            return *err;
          }

          json warnings = BuildUnicodeWarnings(parsed);
          if (replace_instance) {
            store.DeleteOwnedInstance(parsed.checklist, instance_id, ownership);
          }

          for (auto slug : parsed.slugs) {
            slug.instance_id = instance_id;
            slug.instance_principal = template_principal;
            slug.address_id = core::ComposeAddressId(slug.slug_id, slug.instance_id);
            slug.entity_principal = resolution.principal;
            slug.entity_id = resolution.entity_id;
            store.UpsertSlug(slug);
            store.UpsertOwnership(slug, ownership);
          }

          std::vector<std::string> subjects;
          subjects.reserve(parsed.slugs.size());
          for (const auto& slug : parsed.slugs) {
            if (!slug.slug_id.empty()) subjects.push_back(slug.slug_id);
          }
          const auto merged_template_relationships = MergeTemplateRelationshipsWithLineage(
              store, parsed.checklist, subjects, parsed.template_relationships);
          store.ReplaceTemplateRelationshipsForSubjects(subjects, merged_template_relationships);
          const auto lineage_aliases =
              BuildSlugLineageAliases(parsed.slugs, merged_template_relationships);
          for (const auto& warning : lineage_aliases.warnings) {
            warnings.push_back(warning);
          }

          std::unordered_map<std::string, std::vector<core::RelationshipEdge>> address_edges;
          address_edges.reserve(parsed.slugs.size() + parsed.address_relationships.size());
          for (const auto& slug : parsed.slugs) {
            const std::string subject_address = core::ComposeAddressId(slug.slug_id, instance_id);
            address_edges.emplace(subject_address, std::vector<core::RelationshipEdge>{});
          }
            for (const auto& rel : merged_template_relationships) {
              const std::string subject_address =
                  core::ComposeAddressId(rel.subject_slug_id, instance_id);
              const auto target_slug_id = ResolveTemplateTargetForInstance(rel, lineage_aliases);
              if (!target_slug_id) {
                continue;
              }
              const std::string target_address =
                  core::ComposeAddressId(*target_slug_id, instance_id);
              if (address_edges.find(subject_address) == address_edges.end()) {
              warnings.push_back({{"code", "MISSING_SUBJECT"},
                                  {"message", "Subject slug not imported; skipping derived edge."},
                                  {"details", {{"slug_id", rel.subject_slug_id}}}});
              continue;
            }
            if (!store.HasSlugForInstance(rel.subject_slug_id, instance_id)) {
              warnings.push_back({{"code", "MISSING_SUBJECT"},
                                  {"message", "Subject slug does not exist for instance."},
                                  {"details", {{"slug_id", rel.subject_slug_id}}}});
              continue;
            }
            if (!store.HasSlugForInstance(*target_slug_id, instance_id)) {
              warnings.push_back({{"code", "MISSING_TARGET"},
                                  {"message", "Target slug does not exist for instance."},
                                  {"details", {{"slug_id", rel.target_slug_id}}}});
              continue;
            }
            address_edges[subject_address].push_back(
                core::RelationshipEdge{rel.predicate, target_address});
          }

          const auto split_address = [](const std::string& address_id)
              -> std::optional<std::pair<std::string, std::string>> {
            if (!core::IsValidBase32Id(address_id, 32)) {
              return std::nullopt;
            }
            return std::make_pair(address_id.substr(0, 16), address_id.substr(16, 16));
          };
          for (const auto& rel : parsed.address_relationships) {
            const auto subject_split = split_address(rel.subject_address_id);
            const auto target_split = split_address(rel.target_address_id);
            if (!subject_split || !target_split) {
              warnings.push_back({{"code", "INVALID_ADDRESS"},
                                  {"message", "Address relationship has invalid address_id."},
                                  {"details",
                                   {{"subject_address_id", rel.subject_address_id},
                                    {"target_address_id", rel.target_address_id}}}});
              continue;
            }
            if (!store.HasSlugForInstance(subject_split->first, subject_split->second)) {
              warnings.push_back({{"code", "MISSING_SUBJECT"},
                                  {"message", "Address relationship subject does not exist."},
                                  {"details", {{"address_id", rel.subject_address_id}}}});
              continue;
            }
            if (!store.HasSlugForInstance(target_split->first, target_split->second)) {
              warnings.push_back({{"code", "MISSING_TARGET"},
                                  {"message", "Address relationship target does not exist."},
                                  {"details", {{"address_id", rel.target_address_id}}}});
              continue;
            }
            address_edges[rel.subject_address_id].push_back(
                core::RelationshipEdge{rel.predicate, rel.target_address_id});
          }

          for (auto& entry : address_edges) {
            auto& edges = entry.second;
            std::sort(edges.begin(), edges.end(), [](const core::RelationshipEdge& a,
                                                     const core::RelationshipEdge& b) {
              if (a.predicate != b.predicate) return a.predicate < b.predicate;
              return a.target < b.target;
            });
            edges.erase(std::unique(edges.begin(), edges.end(),
                                    [](const core::RelationshipEdge& a,
                                       const core::RelationshipEdge& b) {
                                      return a.predicate == b.predicate && a.target == b.target;
                                    }),
                        edges.end());
          }
          for (const auto& entry : address_edges) {
            store.ReplaceRelationships(entry.first, entry.second);
          }

          std::string apply_instance_id;
          if (apply_data) {
            apply_instance_id = instance_id;
            if (!apply_instance_id_payload.empty()) {
              apply_instance_id = apply_instance_id_payload;
            } else if (!apply_instance_principal.empty()) {
              apply_instance_id = core::ComputeInstanceId(apply_instance_principal);
            }
            std::vector<core::SlugUpdate> updates;
            updates.reserve(parsed.slugs.size());
            for (const auto& slug : parsed.slugs) {
              core::SlugUpdate update;
              update.address_id = core::ComposeAddressId(slug.slug_id, apply_instance_id);
              update.result = slug.result;
              update.status = slug.status;
              update.comment = slug.comment;
              update.entity_principal_override = resolution.principal;
              update.entity_id_override = resolution.entity_id;
              updates.push_back(update);
            }
            store.ApplyBulkUpdates(updates);
            for (auto slug : parsed.slugs) {
              if (!store.HasSlugForInstance(slug.slug_id, apply_instance_id)) {
                continue;
              }
              slug.instance_id = apply_instance_id;
              slug.address_id = core::ComposeAddressId(slug.slug_id, apply_instance_id);
              store.UpsertOwnership(slug, ownership);
            }
          }

          std::string log_line = "POST /api/v1/workspace/markdown/import source=" +
                                 resolved_root->source_name + " pack=" + resolved_root->pack +
                                 " checklist_dir=" + checklist_dir +
                                 " checklist=" + parsed.checklist + " instance=" + instance_id;
          if (apply_data) {
            log_line += " apply_instance=" + apply_instance_id;
          }
          LogInfo(log_line);
          json data{{"source_name", resolved_root->source_name},
                    {"source_path", resolved_root->library_root.string()},
                    {"pack", resolved_root->pack},
                    {"checklist_dir", resolved_root->checklist_dir},
                    {"checklist", parsed.checklist},
                    {"instance_id", instance_id},
                    {"imported", parsed.slugs.size()},
                    {"template_relationships", parsed.template_relationships.size()}};
          if (apply_data) {
            data["apply_instance_id"] = apply_instance_id;
          }
          return OkResponse(data, 200, warnings);
        } catch (const std::exception& ex) {
          return ErrorResponse("INVALID_FIELD", ex.what(), {}, 400);
        }
      };

  auto handle_workspace_markdown_export = [&store](const platform::HttpRequest& request) {
    const auto payload = json::parse(request.body, nullptr, false);
    if (payload.is_discarded()) {
      return ErrorResponse("INVALID_JSON", "Invalid JSON payload.", {}, 400);
    }
    const std::string checklist = payload.value("checklist", "");
    const std::string pack = payload.value("pack", "");
    const std::string source_name = payload.value("source_name", "");
    const std::string checklist_dir_payload = payload.value("checklist_dir", "");
    const bool include_data = payload.value("include_data", false);
    const std::string instance_id = payload.value("instance_id", "");

    if (checklist.empty()) {
      return ErrorResponse("INVALID_FIELD", "checklist is required.", {}, 400);
    }

    std::string export_instance_id = core::ComputeInstanceId("template||default");
    if (!instance_id.empty()) {
      export_instance_id = instance_id;
    } else if (include_data) {
      return ErrorResponse("INVALID_FIELD", "instance_id is required when include_data is true.", {}, 400);
    }

    const auto workspace_roots = LoadChecklistWorkspaceRoots();
    std::string effective_source_name = source_name;
    std::string effective_pack = pack;
    std::string effective_checklist_dir = checklist_dir_payload.empty() ? checklist : checklist_dir_payload;
    if (effective_source_name.empty() && effective_pack.empty() && checklist_dir_payload.empty()) {
      const auto ownerships = store.ListOwnershipsForInstance(checklist, export_instance_id);
      if (ownerships.size() == 1) {
        effective_source_name = ownerships.front().source_name;
        effective_pack = ownerships.front().pack;
        effective_checklist_dir = ownerships.front().checklist_dir;
      } else if (ownerships.size() > 1) {
        return ErrorResponse("AMBIGUOUS_CHECKLIST_OWNERSHIP",
                             "Checklist instance has multiple source/pack owners; specify source_name and pack.",
                             json{{"checklist", checklist},
                                  {"instance_id", export_instance_id},
                                  {"matches", OwnershipsToJson(ownerships)}},
                             400);
      }
    }
    std::string resolve_error;
    json resolve_details = json::object();
    const auto resolved_root = ResolveChecklistRoot(workspace_roots, effective_checklist_dir, effective_pack,
                                                    effective_source_name, true, &resolve_error, &resolve_details);
    if (!resolved_root) {
      return ErrorResponse("INVALID_FIELD",
                           resolve_error.empty() ? "Invalid pack or checklist." : resolve_error,
                           resolve_details, 400);
    }
    const auto resolved = resolved_root->root / "checklist.md";

    try {
      json warnings = json::array();
      const auto ownership = OwnershipFromResolution(*resolved_root, checklist);
      auto slugs = store.QuerySlugs(checklist, export_instance_id, std::nullopt, std::nullopt, std::nullopt,
                                    std::nullopt, ownership);
      if (slugs.empty()) {
        slugs = store.QuerySlugs(checklist, export_instance_id, std::nullopt, std::nullopt, std::nullopt, std::nullopt);
        if (!slugs.empty()) {
          warnings.push_back(
              {{"code", "OWNERSHIP_FILTER_EMPTY"},
               {"message",
                "No rows had persisted ownership for the requested source/pack; exported legacy unowned rows."},
               {"details",
                {{"source_name", resolved_root->source_name},
                 {"pack", resolved_root->pack},
                 {"checklist_dir", resolved_root->checklist_dir}}}});
        }
      }
      if (slugs.empty()) {
        return ErrorResponse("NOT_FOUND", "No slugs found for checklist/instance.",
                             json{{"checklist", checklist}, {"instance_id", export_instance_id}}, 404);
      }
      if (!include_data) {
        for (auto& slug : slugs) {
          slug.result.clear();
          slug.comment.clear();
          slug.status = core::ChecklistStatus::kUnknown;
          slug.timestamp.clear();
          slug.entity_id.clear();
        }
      }
      const auto template_relationships = store.GetTemplateRelationshipsForChecklist(checklist, ownership);
      const auto relationship_mode = include_data
                                         ? core::markdown::RelationshipExportMode::kAddress
                                         : core::markdown::RelationshipExportMode::kTemplate;
      const auto markdown = core::markdown::ExportChecklistMarkdown(
          checklist, slugs, template_relationships, relationship_mode,
          core::markdown::RelationshipIdentityFormat::kId);
      std::string error;
      if (!WriteUtf8File(resolved, markdown, &error)) {
        return ErrorResponse("INTERNAL_ERROR", error, {}, 500);
      }

      LogInfo("POST /api/v1/workspace/markdown/export source=" + resolved_root->source_name +
              " pack=" + resolved_root->pack + " checklist=" + checklist +
              " instance=" + export_instance_id +
              " -> " + resolved.string());
      json data{{"source_name", resolved_root->source_name},
                {"source_path", resolved_root->library_root.string()},
                {"pack", resolved_root->pack},
                {"checklist_dir", resolved_root->checklist_dir},
                {"checklist", checklist},
                {"instance_id", export_instance_id},
                {"path", resolved.string()},
                {"bytes", markdown.size()}};
      if (resolved_root->defaulted) {
        warnings.push_back({{"code", "DEFAULT_PACK"},
                            {"message", "Checklist exported using the default asset pack."},
                            {"details", {{"pack", resolved_root->pack},
                                         {"source_name", resolved_root->source_name},
                                         {"checklist", checklist}}}});
      }
      return OkResponse(data, 201, warnings);
    } catch (const std::exception& ex) {
      return ErrorResponse("INTERNAL_ERROR", ex.what(), {}, 500);
    }
  };

  auto handle_workspace_visualizations_export = [&store](const platform::HttpRequest& request) {
    const auto payload = json::parse(request.body, nullptr, false);
    if (payload.is_discarded() || !payload.is_object()) {
      return ErrorResponse("INVALID_JSON", "Invalid JSON payload.", {}, 400);
    }
    const std::string checklist = TrimString(payload.value("checklist", ""));
    const std::string instance_id = TrimString(payload.value("instance_id", ""));
    if (checklist.empty() || instance_id.empty()) {
      return ErrorResponse("INVALID_FIELD", "checklist and instance_id are required.", {}, 400);
    }
    if (!core::IsValidBase32Id(instance_id, 16)) {
      return ErrorResponse("INVALID_FIELD", "instance_id must be a 16-char Base32 token.",
                           json{{"instance_id", instance_id}}, 400);
    }

    std::string source_name = TrimString(payload.value("source_name", ""));
    std::string pack = TrimString(payload.value("pack", ""));
    std::string checklist_dir = TrimString(payload.value("checklist_dir", checklist));
    if (source_name.empty() && pack.empty() && !payload.contains("checklist_dir")) {
      const auto ownerships = store.ListOwnershipsForInstance(checklist, instance_id);
      if (ownerships.size() == 1) {
        source_name = ownerships.front().source_name;
        pack = ownerships.front().pack;
        checklist_dir = ownerships.front().checklist_dir;
      } else if (ownerships.size() > 1) {
        return ErrorResponse("AMBIGUOUS_CHECKLIST_OWNERSHIP",
                             "Checklist instance has multiple source/pack owners; specify source_name and pack.",
                             json{{"checklist", checklist},
                                  {"instance_id", instance_id},
                                  {"matches", OwnershipsToJson(ownerships)}},
                             400);
      }
    }

    const auto workspace_roots = LoadChecklistWorkspaceRoots();
    std::string resolve_error;
    json resolve_details = json::object();
    const auto resolved_root =
        ResolveChecklistRoot(workspace_roots, checklist_dir, pack, source_name, true,
                             &resolve_error, &resolve_details);
    if (!resolved_root) {
      return ErrorResponse("INVALID_FIELD",
                           resolve_error.empty() ? "Invalid pack or checklist." : resolve_error,
                           resolve_details, 400);
    }

    try {
      json warnings = json::array();
      const auto ownership = OwnershipFromResolution(*resolved_root, checklist);
      auto slugs = store.QuerySlugs(checklist, instance_id, std::nullopt, std::nullopt,
                                    std::nullopt, std::nullopt, ownership);
      if (slugs.empty()) {
        slugs = store.QuerySlugs(checklist, instance_id, std::nullopt, std::nullopt,
                                 std::nullopt, std::nullopt);
        if (!slugs.empty()) {
          warnings.push_back(
              {{"code", "OWNERSHIP_FILTER_EMPTY"},
               {"message",
                "No rows had persisted ownership for the requested source/pack; exported legacy unowned rows."},
               {"details",
                {{"source_name", resolved_root->source_name},
                 {"pack", resolved_root->pack},
                 {"checklist_dir", resolved_root->checklist_dir}}}});
        }
      }
      if (slugs.empty()) {
        return ErrorResponse("NOT_FOUND", "No slugs found for checklist/instance.",
                             json{{"checklist", checklist}, {"instance_id", instance_id}}, 404);
      }

      const auto graph = BuildChecklistGraph(slugs);
      const json graph_json = ChecklistGraphToJson(graph);
      const json workbench_json = BuildRelationshipWorkbench(graph, resolved_root->root);
      const auto output_root = resolved_root->root / "visualizations" / instance_id;
      const auto json_path = output_root / "graph.json";
      const auto dot_path = output_root / "section-flow.dot";
      const auto mermaid_path = output_root / "section-flow.mmd";
      const auto dbml_path = output_root / "runtime-schema.dbml";
      const auto workbench_json_path = output_root / "relationship-workbench.json";
      const auto workbench_dot_path = output_root / "relationship-workbench.dot";
      std::string write_error;
      if (!WriteUtf8File(json_path, graph_json.dump(2) + "\n", &write_error) ||
          !WriteUtf8File(dot_path, RenderChecklistGraphDot(graph), &write_error) ||
          !WriteUtf8File(mermaid_path, RenderChecklistGraphMermaid(graph), &write_error) ||
          !WriteUtf8File(dbml_path, RenderChecklistRuntimeSchemaDbml(), &write_error) ||
          !WriteUtf8File(workbench_json_path, workbench_json.dump(2) + "\n", &write_error) ||
          !WriteUtf8File(workbench_dot_path, RenderRelationshipWorkbenchDot(workbench_json),
                         &write_error)) {
        return ErrorResponse("INTERNAL_ERROR", write_error, {}, 500);
      }

      LogInfo("POST /api/v1/workspace/visualizations/export source=" +
              resolved_root->source_name + " pack=" + resolved_root->pack +
              " checklist=" + checklist + " instance=" + instance_id +
              " -> " + output_root.string());
      json files = json::array({json_path.string(), dot_path.string(), mermaid_path.string(),
                                dbml_path.string(), workbench_json_path.string(),
                                workbench_dot_path.string()});
      return OkResponse(
          json{{"source_name", resolved_root->source_name},
               {"source_path", resolved_root->library_root.string()},
               {"pack", resolved_root->pack},
               {"checklist_dir", resolved_root->checklist_dir},
               {"checklist", checklist},
               {"instance_id", instance_id},
               {"directory", output_root.string()},
               {"files", files}},
          201, warnings);
    } catch (const std::exception& ex) {
      return ErrorResponse("INTERNAL_ERROR", ex.what(), {}, 500);
    }
  };

  auto handle_workspace_asset_pack_export = [](const platform::HttpRequest &request) {
    const auto payload = json::parse(request.body, nullptr, false);
    if (payload.is_discarded()) {
      return ErrorResponse("INVALID_JSON", "Invalid JSON payload.", {}, 400);
    }
    const std::string source_name = payload.value("source_name", "");
    const std::string pack = payload.value("pack", "");
    const std::string checklist_dir_payload = payload.value("checklist_dir", payload.value("checklist", ""));
    if (checklist_dir_payload.empty()) {
      return ErrorResponse("INVALID_FIELD", "checklist or checklist_dir is required.", {}, 400);
    }

    const auto workspace_roots = LoadChecklistWorkspaceRoots();
    std::string resolve_error;
    json resolve_details = json::object();
    const auto resolved_root = ResolveChecklistRoot(workspace_roots, checklist_dir_payload, pack, source_name, false,
                                                    &resolve_error, &resolve_details);
    if (!resolved_root) {
      return ErrorResponse("INVALID_FIELD", resolve_error.empty() ? "Invalid pack or checklist." : resolve_error,
                           resolve_details, 400);
    }

    std::filesystem::path output_path = payload.value("output_path", "");
    if (output_path.empty()) {
      std::string format = ToLowerCopy(payload.value("format", "chk"));
      if (format.empty()) {
        format = "chk";
      }
      if (format != "chk" && format != "7z" && format != "zip") {
        return ErrorResponse("INVALID_FIELD", "format must be one of: chk, 7z, zip.", json{{"format", format}}, 400);
      }
      const std::string extension = format == "7z" ? ".7z" : "." + format;
      output_path =
          std::filesystem::path{".chax"} / "asset-packs" /
          (SanitizeTokenForFileName(resolved_root->source_name) + "-" + SanitizeTokenForFileName(resolved_root->pack) +
           "-" + SanitizeTokenForFileName(resolved_root->checklist_dir) + "-" + UtcTimestampForFileName() + extension);
    }
    if (!output_path.is_absolute()) {
      output_path = std::filesystem::current_path() / output_path;
    }
    output_path = output_path.lexically_normal();
    if (!IsSupportedAssetArchiveExtension(output_path)) {
      return ErrorResponse("INVALID_FIELD", "Archive extension must be one of: .chk, .7z, .zip.",
                           json{{"output_path", output_path.string()}}, 400);
    }
    if (IsPathWithinRoot(resolved_root->root, output_path)) {
      return ErrorResponse(
          "INVALID_FIELD", "output_path must not be inside the checklist asset folder.",
          json{{"output_path", output_path.string()}, {"checklist_root", resolved_root->root.string()}}, 400);
    }

    std::error_code ec;
    std::filesystem::create_directories(output_path.parent_path(), ec);
    if (ec) {
      return ErrorResponse("INTERNAL_ERROR", "Failed to create archive output directory.",
                           json{{"path", output_path.parent_path().string()}, {"message", ec.message()}}, 500);
    }
    std::filesystem::remove(output_path, ec);
    ec.clear();

    const auto relative_root = (std::filesystem::path{resolved_root->pack} / resolved_root->checklist_dir).string();
    std::string error;
    if (!RunSevenZip({"a", "-bso0", "-bsp0", "-bse0", "-t" + AssetArchiveType(output_path), output_path.string(),
                      relative_root, "-mx=9", "-y"},
                     resolved_root->library_root, &error)) {
      return ErrorResponse("INTERNAL_ERROR", error, json{{"output_path", output_path.string()}}, 500);
    }

    const auto size = std::filesystem::file_size(output_path, ec);
    LogInfo("POST /api/v1/workspace/asset-pack/export source=" + resolved_root->source_name + " pack=" +
            resolved_root->pack + " checklist_dir=" + resolved_root->checklist_dir + " -> " + output_path.string());
    return OkResponse(json{{"source_name", resolved_root->source_name},
                           {"source_path", resolved_root->library_root.string()},
                           {"pack", resolved_root->pack},
                           {"checklist_dir", resolved_root->checklist_dir},
                           {"path", output_path.string()},
                           {"archive_type", AssetArchiveType(output_path)},
                           {"bytes", ec ? 0 : size}},
                      201);
  };

  auto handle_workspace_asset_pack_import = [import_workspace_markdown_file](const platform::HttpRequest &request) {
    const auto payload = json::parse(request.body, nullptr, false);
    if (payload.is_discarded()) {
      return ErrorResponse("INVALID_JSON", "Invalid JSON payload.", {}, 400);
    }
    std::filesystem::path archive_path = payload.value("archive_path", "");
    if (archive_path.empty()) {
      return ErrorResponse("INVALID_FIELD", "archive_path is required.", {}, 400);
    }
    if (!archive_path.is_absolute()) {
      archive_path = std::filesystem::current_path() / archive_path;
    }
    archive_path = archive_path.lexically_normal();
    if (!IsSupportedAssetArchiveExtension(archive_path)) {
      return ErrorResponse("INVALID_FIELD", "Archive extension must be one of: .chk, .7z, .zip.",
                           json{{"archive_path", archive_path.string()}}, 400);
    }
    std::error_code ec;
    if (!std::filesystem::exists(archive_path, ec) || !std::filesystem::is_regular_file(archive_path, ec)) {
      return ErrorResponse("NOT_FOUND", "Archive file not found.", json{{"archive_path", archive_path.string()}}, 404);
    }

    const std::string source_name = payload.value("source_name", "");
    const std::string pack_override = payload.value("pack", "");
    const std::string checklist_dir_override = payload.value("checklist_dir", payload.value("checklist", ""));
    if (!pack_override.empty() && !core::IsSafePackToken(pack_override)) {
      return ErrorResponse("INVALID_FIELD", "pack must be a plain folder name.", json{{"pack", pack_override}}, 400);
    }
    if (!checklist_dir_override.empty() && !core::IsSafeChecklistToken(checklist_dir_override)) {
      return ErrorResponse("INVALID_FIELD", "checklist_dir must be a plain folder name.",
                           json{{"checklist_dir", checklist_dir_override}}, 400);
    }

    const auto workspace_roots = LoadChecklistWorkspaceRoots();
    const ChecklistWorkspaceRoot *target_source = nullptr;
    for (const auto &root : workspace_roots) {
      if (!source_name.empty()) {
        if (root.source_name == source_name) {
          target_source = &root;
          break;
        }
      } else if (root.primary) {
        target_source = &root;
        break;
      }
    }
    if (target_source == nullptr && source_name.empty() && !workspace_roots.empty()) {
      target_source = &workspace_roots.front();
    }
    if (target_source == nullptr) {
      return ErrorResponse("INVALID_FIELD", "Checklist source not found.", json{{"source_name", source_name}}, 400);
    }

    const bool replace_files = payload.value("replace_files", false);
    WorkspaceMarkdownImportOptions import_options;
    import_options.template_principal = payload.value("instance_principal", import_options.template_principal);
    import_options.apply_instance_principal = payload.value("apply_instance_principal", "");
    import_options.apply_instance_id_payload = payload.value("apply_instance_id", "");
    import_options.apply_data = payload.value("apply_data", false);
    import_options.replace_instance = payload.value("replace_instance", true);

    ScopedTempDirectory staging(MakeAssetArchiveTempDir());
    std::filesystem::create_directories(staging.path, ec);
    if (ec) {
      return ErrorResponse("INTERNAL_ERROR", "Failed to create asset pack staging directory.",
                           json{{"path", staging.path.string()}, {"message", ec.message()}}, 500);
    }

    std::string error;
    if (!RunSevenZip({"x", "-bso0", "-bsp0", "-bse0", archive_path.string(), "-o" + staging.path.string(), "-y"},
                     std::filesystem::current_path(), &error)) {
      return ErrorResponse("INVALID_FIELD", error, json{{"archive_path", archive_path.string()}}, 400);
    }

    auto staged_items =
        FindStagedAssetChecklists(staging.path, pack_override, checklist_dir_override, GetDefaultLibraryPack());
    if (staged_items.empty()) {
      return ErrorResponse("INVALID_FIELD", "Archive does not contain a checklist.md file.",
                           json{{"archive_path", archive_path.string()}}, 400);
    }
    if (!checklist_dir_override.empty() && staged_items.size() > 1) {
      return ErrorResponse("INVALID_FIELD", "checklist_dir override can only be used with a single-checklist archive.",
                           json{{"checklist_dir", checklist_dir_override}, {"detected", staged_items.size()}}, 400);
    }

    json imported_items = json::array();
    json all_warnings = json::array();
    for (auto &staged : staged_items) {
      std::string read_error;
      const auto content = ReadUtf8FileLimited(staged.source_root / "checklist.md", 2 * 1024 * 1024, &read_error);
      if (!content) {
        return ErrorResponse("INVALID_FIELD", read_error,
                             json{{"path", (staged.source_root / "checklist.md").string()}}, 400);
      }
      core::markdown::ParsedChecklist parsed;
      try {
        parsed = core::markdown::ParseChecklistMarkdown("", *content);
      } catch (const std::exception &ex) {
        return ErrorResponse("INVALID_FIELD", ex.what(), json{{"path", (staged.source_root / "checklist.md").string()}},
                             400);
      }
      if (staged.checklist_dir.empty()) {
        staged.checklist_dir = parsed.checklist;
      }
      if (!core::IsSafePackToken(staged.pack) || !core::IsSafeChecklistToken(staged.checklist_dir)) {
        return ErrorResponse("INVALID_FIELD", "Archive contains an unsafe pack or checklist folder name.",
                             json{{"pack", staged.pack}, {"checklist_dir", staged.checklist_dir}}, 400);
      }

      const auto target_root = target_source->root / staged.pack / staged.checklist_dir;
      if (!IsPathWithinRoot(target_source->root, target_root)) {
        return ErrorResponse("INVALID_FIELD", "Resolved target path escapes checklist source.",
                             json{{"target", target_root.string()}, {"source_path", target_source->root.string()}},
                             400);
      }
      if (!CopyChecklistAssetFolder(staged.source_root, target_root, replace_files, &error)) {
        return ErrorResponse("CONFLICT", error,
                             json{{"target", target_root.string()}, {"replace_files", replace_files}}, 409);
      }

      ChecklistRootResolution resolved{target_source->source_name, target_source->root, staged.pack,
                                       staged.checklist_dir,       target_root,         false};
      json import_data = json::object();
      json import_warnings = json::array();
      if (const auto err = import_workspace_markdown_file(resolved, import_options, &import_data, &import_warnings)) {
        return *err;
      }
      for (auto &warning : import_warnings) {
        all_warnings.push_back(std::move(warning));
      }
      import_data["path"] = target_root.string();
      import_data["archive_source_root"] = staged.source_root.lexically_relative(staging.path).string();
      imported_items.push_back(std::move(import_data));
    }

    LogInfo("POST /api/v1/workspace/asset-pack/import archive=" + archive_path.string() +
            " source=" + target_source->source_name + " imported=" + std::to_string(imported_items.size()));
    return OkResponse(json{{"archive_path", archive_path.string()},
                           {"source_name", target_source->source_name},
                           {"source_path", target_source->root.string()},
                           {"imported", imported_items.size()},
                           {"items", imported_items}},
                      201, all_warnings);
  };

  server.AddHandler(platform::HttpMethod::kGet, "/oauth/authorize", handle_authorize);
  server.AddHandler(platform::HttpMethod::kPost, "/oauth/authorize", handle_authorize_post);
  server.AddHandler(platform::HttpMethod::kPost, "/oauth/token", handle_token);
  server.AddHandler(platform::HttpMethod::kGet, "/oauth/callback", handle_oauth_callback);
  server.AddHandler(platform::HttpMethod::kGet, "/favicon.ico", handle_favicon);

  server.AddHandler(platform::HttpMethod::kGet, "/api/v1/health", handle_health);
  server.AddHandler(platform::HttpMethod::kPost, "/api/v1/admin/shutdown", handle_shutdown);
  add_authed(platform::HttpMethod::kGet, "/api/v1/commands", handle_commands);
  add_authed(platform::HttpMethod::kGet, "/api/v1/hello", handle_hello);
  add_authed(platform::HttpMethod::kPost, "/api/v1/echo", handle_echo);
  add_authed(platform::HttpMethod::kGet, "/api/v1/me", handle_me);
  add_authed(platform::HttpMethod::kGet, "/api/v1/checklists", handle_checklists);
  add_authed(platform::HttpMethod::kDelete, R"(/api/v1/checklists/([^/]+))",
             handle_delete_checklist);
  add_authed(platform::HttpMethod::kDelete,
             R"(/api/v1/checklists/([^/]+)/instances/([^/]+))", handle_delete_instance);
  add_authed(platform::HttpMethod::kPost, "/api/v1/slugs", handle_create_slug);
  add_authed(platform::HttpMethod::kGet, "/api/v1/slugs", handle_slugs);
  add_authed(platform::HttpMethod::kGet, R"(/api/v1/slugs/(.+))", handle_slug);
  add_authed(platform::HttpMethod::kGet, "/api/v1/visualizations/graph",
             handle_visualization_graph);
  add_authed(platform::HttpMethod::kGet, "/api/v1/visualizations/workbench",
             handle_visualization_workbench);
  add_authed(platform::HttpMethod::kGet, R"(/api/v1/checklists/(.+))", handle_checklist);
  add_authed(platform::HttpMethod::kGet, R"(/api/v1/relationships/address/(.+))",
             handle_relationships);
  add_authed(platform::HttpMethod::kPost, "/api/v1/relationships/template",
             handle_relationships_template_create);
  add_authed(platform::HttpMethod::kGet, "/api/v1/relationships/template",
             handle_relationships_template_list);
  add_authed(platform::HttpMethod::kPost, "/api/v1/relationships/address",
             handle_relationships_address_create);
  add_authed(platform::HttpMethod::kGet, "/api/v1/relationships/address",
             handle_relationships_address_list);
  add_authed(platform::HttpMethod::kPatch, R"(/api/v1/slugs/(.+))", handle_update);
  add_authed(platform::HttpMethod::kPost, "/api/v1/slugs/bulk-update", handle_update_bulk);
  add_authed(platform::HttpMethod::kGet, R"(/api/v1/history/(.+))", handle_history);
  add_authed(platform::HttpMethod::kGet, "/api/v1/export/json", handle_export_json);
  add_authed(platform::HttpMethod::kGet, "/api/v1/export/jsonl", handle_export_jsonl);
  add_authed(platform::HttpMethod::kPost, "/api/v1/import/jsonl", handle_import_jsonl);
  add_authed(platform::HttpMethod::kGet, R"(/api/v1/export/markdown/(.+))",
             handle_export_markdown);
  add_authed(platform::HttpMethod::kPost, "/api/v1/export/report", handle_export_report);
  add_authed(platform::HttpMethod::kPost, "/api/v1/import/markdown", handle_import_markdown);
  add_authed(platform::HttpMethod::kGet, "/api/v1/local/settings", handle_local_settings_get);
  add_authed(platform::HttpMethod::kPost, "/api/v1/local/settings", handle_local_settings_post);
  add_authed(platform::HttpMethod::kGet, "/api/v1/workspace/markdown/templates",
             handle_workspace_markdown_templates);
  add_authed(platform::HttpMethod::kPost, "/api/v1/workspace/markdown/import",
             handle_workspace_markdown_import);
  add_authed(platform::HttpMethod::kPost, "/api/v1/workspace/markdown/export",
             handle_workspace_markdown_export);
  add_authed(platform::HttpMethod::kPost, "/api/v1/workspace/visualizations/export",
             handle_workspace_visualizations_export);
  add_authed(platform::HttpMethod::kPost, "/api/v1/workspace/asset-pack/export", handle_workspace_asset_pack_export);
  add_authed(platform::HttpMethod::kPost, "/api/v1/workspace/asset-pack/import", handle_workspace_asset_pack_import);
  add_authed(platform::HttpMethod::kGet, "/api/v1/workspace/scripts",
             handle_workspace_scripts_list);
  add_authed(platform::HttpMethod::kPost, "/api/v1/workspace/scripts/run",
             handle_workspace_scripts_run);
  add_authed(platform::HttpMethod::kPost, "/api/v1/entities", handle_entities_create);
  add_authed(platform::HttpMethod::kGet, "/api/v1/entities", handle_entities_list);
  add_authed(platform::HttpMethod::kPost, "/api/v1/instances", handle_instances_create);
  add_authed(platform::HttpMethod::kGet, "/api/v1/instances", handle_instances_list);
  add_authed(platform::HttpMethod::kPost, "/api/v1/predicates", handle_predicates_create);
  add_authed(platform::HttpMethod::kGet, "/api/v1/predicates", handle_predicates_list);
  add_authed(platform::HttpMethod::kGet, R"(/api/v1/evaluate/slug/(.+))", handle_evaluate_slug);
  add_authed(platform::HttpMethod::kPost, "/api/v1/evaluate/graph", handle_evaluate_graph);

  server.AddHandler(platform::HttpMethod::kOptions, "/oauth/authorize", HandleCorsPreflight);
  server.AddHandler(platform::HttpMethod::kOptions, "/oauth/token", HandleCorsPreflight);
  server.AddHandler(platform::HttpMethod::kOptions, "/api/v1/commands", HandleCorsPreflight);
  server.AddHandler(platform::HttpMethod::kOptions, "/api/v1/health", HandleCorsPreflight);
  server.AddHandler(platform::HttpMethod::kOptions, "/api/v1/admin/shutdown", HandleCorsPreflight);
  server.AddHandler(platform::HttpMethod::kOptions, "/api/v1/hello", HandleCorsPreflight);
  server.AddHandler(platform::HttpMethod::kOptions, "/api/v1/echo", HandleCorsPreflight);
  server.AddHandler(platform::HttpMethod::kOptions, "/api/v1/predicates", HandleCorsPreflight);
  server.AddHandler(platform::HttpMethod::kOptions, R"(/api/v1/checklists/[^/]+)",
                    HandleCorsPreflight);
  server.AddHandler(platform::HttpMethod::kOptions, R"(/api/v1/checklists/[^/]+/instances/[^/]+)",
                    HandleCorsPreflight);
  add_authed(platform::HttpMethod::kPost, "/api/echo", handle_echo);
  server.AddHandler(platform::HttpMethod::kOptions, "/api/echo", HandleCorsPreflight);
  server.AddHandler(platform::HttpMethod::kOptions, "/api/v1/checklists", HandleCorsPreflight);
  server.AddHandler(platform::HttpMethod::kOptions, "/api/v1/slugs", HandleCorsPreflight);
  server.AddHandler(platform::HttpMethod::kOptions, R"(/api/v1/slugs/.*)", HandleCorsPreflight);
  server.AddHandler(platform::HttpMethod::kOptions, "/api/v1/visualizations/graph",
                    HandleCorsPreflight);
  server.AddHandler(platform::HttpMethod::kOptions, "/api/v1/visualizations/workbench",
                    HandleCorsPreflight);
  server.AddHandler(platform::HttpMethod::kOptions, R"(/api/v1/history/.*)", HandleCorsPreflight);
  server.AddHandler(platform::HttpMethod::kOptions, "/api/v1/slugs", HandleCorsPreflight);
  server.AddHandler(platform::HttpMethod::kOptions, R"(/api/v1/slugs/.*)", HandleCorsPreflight);
  server.AddHandler(platform::HttpMethod::kOptions, R"(/api/v1/checklists/.*)", HandleCorsPreflight);
  server.AddHandler(platform::HttpMethod::kOptions, R"(/api/v1/relationships/address/.*)",
                    HandleCorsPreflight);
  server.AddHandler(platform::HttpMethod::kOptions, "/api/v1/relationships/template",
                    HandleCorsPreflight);
  server.AddHandler(platform::HttpMethod::kOptions, "/api/v1/relationships/address",
                    HandleCorsPreflight);
  server.AddHandler(platform::HttpMethod::kOptions, R"(/api/v1/slugs/.*)", HandleCorsPreflight);
  server.AddHandler(platform::HttpMethod::kOptions, "/api/v1/slugs/bulk-update",
                    HandleCorsPreflight);
  server.AddHandler(platform::HttpMethod::kOptions, "/api/v1/export/json", HandleCorsPreflight);
  server.AddHandler(platform::HttpMethod::kOptions, "/api/v1/export/jsonl", HandleCorsPreflight);
  server.AddHandler(platform::HttpMethod::kOptions, "/api/v1/import/jsonl", HandleCorsPreflight);
  server.AddHandler(platform::HttpMethod::kOptions, R"(/api/v1/export/markdown/.*)",
                    HandleCorsPreflight);
  server.AddHandler(platform::HttpMethod::kOptions, "/api/v1/export/report", HandleCorsPreflight);
  server.AddHandler(platform::HttpMethod::kOptions, "/api/v1/import/markdown",
                    HandleCorsPreflight);
  server.AddHandler(platform::HttpMethod::kOptions, "/api/v1/local/settings",
                    HandleCorsPreflight);
  server.AddHandler(platform::HttpMethod::kOptions, "/api/v1/workspace/markdown/templates",
                    HandleCorsPreflight);
  server.AddHandler(platform::HttpMethod::kOptions, "/api/v1/workspace/markdown/import",
                    HandleCorsPreflight);
  server.AddHandler(platform::HttpMethod::kOptions, "/api/v1/workspace/markdown/export",
                    HandleCorsPreflight);
  server.AddHandler(platform::HttpMethod::kOptions, "/api/v1/workspace/visualizations/export",
                    HandleCorsPreflight);
  server.AddHandler(platform::HttpMethod::kOptions, "/api/v1/workspace/asset-pack/export", HandleCorsPreflight);
  server.AddHandler(platform::HttpMethod::kOptions, "/api/v1/workspace/asset-pack/import", HandleCorsPreflight);
  server.AddHandler(platform::HttpMethod::kOptions, "/api/v1/workspace/scripts",
                    HandleCorsPreflight);
  server.AddHandler(platform::HttpMethod::kOptions, "/api/v1/workspace/scripts/run",
                    HandleCorsPreflight);
  server.AddHandler(platform::HttpMethod::kOptions, "/api/v1/entities", HandleCorsPreflight);
  server.AddHandler(platform::HttpMethod::kOptions, "/api/v1/instances", HandleCorsPreflight);
  server.AddHandler(platform::HttpMethod::kOptions, "/api/v1/evaluate/graph", HandleCorsPreflight);
  server.AddHandler(platform::HttpMethod::kOptions, R"(/api/v1/evaluate/slug/.*)",
                    HandleCorsPreflight);

  auto register_static_root = [&](const std::string& mount, const std::string& root_path) {
    std::error_code ec;
    const std::filesystem::path root = std::filesystem::weakly_canonical(root_path, ec);
    if (ec) {
      LogWarn("Static root not found for " + mount + ": " + root_path);
      return false;
    }
    auto serve_static = [root, mount](const platform::HttpRequest& request) {
      std::string rel = request.path;
      const std::string mount_slash = mount + "/";
      if (rel == mount || rel == mount_slash) {
        rel = mount_slash + "index.html";
      }
      if (rel.rfind(mount_slash, 0) == 0) {
        rel = rel.substr(mount.size());
        while (!rel.empty() && (rel.front() == '/' || rel.front() == '\\')) {
          rel.erase(rel.begin());
        }
      }
      rel = UrlDecode(rel);
      while (!rel.empty() && (rel.front() == '/' || rel.front() == '\\')) {
        rel.erase(rel.begin());
      }
      std::filesystem::path target = root / rel;
      std::error_code file_ec;
      auto canon = std::filesystem::weakly_canonical(target, file_ec);
      if (file_ec || canon.string().find(root.string()) != 0) {
        return ErrorResponse("NOT_FOUND", "File not found.", {}, 404);
      }
      std::ifstream file(canon, std::ios::binary);
      if (!file) {
        return ErrorResponse("NOT_FOUND", "File not found.", {}, 404);
      }
      std::ostringstream oss;
      oss << file.rdbuf();
      const std::string body = oss.str();
      std::string content_type = "text/plain";
      const auto ext = canon.extension().string();
      if (ext == ".html") content_type = "text/html";
      else if (ext == ".js") content_type = "application/javascript";
      else if (ext == ".css") content_type = "text/css";
      else if (ext == ".json") content_type = "application/json";
      else if (ext == ".png") content_type = "image/png";
      else if (ext == ".gif") content_type = "image/gif";
      else if (ext == ".jpg" || ext == ".jpeg") content_type = "image/jpeg";
      else if (ext == ".webp") content_type = "image/webp";
      else if (ext == ".avif") content_type = "image/avif";
      else if (ext == ".svg") content_type = "image/svg+xml";
      else if (ext == ".mp4") content_type = "video/mp4";
      else if (ext == ".webm") content_type = "video/webm";
      else if (ext == ".mkv") content_type = "video/x-matroska";
      platform::HttpResponse resp;
      resp.status = 200;
      resp.content_type = content_type;
      resp.body = body;
      ApplyCors(resp);
      return resp;
    };
    server.AddHandler(platform::HttpMethod::kGet, mount, serve_static);
    server.AddHandler(platform::HttpMethod::kGet, mount + "/.*", serve_static);
    return true;
  };

  if (config.serve_ui) {
    const std::filesystem::path library_root = GetLibraryRoot();
    std::error_code ec;
    std::filesystem::create_directories(library_root, ec);
    if (ec) {
      LogWarn("Failed to create checklists root for static assets: " + library_root.string());
    }
    register_static_root("/checklists", library_root.string());
  }

  const bool ui_served = config.serve_ui && register_static_root("/ui", config.ui_root);
  const bool vui_served = config.serve_vui && register_static_root("/vui", config.vui_root);
  if (ui_served || vui_served) {
    const std::string location = ui_served ? "/ui/" : "/vui/";
    auto redirect_root = [location](const platform::HttpRequest&) {
      platform::HttpResponse resp;
      resp.status = 302;
      resp.headers["Location"] = location;
      ApplyCors(resp);
      return resp;
    };
    server.AddHandler(platform::HttpMethod::kGet, "/", redirect_root);
  }
}

ServerConfig LoadServerConfig() {
  ServerConfig config;
  config.whisper_server_path =
      platform::WithExecutableExtension("CHAX-CLIENT/vui/whisper.cpp/build/bin/whisper-server")
          .string();
  config.whisper_model_path = "CHAX-CLIENT/vui/whisper.cpp/models/ggml-tiny.en-q8_0.bin";
  config.whisper_model_fallback_path = "CHAX-CLIENT/vui/whisper.cpp/models/ggml-tiny.en.bin";

  auto trim = [](std::string value) {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())) != 0) {
      value.erase(value.begin());
    }
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())) != 0) {
      value.pop_back();
    }
    return value;
  };

  auto split_list = [&](const std::string& raw) {
    std::vector<std::string> tokens;
    std::string current;
    for (char ch : raw) {
      if (ch == ',' || std::isspace(static_cast<unsigned char>(ch)) != 0) {
        const auto token = trim(current);
        if (!token.empty()) {
          tokens.push_back(token);
        }
        current.clear();
      } else {
        current.push_back(ch);
      }
    }
    const auto token = trim(current);
    if (!token.empty()) {
      tokens.push_back(token);
    }
    return tokens;
  };

  if (const char* host = std::getenv("CHAX_HOST")) {
    config.host = host;
  }
  if (const char* port = std::getenv("CHAX_PORT")) {
    try {
      const int parsed = std::stoi(port);
      if (parsed > 0 && parsed <= 65535) {
        config.port = parsed;
      } else {
        LogWarn("CHAX_PORT is outside the valid range, falling back to default 8080");
      }
    } catch (const std::exception& ex) {
      LogWarn(std::string{"Failed to parse CHAX_PORT: "} + ex.what());
    }
  }
  if (const char* db_path = std::getenv("CHAX_DB")) {
    config.database_path = db_path;
  }
  if (const char* chain_depth = std::getenv("CHAX_PREDICATE_CHAIN_DEPTH")) {
    try {
      const int parsed = std::stoi(chain_depth);
      if (parsed >= 0) {
        config.predicate_chain_depth = parsed;
      } else {
        LogWarn("CHAX_PREDICATE_CHAIN_DEPTH must be >= 0; using default.");
      }
    } catch (const std::exception& ex) {
      LogWarn(std::string{"Failed to parse CHAX_PREDICATE_CHAIN_DEPTH: "} + ex.what());
    }
  }
  if (const char* seed = std::getenv("CHAX_SEED_DEMO")) {
    const std::string value = seed;
    if (value == "0" || value == "false" || value == "FALSE") {
      config.seed_demo_data = false;
    }
  }
  if (const char* open_browser = std::getenv("CHAX_OPEN_BROWSER")) {
    const std::string value = open_browser;
    if (value == "0" || value == "false" || value == "FALSE") {
      config.open_browser = false;
    }
  }
  if (const char* serve_whisper = std::getenv("CHAX_WHISPER_AUTOSTART")) {
    const std::string value = serve_whisper;
    if (value == "0" || value == "false" || value == "FALSE") {
      config.whisper_autostart = false;
    } else if (value == "1" || value == "true" || value == "TRUE") {
      config.whisper_autostart = true;
    }
  }
  if (const char* whisper_host = std::getenv("CHAX_WHISPER_HOST")) {
    config.whisper_host = whisper_host;
  }
  if (const char* whisper_port = std::getenv("CHAX_WHISPER_PORT")) {
    try {
      const int parsed = std::stoi(whisper_port);
      if (parsed > 0 && parsed <= 65535) {
        config.whisper_port = parsed;
      } else {
        LogWarn("CHAX_WHISPER_PORT is outside the valid range, falling back to default 8081");
      }
    } catch (const std::exception& ex) {
      LogWarn(std::string{"Failed to parse CHAX_WHISPER_PORT: "} + ex.what());
    }
  }
  if (const char* whisper_server = std::getenv("CHAX_WHISPER_SERVER")) {
    config.whisper_server_path = whisper_server;
  }
  if (const char* whisper_model = std::getenv("CHAX_WHISPER_MODEL")) {
    config.whisper_model_path = whisper_model;
  }
  if (const char* whisper_fallback = std::getenv("CHAX_WHISPER_MODEL_FALLBACK")) {
    config.whisper_model_fallback_path = whisper_fallback;
  }
  if (const char* serve_vui = std::getenv("CHAX_SERVE_VUI")) {
    const std::string value = serve_vui;
    if (value == "0" || value == "false" || value == "FALSE") {
      config.serve_vui = false;
    } else if (value == "1" || value == "true" || value == "TRUE") {
      config.serve_vui = true;
    }
  }
  if (const char* bg = std::getenv("CHAX_BACKGROUND_PROCESSES")) {
    const std::string value = bg;
    if (value == "0" || value == "false" || value == "FALSE") {
      config.background_processes_enabled = false;
    } else if (value == "1" || value == "true" || value == "TRUE") {
      config.background_processes_enabled = true;
    }
  }
  if (const char* bg_root = std::getenv("CHAX_BACKGROUND_PROCESSES_ROOT")) {
    config.background_processes_root = bg_root;
  }
  if (const char* vui_root = std::getenv("CHAX_VUI_ROOT")) {
    config.vui_root = vui_root;
  }
  if (const char* base = std::getenv("BASE_URL")) {
    config.base_url = base;
  }
  if (config.base_url.empty()) {
    config.base_url = "http://" + config.host + ":" + std::to_string(config.port);
  }

  if (const char* auth_provider = std::getenv("AUTH_PROVIDER")) {
    config.auth_provider = auth_provider;
  }
  if (const char* salt = std::getenv("ENTITY_SALT")) {
    config.entity_salt = salt;
  }
  if (const char* guest_provider = std::getenv("GUEST_PROVIDER")) {
    config.guest_provider = guest_provider;
  }
  if (const char* guest_name = std::getenv("GUEST_NAME")) {
    config.guest_name = guest_name;
  }
  if (const auto local_noauth = ParseEnvBool(std::getenv("CHAX_LOCALHOST_NOAUTH"))) {
    config.localhost_noauth = *local_noauth;
    config.localhost_noauth_overridden = true;
  }
  if (const auto simple_auth = ParseEnvBool(std::getenv("OAUTH_AUTHORIZE_SIMPLE"))) {
    config.simple_authorize = *simple_auth;
    config.simple_authorize_overridden = true;
  }
  if (!config.serve_ui) {
    config.open_browser = false;
  }
  if (!config.serve_vui) {
    config.whisper_autostart = false;
  }

  if (const char* client_id = std::getenv("OAUTH_CLIENT_ID")) {
    config.oauth_client_id = client_id;
  }
  if (config.oauth_client_id.empty()) {
    config.oauth_client_id = GenerateTokenString(24);
    config.oauth_client_generated = true;
  }

  std::string client_secret;
  if (const char* secret_env = std::getenv("OAUTH_CLIENT_SECRET")) {
    client_secret = secret_env;
  }
  if (client_secret.empty()) {
    client_secret = GenerateTokenString(48);
    config.oauth_client_generated = true;
  }
  config.oauth_client_secret = client_secret;

  if (const char* scopes_env = std::getenv("OAUTH_SCOPES")) {
    const auto scopes = split_list(scopes_env);
    if (!scopes.empty()) {
      config.oauth_scopes = scopes;
    }
  }

  if (const char* redirects_env = std::getenv("OAUTH_REDIRECT_URIS")) {
    const auto redirects = split_list(redirects_env);
    if (!redirects.empty()) {
      config.oauth_redirect_uris = redirects;
    }
  }
  if (config.oauth_redirect_uris.empty()) {
    config.oauth_redirect_uris.push_back(config.base_url + "/oauth/callback");
  }
  if (config.serve_ui) {
    const std::string ui_redirect = config.base_url + "/ui/oauth_callback.html";
    if (std::find(config.oauth_redirect_uris.begin(), config.oauth_redirect_uris.end(), ui_redirect) ==
        config.oauth_redirect_uris.end()) {
      config.oauth_redirect_uris.push_back(ui_redirect);
    }
  }

  std::string admin_password;
  if (const char* admin_user = std::getenv("ADMIN_USER")) {
    config.admin_user = admin_user;
  }
  if (const char* admin_pw = std::getenv("ADMIN_PASSWORD")) {
    admin_password = admin_pw;
    config.admin_password_plain = admin_password;
  }
  if (admin_password.empty()) {
    admin_password = GenerateTokenString(16);
    config.admin_password_generated = true;
    config.admin_password_plain = admin_password;
  }
  config.admin_password_hash = HashSecret(admin_password);

  if (const char* shutdown_token = std::getenv("CHAX_SHUTDOWN_TOKEN")) {
    config.shutdown_token = shutdown_token;
  }

  auto parse_duration = [](const char* env_name, std::chrono::seconds current_value) {
    if (const char* raw = std::getenv(env_name)) {
      try {
        const auto parsed = std::stoi(raw);
        if (parsed > 0) {
          return std::chrono::seconds(parsed);
        }
      } catch (...) {
      }
    }
    return current_value;
  };

  config.auth_code_ttl = parse_duration("OAUTH_CODE_TTL_SECONDS", config.auth_code_ttl);
  config.access_token_ttl = parse_duration("OAUTH_ACCESS_TTL_SECONDS", config.access_token_ttl);
  config.refresh_token_ttl = parse_duration("OAUTH_REFRESH_TTL_SECONDS", config.refresh_token_ttl);
  config.issue_refresh_tokens = config.refresh_token_ttl.count() > 0;

  if (const char* rate_limit = std::getenv("OAUTH_TOKEN_RATE_LIMIT")) {
    try {
      const int parsed = std::stoi(rate_limit);
      if (parsed >= 0) {
        config.token_rate_limit = parsed;
      }
    } catch (...) {
    }
  }
  config.token_rate_window =
      parse_duration("OAUTH_TOKEN_RATE_WINDOW_SECONDS", config.token_rate_window);

  return config;
}

}  // namespace core
