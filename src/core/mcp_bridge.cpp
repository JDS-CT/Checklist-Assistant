#include "core/mcp_bridge.hpp"

#include <cstdlib>
#include <map>
#include <sstream>
#include <stdexcept>

namespace core::mcp {
namespace {

std::string EncodePathSegment(const std::string& value) {
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

const std::vector<ToolDefinition>& ToolCatalog() {
  static const std::vector<ToolDefinition> kTools = {
      {"chax.list_commands",
       "GET",
       "/api/v1/commands",
       "List every HTTP API command exposed by the demo server.",
       nlohmann::json::object()},
      {"chax.list_checklists",
       "GET",
       "/api/v1/checklists",
       "Enumerate every checklist currently present in the runtime store.",
       nlohmann::json::object()},
      {"chax.list_slugs",
       "GET",
       "/api/v1/slugs",
       "List slugs with optional filters and cursor-based pagination.",
       {{"type", "object"},
        {"properties",
         {{"checklist", {{"type", "string"}, {"description", "Checklist filter (optional)."}}},
          {"section", {{"type", "string"}, {"description", "Section filter (optional)."}}},
          {"status",
           {{"type", "string"},
            {"description", "Status filter (Pass, Fail, NA, Other)."}}},
          {"limit", {{"type", "integer"}, {"minimum", 1}}},
          {"cursor", {{"type", "integer"}, {"minimum", 0}}}}},
        {"additionalProperties", false}}},
      {"chax.health",
       "GET",
       "/api/v1/health",
       "Check server readiness, uptime, and version metadata.",
       nlohmann::json::object()},
      {"chax.hello",
       "GET",
       "/api/v1/hello",
       "Return a greeting (arguments: optional 'name').",
       {
           {"type", "object"},
           {"properties",
            {
                {"name",
                {{"type", "string"}, {"description", "Name used in the hello response."}}}
            }},
           {"additionalProperties", false},
       }},
      {"chax.echo",
       "POST",
       "/api/v1/echo",
       "Echo the provided payload for integration smoke testing.",
       {
           {"type", "object"},
           {"properties",
            {{"payload",
              {{"type", "string"},
               {"description", "JSON payload to echo (stringified JSON)."}}}}},
          {"required", {"payload"}},
          {"additionalProperties", false},
       }},
      {"chax.get_slug",
       "GET",
       "/api/v1/slugs/{address_id}",
       "Fetch a single slug by Address ID.",
       {{"type", "object"},
        {"properties",
         {{"address_id",
           {{"type", "string"}, {"description", "Address ID to retrieve."}}}}},
        {"required", {"address_id"}},
        {"additionalProperties", false}}},
      {"chax.get_checklist",
       "GET",
       "/api/v1/checklists/{checklist}",
       "Fetch all slugs belonging to a checklist (returns items array).",
       {{"type", "object"},
        {"properties",
         {{"checklist",
           {{"type", "string"}, {"description", "Checklist name to retrieve."}}}}},
        {"required", {"checklist"}},
        {"additionalProperties", false}}},
      {"chax.relationships",
       "GET",
       "/api/v1/relationships/address/{address_id}",
       "Inspect incoming and outgoing relationships for a slug.",
       {{"type", "object"},
        {"properties",
         {{"address_id",
           {{"type", "string"}, {"description", "Address ID whose graph should be returned."}}}}},
        {"required", {"address_id"}},
        {"additionalProperties", false}}},
      {"chax.update_slug",
       "PATCH",
       "/api/v1/slugs/{address_id}",
       "Apply the minimal update contract to a slug (result/status/comment/timestamp).",
       {{"type", "object"},
        {"properties",
         {{"address_id",
           {{"type", "string"},
            {"description", "Slug to update (required)."}}},
          {"status", {{"type", "string"}, {"description", "Pass, Fail, NA, or Other."}}},
          {"result", {{"type", "string"}, {"description", "New measured result value."}}},
          {"comment", {{"type", "string"}, {"description", "Operator note or annotation."}}},
          {"timestamp",
           {{"type", "string"}, {"description", "ISO8601 timestamp override (optional)."}}}}},
        {"required", {"address_id"}},
        {"additionalProperties", false}}},
      {"chax.create_template_relationship",
       "POST",
       "/api/v1/relationships/template",
       "Create a template-level relationship between two slug_ids.",
       {{"type", "object"},
        {"properties",
         {{"subject_slug_id",
           {{"type", "string"}, {"description", "Source slug_id (16-char Base32)."}}},
          {"predicate", {{"type", "string"}, {"description", "Relationship predicate."}}},
          {"target_slug_id",
           {{"type", "string"}, {"description", "Target slug_id (16-char Base32)."}}}}},
        {"required", {"subject_slug_id", "predicate", "target_slug_id"}},
        {"additionalProperties", false}}},
      {"chax.list_template_relationships",
       "GET",
       "/api/v1/relationships/template",
       "List template-level relationships with filters and cursor pagination.",
       {{"type", "object"},
        {"properties",
         {{"subject_slug_id",
           {{"type", "string"}, {"description", "Filter by subject slug_id (optional)."}}},
          {"target_slug_id",
           {{"type", "string"}, {"description", "Filter by target slug_id (optional)."}}},
          {"predicate",
           {{"type", "string"}, {"description", "Filter by predicate name (optional)."}}},
          {"limit", {{"type", "integer"}, {"minimum", 1}}},
          {"cursor", {{"type", "integer"}, {"minimum", 0}}}}},
        {"additionalProperties", false}}},
      {"chax.create_address_relationship",
       "POST",
       "/api/v1/relationships/address",
       "Create an address-level relationship between two address_ids.",
       {{"type", "object"},
        {"properties",
         {{"subject_address_id",
           {{"type", "string"}, {"description", "Source address_id (32-char Base32)."}}},
          {"predicate", {{"type", "string"}, {"description", "Relationship predicate."}}},
          {"target_address_id",
           {{"type", "string"}, {"description", "Target address_id (32-char Base32)."}}}}},
        {"required", {"subject_address_id", "predicate", "target_address_id"}},
        {"additionalProperties", false}}},
      {"chax.list_address_relationships",
       "GET",
       "/api/v1/relationships/address",
       "List address-level relationships with filters and cursor pagination.",
       {{"type", "object"},
        {"properties",
         {{"subject_address_id",
           {{"type", "string"}, {"description", "Filter by subject address_id (optional)."}}},
          {"target_address_id",
           {{"type", "string"}, {"description", "Filter by target address_id (optional)."}}},
          {"predicate",
           {{"type", "string"}, {"description", "Filter by predicate name (optional)."}}},
          {"limit", {{"type", "integer"}, {"minimum", 1}}},
          {"cursor", {{"type", "integer"}, {"minimum", 0}}}}},
        {"additionalProperties", false}}},
      {"chax.create_entity",
       "POST",
       "/api/v1/entities",
       "Create or upsert an entity principal in the catalog.",
       {{"type", "object"},
        {"properties",
         {{"principal", {{"type", "string"}, {"description", "Entity principal (required)."}}},
          {"kind",
           {{"type", "string"},
            {"description", "Entity kind (e.g., user, service). Defaults to user."}}},
          {"display_name",
           {{"type", "string"}, {"description", "Friendly label for the entity (optional)."}}}}},
        {"required", {"principal"}},
        {"additionalProperties", false}}},
      {"chax.list_entities",
       "GET",
       "/api/v1/entities",
       "List entities from the catalog.",
       {{"type", "object"},
        {"properties",
         {{"limit", {{"type", "integer"}, {"minimum", 1}}},
          {"cursor", {{"type", "integer"}, {"minimum", 0}}}}},
        {"additionalProperties", false}}},
      {"chax.create_instance",
       "POST",
       "/api/v1/instances",
       "Create or upsert an instance principal in the catalog.",
       {{"type", "object"},
        {"properties",
         {{"principal", {{"type", "string"}, {"description", "Instance principal (required)."}}},
          {"label", {{"type", "string"}, {"description", "Friendly label (optional)."}}},
          {"meta",
           {{"type", "string"},
            {"description", "Opaque metadata string for downstream use (optional)."}}}}},
        {"required", {"principal"}},
        {"additionalProperties", false}}},
      {"chax.list_instances",
       "GET",
       "/api/v1/instances",
       "List instances from the catalog.",
       {{"type", "object"},
        {"properties",
         {{"limit", {{"type", "integer"}, {"minimum", 1}}},
          {"cursor", {{"type", "integer"}, {"minimum", 0}}}}},
        {"additionalProperties", false}}},
      {"chax.evaluate_slug",
       "GET",
       "/api/v1/evaluate/slug/{address_id}",
       "Evaluate a single slug (read-only) and return effective status plus flags.",
       {{"type", "object"},
        {"properties",
         {{"address_id",
           {{"type", "string"}, {"description", "Address ID to evaluate."}}}}},
        {"required", {"address_id"}},
        {"additionalProperties", false}}},
      {"chax.evaluate_graph",
       "POST",
       "/api/v1/evaluate/graph",
       "Evaluate a set of address_ids and return nodes with effective status/flags.",
       {{"type", "object"},
        {"properties",
         {{"root_address_ids",
           {{"type", "array"},
            {"items", {{"type", "string"}}},
            {"description", "Array of address_ids to evaluate."}}}}},
        {"required", {"root_address_ids"}},
        {"additionalProperties", false}}},
      {"chax.export_json",
       "GET",
       "/api/v1/export/json",
       "Export every slug as a JSON array for downstream processing.",
       nlohmann::json::object()},
      {"chax.export_markdown",
       "GET",
       "/api/v1/export/markdown/{checklist}",
       "Export a checklist as Markdown for authoring.",
       {{"type", "object"},
        {"properties",
         {{"checklist",
           {{"type", "string"}, {"description", "Checklist name to export."}}}}},
        {"required", {"checklist"}},
        {"additionalProperties", false}}},
      {"chax.import_markdown",
       "POST",
       "/api/v1/import/markdown",
       "Import canonical Markdown for a checklist instance (defaults to instance||default).",
       {{"type", "object"},
        {"properties",
         {{"checklist",
           {{"type", "string"}, {"description", "Checklist name to import."}}},
          {"instance_principal",
           {{"type", "string"},
            {"description", "Instance principal to derive instance_id (optional)."}}},
          {"markdown",
           {{"type", "string"}, {"description", "Markdown content to ingest."}}}}},
        {"required", {"checklist", "markdown"}},
        {"additionalProperties", false}}},
  };
  return kTools;
}

nlohmann::json MakeToolSchemaJson(const ToolDefinition& tool) {
  return {{"name", tool.name}, {"description", tool.description}, {"inputSchema", tool.input_schema}};
}

nlohmann::json TryParseJson(const std::string& text) {
  nlohmann::json parsed = nlohmann::json::parse(text, nullptr, false);
  if (parsed.is_discarded()) {
    return {};
  }
  return parsed;
}

std::string ToString(const nlohmann::json& value) {
  if (value.is_string()) {
    return value.get<std::string>();
  }
  if (value.is_null()) {
    return {};
  }
  return value.dump();
}

std::string RequireStringArg(const nlohmann::json& arguments, const std::string& key) {
  const auto it = arguments.find(key);
  if (it == arguments.end() || !it->is_string()) {
    throw std::invalid_argument("Missing or invalid argument: " + key);
  }
  return it->get<std::string>();
}

}  // namespace

Bridge::Bridge(std::string base_url) : client_(std::move(base_url)) {
  if (const char* token = std::getenv("CHAX_BEARER_TOKEN")) {
    SetBearerToken(token);
  }
}

void Bridge::SetBearerToken(std::string token) {
  if (token.empty()) {
    default_headers_.erase("Authorization");
  } else {
    default_headers_["Authorization"] = "Bearer " + token;
  }
}

platform::HttpClientResponse Bridge::Get(const std::string& path,
                                         const std::map<std::string, std::string>& query) const {
  return client_.Get(path, query, default_headers_);
}

platform::HttpClientResponse Bridge::Post(const std::string& path, const std::string& body,
                                          const std::map<std::string, std::string>& query,
                                          const std::string& content_type) const {
  return client_.Post(path, body, query, content_type, default_headers_);
}

platform::HttpClientResponse Bridge::Patch(const std::string& path, const std::string& body,
                                           const std::map<std::string, std::string>& query,
                                           const std::string& content_type) const {
  return client_.Patch(path, body, query, content_type, default_headers_);
}

nlohmann::json Bridge::ToolSchemasJson() const {
  nlohmann::json tools = nlohmann::json::array();
  for (const auto& tool : ToolCatalog()) {
    tools.push_back(MakeToolSchemaJson(tool));
  }
  return tools;
}

const std::vector<ToolDefinition>& Bridge::Definitions() const { return ToolCatalog(); }

platform::HttpClientResponse Bridge::CallTool(const std::string& name,
                                              const nlohmann::json& arguments) const {
  if (name == "chax.list_commands") {
    return Get("/api/v1/commands");
  }
  if (name == "chax.list_checklists") {
    return Get("/api/v1/checklists");
  }
  if (name == "chax.list_slugs") {
    std::map<std::string, std::string> query;
    auto maybe_add = [&](const std::string& key) {
      if (const auto it = arguments.find(key); it != arguments.end()) {
        const std::string value = ToString(*it);
        if (!value.empty()) {
          query[key] = value;
        }
      }
    };
    maybe_add("checklist");
    maybe_add("section");
    maybe_add("status");
    maybe_add("limit");
    maybe_add("cursor");
    return Get("/api/v1/slugs", query);
  }
  if (name == "chax.health") {
    return Get("/api/v1/health");
  }
  if (name == "chax.hello") {
    std::map<std::string, std::string> query;
    if (const auto it = arguments.find("name"); it != arguments.end()) {
      query["name"] = ToString(*it);
    }
    return Get("/api/v1/hello", query);
  }
  if (name == "chax.echo") {
    const auto it = arguments.find("payload");
    if (it == arguments.end()) {
      throw std::invalid_argument("chax.echo requires a 'payload' argument");
    }
    const std::string payload = ToString(*it);
    return Post("/api/v1/echo", payload);
  }
  if (name == "chax.get_slug") {
    const auto id = EncodePathSegment(RequireStringArg(arguments, "address_id"));
    return Get("/api/v1/slugs/" + id);
  }
  if (name == "chax.get_checklist") {
    const auto checklist = EncodePathSegment(RequireStringArg(arguments, "checklist"));
    return Get("/api/v1/checklists/" + checklist);
  }
  if (name == "chax.relationships") {
    const auto id = EncodePathSegment(RequireStringArg(arguments, "address_id"));
    return Get("/api/v1/relationships/address/" + id);
  }
  if (name == "chax.create_template_relationship") {
    nlohmann::json payload = {
        {"subject_slug_id", RequireStringArg(arguments, "subject_slug_id")},
        {"predicate", RequireStringArg(arguments, "predicate")},
        {"target_slug_id", RequireStringArg(arguments, "target_slug_id")},
    };
    return Post("/api/v1/relationships/template", payload.dump());
  }
  if (name == "chax.list_template_relationships") {
    std::map<std::string, std::string> query;
    auto maybe_add = [&](const std::string& key) {
      if (const auto it = arguments.find(key); it != arguments.end()) {
        const std::string value = ToString(*it);
        if (!value.empty()) {
          query[key] = value;
        }
      }
    };
    maybe_add("subject_slug_id");
    maybe_add("target_slug_id");
    maybe_add("predicate");
    maybe_add("limit");
    maybe_add("cursor");
    return Get("/api/v1/relationships/template", query);
  }
  if (name == "chax.create_address_relationship") {
    nlohmann::json payload = {
        {"subject_address_id", RequireStringArg(arguments, "subject_address_id")},
        {"predicate", RequireStringArg(arguments, "predicate")},
        {"target_address_id", RequireStringArg(arguments, "target_address_id")},
    };
    return Post("/api/v1/relationships/address", payload.dump());
  }
  if (name == "chax.list_address_relationships") {
    std::map<std::string, std::string> query;
    auto maybe_add = [&](const std::string& key) {
      if (const auto it = arguments.find(key); it != arguments.end()) {
        const std::string value = ToString(*it);
        if (!value.empty()) {
          query[key] = value;
        }
      }
    };
    maybe_add("subject_address_id");
    maybe_add("target_address_id");
    maybe_add("predicate");
    maybe_add("limit");
    maybe_add("cursor");
    return Get("/api/v1/relationships/address", query);
  }
  if (name == "chax.create_entity") {
    nlohmann::json payload = {{"principal", RequireStringArg(arguments, "principal")}};
    if (const auto it = arguments.find("kind"); it != arguments.end()) {
      payload["kind"] = ToString(*it);
    }
    if (const auto it = arguments.find("display_name"); it != arguments.end()) {
      payload["display_name"] = ToString(*it);
    }
    return Post("/api/v1/entities", payload.dump());
  }
  if (name == "chax.list_entities") {
    std::map<std::string, std::string> query;
    auto maybe_add = [&](const std::string& key) {
      if (const auto it = arguments.find(key); it != arguments.end()) {
        const std::string value = ToString(*it);
        if (!value.empty()) {
          query[key] = value;
        }
      }
    };
    maybe_add("limit");
    maybe_add("cursor");
    return Get("/api/v1/entities", query);
  }
  if (name == "chax.create_instance") {
    nlohmann::json payload = {{"principal", RequireStringArg(arguments, "principal")}};
    if (const auto it = arguments.find("label"); it != arguments.end()) {
      payload["label"] = ToString(*it);
    }
    if (const auto it = arguments.find("meta"); it != arguments.end()) {
      payload["meta"] = ToString(*it);
    }
    return Post("/api/v1/instances", payload.dump());
  }
  if (name == "chax.list_instances") {
    std::map<std::string, std::string> query;
    auto maybe_add = [&](const std::string& key) {
      if (const auto it = arguments.find(key); it != arguments.end()) {
        const std::string value = ToString(*it);
        if (!value.empty()) {
          query[key] = value;
        }
      }
    };
    maybe_add("limit");
    maybe_add("cursor");
    return Get("/api/v1/instances", query);
  }
  if (name == "chax.evaluate_slug") {
    const auto id = EncodePathSegment(RequireStringArg(arguments, "address_id"));
    return Get("/api/v1/evaluate/slug/" + id);
  }
  if (name == "chax.evaluate_graph") {
    const auto it = arguments.find("root_address_ids");
    if (it == arguments.end() || !it->is_array()) {
      throw std::invalid_argument("chax.evaluate_graph requires an array root_address_ids");
    }
    nlohmann::json payload = {{"root_address_ids", *it}};
    return Post("/api/v1/evaluate/graph", payload.dump());
  }
  if (name == "chax.update_slug") {
    nlohmann::json payload = nlohmann::json::object();
    const std::string address_id = RequireStringArg(arguments, "address_id");
    if (const auto it = arguments.find("status"); it != arguments.end()) {
      payload["status"] = ToString(*it);
    }
    if (const auto it = arguments.find("result"); it != arguments.end()) {
      payload["result"] = ToString(*it);
    }
    if (const auto it = arguments.find("comment"); it != arguments.end()) {
      payload["comment"] = ToString(*it);
    }
    if (const auto it = arguments.find("timestamp"); it != arguments.end()) {
      payload["timestamp"] = ToString(*it);
    }
    const auto id = EncodePathSegment(address_id);
    return Patch("/api/v1/slugs/" + id, payload.dump());
  }
  if (name == "chax.export_json") {
    return Get("/api/v1/export/json");
  }
  if (name == "chax.export_markdown") {
    const auto checklist = EncodePathSegment(RequireStringArg(arguments, "checklist"));
    return Get("/api/v1/export/markdown/" + checklist);
  }
  if (name == "chax.import_markdown") {
    const auto checklist = RequireStringArg(arguments, "checklist");
    const auto markdown = RequireStringArg(arguments, "markdown");
    std::map<std::string, std::string> query{{"checklist", checklist}};
    if (const auto it = arguments.find("instance_principal");
        it != arguments.end() && it->is_string()) {
      query.emplace("instance_principal", it->get<std::string>());
    }
    return Post("/api/v1/import/markdown", markdown, query, "text/markdown");
  }
  throw std::invalid_argument("Unknown tool: " + name);
}

std::string FormatResponseForDisplay(const platform::HttpClientResponse& response) {
  std::ostringstream stream;
  stream << "HTTP " << response.status;
  if (!response.content_type.empty()) {
    stream << " (" << response.content_type << ")";
  }
  stream << '\n';

  const auto parsed = TryParseJson(response.body);
  if (!parsed.empty()) {
    stream << parsed.dump(2);
  } else {
    stream << response.body;
  }
  return stream.str();
}

}  // namespace core::mcp
