#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "core/checklist_store.hpp"
#include "core/graph_projection.hpp"

namespace {

void Assert(bool condition, const std::string& message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

core::ChecklistSlug MakeSlug(const std::string& checklist, const std::string& instance_id,
                             std::int64_t order, const std::string& section,
                             const std::string& procedure, const std::string& spec) {
  core::ChecklistSlug slug;
  slug.checklist = checklist;
  slug.instance_id = instance_id;
  slug.address_order = order;
  slug.section = section;
  slug.procedure = procedure;
  slug.action = "Perform " + procedure;
  slug.spec = spec;
  slug.instructions = "Synthetic graph projection test instruction.";
  slug.slug_id = core::ComputeSlugId(slug.checklist, slug.section, slug.procedure, slug.action,
                                     slug.spec, slug.instructions);
  slug.address_id = core::ComposeAddressId(slug.slug_id, slug.instance_id);
  return slug;
}

}  // namespace

int main() {
  try {
    const std::string checklist = "graph-projection-test";
    const std::string instance_id = core::ComputeInstanceId("instance||graph-projection-test");
    auto first = MakeSlug(checklist, instance_id, 1000, "Prepare", "Measure input", ">= 10 V");
    auto second = MakeSlug(checklist, instance_id, 2000, "Prepare", "Gate review", "Pass");
    auto third = MakeSlug(checklist, instance_id, 3000, "Finish", "Record outcome", "note");

    first.relationships = {
        {"passPropagateValidatedPass", second.address_id},
        {"recordsMetric", "EXTERNALADDRESS00000000000000000"},
    };
    second.relationships = {
        {"BoolVerifyValidatedStatus", third.address_id},
        {"slugPredecessor", "LEGACYSLUG000000"},
    };

    const auto graph = core::BuildChecklistGraph({first, second, third});
    Assert(graph.checklist == checklist, "Projection should retain checklist identity");
    Assert(graph.instance_id == instance_id, "Projection should retain instance identity");
    Assert(graph.nodes.size() == 3, "Projection should contain every selected row");
    Assert(graph.edges.size() == 6, "Projection should emit two order and four relationship edges");
    Assert(graph.nodes[0].spec_kind == "comparator", "Comparator spec should be derived");
    Assert(graph.nodes[1].spec_kind == "boolean", "Boolean spec should be derived");
    Assert(graph.nodes[1].visual_shape == "decision", "Verify relationship should derive decision shape");
    Assert(graph.nodes[0].outgoing_relationship_count == 2,
           "Outgoing relationship count should exclude checklist order");
    Assert(graph.nodes[1].incoming_relationship_count == 1,
           "Incoming relationship count should include local relationship edges only");

    bool found_order = false;
    bool found_external = false;
    bool found_lineage = false;
    for (const auto& edge : graph.edges) {
      if (edge.kind == "checklistOrder" && edge.source_address_id == first.address_id &&
          edge.target_address_id == second.address_id) {
        found_order = true;
      }
      if (edge.predicate == "recordsMetric" && edge.is_external &&
          edge.external_category == "unknown_external") {
        found_external = true;
      }
      if (edge.predicate == "slugPredecessor" && edge.is_lineage &&
          edge.external_category == "legacy") {
        found_lineage = true;
      }
    }
    Assert(found_order, "Checklist order should be a derived display edge");
    Assert(found_external, "Unknown relationship targets should be retained as external");
    Assert(found_lineage, "Lineage relationships should be classified as legacy metadata");

    const auto dot = core::RenderChecklistGraphDot(graph);
    const auto mermaid = core::RenderChecklistGraphMermaid(graph);
    const auto dbml = core::RenderChecklistRuntimeSchemaDbml();
    Assert(dot.find("digraph checklist") != std::string::npos, "DOT export should be generated");
    Assert(dot.find("checklistOrder") != std::string::npos,
           "DOT export should preserve derived order labels");
    Assert(mermaid.find("flowchart LR") != std::string::npos,
           "Mermaid export should be generated");
    Assert(mermaid.find("passPropagateValidatedPass") != std::string::npos,
           "Mermaid export should preserve relationship predicates");
    Assert(dbml.find("Project checklist_assistant_runtime") != std::string::npos,
           "DBML export should identify the runtime schema");
    Assert(dbml.find("Table address_relationships") != std::string::npos,
           "DBML export should include address relationship storage");
    Assert(dbml.find("Ref: address_relationships.subject_address_id > slugs.address_id") !=
               std::string::npos,
           "DBML export should expose self-referential runtime relationship references");
    std::cout << "CHAX_STEP|graph_projection_test|native projection|Pass|graph projection and exports verified\n";
    return 0;
  } catch (const std::exception& ex) {
    std::cerr << "graph_projection_test failure: " << ex.what() << "\n";
    return 1;
  }
}
