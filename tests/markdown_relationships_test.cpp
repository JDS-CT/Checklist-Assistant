#include <iostream>
#include <stdexcept>
#include <string>

#include "core/checklist_markdown.hpp"

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
  std::cout << "CHAX_STEP|markdown_relationships_test|" << procedure << "|"
            << (pass ? "Pass" : "Fail") << "|" << SanitizeMessage(message) << "\n";
}

}  // namespace

int main() {
  std::string current_step;
  try {
    const std::string checklist = "Relationship Markdown Test";
    const std::string section = "Section A";
    const std::string proc_a = "Procedure A";
    const std::string action_a = "Action A";
    const std::string spec_a = "Spec A";
    const std::string instr_a = "Instructions A";
    const std::string proc_b = "Procedure B";
    const std::string action_b = "Action B";
    const std::string spec_b = "Spec B";
    const std::string instr_b = "Instructions B";
    const std::string expected_slug_a =
        core::ComputeSlugId(checklist, section, proc_a, action_a, spec_a, instr_a);
    const std::string expected_slug_b =
        core::ComputeSlugId(checklist, section, proc_b, action_b, spec_b, instr_b);
    const std::string instance_id = core::ComputeInstanceId("template||default");
    const std::string address_a = core::ComposeAddressId(expected_slug_a, instance_id);
    const std::string address_b = core::ComposeAddressId(expected_slug_b, instance_id);
    const std::string section_proc_target = "(" + section + ", " + proc_b + ")";

    const std::string markdown =
        "# Checklist: " + checklist + "\n\n" +
        "## Section: " + section + "\n\n" +
        "### Procedure: " + proc_a + "\n" +
        "- Action: " + action_a + "\n" +
        "- Spec: " + spec_a + "\n" +
        "- Result: \n" +
        "- Status: \n" +
        "- Comment: \n\n" +
        "#### Instructions\n" +
        instr_a + "\n" +
        "#### Relationships\n" +
        "- passPropagateValidatedPass " + address_b + "\n" +
        "- passVerifyImpliedPass " + expected_slug_b + "\n" +
        "- passRequiresVerified " + section_proc_target + "\n\n" +
        "### Procedure: " + proc_b + "\n" +
        "- Action: " + action_b + "\n" +
        "- Spec: " + spec_b + "\n" +
        "- Result: \n" +
        "- Status: \n" +
        "- Comment: \n\n" +
        "#### Instructions\n" +
        instr_b + "\n" +
        "#### Relationships\n" +
        "- " + address_b + "\n" +
        "- passPropagateValidatedPass " + expected_slug_a + "\n";

    current_step = "parse markdown";
    const auto parsed = core::markdown::ParseChecklistMarkdown("", markdown);
    Assert(parsed.slugs.size() == 2, "Expected 2 slugs");
    RecordStep(current_step, true, "parsed slugs");

    const auto find_by_procedure = [&](const std::string& procedure) -> const core::ChecklistSlug* {
      for (const auto& slug : parsed.slugs) {
        if (slug.procedure == procedure) {
          return &slug;
        }
      }
      return nullptr;
    };

    current_step = "validate slug ids";
    const auto* slug_a = find_by_procedure(proc_a);
    const auto* slug_b = find_by_procedure(proc_b);
    Assert(slug_a != nullptr && slug_b != nullptr, "Missing expected procedures");
    Assert(slug_a->slug_id == expected_slug_a, "Slug A id mismatch");
    Assert(slug_b->slug_id == expected_slug_b, "Slug B id mismatch");
    RecordStep(current_step, true, "slug ids match");

    current_step = "template relationships";
    bool rel_address = false;
    bool rel_slug = false;
    bool rel_tuple = false;
    for (const auto& rel : parsed.template_relationships) {
      if (rel.subject_slug_id != expected_slug_a || rel.target_slug_id != expected_slug_b) {
        continue;
      }
      if (rel.predicate == "passPropagateValidatedPass") rel_address = true;
      if (rel.predicate == "passVerifyImpliedPass") rel_slug = true;
      if (rel.predicate == "passRequiresVerified") rel_tuple = true;
    }
    Assert(rel_address && rel_slug && rel_tuple, "Missing expected template relationships");
    RecordStep(current_step, true, "template relationships ok");

    current_step = "address relationships";
    bool found_address_rel = false;
    for (const auto& rel : parsed.address_relationships) {
      if (rel.subject_address_id == address_b &&
          rel.target_address_id == address_a &&
          rel.predicate == "passPropagateValidatedPass") {
        found_address_rel = true;
        break;
      }
    }
    Assert(found_address_rel, "Missing expected address relationship");
    RecordStep(current_step, true, "address relationship ok");

    current_step = "export markdown";
    const auto export_md = core::markdown::ExportChecklistMarkdown(
        checklist, parsed.slugs, parsed.template_relationships,
        core::markdown::RelationshipExportMode::kTemplate,
        core::markdown::RelationshipIdentityFormat::kId);
    Assert(export_md.find("- " + expected_slug_a + "\n") != std::string::npos,
           "Export missing slug_id source line");
    Assert(export_md.find("passPropagateValidatedPass " + expected_slug_b) != std::string::npos,
           "Export missing address-id target relationship");
    Assert(export_md.find("passVerifyImpliedPass " + expected_slug_b) != std::string::npos,
           "Export missing slug-id target relationship");
    Assert(export_md.find("passRequiresVerified " + expected_slug_b) != std::string::npos,
           "Export missing section/procedure target relationship");
    RecordStep(current_step, true, "export markdown ok");

    return 0;
  } catch (const std::exception& ex) {
    if (!current_step.empty()) {
      RecordStep(current_step, false, ex.what());
    }
    std::cerr << "markdown_relationships_test failure: " << ex.what() << "\n";
    return 1;
  }
}
