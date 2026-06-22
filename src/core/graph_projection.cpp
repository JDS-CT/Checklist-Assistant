#include "core/graph_projection.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <map>
#include <sstream>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <utility>

namespace core {
namespace {

constexpr std::string_view kSlugSuccessorPredicate = "slugSuccessor";
constexpr std::string_view kSlugPredecessorPredicate = "slugPredecessor";
constexpr std::string_view kAddressSuccessorPredicate = "addressSuccessor";
constexpr std::string_view kAddressPredecessorPredicate = "addressPredecessor";

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

}  // namespace core
