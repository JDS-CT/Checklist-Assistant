#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "core/checklist_store.hpp"

namespace core {

struct ChecklistGraphNode {
  std::string address_id;
  std::string slug_id;
  std::int64_t address_order = 0;
  std::string section;
  std::string procedure;
  std::string action;
  std::string spec;
  std::string result;
  std::string status;
  std::string comment;
  std::string instructions;
  std::string spec_kind;
  std::string visual_shape;
  std::size_t incoming_relationship_count = 0;
  std::size_t outgoing_relationship_count = 0;
};

struct ChecklistGraphEdge {
  std::string source_address_id;
  std::string target_address_id;
  std::string predicate;
  std::string kind;
  bool is_lineage = false;
  bool is_external = false;
  std::string external_category;
};

struct ChecklistGraph {
  std::string checklist;
  std::string instance_id;
  std::vector<ChecklistGraphNode> nodes;
  std::vector<ChecklistGraphEdge> edges;
  std::vector<std::string> warnings;
};

ChecklistGraph BuildChecklistGraph(const std::vector<ChecklistSlug>& slugs);
std::string RenderChecklistGraphDot(const ChecklistGraph& graph);
std::string RenderChecklistGraphMermaid(const ChecklistGraph& graph);

}  // namespace core
