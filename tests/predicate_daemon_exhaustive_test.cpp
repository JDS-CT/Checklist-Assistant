#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include "core/checklist_markdown.hpp"
#include "core/checklist_store.hpp"
#include "core/logging.hpp"
#include "nlohmann/json.hpp"

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
  std::cout << "CHAX_STEP|predicate_daemon_exhaustive_test|" << procedure << "|"
            << (pass ? "Pass" : "Fail") << "|" << SanitizeMessage(message) << "\n";
}

void RemoveIfExists(const std::string& path) {
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

void AppendJsonl(const std::filesystem::path& path, const nlohmann::json& entry) {
  std::ofstream out(path, std::ios::app);
  if (!out.is_open()) {
    throw std::runtime_error("Failed to open predicate summary file: " + path.string());
  }
  out << entry.dump() << "\n";
}

struct CanonicalPredicateTokenParts {
  std::string_view subject_state;
  std::string_view relation;
  std::string_view type;
  std::string_view object_state;
};

std::string TokenToString(const CanonicalPredicateTokenParts& parts) {
  return std::string(parts.subject_state) + std::string(parts.relation) + std::string(parts.type) +
         std::string(parts.object_state);
}

core::ChecklistStatus SubjectStateToStatus(std::string_view subject_state) {
  if (subject_state == "pass") return core::ChecklistStatus::kPass;
  if (subject_state == "fail") return core::ChecklistStatus::kFail;
  if (subject_state == "other") return core::ChecklistStatus::kOther;
  if (subject_state == "na") return core::ChecklistStatus::kNA;
  return core::ChecklistStatus::kUnknown;
}

core::ChecklistStatus ObjectStateToStatus(std::string_view object_state) {
  if (object_state == "Pass") return core::ChecklistStatus::kPass;
  if (object_state == "Fail") return core::ChecklistStatus::kFail;
  if (object_state == "Other") return core::ChecklistStatus::kOther;
  if (object_state == "Na") return core::ChecklistStatus::kNA;
  return core::ChecklistStatus::kUnknown;
}

core::ChecklistStatus PickDifferentStatus(core::ChecklistStatus status) {
  static const core::ChecklistStatus kOptions[] = {
      core::ChecklistStatus::kPass,
      core::ChecklistStatus::kFail,
      core::ChecklistStatus::kOther,
      core::ChecklistStatus::kNA,
  };
  for (const auto& option : kOptions) {
    if (option != status) return option;
  }
  return core::ChecklistStatus::kFail;
}

core::ChecklistSlug MakeSlug(const std::string& checklist,
                            const std::string& procedure,
                            const std::string& instance_principal,
                            const std::string& entity_principal) {
  core::ChecklistSlug slug;
  slug.checklist = checklist;
  slug.section = "Predicate Tests";
  slug.procedure = procedure;
  slug.action = "Action";
  slug.spec = "Spec";
  slug.instructions = "Step";
  slug.result = "";
  slug.status = core::ChecklistStatus::kUnknown;
  slug.comment = "";
  slug.timestamp = core::CurrentTimestampIsoUtc();
  slug.instance_principal = instance_principal;
  slug.instance_id = core::ComputeInstanceId(instance_principal);
  slug.slug_id = core::ComputeSlugId(slug.checklist, slug.section, slug.procedure, slug.action, slug.spec,
                                     slug.instructions);
  slug.address_id = core::ComposeAddressId(slug.slug_id, slug.instance_id);
  slug.entity_principal = entity_principal;
  slug.entity_id = core::ComputeEntityId(entity_principal);
  return slug;
}

void ApplyStatusUpdate(core::ChecklistStore& store,
                       const std::string& address_id,
                       core::ChecklistStatus status,
                       const std::string& comment,
                       const std::string& entity_principal) {
  core::SlugUpdate update;
  update.address_id = address_id;
  update.status = status;
  update.comment = comment;
  update.entity_principal_override = entity_principal;
  store.ApplyUpdate(update);
}

void ApplyResultUpdate(core::ChecklistStore& store,
                       const std::string& address_id,
                       const std::string& result,
                       const std::string& entity_principal) {
  core::SlugUpdate update;
  update.address_id = address_id;
  update.result = result;
  update.entity_principal_override = entity_principal;
  store.ApplyUpdate(update);
}

}  // namespace

int main() {
  const auto db_path = (std::filesystem::temp_directory_path() / "chax-predicate-daemon-exhaustive-test.db").string();
  RemoveIfExists(db_path);

  std::string current_step;
  try {
    core::logging::SetLogLevel(core::logging::LogLevel::kError);

    const auto artifacts_root =
        ResolveRepoRoot() / ".chax" / "test-artifacts" / "predicate-daemon-exhaustive";
    const auto predicate_summary_path = artifacts_root / "predicate_matrix.jsonl";
    {
      std::error_code ec;
      std::filesystem::create_directories(artifacts_root, ec);
    }
    RemoveIfExists(predicate_summary_path.string());
    AppendJsonl(predicate_summary_path,
                {{"event", "predicate_matrix_setup"},
                 {"checklist", "predicate-daemon-exhaustive-test"},
                 {"instance_principal", "instance||predicate-daemon-exhaustive"}});

    core::ChecklistStore store(db_path);
    store.Initialize(/*seed_demo_data=*/false);

    current_step = "predicate matrix";
    const std::string checklist = "predicate-daemon-exhaustive-test";
    const std::string instance_principal = "instance||predicate-daemon-exhaustive";
    const std::string user_principal = "user||provider=test||username=predicate-daemon";
    const std::string daemon_principal = "system||chax-predicate-daemon";
    const std::string daemon_entity_id = core::ComputeEntityId(daemon_principal);
    std::vector<core::AddressRelationship> expected_relationships;
    expected_relationships.reserve(320);

    const auto record_relationship =
        [&](const std::string& subject, const std::string& predicate, const std::string& target) {
          expected_relationships.push_back(core::AddressRelationship{subject, predicate, target});
          store.InsertAddressRelationship(subject, core::RelationshipEdge{predicate, target});
        };

    constexpr std::string_view kSubjectStates[] = {"pass", "fail", "other", "na"};
    constexpr std::string_view kRelations[] = {"Propagate", "Sync", "Verify"};
    constexpr std::string_view kTypes[] = {"Validated", "Implied", "Assumed", "AndGate", "OrGate"};
    constexpr std::string_view kObjectStates[] = {"Pass", "Fail", "Other", "Na"};

    int ordinal = 0;
    for (const auto& subject_state : kSubjectStates) {
      for (const auto& relation : kRelations) {
        for (const auto& type : kTypes) {
          for (const auto& object_state : kObjectStates) {
            const CanonicalPredicateTokenParts parts{subject_state, relation, type, object_state};
            const std::string predicate_token = TokenToString(parts);

            std::ostringstream code;
            code << "P" << ordinal++;
            const std::string source_procedure = "PredicateSourceTest" + code.str();
            const std::string target_procedure = "PredicateTargetTest" + code.str();

            const auto source = MakeSlug(checklist, source_procedure, instance_principal, user_principal);
            const auto target = MakeSlug(checklist, target_procedure, instance_principal, user_principal);
            store.UpsertSlug(source);
            store.UpsertSlug(target);
            record_relationship(source.address_id, predicate_token, target.address_id);

            const std::string prep_comment = "prep-" + predicate_token;
            ApplyStatusUpdate(store, target.address_id, core::ChecklistStatus::kFail, prep_comment, user_principal);

            const auto expected_subject_status = SubjectStateToStatus(subject_state);
            const auto non_expected_subject_status = PickDifferentStatus(expected_subject_status);
            ApplyStatusUpdate(store, source.address_id, non_expected_subject_status, "set non-active", user_principal);
            ApplyStatusUpdate(store, source.address_id, expected_subject_status, "set active", user_principal);

            const auto target_after = store.GetSlugOrThrow(target.address_id);
            if (relation == "Propagate") {
              const auto expected_target_status = ObjectStateToStatus(object_state);
              Assert(target_after.status == expected_target_status,
                     "Propagate should set target status for " + predicate_token);
              const std::string expected_comment =
                  "Filled per relationship " + predicate_token + " " + source.address_id;
              Assert(target_after.comment == expected_comment,
                     "Propagate should annotate target comment for " + predicate_token);
              Assert(target_after.entity_id == daemon_entity_id,
                     "Propagate should stamp daemon entity_id for " + predicate_token);
              AppendJsonl(predicate_summary_path,
                          {{"event", "predicate_matrix_case"},
                           {"predicate", predicate_token},
                           {"subject_state", std::string(subject_state)},
                           {"relation", std::string(relation)},
                           {"type", std::string(type)},
                           {"object_state", std::string(object_state)},
                           {"expected_status", core::StatusToString(expected_target_status)},
                           {"actual_status", core::StatusToString(target_after.status)},
                           {"expected_comment", expected_comment},
                           {"actual_comment", target_after.comment},
                           {"case", code.str()}});
            } else {
              Assert(target_after.status == core::ChecklistStatus::kFail,
                     "Non-propagate predicates must not mutate target status for " + predicate_token);
              Assert(target_after.comment == prep_comment,
                     "Non-propagate predicates must not mutate target comment for " + predicate_token);
              AppendJsonl(predicate_summary_path,
                          {{"event", "predicate_matrix_case"},
                           {"predicate", predicate_token},
                           {"subject_state", std::string(subject_state)},
                           {"relation", std::string(relation)},
                           {"type", std::string(type)},
                           {"object_state", std::string(object_state)},
                           {"expected_status", "Fail"},
                           {"actual_status", core::StatusToString(target_after.status)},
                           {"expected_comment", prep_comment},
                           {"actual_comment", target_after.comment},
                           {"case", code.str()}});
            }
          }
        }
      }
    }
    AppendJsonl(predicate_summary_path,
                {{"event", "predicate_matrix_complete"}, {"case_count", ordinal}});
    RecordStep(current_step, true, "predicate matrix ok");

    // Multi-edge re-propagation: subject can drive different outcomes across separate sweeps.
    current_step = "multi-edge";
    const auto multi_source = MakeSlug(checklist, "MultiEdgeSource", instance_principal, user_principal);
    const auto multi_target = MakeSlug(checklist, "MultiEdgeTarget", instance_principal, user_principal);
    store.UpsertSlug(multi_source);
    store.UpsertSlug(multi_target);
    record_relationship(multi_source.address_id, "passPropagateValidatedPass", multi_target.address_id);
    record_relationship(multi_source.address_id, "failPropagateValidatedFail", multi_target.address_id);
    ApplyStatusUpdate(store, multi_target.address_id, core::ChecklistStatus::kOther, "multi-prep", user_principal);
    ApplyStatusUpdate(store, multi_source.address_id, core::ChecklistStatus::kPass, "multi-pass", user_principal);
    Assert(store.GetSlugOrThrow(multi_target.address_id).status == core::ChecklistStatus::kPass,
           "Multi-edge: Pass should propagate Pass");
    ApplyStatusUpdate(store, multi_source.address_id, core::ChecklistStatus::kFail, "multi-fail", user_principal);
    Assert(store.GetSlugOrThrow(multi_target.address_id).status == core::ChecklistStatus::kFail,
           "Multi-edge: Fail should propagate Fail");
    AppendJsonl(predicate_summary_path,
                {{"event", "multi_edge"},
                 {"source", multi_source.address_id},
                 {"target", multi_target.address_id},
                 {"final_status", core::StatusToString(store.GetSlugOrThrow(multi_target.address_id).status)}});
    RecordStep(current_step, true, "multi-edge ok");

    // Gate modes: AndGate requires all contributors; OrGate requires any contributor.
    current_step = "gate modes";
    const auto and_source_a = MakeSlug(checklist, "AndGateSourceA", instance_principal, user_principal);
    const auto and_source_b = MakeSlug(checklist, "AndGateSourceB", instance_principal, user_principal);
    const auto and_target = MakeSlug(checklist, "AndGateTarget", instance_principal, user_principal);
    store.UpsertSlug(and_source_a);
    store.UpsertSlug(and_source_b);
    store.UpsertSlug(and_target);
    record_relationship(and_source_a.address_id, "passPropagateAndGatePass", and_target.address_id);
    record_relationship(and_source_b.address_id, "passPropagateAndGatePass", and_target.address_id);
    ApplyStatusUpdate(store, and_target.address_id, core::ChecklistStatus::kFail, "and-prep", user_principal);
    ApplyStatusUpdate(store, and_source_a.address_id, core::ChecklistStatus::kPass, "and-a-pass", user_principal);
    Assert(store.GetSlugOrThrow(and_target.address_id).status == core::ChecklistStatus::kFail,
           "AndGate should wait until all matching contributors are active");
    ApplyStatusUpdate(store, and_source_b.address_id, core::ChecklistStatus::kPass, "and-b-pass", user_principal);
    Assert(store.GetSlugOrThrow(and_target.address_id).status == core::ChecklistStatus::kPass,
           "AndGate should propagate once all matching contributors are active");

    const auto or_source_a = MakeSlug(checklist, "OrGateSourceA", instance_principal, user_principal);
    const auto or_source_b = MakeSlug(checklist, "OrGateSourceB", instance_principal, user_principal);
    const auto or_target = MakeSlug(checklist, "OrGateTarget", instance_principal, user_principal);
    store.UpsertSlug(or_source_a);
    store.UpsertSlug(or_source_b);
    store.UpsertSlug(or_target);
    record_relationship(or_source_a.address_id, "passPropagateOrGatePass", or_target.address_id);
    record_relationship(or_source_b.address_id, "passPropagateOrGatePass", or_target.address_id);
    ApplyStatusUpdate(store, or_target.address_id, core::ChecklistStatus::kFail, "or-prep", user_principal);
    ApplyStatusUpdate(store, or_source_a.address_id, core::ChecklistStatus::kPass, "or-a-pass", user_principal);
    Assert(store.GetSlugOrThrow(or_target.address_id).status == core::ChecklistStatus::kPass,
           "OrGate should propagate when any matching contributor is active");
    ApplyStatusUpdate(store, or_target.address_id, core::ChecklistStatus::kFail, "or-reset", user_principal);
    ApplyStatusUpdate(store, or_source_a.address_id, core::ChecklistStatus::kFail, "or-a-fail", user_principal);
    ApplyStatusUpdate(store, or_source_b.address_id, core::ChecklistStatus::kPass, "or-b-pass", user_principal);
    Assert(store.GetSlugOrThrow(or_target.address_id).status == core::ChecklistStatus::kPass,
           "OrGate should propagate when a different contributor becomes active");
    AppendJsonl(predicate_summary_path,
                {{"event", "gate_modes"},
                 {"and_target", and_target.address_id},
                 {"and_target_status", core::StatusToString(store.GetSlugOrThrow(and_target.address_id).status)},
                 {"or_target", or_target.address_id},
                 {"or_target_status", core::StatusToString(store.GetSlugOrThrow(or_target.address_id).status)}});
    RecordStep(current_step, true, "gate modes ok");

    // BoolVerify bridge semantics: Status/Comment/literal status and gate aggregation.
    current_step = "verify bridge";
    auto verify_self = MakeSlug(checklist, "VerifySelf", instance_principal, user_principal);
    verify_self.spec = ">=10";
    verify_self.slug_id = core::ComputeSlugId(
        verify_self.checklist, verify_self.section, verify_self.procedure, verify_self.action,
        verify_self.spec, verify_self.instructions);
    verify_self.address_id = core::ComposeAddressId(verify_self.slug_id, verify_self.instance_id);
    store.UpsertSlug(verify_self);
    record_relationship(verify_self.address_id, "BoolVerifyValidatedStatus", verify_self.address_id);
    ApplyResultUpdate(store, verify_self.address_id, "12", user_principal);
    Assert(store.GetSlugOrThrow(verify_self.address_id).status == core::ChecklistStatus::kPass,
           "BoolVerifyValidatedStatus true should set Pass");
    ApplyResultUpdate(store, verify_self.address_id, "8", user_principal);
    Assert(store.GetSlugOrThrow(verify_self.address_id).status == core::ChecklistStatus::kFail,
           "BoolVerifyValidatedStatus false should set Fail");
    ApplyResultUpdate(store, verify_self.address_id, "not-a-number", user_principal);
    const auto verify_self_after = store.GetSlugOrThrow(verify_self.address_id);
    Assert(verify_self_after.status == core::ChecklistStatus::kFail,
           "BoolVerifyValidatedStatus indeterminate should not overwrite status");
    Assert(verify_self_after.comment.empty(),
           "BoolVerifyValidatedStatus indeterminate should not overwrite operator comments");
    Assert(verify_self_after.entity_id == core::ComputeEntityId(user_principal),
           "BoolVerifyValidatedStatus indeterminate should preserve user entity ownership");
    ApplyResultUpdate(store, verify_self.address_id, "12", user_principal);
    const auto verify_self_after_recovery = store.GetSlugOrThrow(verify_self.address_id);
    Assert(verify_self_after_recovery.status == core::ChecklistStatus::kPass,
           "BoolVerifyValidatedStatus should recover to Pass when result becomes valid");
    Assert(verify_self_after_recovery.comment.empty(),
           "BoolVerifyValidatedStatus should preserve operator comment field ownership");

    auto verify_unit_comparator =
        MakeSlug(checklist, "VerifyUnitComparator", instance_principal, user_principal);
    verify_unit_comparator.spec = "=2.5 mbar";
    verify_unit_comparator.slug_id = core::ComputeSlugId(
        verify_unit_comparator.checklist, verify_unit_comparator.section,
        verify_unit_comparator.procedure, verify_unit_comparator.action,
        verify_unit_comparator.spec, verify_unit_comparator.instructions);
    verify_unit_comparator.address_id =
        core::ComposeAddressId(verify_unit_comparator.slug_id, verify_unit_comparator.instance_id);
    store.UpsertSlug(verify_unit_comparator);
    record_relationship(verify_unit_comparator.address_id, "BoolVerifyValidatedStatus",
                        verify_unit_comparator.address_id);
    ApplyResultUpdate(store, verify_unit_comparator.address_id, "0.0362594 psi", user_principal);
    auto verify_unit_comparator_after = store.GetSlugOrThrow(verify_unit_comparator.address_id);
    Assert(verify_unit_comparator_after.status == core::ChecklistStatus::kPass,
           "Comparator equality should allow conversion-scale tolerance from spec precision");
    Assert(verify_unit_comparator_after.result.find(" mbar") != std::string::npos,
           "Comparator scalar result should normalize to the spec unit for storage/reporting");
    ApplyResultUpdate(store, verify_unit_comparator.address_id, "0.034 psi", user_principal);
    Assert(store.GetSlugOrThrow(verify_unit_comparator.address_id).status == core::ChecklistStatus::kFail,
           "Comparator equality should fail outside precision tolerance");
    ApplyResultUpdate(store, verify_unit_comparator.address_id, "2.5", user_principal);
    verify_unit_comparator_after = store.GetSlugOrThrow(verify_unit_comparator.address_id);
    Assert(verify_unit_comparator_after.status == core::ChecklistStatus::kPass,
           "Comparator equality should assume result units from the spec when omitted");
    Assert(verify_unit_comparator_after.result == "2.5 mbar",
           "Unitless comparator result should be stored using the spec unit");

    auto verify_unit_scalar =
        MakeSlug(checklist, "VerifyUnitScalar", instance_principal, user_principal);
    verify_unit_scalar.spec = "2.5 mbar";
    verify_unit_scalar.slug_id = core::ComputeSlugId(
        verify_unit_scalar.checklist, verify_unit_scalar.section, verify_unit_scalar.procedure,
        verify_unit_scalar.action, verify_unit_scalar.spec, verify_unit_scalar.instructions);
    verify_unit_scalar.address_id =
        core::ComposeAddressId(verify_unit_scalar.slug_id, verify_unit_scalar.instance_id);
    store.UpsertSlug(verify_unit_scalar);
    record_relationship(verify_unit_scalar.address_id, "BoolVerifyValidatedStatus",
                        verify_unit_scalar.address_id);
    ApplyResultUpdate(store, verify_unit_scalar.address_id, "0.0362594 psi", user_principal);
    auto verify_unit_scalar_after = store.GetSlugOrThrow(verify_unit_scalar.address_id);
    Assert(verify_unit_scalar_after.status == core::ChecklistStatus::kPass,
           "Scalar equality should allow conversion-scale tolerance from spec precision");
    ApplyResultUpdate(store, verify_unit_scalar.address_id, "0.034 psi", user_principal);
    Assert(store.GetSlugOrThrow(verify_unit_scalar.address_id).status == core::ChecklistStatus::kFail,
           "Scalar equality should fail outside precision tolerance");

    auto verify_bracket_hyphen =
        MakeSlug(checklist, "VerifyBracketHyphen", instance_principal, user_principal);
    verify_bracket_hyphen.spec = "[2.5 - 3.5] bar";
    verify_bracket_hyphen.slug_id = core::ComputeSlugId(
        verify_bracket_hyphen.checklist, verify_bracket_hyphen.section,
        verify_bracket_hyphen.procedure, verify_bracket_hyphen.action,
        verify_bracket_hyphen.spec, verify_bracket_hyphen.instructions);
    verify_bracket_hyphen.address_id =
        core::ComposeAddressId(verify_bracket_hyphen.slug_id, verify_bracket_hyphen.instance_id);
    store.UpsertSlug(verify_bracket_hyphen);
    record_relationship(verify_bracket_hyphen.address_id, "BoolVerifyValidatedStatus",
                        verify_bracket_hyphen.address_id);
    ApplyResultUpdate(store, verify_bracket_hyphen.address_id, "2600 mbar", user_principal);
    auto verify_bracket_after = store.GetSlugOrThrow(verify_bracket_hyphen.address_id);
    Assert(verify_bracket_after.status == core::ChecklistStatus::kPass,
           "Bracket range hyphen compatibility form should parse with trailing unit");
    ApplyResultUpdate(store, verify_bracket_hyphen.address_id, "3", user_principal);
    verify_bracket_after = store.GetSlugOrThrow(verify_bracket_hyphen.address_id);
    Assert(verify_bracket_after.status == core::ChecklistStatus::kPass,
           "Bracket range should assume result units from spec trailing unit when omitted");
    Assert(verify_bracket_after.result == "3 bar",
           "Unitless bracket-range result should be stored in the spec unit");
    ApplyResultUpdate(store, verify_bracket_hyphen.address_id, "2.4 bar", user_principal);
    Assert(store.GetSlugOrThrow(verify_bracket_hyphen.address_id).status == core::ChecklistStatus::kFail,
           "Bracket range hyphen compatibility form should fail outside bounds");

    auto verify_flow_lph =
        MakeSlug(checklist, "VerifyFlowLphAlias", instance_principal, user_principal);
    verify_flow_lph.spec = "=2 L/h";
    verify_flow_lph.slug_id = core::ComputeSlugId(
        verify_flow_lph.checklist, verify_flow_lph.section, verify_flow_lph.procedure,
        verify_flow_lph.action, verify_flow_lph.spec, verify_flow_lph.instructions);
    verify_flow_lph.address_id =
        core::ComposeAddressId(verify_flow_lph.slug_id, verify_flow_lph.instance_id);
    store.UpsertSlug(verify_flow_lph);
    record_relationship(verify_flow_lph.address_id, "BoolVerifyValidatedStatus",
                        verify_flow_lph.address_id);
    ApplyResultUpdate(store, verify_flow_lph.address_id, "2 lph", user_principal);
    Assert(store.GetSlugOrThrow(verify_flow_lph.address_id).status == core::ChecklistStatus::kPass,
           "Flow aliases should accept lph as liters per hour");

    auto verify_flow_gph =
        MakeSlug(checklist, "VerifyFlowGphAlias", instance_principal, user_principal);
    verify_flow_gph.spec = ">=2 L/h";
    verify_flow_gph.slug_id = core::ComputeSlugId(
        verify_flow_gph.checklist, verify_flow_gph.section, verify_flow_gph.procedure,
        verify_flow_gph.action, verify_flow_gph.spec, verify_flow_gph.instructions);
    verify_flow_gph.address_id =
        core::ComposeAddressId(verify_flow_gph.slug_id, verify_flow_gph.instance_id);
    store.UpsertSlug(verify_flow_gph);
    record_relationship(verify_flow_gph.address_id, "BoolVerifyValidatedStatus",
                        verify_flow_gph.address_id);
    ApplyResultUpdate(store, verify_flow_gph.address_id, "0.53 gph", user_principal);
    Assert(store.GetSlugOrThrow(verify_flow_gph.address_id).status == core::ChecklistStatus::kPass,
           "Flow aliases should accept gph as gallons per hour");

    auto verify_comment_source =
        MakeSlug(checklist, "VerifyCommentSource", instance_principal, user_principal);
    verify_comment_source.spec = "=1";
    verify_comment_source.slug_id = core::ComputeSlugId(
        verify_comment_source.checklist, verify_comment_source.section,
        verify_comment_source.procedure, verify_comment_source.action, verify_comment_source.spec,
        verify_comment_source.instructions);
    verify_comment_source.address_id =
        core::ComposeAddressId(verify_comment_source.slug_id, verify_comment_source.instance_id);
    const auto verify_comment_target =
        MakeSlug(checklist, "VerifyCommentTarget", instance_principal, user_principal);
    store.UpsertSlug(verify_comment_source);
    store.UpsertSlug(verify_comment_target);
    record_relationship(verify_comment_source.address_id, "BoolVerifyValidatedComment",
                        verify_comment_target.address_id);
    ApplyResultUpdate(store, verify_comment_source.address_id, "1", user_principal);
    Assert(store.GetSlugOrThrow(verify_comment_target.address_id).comment == "TRUE",
           "BoolVerifyValidatedComment true should write TRUE");
    ApplyResultUpdate(store, verify_comment_source.address_id, "2", user_principal);
    Assert(store.GetSlugOrThrow(verify_comment_target.address_id).comment == "FALSE",
           "BoolVerifyValidatedComment false should write FALSE");
    ApplyResultUpdate(store, verify_comment_source.address_id, "x", user_principal);
    Assert(store.GetSlugOrThrow(verify_comment_target.address_id).comment == "INDETERMINATE",
           "BoolVerifyValidatedComment indeterminate should write INDETERMINATE");

    auto verify_literal_source =
        MakeSlug(checklist, "VerifyLiteralSource", instance_principal, user_principal);
    verify_literal_source.spec = "=1";
    verify_literal_source.slug_id = core::ComputeSlugId(
        verify_literal_source.checklist, verify_literal_source.section,
        verify_literal_source.procedure, verify_literal_source.action, verify_literal_source.spec,
        verify_literal_source.instructions);
    verify_literal_source.address_id =
        core::ComposeAddressId(verify_literal_source.slug_id, verify_literal_source.instance_id);
    const auto verify_literal_target =
        MakeSlug(checklist, "VerifyLiteralTarget", instance_principal, user_principal);
    store.UpsertSlug(verify_literal_source);
    store.UpsertSlug(verify_literal_target);
    record_relationship(verify_literal_source.address_id, "BoolVerifyValidatedPass",
                        verify_literal_target.address_id);
    ApplyStatusUpdate(store, verify_literal_target.address_id, core::ChecklistStatus::kFail,
                      "literal-prep", user_principal);
    ApplyResultUpdate(store, verify_literal_source.address_id, "1", user_principal);
    Assert(store.GetSlugOrThrow(verify_literal_target.address_id).status == core::ChecklistStatus::kPass,
           "BoolVerifyValidatedPass true should set Pass");
    ApplyStatusUpdate(store, verify_literal_target.address_id, core::ChecklistStatus::kFail,
                      "literal-reset", user_principal);
    ApplyResultUpdate(store, verify_literal_source.address_id, "2", user_principal);
    Assert(store.GetSlugOrThrow(verify_literal_target.address_id).status == core::ChecklistStatus::kFail,
           "BoolVerifyValidatedPass false should not write target status");

    auto verify_and_source_a =
        MakeSlug(checklist, "VerifyAndSourceA", instance_principal, user_principal);
    auto verify_and_source_b =
        MakeSlug(checklist, "VerifyAndSourceB", instance_principal, user_principal);
    verify_and_source_a.spec = "=1";
    verify_and_source_a.slug_id = core::ComputeSlugId(
        verify_and_source_a.checklist, verify_and_source_a.section, verify_and_source_a.procedure,
        verify_and_source_a.action, verify_and_source_a.spec, verify_and_source_a.instructions);
    verify_and_source_a.address_id =
        core::ComposeAddressId(verify_and_source_a.slug_id, verify_and_source_a.instance_id);
    verify_and_source_b.spec = "=1";
    verify_and_source_b.slug_id = core::ComputeSlugId(
        verify_and_source_b.checklist, verify_and_source_b.section, verify_and_source_b.procedure,
        verify_and_source_b.action, verify_and_source_b.spec, verify_and_source_b.instructions);
    verify_and_source_b.address_id =
        core::ComposeAddressId(verify_and_source_b.slug_id, verify_and_source_b.instance_id);
    const auto verify_and_target =
        MakeSlug(checklist, "VerifyAndTarget", instance_principal, user_principal);
    store.UpsertSlug(verify_and_source_a);
    store.UpsertSlug(verify_and_source_b);
    store.UpsertSlug(verify_and_target);
    record_relationship(verify_and_source_a.address_id, "BoolVerifyAndGateStatus",
                        verify_and_target.address_id);
    record_relationship(verify_and_source_b.address_id, "BoolVerifyAndGateStatus",
                        verify_and_target.address_id);
    ApplyStatusUpdate(store, verify_and_target.address_id, core::ChecklistStatus::kPass,
                      "verify-and-prep", user_principal);
    ApplyResultUpdate(store, verify_and_source_a.address_id, "1", user_principal);
    Assert(store.GetSlugOrThrow(verify_and_target.address_id).status == core::ChecklistStatus::kPass,
           "BoolVerifyAndGateStatus with indeterminate peers should not write status");
    ApplyResultUpdate(store, verify_and_source_b.address_id, "2", user_principal);
    Assert(store.GetSlugOrThrow(verify_and_target.address_id).status == core::ChecklistStatus::kFail,
           "BoolVerifyAndGateStatus false contributor should set Fail");
    ApplyResultUpdate(store, verify_and_source_b.address_id, "1", user_principal);
    Assert(store.GetSlugOrThrow(verify_and_target.address_id).status == core::ChecklistStatus::kPass,
           "BoolVerifyAndGateStatus all true contributors should set Pass");

    auto verify_or_source_a =
        MakeSlug(checklist, "VerifyOrSourceA", instance_principal, user_principal);
    auto verify_or_source_b =
        MakeSlug(checklist, "VerifyOrSourceB", instance_principal, user_principal);
    verify_or_source_a.spec = "=1";
    verify_or_source_a.slug_id = core::ComputeSlugId(
        verify_or_source_a.checklist, verify_or_source_a.section, verify_or_source_a.procedure,
        verify_or_source_a.action, verify_or_source_a.spec, verify_or_source_a.instructions);
    verify_or_source_a.address_id =
        core::ComposeAddressId(verify_or_source_a.slug_id, verify_or_source_a.instance_id);
    verify_or_source_b.spec = "=1";
    verify_or_source_b.slug_id = core::ComputeSlugId(
        verify_or_source_b.checklist, verify_or_source_b.section, verify_or_source_b.procedure,
        verify_or_source_b.action, verify_or_source_b.spec, verify_or_source_b.instructions);
    verify_or_source_b.address_id =
        core::ComposeAddressId(verify_or_source_b.slug_id, verify_or_source_b.instance_id);
    const auto verify_or_target =
        MakeSlug(checklist, "VerifyOrTarget", instance_principal, user_principal);
    store.UpsertSlug(verify_or_source_a);
    store.UpsertSlug(verify_or_source_b);
    store.UpsertSlug(verify_or_target);
    record_relationship(verify_or_source_a.address_id, "BoolVerifyOrGateStatus",
                        verify_or_target.address_id);
    record_relationship(verify_or_source_b.address_id, "BoolVerifyOrGateStatus",
                        verify_or_target.address_id);
    ApplyStatusUpdate(store, verify_or_target.address_id, core::ChecklistStatus::kPass,
                      "verify-or-prep", user_principal);
    ApplyResultUpdate(store, verify_or_source_a.address_id, "2", user_principal);
    Assert(store.GetSlugOrThrow(verify_or_target.address_id).status == core::ChecklistStatus::kPass,
           "BoolVerifyOrGateStatus with indeterminate peers should not write status");
    ApplyResultUpdate(store, verify_or_source_b.address_id, "2", user_principal);
    Assert(store.GetSlugOrThrow(verify_or_target.address_id).status == core::ChecklistStatus::kFail,
           "BoolVerifyOrGateStatus all false contributors should set Fail");
    ApplyResultUpdate(store, verify_or_source_b.address_id, "1", user_principal);
    Assert(store.GetSlugOrThrow(verify_or_target.address_id).status == core::ChecklistStatus::kPass,
           "BoolVerifyOrGateStatus any true contributor should set Pass");
    RecordStep(current_step, true, "verify bridge ok");

    // Single-trigger check: only predicates matching subject state should fire.
    current_step = "single-trigger";
    const auto st_source = MakeSlug(checklist, "SingleTriggerSource", instance_principal, user_principal);
    const auto st_pass_target =
        MakeSlug(checklist, "SingleTriggerPassTarget", instance_principal, user_principal);
    const auto st_fail_target =
        MakeSlug(checklist, "SingleTriggerFailTarget", instance_principal, user_principal);
    store.UpsertSlug(st_source);
    store.UpsertSlug(st_pass_target);
    store.UpsertSlug(st_fail_target);
    record_relationship(st_source.address_id, "passPropagateValidatedPass", st_pass_target.address_id);
    record_relationship(st_source.address_id, "failPropagateValidatedPass", st_fail_target.address_id);
    ApplyStatusUpdate(store, st_pass_target.address_id, core::ChecklistStatus::kFail, "prep-pass", user_principal);
    ApplyStatusUpdate(store, st_fail_target.address_id, core::ChecklistStatus::kFail, "prep-fail", user_principal);
    ApplyStatusUpdate(store, st_source.address_id, core::ChecklistStatus::kOther, "set other", user_principal);
    ApplyStatusUpdate(store, st_source.address_id, core::ChecklistStatus::kPass, "set pass", user_principal);
    Assert(store.GetSlugOrThrow(st_pass_target.address_id).status == core::ChecklistStatus::kPass,
           "passPropagateValidatedPass should fire when subject is Pass");
    Assert(store.GetSlugOrThrow(st_fail_target.address_id).status == core::ChecklistStatus::kFail,
           "failPropagateValidatedPass must remain inactive when subject is Pass");
    AppendJsonl(predicate_summary_path,
                {{"event", "single_trigger"},
                 {"source", st_source.address_id},
                 {"pass_target", st_pass_target.address_id},
                 {"fail_target", st_fail_target.address_id},
                 {"pass_target_status",
                  core::StatusToString(store.GetSlugOrThrow(st_pass_target.address_id).status)},
                 {"fail_target_status",
                  core::StatusToString(store.GetSlugOrThrow(st_fail_target.address_id).status)}});
    RecordStep(current_step, true, "single-trigger ok");

    // Field propagate: ResultPropagateValidatedResult copies the source result verbatim.
    current_step = "field propagate";
    const auto fp_source = MakeSlug(checklist, "FieldPropSource", instance_principal, user_principal);
    const auto fp_target = MakeSlug(checklist, "FieldPropTarget", instance_principal, user_principal);
    store.UpsertSlug(fp_source);
    store.UpsertSlug(fp_target);
    record_relationship(fp_source.address_id, "ResultPropagateValidatedResult", fp_target.address_id);

    core::SlugUpdate target_prep;
    target_prep.address_id = fp_target.address_id;
    target_prep.comment = "keep-comment";
    target_prep.entity_principal_override = user_principal;
    store.ApplyUpdate(target_prep);

    ApplyResultUpdate(store, fp_source.address_id, "verbatim copy", user_principal);
    const auto fp_target_after = store.GetSlugOrThrow(fp_target.address_id);
    Assert(fp_target_after.result == "verbatim copy",
           "ResultPropagateValidatedResult should copy result to target");
    Assert(fp_target_after.comment == "keep-comment",
           "ResultPropagateValidatedResult should not alter target comment");
    Assert(fp_target_after.entity_id == daemon_entity_id,
           "ResultPropagateValidatedResult should stamp daemon entity_id");
    AppendJsonl(predicate_summary_path,
                {{"event", "field_propagate"},
                 {"source", fp_source.address_id},
                 {"target", fp_target.address_id},
                 {"result", fp_target_after.result}});
    RecordStep(current_step, true, "field propagate ok");

    // Fan-out: one subject updating many targets.
    current_step = "fanout";
    const auto fan_source = MakeSlug(checklist, "FanoutSource", instance_principal, user_principal);
    store.UpsertSlug(fan_source);
    std::vector<std::string> fan_targets;
    for (int i = 0; i < 11; ++i) {
      const auto tgt = MakeSlug(checklist, "FanoutTarget" + std::to_string(i), instance_principal, user_principal);
      store.UpsertSlug(tgt);
      fan_targets.push_back(tgt.address_id);
      record_relationship(fan_source.address_id, "passPropagateValidatedPass", tgt.address_id);
      ApplyStatusUpdate(store, tgt.address_id, core::ChecklistStatus::kFail, "fanout-prep", user_principal);
    }
    ApplyStatusUpdate(store, fan_source.address_id, core::ChecklistStatus::kOther, "fanout-other", user_principal);
    ApplyStatusUpdate(store, fan_source.address_id, core::ChecklistStatus::kPass, "fanout-pass", user_principal);
    for (const auto& tgt : fan_targets) {
      Assert(store.GetSlugOrThrow(tgt).status == core::ChecklistStatus::kPass,
             "Fanout targets must be set to Pass");
    }
    AppendJsonl(predicate_summary_path,
                {{"event", "fanout"},
                 {"source", fan_source.address_id},
                 {"target_count", static_cast<int>(fan_targets.size())}});
    RecordStep(current_step, true, "fanout ok");

    // Cycle safety: the configured depth may traverse A -> B -> C, but must
    // not re-enter and overwrite the initiating row as though it were a
    // fixpoint calculation.
    current_step = "cycle safety";
    const auto cyc_a = MakeSlug(checklist, "CycleA", instance_principal, user_principal);
    const auto cyc_b = MakeSlug(checklist, "CycleB", instance_principal, user_principal);
    const auto cyc_c = MakeSlug(checklist, "CycleC", instance_principal, user_principal);
    store.UpsertSlug(cyc_a);
    store.UpsertSlug(cyc_b);
    store.UpsertSlug(cyc_c);
    record_relationship(cyc_a.address_id, "passPropagateValidatedPass", cyc_b.address_id);
    record_relationship(cyc_b.address_id, "passPropagateValidatedPass", cyc_c.address_id);
    record_relationship(cyc_c.address_id, "passPropagateValidatedPass", cyc_a.address_id);
    ApplyStatusUpdate(store, cyc_a.address_id, core::ChecklistStatus::kFail, "cycle-a-prep", user_principal);
    ApplyStatusUpdate(store, cyc_b.address_id, core::ChecklistStatus::kFail, "cycle-b-prep", user_principal);
    ApplyStatusUpdate(store, cyc_c.address_id, core::ChecklistStatus::kFail, "cycle-c-prep", user_principal);
    ApplyStatusUpdate(store, cyc_a.address_id, core::ChecklistStatus::kPass, "cycle-a-pass", user_principal);
    Assert(store.GetSlugOrThrow(cyc_b.address_id).status == core::ChecklistStatus::kPass,
           "Cycle: A should propagate to B");
    Assert(store.GetSlugOrThrow(cyc_c.address_id).status == core::ChecklistStatus::kPass,
           "Cycle: configured two-hop chain should cascade B->C");
    Assert(store.GetSlugOrThrow(cyc_a.address_id).comment == "cycle-a-pass",
           "Cycle: daemon must not re-enter and overwrite the initiating row");
    AppendJsonl(predicate_summary_path,
                {{"event", "cycle_safety"},
                 {"cycle_a", cyc_a.address_id},
                 {"cycle_b", cyc_b.address_id},
                 {"cycle_c", cyc_c.address_id},
                 {"cycle_a_comment", store.GetSlugOrThrow(cyc_a.address_id).comment},
                 {"cycle_b_status", core::StatusToString(store.GetSlugOrThrow(cyc_b.address_id).status)},
                 {"cycle_c_status", core::StatusToString(store.GetSlugOrThrow(cyc_c.address_id).status)}});
    RecordStep(current_step, true, "cycle safety ok");

    current_step = "export import rels";
    const auto slugs =
        store.QuerySlugs(checklist, core::ComputeInstanceId(instance_principal), std::nullopt,
                         std::nullopt, std::nullopt, std::nullopt);
    const auto markdown = core::markdown::ExportChecklistMarkdown(
        checklist, slugs, {}, core::markdown::RelationshipExportMode::kAddress,
        core::markdown::RelationshipIdentityFormat::kId);
    const auto parsed = core::markdown::ParseChecklistMarkdown(checklist, markdown);

    std::unordered_set<std::string> expected_keys;
    expected_keys.reserve(expected_relationships.size());
    for (const auto& rel : expected_relationships) {
      expected_keys.insert(rel.subject_address_id + "|" + rel.predicate + "|" + rel.target_address_id);
    }

    std::unordered_set<std::string> actual_keys;
    actual_keys.reserve(parsed.address_relationships.size());
    for (const auto& rel : parsed.address_relationships) {
      actual_keys.insert(rel.subject_address_id + "|" + rel.predicate + "|" + rel.target_address_id);
    }

    Assert(actual_keys.size() == expected_keys.size(),
           "Export/import must preserve all address relationships.");
    for (const auto& key : expected_keys) {
      Assert(actual_keys.count(key) == 1,
             "Export/import missing relationship: " + key);
    }
    AppendJsonl(predicate_summary_path,
                {{"event", "export_import"},
                 {"expected_count", static_cast<int>(expected_keys.size())},
                 {"actual_count", static_cast<int>(actual_keys.size())}});
    RecordStep(current_step, true, "export/import ok");

    RemoveIfExists(db_path);
    return 0;
  } catch (const std::exception& ex) {
    if (!current_step.empty()) {
      RecordStep(current_step, false, ex.what());
    }
    std::cerr << "[predicate-daemon-exhaustive] FAIL: " << ex.what() << "\n";
    RemoveIfExists(db_path);
    return 1;
  }
}
