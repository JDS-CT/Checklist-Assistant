#include "core/graph_projection.hpp"

#include "core/checklist_markdown.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <sstream>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "nlohmann/json.hpp"

namespace core {
namespace {

constexpr std::string_view kSlugSuccessorPredicate = "slugSuccessor";
constexpr std::string_view kSlugPredecessorPredicate = "slugPredecessor";
constexpr std::string_view kAddressSuccessorPredicate = "addressSuccessor";
constexpr std::string_view kAddressPredecessorPredicate = "addressPredecessor";

using nlohmann::json;

std::string Trim(std::string value) {
  const auto is_space = [](unsigned char ch) { return std::isspace(ch) != 0; };
  while (!value.empty() && is_space(static_cast<unsigned char>(value.front()))) {
    value.erase(value.begin());
  }
  while (!value.empty() && is_space(static_cast<unsigned char>(value.back()))) {
    value.pop_back();
  }
  return value;
}

std::string ToLower(std::string value) {
  for (char& ch : value) {
    ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  }
  return value;
}

bool IsLineagePredicate(const std::string& predicate) {
  return predicate == kSlugSuccessorPredicate || predicate == kSlugPredecessorPredicate ||
         predicate == kAddressSuccessorPredicate || predicate == kAddressPredecessorPredicate;
}

std::string DeriveSpecKind(const std::string& spec) {
  const std::string value = Trim(spec);
  if (value.empty()) {
    return "empty";
  }
  const std::string lower = ToLower(value);
  if (lower == "true" || lower == "false" || lower == "yes" || lower == "no" ||
      lower == "y" || lower == "n" || lower == "pass" || lower == "fail") {
    return "boolean";
  }
  if (value.rfind("<=", 0) == 0 || value.rfind(">=", 0) == 0 ||
      value.rfind("==", 0) == 0 || value.rfind("!=", 0) == 0 ||
      value.front() == '<' || value.front() == '>' || value.front() == '=') {
    return "comparator";
  }
  if ((value.front() == '[' || value.front() == '(') &&
      (value.back() == ']' || value.back() == ')') && value.find(',') != std::string::npos) {
    return "interval";
  }
  if (value.find("..") != std::string::npos) {
    return "range";
  }

  char* end = nullptr;
  std::strtod(value.c_str(), &end);
  if (end != value.c_str()) {
    while (*end != '\0' && std::isspace(static_cast<unsigned char>(*end)) != 0) {
      ++end;
    }
    bool unit_is_valid = true;
    for (const char* current = end; *current != '\0'; ++current) {
      const unsigned char ch = static_cast<unsigned char>(*current);
      if (!std::isalnum(ch) && ch != '/' && ch != '_' && ch != '-' && ch != '^' && ch != '%') {
        unit_is_valid = false;
        break;
      }
    }
    if (unit_is_valid) {
      return "scalar";
    }
  }
  return "qualitative";
}

std::string DeriveVisualShape(const ChecklistSlug& slug) {
  const std::string searchable =
      ToLower(slug.section + " " + slug.procedure + " " + slug.action);
  if (searchable.find("metric") != std::string::npos ||
      searchable.find("feedback") != std::string::npos) {
    return "metric";
  }

  std::size_t operational_count = 0;
  std::size_t status_branch_count = 0;
  bool has_non_pass_branch = false;
  bool has_gate_or_verify = false;
  for (const auto& edge : slug.relationships) {
    if (IsLineagePredicate(edge.predicate)) {
      continue;
    }
    ++operational_count;
    const std::string predicate_lower = ToLower(edge.predicate);
    if (predicate_lower.find("propagate") != std::string::npos &&
        (predicate_lower.rfind("pass", 0) == 0 || predicate_lower.rfind("fail", 0) == 0 ||
         predicate_lower.rfind("other", 0) == 0 || predicate_lower.rfind("na", 0) == 0)) {
      ++status_branch_count;
      if (predicate_lower.rfind("fail", 0) == 0 || predicate_lower.rfind("other", 0) == 0 ||
          predicate_lower.rfind("na", 0) == 0) {
        has_non_pass_branch = true;
      }
    }
    if (edge.predicate.rfind("BoolVerify", 0) == 0 ||
        edge.predicate.find("Gate") != std::string::npos) {
      has_gate_or_verify = true;
    }
  }
  if (operational_count == 0) {
    return "terminal";
  }
  if (has_non_pass_branch || has_gate_or_verify || status_branch_count > 1) {
    return "decision";
  }
  return "action";
}

std::string EscapeDot(const std::string& value) {
  std::string result;
  result.reserve(value.size() + 8);
  for (const char ch : value) {
    switch (ch) {
      case '\\':
        result += "\\\\";
        break;
      case '"':
        result += "\\\"";
        break;
      case '\n':
      case '\r':
        result += "\\n";
        break;
      default:
        result.push_back(ch);
        break;
    }
  }
  return result;
}

std::string EscapeMermaid(std::string value) {
  for (char& ch : value) {
    if (ch == '"' || ch == '[' || ch == ']' || ch == '|' || ch == '{' || ch == '}') {
      ch = '\'';
    } else if (ch == '\n' || ch == '\r') {
      ch = ' ';
    }
  }
  return value;
}

std::string DotShapeFor(const std::string& visual_shape) {
  if (visual_shape == "decision") {
    return "diamond";
  }
  if (visual_shape == "terminal") {
    return "oval";
  }
  if (visual_shape == "metric") {
    return "note";
  }
  return "box";
}

std::string ShortLabel(const std::string& value, std::size_t limit = 80) {
  if (value.size() <= limit) {
    return value;
  }
  return value.substr(0, limit - 3) + "...";
}

bool IsSafeRelativePath(const std::filesystem::path& value) {
  if (value.empty() || value.is_absolute() || value.has_root_name()) {
    return false;
  }
  for (const auto& part : value) {
    if (part == "..") {
      return false;
    }
  }
  return true;
}

std::vector<std::vector<std::string>> ParseCsv(const std::string& input) {
  std::vector<std::vector<std::string>> rows;
  std::vector<std::string> row;
  std::string cell;
  bool quoted = false;
  for (std::size_t index = 0; index < input.size(); ++index) {
    const char ch = input[index];
    if (quoted) {
      if (ch == '"') {
        if (index + 1 < input.size() && input[index + 1] == '"') {
          cell.push_back('"');
          ++index;
        } else {
          quoted = false;
        }
      } else {
        cell.push_back(ch);
      }
      continue;
    }
    if (ch == '"' && cell.empty()) {
      quoted = true;
    } else if (ch == ',') {
      row.push_back(std::move(cell));
      cell.clear();
    } else if (ch == '\n') {
      if (!cell.empty() && cell.back() == '\r') {
        cell.pop_back();
      }
      row.push_back(std::move(cell));
      cell.clear();
      rows.push_back(std::move(row));
      row.clear();
    } else {
      cell.push_back(ch);
    }
  }
  if (!cell.empty() || !row.empty()) {
    if (!cell.empty() && cell.back() == '\r') {
      cell.pop_back();
    }
    row.push_back(std::move(cell));
    rows.push_back(std::move(row));
  }
  if (!rows.empty() && !rows.front().empty() && rows.front().front().size() >= 3 &&
      static_cast<unsigned char>(rows.front().front()[0]) == 0xEF &&
      static_cast<unsigned char>(rows.front().front()[1]) == 0xBB &&
      static_cast<unsigned char>(rows.front().front()[2]) == 0xBF) {
    rows.front().front().erase(0, 3);
  }
  return rows;
}

std::optional<std::pair<std::string, std::string>> ParseHeaderDerivedBinding(
    const std::string& header) {
  const auto separator = header.find('-');
  if (separator != 16 || header.size() <= separator + 1) {
    return std::nullopt;
  }
  const std::string slug_id = header.substr(0, separator);
  for (const char ch : slug_id) {
    if (std::isalnum(static_cast<unsigned char>(ch)) == 0) {
      return std::nullopt;
    }
  }
  const auto label_start = header.find('(', separator + 1);
  const std::string field = header.substr(separator + 1,
                                          label_start == std::string::npos
                                              ? std::string::npos
                                              : label_start - separator - 1);
  if (field != "result" && field != "status" && field != "comment") {
    return std::nullopt;
  }
  return std::make_pair(slug_id, field);
}

std::string NodeColorFor(const std::string& kind) {
  if (kind == "dataset") {
    return "#e8f4ea";
  }
  if (kind == "dataset_column") {
    return "#f5f7df";
  }
  if (kind == "terminal" || kind == "external") {
    return "#fff4e5";
  }
  if (kind == "mutation_source") {
    return "#f3e8ff";
  }
  return "#eaf1fb";
}

std::string NodeShapeFor(const std::string& kind) {
  if (kind == "dataset") {
    return "cylinder";
  }
  if (kind == "dataset_column") {
    return "note";
  }
  if (kind == "terminal") {
    return "oval";
  }
  if (kind == "external") {
    return "component";
  }
  if (kind == "mutation_source") {
    return "hexagon";
  }
  return "box";
}

}  // namespace

ChecklistGraph BuildChecklistGraph(const std::vector<ChecklistSlug>& slugs) {
  ChecklistGraph graph;
  if (slugs.empty()) {
    return graph;
  }

  std::vector<const ChecklistSlug*> ordered;
  ordered.reserve(slugs.size());
  for (const auto& slug : slugs) {
    if (graph.checklist.empty()) {
      graph.checklist = slug.checklist;
      graph.instance_id = slug.instance_id;
    }
    if (slug.checklist != graph.checklist || slug.instance_id != graph.instance_id) {
      graph.warnings.push_back(
          "Projection input contains multiple checklists or instances; incompatible rows were ignored.");
      continue;
    }
    ordered.push_back(&slug);
  }
  std::sort(ordered.begin(), ordered.end(), [](const ChecklistSlug* lhs, const ChecklistSlug* rhs) {
    if (lhs->address_order != rhs->address_order) {
      return lhs->address_order < rhs->address_order;
    }
    return lhs->address_id < rhs->address_id;
  });

  std::unordered_map<std::string, std::size_t> node_index;
  node_index.reserve(ordered.size());
  for (const ChecklistSlug* slug : ordered) {
    ChecklistGraphNode node;
    node.address_id = slug->address_id;
    node.slug_id = slug->slug_id;
    node.address_order = slug->address_order;
    node.section = slug->section;
    node.procedure = slug->procedure;
    node.action = slug->action;
    node.spec = slug->spec;
    node.result = slug->result;
    node.status = StatusToString(slug->status);
    node.comment = slug->comment;
    node.instructions = slug->instructions;
    node.spec_kind = DeriveSpecKind(slug->spec);
    node.visual_shape = DeriveVisualShape(*slug);
    node_index.emplace(node.address_id, graph.nodes.size());
    graph.nodes.push_back(std::move(node));
  }

  for (std::size_t index = 1; index < graph.nodes.size(); ++index) {
    graph.edges.push_back(ChecklistGraphEdge{
        graph.nodes[index - 1].address_id,
        graph.nodes[index].address_id,
        "checklistOrder",
        "checklistOrder",
        false,
        false,
        "local",
    });
  }

  for (const ChecklistSlug* slug : ordered) {
    std::vector<RelationshipEdge> relationships = slug->relationships;
    std::sort(relationships.begin(), relationships.end(),
              [](const RelationshipEdge& lhs, const RelationshipEdge& rhs) {
                return std::tie(lhs.predicate, lhs.target) < std::tie(rhs.predicate, rhs.target);
              });
    for (const auto& relationship : relationships) {
      const bool is_lineage = IsLineagePredicate(relationship.predicate);
      const bool is_external = node_index.find(relationship.target) == node_index.end();
      ChecklistGraphEdge edge{
          slug->address_id,
          relationship.target,
          relationship.predicate,
          "relationship",
          is_lineage,
          is_external,
          is_external ? (is_lineage ? "legacy" : "unknown_external") : "local",
      };
      graph.edges.push_back(std::move(edge));
    }
  }

  for (const auto& edge : graph.edges) {
    if (edge.kind != "relationship") {
      continue;
    }
    const auto source = node_index.find(edge.source_address_id);
    if (source != node_index.end()) {
      ++graph.nodes[source->second].outgoing_relationship_count;
    }
    const auto target = node_index.find(edge.target_address_id);
    if (target != node_index.end()) {
      ++graph.nodes[target->second].incoming_relationship_count;
    }
  }
  return graph;
}

std::string RenderChecklistGraphDot(const ChecklistGraph& graph) {
  std::ostringstream out;
  out << "digraph checklist {\n"
      << "  graph [rankdir=\"LR\", concentrate=\"true\", overlap=\"false\"];\n"
      << "  node [style=\"rounded,filled\", fillcolor=\"#eaf1fb\", color=\"#486581\", "
         "fontname=\"Segoe UI\"];\n"
      << "  edge [fontname=\"Segoe UI\", fontsize=\"10\", color=\"#61758a\"];\n\n";

  std::map<std::string, std::vector<std::size_t>> sections;
  for (std::size_t index = 0; index < graph.nodes.size(); ++index) {
    sections[graph.nodes[index].section].push_back(index);
  }
  std::unordered_map<std::string, std::size_t> node_index;
  for (std::size_t index = 0; index < graph.nodes.size(); ++index) {
    node_index.emplace(graph.nodes[index].address_id, index);
  }
  std::size_t section_index = 0;
  for (const auto& entry : sections) {
    out << "  subgraph cluster_section_" << section_index++ << " {\n"
        << "    label=\"" << EscapeDot(entry.first) << "\";\n"
        << "    color=\"#b5c5d8\";\n"
        << "    style=\"rounded\";\n";
    for (const std::size_t index : entry.second) {
      const auto& node = graph.nodes[index];
      out << "    n" << index << " [shape=\"" << DotShapeFor(node.visual_shape) << "\", label=\""
          << EscapeDot(node.procedure + "\\n" + node.action) << "\"];\n";
    }
    out << "  }\n\n";
  }

  std::unordered_map<std::string, std::size_t> external_index;
  for (const auto& edge : graph.edges) {
    if (!edge.is_external) {
      continue;
    }
    if (external_index.emplace(edge.target_address_id, external_index.size()).second) {
      const std::size_t index = external_index.at(edge.target_address_id);
      out << "  x" << index << " [shape=\"component\", fillcolor=\"#fff4e5\", color=\"#a45d13\", "
          << "label=\"external\\n" << EscapeDot(edge.target_address_id) << "\"];\n";
    }
  }
  if (!external_index.empty()) {
    out << "\n";
  }
  for (const auto& edge : graph.edges) {
    const auto source = node_index.find(edge.source_address_id);
    if (source == node_index.end()) {
      continue;
    }
    std::string target;
    const auto local_target = node_index.find(edge.target_address_id);
    if (local_target != node_index.end()) {
      target = "n" + std::to_string(local_target->second);
    } else {
      target = "x" + std::to_string(external_index.at(edge.target_address_id));
    }
    const std::string style = edge.kind == "checklistOrder" ? "dotted" : "solid";
    out << "  n" << source->second << " -> " << target << " [label=\""
        << EscapeDot(edge.predicate) << "\", style=\"" << style << "\"];\n";
  }
  out << "}\n";
  return out.str();
}

std::string RenderChecklistGraphMermaid(const ChecklistGraph& graph) {
  std::ostringstream out;
  out << "flowchart LR\n"
      << "  %% Checklist Assistant graph projection. checklistOrder is derived display order.\n\n";

  std::map<std::string, std::vector<std::size_t>> sections;
  for (std::size_t index = 0; index < graph.nodes.size(); ++index) {
    sections[graph.nodes[index].section].push_back(index);
  }
  std::unordered_map<std::string, std::size_t> node_index;
  for (std::size_t index = 0; index < graph.nodes.size(); ++index) {
    node_index.emplace(graph.nodes[index].address_id, index);
  }
  std::size_t section_index = 0;
  for (const auto& entry : sections) {
    out << "  subgraph section_" << section_index++ << "[\"" << EscapeMermaid(entry.first) << "\"]\n";
    for (const std::size_t index : entry.second) {
      const auto& node = graph.nodes[index];
      out << "    n" << index << "[\"" << EscapeMermaid(node.procedure);
      if (!node.action.empty()) {
        out << "<br/>" << EscapeMermaid(node.action);
      }
      out << "\"]\n";
    }
    out << "  end\n\n";
  }

  std::unordered_map<std::string, std::size_t> external_index;
  for (const auto& edge : graph.edges) {
    if (edge.is_external && external_index.emplace(edge.target_address_id, external_index.size()).second) {
      out << "  x" << external_index.at(edge.target_address_id) << "[\"external<br/>"
          << EscapeMermaid(edge.target_address_id) << "\"]\n";
    }
  }
  if (!external_index.empty()) {
    out << "\n";
  }
  for (const auto& edge : graph.edges) {
    const auto source = node_index.find(edge.source_address_id);
    if (source == node_index.end()) {
      continue;
    }
    const auto target = node_index.find(edge.target_address_id);
    const std::string target_id =
        target != node_index.end() ? "n" + std::to_string(target->second)
                                   : "x" + std::to_string(external_index.at(edge.target_address_id));
    if (edge.kind == "checklistOrder") {
      out << "  n" << source->second << " ==>|order| " << target_id << "\n";
    } else {
      out << "  n" << source->second << " -.->|" << EscapeMermaid(edge.predicate) << "| "
          << target_id << "\n";
    }
  }
  return out.str();
}

std::string RenderChecklistRuntimeSchemaDbml() {
  return R"dbml(Project checklist_assistant_runtime {
  database_type: 'SQLite'
  Note: 'Mode A core runtime schema. Relationship predicates remain opaque values; profile and deployment-specific domain tables are intentionally not represented here.'
}

Table entities {
  entity_id text [pk]
  principal text [not null]
  kind text [not null]
  display_name text
  meta text
}

Table instance_catalog {
  instance_id text [pk]
  principal text [not null]
  label text
  meta text
}

Table slugs {
  address_id text [pk]
  slug_id text [not null]
  instance_id text [not null]
  checklist text [not null]
  section text [not null]
  procedure text [not null]
  action text [not null]
  spec text [not null]
  instructions text [not null]
  result text
  status text [not null, note: 'Pass, Fail, NA, Other, or Unknown']
  comment text
  timestamp text
  entity_id text [not null]
}

Table slug_order {
  address_id text [pk]
  address_order integer [not null, note: 'Gapped persisted order; compact display order is derived']
}

Table slug_ownership {
  slug_id text [not null]
  checklist text [not null]
  source_name text [not null]
  source_path text
  pack text [not null]
  checklist_dir text [not null]
  updated_at text [not null]

  indexes {
    (slug_id, source_name, pack, checklist_dir) [pk]
  }
}

Table address_ownership {
  address_id text [not null]
  slug_id text [not null]
  instance_id text [not null]
  checklist text [not null]
  source_name text [not null]
  source_path text
  pack text [not null]
  checklist_dir text [not null]
  updated_at text [not null]

  indexes {
    (address_id, source_name, pack, checklist_dir) [pk]
  }
}

Table template_relationships {
  subject_slug_id text [not null, note: 'Logical template row identity']
  predicate text [not null, note: 'Opaque predicate; optional catalog entry in predicates']
  target_slug_id text [not null, note: 'Logical template row identity; self-reference is valid for predicate-defined self-rules']
}

Table address_relationships {
  subject_address_id text [not null, note: 'Runtime row identity']
  predicate text [not null, note: 'Opaque predicate; optional catalog entry in predicates']
  target_address_id text [not null, note: 'Runtime row identity; self-reference is predicate-dependent']
}

Table predicates {
  name text [pk]
  kind text [not null]
  status text [not null]
  description text
  meta text
}

Table history {
  address_id text [not null]
  timestamp text [not null]
  result text
  status text
  comment text
  entity_id text [not null]

  indexes {
    (address_id, timestamp) [pk]
  }
}

Ref: slugs.entity_id > entities.entity_id
Ref: slug_order.address_id - slugs.address_id
Ref: address_ownership.address_id > slugs.address_id
Ref: address_relationships.subject_address_id > slugs.address_id
Ref: address_relationships.target_address_id > slugs.address_id
Ref: history.address_id > slugs.address_id
Ref: history.entity_id > entities.entity_id
)dbml";
}

json BuildRelationshipWorkbench(const ChecklistGraph& graph,
                                const std::filesystem::path& checklist_root) {
  json nodes = json::array();
  json edges = json::array();
  json findings = json::array();
  std::unordered_set<std::string> node_ids;
  std::unordered_set<std::string> connected_addresses;
  std::unordered_map<std::string, std::vector<std::string>> self_predicates_by_address;
  std::unordered_set<std::string> non_self_predicate_addresses;
  std::unordered_set<std::string> declared_support_addresses;
  std::unordered_map<std::string, std::vector<std::string>> addresses_by_slug;
  std::unordered_set<std::string> local_addresses;
  std::size_t predicate_count = 0;
  std::size_t binding_count = 0;
  std::size_t dataset_count = 0;

  const auto add_node = [&nodes, &node_ids](const std::string& id, const std::string& kind,
                                             const std::string& title, const std::string& subtitle,
                                             const json& details = json::object()) {
    if (!node_ids.insert(id).second) {
      return;
    }
    json node{{"id", id}, {"kind", kind}, {"title", title}, {"subtitle", subtitle}};
    if (!details.empty()) {
      node["details"] = details;
    }
    nodes.push_back(std::move(node));
  };
  const auto add_edge = [&edges](const std::string& source_id, const std::string& target_id,
                                 const std::string& edge_class, const std::string& label,
                                 const json& details = json::object()) {
    json edge{{"source_id", source_id},
              {"target_id", target_id},
              {"class", edge_class},
              {"label", label}};
    if (!details.empty()) {
      edge["details"] = details;
    }
    edges.push_back(std::move(edge));
  };
  const auto add_finding = [&findings](const std::string& code, const std::string& severity,
                                       const std::string& message, const std::string& node_id,
                                       const json& details = json::object()) {
    json finding{{"code", code}, {"severity", severity}, {"message", message}};
    if (!node_id.empty()) {
      finding["node_id"] = node_id;
    }
    if (!details.empty()) {
      finding["details"] = details;
    }
    findings.push_back(std::move(finding));
  };

  for (const auto& node : graph.nodes) {
    const std::string node_id = "row:" + node.address_id;
    add_node(node_id, "checklist_row", node.procedure,
             node.action.empty() ? node.section : node.section + " · " + node.action,
             {{"address_id", node.address_id}, {"slug_id", node.slug_id}, {"section", node.section}});
    local_addresses.insert(node.address_id);
    addresses_by_slug[node.slug_id].push_back(node.address_id);
  }

  for (const auto& edge : graph.edges) {
    if (edge.kind != "relationship") {
      continue;
    }
    const std::string source_id = "row:" + edge.source_address_id;
    const bool target_is_local =
        local_addresses.find(edge.target_address_id) != local_addresses.end();
    std::string target_id;
    if (target_is_local) {
      target_id = "row:" + edge.target_address_id;
      connected_addresses.insert(edge.target_address_id);
    } else {
      target_id = "external:" + edge.target_address_id;
      add_node(target_id, "external", edge.target_address_id, edge.external_category,
               {{"address_id", edge.target_address_id}});
    }
    add_edge(source_id, target_id, "predicate", edge.predicate,
             {{"is_lineage", edge.is_lineage}, {"is_external", edge.is_external}});
    connected_addresses.insert(edge.source_address_id);
    if (!edge.is_lineage && target_is_local &&
        edge.source_address_id == edge.target_address_id) {
      self_predicates_by_address[edge.source_address_id].push_back(edge.predicate);
    } else if (!edge.is_lineage) {
      non_self_predicate_addresses.insert(edge.source_address_id);
      if (target_is_local) {
        non_self_predicate_addresses.insert(edge.target_address_id);
      }
    }
    ++predicate_count;
  }

  std::unordered_map<std::string, std::vector<std::string>> legacy_alias_slugs;
  std::vector<TemplateRelationship> markdown_relationships;
  {
    const auto markdown_path = checklist_root / "checklist.md";
    std::ifstream input(markdown_path, std::ios::binary);
    if (input) {
      const std::string contents((std::istreambuf_iterator<char>(input)),
                                 std::istreambuf_iterator<char>());
      try {
        const auto parsed = markdown::ParseChecklistMarkdown("", contents);
        markdown_relationships = parsed.template_relationships;
        for (const auto& relationship : parsed.template_relationships) {
          if (relationship.predicate == kSlugPredecessorPredicate) {
            legacy_alias_slugs[relationship.target_slug_id].push_back(
                relationship.subject_slug_id);
          } else if (relationship.predicate == kSlugSuccessorPredicate) {
            legacy_alias_slugs[relationship.subject_slug_id].push_back(
                relationship.target_slug_id);
          }
        }
      } catch (const std::exception& ex) {
        add_finding("RELATIONSHIP_ALIAS_PARSE_FAILED", "warning",
                    "Legacy relationship aliases could not be read from checklist.md.", "",
                    {{"path", markdown_path.string()}, {"error", ex.what()}});
      }
    }
  }
  const auto resolve_addresses = [&addresses_by_slug, &legacy_alias_slugs](
                                     const std::string& slug_id, std::string* alias_used) {
    std::vector<std::string> resolved;
    const auto direct = addresses_by_slug.find(slug_id);
    if (direct != addresses_by_slug.end()) {
      return direct->second;
    }
    const auto legacy = legacy_alias_slugs.find(slug_id);
    if (legacy == legacy_alias_slugs.end()) {
      return resolved;
    }
    if (alias_used) {
      *alias_used = slug_id;
    }
    for (const auto& current_slug_id : legacy->second) {
      const auto current = addresses_by_slug.find(current_slug_id);
      if (current == addresses_by_slug.end()) {
        continue;
      }
      resolved.insert(resolved.end(), current->second.begin(), current->second.end());
    }
    return resolved;
  };
  std::unordered_map<std::string, json> unresolved_markdown_relationships;
  for (const auto& relationship : markdown_relationships) {
    if (relationship.predicate == kSlugPredecessorPredicate ||
        relationship.predicate == kSlugSuccessorPredicate ||
        addresses_by_slug.find(relationship.subject_slug_id) == addresses_by_slug.end()) {
      continue;
    }
    std::string alias_used;
    if (!resolve_addresses(relationship.target_slug_id, &alias_used).empty()) {
      continue;
    }
    unresolved_markdown_relationships[relationship.subject_slug_id].push_back(
        {{"predicate", relationship.predicate}, {"target_slug_id", relationship.target_slug_id}});
  }
  const auto render_legacy_alias = [&add_node, &add_edge](const std::string& legacy_slug_id,
                                                           const std::vector<std::string>& addresses) {
    if (legacy_slug_id.empty()) {
      return;
    }
    const std::string alias_id = "legacy_alias:" + legacy_slug_id;
    add_node(alias_id, "external", legacy_slug_id, "legacy slug alias",
             {{"slug_id", legacy_slug_id}, {"role", "legacy_alias"}});
    for (const auto& address_id : addresses) {
      add_edge(alias_id, "row:" + address_id, "legacy_alias", "maps to current row",
               {{"legacy_slug_id", legacy_slug_id}});
    }
  };

  const auto relationship_directory = checklist_root / "relationships";
  const auto bindings_path = relationship_directory / "bindings.json";
  json declarations = json::object();
  bool has_declarations = false;
  {
    std::ifstream input(bindings_path, std::ios::binary);
    if (!input) {
      add_finding("RELATIONSHIP_PACKAGE_MISSING", "warning",
                  "No relationships/bindings.json was found; legacy rows remain visible but relationship completeness cannot be declared.",
                  "", {{"path", bindings_path.string()}});
    } else {
      const std::string contents((std::istreambuf_iterator<char>(input)),
                                 std::istreambuf_iterator<char>());
      declarations = json::parse(contents, nullptr, false);
      if (declarations.is_discarded() || !declarations.is_object()) {
        add_finding("RELATIONSHIP_DECLARATIONS_INVALID", "error",
                    "relationships/bindings.json is not a JSON object.", "",
                    {{"path", bindings_path.string()}});
        declarations = json::object();
      } else {
        has_declarations = true;
        const std::string schema = declarations.value("schema", "");
        if (schema != "chax-relationships-v1") {
          add_finding("RELATIONSHIP_SCHEMA_UNSUPPORTED", "warning",
                      "The relationship declaration schema is not chax-relationships-v1.", "",
                      {{"schema", schema}, {"path", bindings_path.string()}});
        }
      }
    }
  }

  if (has_declarations) {
    const json terminal_rows = declarations.value("completeness", json::object())
                                   .value("terminal_rows", json::array());
    if (!terminal_rows.is_array()) {
      add_finding("TERMINAL_ROWS_INVALID", "warning",
                  "completeness.terminal_rows must be an array when present.", "");
    } else {
      for (const auto& terminal : terminal_rows) {
        if (!terminal.is_object()) {
          add_finding("TERMINAL_ROW_INVALID", "warning",
                      "A terminal row declaration must be an object.", "");
          continue;
        }
        const std::string slug_id = terminal.value("slug_id", "");
        const std::string stakeholder = terminal.value("stakeholder", "");
        const std::string reason = terminal.value("reason", "");
        std::string legacy_alias;
        const auto matching = resolve_addresses(slug_id, &legacy_alias);
        if (slug_id.empty() || stakeholder.empty() || reason.empty() ||
            matching.empty()) {
          add_finding("TERMINAL_ROW_UNKNOWN", "warning",
                      "A terminal relationship must name an imported row, stakeholder, and reason.", "",
                      {{"slug_id", slug_id}, {"stakeholder", stakeholder}});
          continue;
        }
        const std::string terminal_id = "terminal:" + slug_id;
        add_node(terminal_id, "terminal", stakeholder, reason,
                 {{"slug_id", slug_id}, {"stakeholder", stakeholder}, {"reason", reason}});
        render_legacy_alias(legacy_alias, matching);
        for (const auto& address_id : matching) {
          add_edge("row:" + address_id, terminal_id, "declared_terminal", "terminal outcome",
                   {{"stakeholder", stakeholder}, {"reason", reason}});
          connected_addresses.insert(address_id);
          declared_support_addresses.insert(address_id);
        }
      }
    }

    const json mutation_sources = declarations.value("mutation_sources", json::array());
    if (!mutation_sources.is_array()) {
      add_finding("MUTATION_SOURCES_INVALID", "warning",
                  "mutation_sources must be an array when present.", "");
    } else {
      std::size_t mutation_index = 0;
      for (const auto& source : mutation_sources) {
        if (!source.is_object()) {
          continue;
        }
        const std::string slug_id = source.value("slug_id", "");
        const std::string name = source.value("name", source.value("kind", "Declared mutation source"));
        std::string legacy_alias;
        const auto matching = resolve_addresses(slug_id, &legacy_alias);
        if (slug_id.empty() || matching.empty()) {
          add_finding("MUTATION_SOURCE_TARGET_MISSING", "warning",
                      "A declared mutation source does not identify an imported target row.", "",
                      {{"slug_id", slug_id}, {"name", name}});
          continue;
        }
        const std::string source_id = "mutation:" + std::to_string(mutation_index++);
        add_node(source_id, "mutation_source", name, source.value("field", "mutable field"), source);
        render_legacy_alias(legacy_alias, matching);
        for (const auto& address_id : matching) {
          add_edge(source_id, "row:" + address_id, "direct_write", source.value("field", "write"), source);
          connected_addresses.insert(address_id);
          declared_support_addresses.insert(address_id);
        }
      }
    }

    const json datasets = declarations.value("datasets", json::array());
    if (!datasets.is_array()) {
      add_finding("DATASETS_INVALID", "warning", "datasets must be an array when present.", "");
    } else {
      for (std::size_t dataset_index = 0; dataset_index < datasets.size(); ++dataset_index) {
        const json& dataset = datasets[dataset_index];
        if (!dataset.is_object()) {
          add_finding("DATASET_INVALID", "warning", "A dataset declaration must be an object.", "");
          continue;
        }
        const std::string relative_path = dataset.value("path", "");
        if (!IsSafeRelativePath(std::filesystem::path(relative_path))) {
          add_finding("DATASET_PATH_INVALID", "warning",
                      "A relationship dataset path must be a relative path inside the checklist package.", "",
                      {{"path", relative_path}});
          continue;
        }
        const auto data_path = checklist_root / std::filesystem::path(relative_path);
        std::ifstream input(data_path, std::ios::binary);
        if (!input) {
          add_finding("DATASET_MISSING", "warning",
                      "A declared relationship dataset could not be read.", "",
                      {{"path", data_path.string()}});
          continue;
        }
        const std::string contents((std::istreambuf_iterator<char>(input)),
                                   std::istreambuf_iterator<char>());
        const auto csv_rows = ParseCsv(contents);
        if (csv_rows.empty() || csv_rows.front().empty()) {
          add_finding("DATASET_EMPTY", "warning",
                      "A declared relationship dataset has no CSV header row.", "",
                      {{"path", relative_path}});
          continue;
        }

        const std::string dataset_id = "dataset:" + std::to_string(dataset_index);
        const auto& headers = csv_rows.front();
        const std::size_t record_count = csv_rows.size() - 1;
        add_node(dataset_id, "dataset", relative_path,
                 std::to_string(record_count) + " records · " + std::to_string(headers.size()) + " columns",
                 {{"path", relative_path}, {"records", record_count}, {"columns", headers.size()}});
        ++dataset_count;

        const json lookup = dataset.value("lookup", json::object());
        bool lookup_resolved = false;
        if (lookup.is_object()) {
          const std::string lookup_slug_id = lookup.value("slug_id", "");
          const std::string lookup_field = lookup.value("field", "");
          const std::string lookup_column = lookup.value("column", "");
          std::string legacy_alias;
          const auto matching = resolve_addresses(lookup_slug_id, &legacy_alias);
          if (lookup_slug_id.empty() || lookup_field.empty() || lookup_column.empty() ||
              matching.empty()) {
            add_finding("LOOKUP_KEY_INVALID", "warning",
                        "A dataset lookup must reference an imported slug, field, and source column.",
                        dataset_id, {{"slug_id", lookup_slug_id}, {"column", lookup_column}});
          } else {
            lookup_resolved = true;
            render_legacy_alias(legacy_alias, matching);
            for (const auto& address_id : matching) {
              add_edge("row:" + address_id, dataset_id, "lookup_key",
                       lookup_field + " selects " + lookup_column,
                       {{"field", lookup_field}, {"column", lookup_column},
                        {"legacy_slug_id", legacy_alias}});
              connected_addresses.insert(address_id);
              declared_support_addresses.insert(address_id);
              ++binding_count;
            }
          }
        }

        const bool header_derived = dataset.value("bindings", "") == "header-derived";
        std::size_t bound_column_count = 0;
        for (std::size_t column_index = 0; column_index < headers.size(); ++column_index) {
          std::size_t nonempty_count = 0;
          std::size_t character_count = 0;
          std::unordered_set<std::string> distinct_values;
          for (std::size_t row_index = 1; row_index < csv_rows.size(); ++row_index) {
            const auto& row = csv_rows[row_index];
            const std::string value = column_index < row.size() ? row[column_index] : "";
            if (!value.empty()) {
              ++nonempty_count;
              character_count += value.size();
              distinct_values.insert(value);
            }
          }
          const std::string column_id = dataset_id + ":column:" + std::to_string(column_index);
          add_node(column_id, "dataset_column", ShortLabel(headers[column_index]),
                   std::to_string(nonempty_count) + "/" + std::to_string(record_count) +
                       " populated · " + std::to_string(distinct_values.size()) + " distinct",
                   {{"header", headers[column_index]},
                    {"records", record_count},
                    {"nonempty", nonempty_count},
                    {"distinct", distinct_values.size()},
                    {"characters", character_count}});
          add_edge(dataset_id, column_id, "contains_column", "column");

          if (record_count > 0 && nonempty_count == record_count && distinct_values.size() == 1) {
            add_finding("CONSTANT_COLUMN", "info",
                        "Every dataset record has the same non-empty value in this column.", column_id,
                        {{"header", headers[column_index]},
                         {"records", record_count},
                         {"nonempty", nonempty_count},
                         {"distinct", distinct_values.size()},
                         {"characters", character_count},
                         {"recommendation",
                          "Review a future normalization: store the shared literal once in a named "
                          "reference record, retain this CSV column through a reviewed migration, and "
                          "bind consumers explicitly. No data is changed by this finding."}});
          }
          if (nonempty_count > 1 && nonempty_count < record_count && distinct_values.size() == 1) {
            add_finding("REPEATED_LITERAL", "info",
                        "The same non-empty value occurs in every populated cell; other records are blank.",
                        column_id,
                        {{"header", headers[column_index]},
                         {"records", record_count},
                         {"nonempty", nonempty_count},
                         {"blank", record_count - nonempty_count},
                         {"distinct", distinct_values.size()},
                         {"characters", character_count}});
          }

          if (!header_derived) {
            continue;
          }
          const auto parsed_binding = ParseHeaderDerivedBinding(headers[column_index]);
          if (!parsed_binding) {
            continue;
          }
          std::string legacy_alias;
          const auto matching = resolve_addresses(parsed_binding->first, &legacy_alias);
          if (matching.empty()) {
            const std::string unresolved_id = "unresolved:" + parsed_binding->first + ":" + parsed_binding->second;
            add_node(unresolved_id, "external", parsed_binding->first, parsed_binding->second,
                     {{"slug_id", parsed_binding->first}, {"field", parsed_binding->second}});
            add_edge(column_id, unresolved_id, "column_binding", parsed_binding->second);
            add_finding("COLUMN_BINDING_TARGET_MISSING", "warning",
                        "A header-derived binding does not match an imported checklist row.", column_id,
                        {{"slug_id", parsed_binding->first}, {"field", parsed_binding->second}});
            continue;
          }
          ++bound_column_count;
          render_legacy_alias(legacy_alias, matching);
          for (const auto& address_id : matching) {
            add_edge(column_id, "row:" + address_id, "column_binding", parsed_binding->second,
                     {{"slug_id", parsed_binding->first}, {"field", parsed_binding->second},
                      {"legacy_slug_id", legacy_alias}});
            connected_addresses.insert(address_id);
            declared_support_addresses.insert(address_id);
            ++binding_count;
          }
        }
        const std::size_t bound_row_count = [&]() {
          std::unordered_set<std::string> addresses;
          for (const auto& edge : edges) {
            if (edge.value("class", "") == "column_binding" &&
                edge.value("source_id", "").rfind(dataset_id + ":column:", 0) == 0) {
              const std::string target_id = edge.value("target_id", "");
              if (target_id.rfind("row:", 0) == 0) {
                addresses.insert(target_id.substr(4));
              }
            }
          }
          return addresses.size();
        }();
        const double bound_row_coverage = graph.nodes.empty()
                                                ? 0.0
                                                : static_cast<double>(bound_row_count) /
                                                      static_cast<double>(graph.nodes.size());
        if (lookup_resolved && bound_column_count >= 3 && bound_row_coverage >= 0.25) {
          add_finding("HIGH_FAN_OUT_LOOKUP_KEY", "info",
                      "One declared lookup binds a substantial share of the current checklist.",
                      dataset_id,
                      {{"bound_columns", bound_column_count},
                       {"bound_rows", bound_row_count},
                       {"checklist_rows", graph.nodes.size()},
                       {"bound_row_coverage", bound_row_coverage},
                       {"minimum_bound_columns", 3},
                       {"minimum_bound_row_coverage", 0.25}});
        }
      }
    }
  }

  std::size_t orphan_count = 0;
  for (const auto& node : graph.nodes) {
    if (connected_addresses.find(node.address_id) != connected_addresses.end()) {
      continue;
    }
    ++orphan_count;
    json details{{"address_id", node.address_id},
                 {"slug_id", node.slug_id},
                 {"section", node.section},
                 {"procedure", node.procedure},
                 {"action", node.action},
                 {"derived_checklist_order_excluded", true}};
    const auto unresolved = unresolved_markdown_relationships.find(node.slug_id);
    if (unresolved != unresolved_markdown_relationships.end()) {
      details["unresolved_markdown_relationships"] = unresolved->second;
    }
    add_finding("ORPHAN_ROW", "warning",
                "This row has no emitted predicate, lookup/binding, declared mutation source, or declared terminal relationship.",
                "row:" + node.address_id, details);
  }

  std::size_t self_only_count = 0;
  for (const auto& node : graph.nodes) {
    const auto self_predicates = self_predicates_by_address.find(node.address_id);
    if (self_predicates == self_predicates_by_address.end() ||
        non_self_predicate_addresses.find(node.address_id) != non_self_predicate_addresses.end() ||
        declared_support_addresses.find(node.address_id) != declared_support_addresses.end()) {
      continue;
    }
    ++self_only_count;
    add_finding(
        "SELF_ONLY_RELATIONSHIP", "warning",
        "This row has operational predicate edges only to itself. It may be a valid local calculation, "
        "but declare an external automation source, downstream consumer, stakeholder/terminal purpose, "
        "or connect it to the wider procedure.",
        "row:" + node.address_id,
        {{"address_id", node.address_id},
         {"slug_id", node.slug_id},
         {"section", node.section},
         {"procedure", node.procedure},
         {"action", node.action},
         {"self_predicates", self_predicates->second},
         {"suppressed_by", "non_self_predicate_or_declared_support"}});
  }

  return {{"schema", "chax-relationship-workbench-v1"},
          {"checklist", graph.checklist},
          {"instance_id", graph.instance_id},
          {"relationship_directory", relationship_directory.string()},
          {"nodes", nodes},
          {"edges", edges},
          {"findings", findings},
          {"summary",
            {{"rows", graph.nodes.size()},
            {"predicate_edges", predicate_count},
            {"binding_edges", binding_count},
            {"datasets", dataset_count},
            {"orphan_rows", orphan_count},
            {"self_only_rows", self_only_count}}}};
}

std::string RenderRelationshipWorkbenchDot(const json& workbench) {
  std::ostringstream out;
  out << "digraph relationship_workbench {\n"
      << "  graph [rankdir=\"LR\", concentrate=\"true\", overlap=\"false\"];\n"
      << "  node [style=\"rounded,filled\", fontname=\"Segoe UI\"];\n"
      << "  edge [fontname=\"Segoe UI\", fontsize=\"10\", color=\"#61758a\"];\n\n";
  std::unordered_map<std::string, std::size_t> indexes;
  if (workbench.contains("nodes") && workbench["nodes"].is_array()) {
    for (const auto& node : workbench["nodes"]) {
      const std::string id = node.value("id", "");
      if (id.empty()) {
        continue;
      }
      const std::size_t index = indexes.size();
      indexes.emplace(id, index);
      const std::string kind = node.value("kind", "checklist_row");
      const std::string title = node.value("title", id);
      const std::string subtitle = node.value("subtitle", "");
      out << "  n" << index << " [shape=\"" << NodeShapeFor(kind) << "\", fillcolor=\""
          << NodeColorFor(kind) << "\", label=\"" << EscapeDot(ShortLabel(title));
      if (!subtitle.empty()) {
        out << "\\n" << EscapeDot(ShortLabel(subtitle));
      }
      out << "\"];\n";
    }
  }
  out << "\n";
  if (workbench.contains("edges") && workbench["edges"].is_array()) {
    for (const auto& edge : workbench["edges"]) {
      const auto source = indexes.find(edge.value("source_id", ""));
      const auto target = indexes.find(edge.value("target_id", ""));
      if (source == indexes.end() || target == indexes.end()) {
        continue;
      }
      const std::string edge_class = edge.value("class", "relationship");
      const std::string style = edge_class == "contains_column" ? "dotted" : "solid";
      out << "  n" << source->second << " -> n" << target->second << " [label=\""
          << EscapeDot(ShortLabel(edge.value("label", edge_class))) << "\", style=\"" << style
          << "\"];\n";
    }
  }
  out << "}\n";
  return out.str();
}

}  // namespace core
