#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "core/checklist_store.hpp"
#include "core/checklist_markdown.hpp"
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

    const auto workbench_root = std::filesystem::temp_directory_path() / "chax-workbench-test";
    const std::string legacy_serial_slug_id = "1234567890ABCDEF";
    std::filesystem::remove_all(workbench_root);
    std::filesystem::create_directories(workbench_root / "relationships");
    std::filesystem::create_directories(workbench_root / "data");
    {
      std::ofstream markdown(workbench_root / "checklist.md", std::ios::binary);
      markdown << core::markdown::ExportChecklistMarkdown(
          checklist, {first, second, third},
          {{first.slug_id, "slugPredecessor", legacy_serial_slug_id}},
          core::markdown::RelationshipExportMode::kTemplate,
          core::markdown::RelationshipIdentityFormat::kId);
    }
    {
      std::ofstream bindings(workbench_root / "relationships" / "bindings.json", std::ios::binary);
      bindings << "{\n"
               << "  \"schema\": \"chax-relationships-v1\",\n"
               << "  \"completeness\": {\"orphan_policy\": \"warn\", \"terminal_rows\": [\n"
               << "    {\"slug_id\": \"" << third.slug_id
               << "\", \"stakeholder\": \"handoff\", \"reason\": \"final record\"}\n"
               << "  ]},\n"
               << "  \"datasets\": [{\n"
               << "    \"path\": \"data/records.csv\",\n"
               << "    \"lookup\": {\"column\": \"" << legacy_serial_slug_id
               << "-result(Machine)\", \"slug_id\": \"" << legacy_serial_slug_id
               << "\", \"field\": \"result\"},\n"
               << "    \"bindings\": \"header-derived\"\n"
               << "  }],\n"
               << "  \"mutation_sources\": []\n"
               << "}\n";
    }
    {
      std::ofstream data(workbench_root / "data" / "records.csv", std::ios::binary);
      data << legacy_serial_slug_id << "-result(Machine)," << second.slug_id << "-comment(Review)\n"
           << "M-001,Ready\n"
           << "M-001,Ready\n";
    }
    const auto workbench = core::BuildRelationshipWorkbench(graph, workbench_root);
    Assert(workbench.value("schema", "") == "chax-relationship-workbench-v1",
           "Workbench should identify its portable analysis schema");
    Assert(workbench["summary"].value("datasets", 0) == 1,
           "Workbench should analyze the declared CSV dataset");
    Assert(workbench["summary"].value("orphan_rows", 99) == 0,
           "Predicate, binding, and declared terminal edges should account for each row");
    bool found_lookup = false;
    bool found_column_binding = false;
    bool found_terminal = false;
    bool found_legacy_alias = false;
    bool found_constant = false;
    for (const auto& edge : workbench["edges"]) {
      found_lookup = found_lookup || edge.value("class", "") == "lookup_key";
      found_column_binding = found_column_binding || edge.value("class", "") == "column_binding";
      found_terminal = found_terminal || edge.value("class", "") == "declared_terminal";
      found_legacy_alias = found_legacy_alias || edge.value("class", "") == "legacy_alias";
    }
    for (const auto& finding : workbench["findings"]) {
      found_constant = found_constant || finding.value("code", "") == "CONSTANT_COLUMN";
    }
    Assert(found_lookup, "Workbench should expose a lookup-key edge");
    Assert(found_column_binding, "Workbench should expose header-derived bindings");
    Assert(found_terminal, "Workbench should expose declared terminal relationships");
    Assert(found_legacy_alias, "Workbench should expose legacy slug aliases rather than guessing by label");
    Assert(found_constant, "Workbench should identify constant dataset columns");
    const auto workbench_dot = core::RenderRelationshipWorkbenchDot(workbench);
    Assert(workbench_dot.find("digraph relationship_workbench") != std::string::npos,
           "Workbench should export a DOT graph");
    std::filesystem::remove_all(workbench_root);
    std::cout << "CHAX_STEP|graph_projection_test|native projection|Pass|graph projection and exports verified\n";
    return 0;
  } catch (const std::exception& ex) {
    std::cerr << "graph_projection_test failure: " << ex.what() << "\n";
    return 1;
  }
}
