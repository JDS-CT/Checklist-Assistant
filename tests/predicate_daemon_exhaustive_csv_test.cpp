#include <algorithm>
#include <array>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

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
  std::cout << "CHAX_STEP|predicate_daemon_exhaustive_csv_test|" << procedure << "|"
            << (pass ? "Pass" : "Fail") << "|" << SanitizeMessage(message) << "\n";
}

void SetEnvValue(const std::string& key, const std::string& value) {
#if defined(_WIN32)
  _putenv_s(key.c_str(), value.c_str());
#else
  setenv(key.c_str(), value.c_str(), 1);
#endif
}

void RemoveIfExists(const std::filesystem::path& path) {
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

enum class SlotField { kResult, kStatus, kComment, kTimestamp };

std::string_view SlotFieldToken(SlotField field) {
  switch (field) {
    case SlotField::kResult:
      return "Result";
    case SlotField::kStatus:
      return "Status";
    case SlotField::kComment:
      return "Comment";
    case SlotField::kTimestamp:
      return "Timestamp";
  }
  return "Result";
}

std::string_view SlotFieldLower(SlotField field) {
  switch (field) {
    case SlotField::kResult:
      return "result";
    case SlotField::kStatus:
      return "status";
    case SlotField::kComment:
      return "comment";
    case SlotField::kTimestamp:
      return "timestamp";
  }
  return "result";
}

std::string ComputeLegacySlugId(const std::string& legacy_checklist,
                                const core::ChecklistSlug& slug) {
  return core::ComputeSlugId(legacy_checklist, slug.section, slug.procedure, slug.action,
                             slug.spec, slug.instructions);
}

core::ChecklistSlug MakeSlug(const std::string& checklist,
                             const std::string& procedure,
                             const std::string& instance_principal,
                             const std::string& entity_principal) {
  core::ChecklistSlug slug;
  slug.checklist = checklist;
  slug.section = "CSV Prefill Matrix";
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
  slug.slug_id = core::ComputeSlugId(slug.checklist, slug.section, slug.procedure, slug.action,
                                     slug.spec, slug.instructions);
  slug.address_id = core::ComposeAddressId(slug.slug_id, slug.instance_id);
  slug.entity_principal = entity_principal;
  slug.entity_id = core::ComputeEntityId(entity_principal);
  return slug;
}

struct PrefillCase {
  SlotField subject;
  SlotField object;
  std::string predicate;
  std::string target_slug_id;
  std::string target_address_id;
};

std::string ExpectedValue(std::size_t row_index, SlotField row_subject, SlotField object,
                          const std::array<std::string, 4>& status_values,
                          const std::array<std::string, 4>& timestamp_values) {
  if (object == SlotField::kStatus) {
    return status_values[row_index];
  }
  if (object == SlotField::kTimestamp) {
    return timestamp_values[row_index];
  }
  std::string value = "prefill-";
  value += std::string(SlotFieldLower(row_subject));
  value += "-";
  value += std::string(SlotFieldLower(object));
  value += "-";
  value += std::to_string(row_index);
  return value;
}

std::string ReadField(const core::ChecklistSlug& slug, SlotField field) {
  switch (field) {
    case SlotField::kResult:
      return slug.result;
    case SlotField::kStatus:
      return core::StatusToString(slug.status);
    case SlotField::kComment:
      return slug.comment;
    case SlotField::kTimestamp:
      return slug.timestamp;
  }
  return {};
}

void ClearTargetField(core::ChecklistStore& store, const PrefillCase& item) {
  core::SlugUpdate update;
  update.address_id = item.target_address_id;
  switch (item.object) {
    case SlotField::kResult:
      update.result = "";
      break;
    case SlotField::kStatus:
      update.status = core::ChecklistStatus::kUnknown;
      break;
    case SlotField::kComment:
      update.comment = "";
      break;
    case SlotField::kTimestamp:
      update.timestamp = "";
      break;
  }
  store.ApplyUpdate(update);
}

}  // namespace

int main() {
  const auto db_path =
      (std::filesystem::temp_directory_path() / "chax-predicate-daemon-exhaustive-csv.db").string();
  RemoveIfExists(db_path);

  std::string current_step;
  try {
    core::logging::SetLogLevel(core::logging::LogLevel::kError);

    const auto repo_root = ResolveRepoRoot();
    const auto artifacts_root =
        repo_root / ".chax" / "test-artifacts" / "predicate-daemon-exhaustive-csv";
    const auto library_root = artifacts_root / "library";
    SetEnvValue("CHAX_CHECKLISTS_ROOT", library_root.string());
    const auto summary_path = artifacts_root / "prefill_slot_matrix.jsonl";
    {
      std::error_code ec;
      std::filesystem::create_directories(artifacts_root, ec);
    }
    RemoveIfExists(summary_path);

    const std::string checklist = "predicate-daemon-exhaustive-csv";
    const std::string legacy_checklist = checklist + "-legacy";
    const std::string self_checklist = "predicate-daemon-self-prefill";
    const std::string composed_checklist = "predicate-daemon-composed-prefill";
    const std::string instance_principal = "instance||predicate-daemon-exhaustive-csv";
    const std::string user_principal = "user||provider=test||username=prefill-csv";
    constexpr bool kUseLineageHeaders = true;

    const std::string pack = "test-pack";
    const auto dataset_dir = library_root / pack / checklist / "data";
    const auto dataset_path = dataset_dir / (checklist + ".csv");
    const auto self_dataset_dir = library_root / pack / self_checklist / "data";
    const auto self_dataset_path = self_dataset_dir / (self_checklist + ".csv");
    const auto composed_dataset_dir = library_root / pack / composed_checklist / "data";
    const auto composed_dataset_path = composed_dataset_dir / (composed_checklist + ".csv");
    {
      std::error_code ec;
      std::filesystem::create_directories(dataset_dir, ec);
    }

    core::ChecklistStore store(db_path);
    store.Initialize(/*seed_demo_data=*/false);

    current_step = "csv prefill matrix";
    const auto subject_slug = MakeSlug(checklist, "CsvPrefillSubject", instance_principal, user_principal);
    store.UpsertSlug(subject_slug);
    std::unordered_map<std::string, std::string> legacy_slug_ids;
    if (kUseLineageHeaders) {
      const std::string legacy_id = ComputeLegacySlugId(legacy_checklist, subject_slug);
      legacy_slug_ids.emplace(subject_slug.slug_id, legacy_id);
      store.InsertTemplateRelationship(
          core::TemplateRelationship{subject_slug.slug_id, "slugPredecessor", legacy_id});
    }

    constexpr SlotField kSubjectFields[] = {SlotField::kResult, SlotField::kStatus,
                                            SlotField::kComment, SlotField::kTimestamp};
    constexpr SlotField kObjectFields[] = {SlotField::kResult, SlotField::kStatus,
                                           SlotField::kComment, SlotField::kTimestamp};
    const std::array<std::string, 4> subject_keys = {
        "subject-result-key", "Fail", "subject-comment-key", "2026-01-27T00:00:00Z"};
    const std::array<std::string, 4> status_values = {"Pass", "Fail", "Other", "NA"};
    const std::array<std::string, 4> timestamp_values = {
        "2026-01-27T01:00:00Z", "2026-01-27T02:00:00Z", "2026-01-27T03:00:00Z",
        "2026-01-27T04:00:00Z"};

    std::vector<PrefillCase> cases;
    cases.reserve(16);
    int ordinal = 0;
    for (const auto& subject_field : kSubjectFields) {
      for (const auto& object_field : kObjectFields) {
        std::ostringstream proc;
        proc << "CsvPrefillTarget" << ordinal++;
        const auto target_slug = MakeSlug(checklist, proc.str(), instance_principal, user_principal);
        store.UpsertSlug(target_slug);
        if (kUseLineageHeaders) {
          const std::string legacy_id = ComputeLegacySlugId(legacy_checklist, target_slug);
          legacy_slug_ids.emplace(target_slug.slug_id, legacy_id);
          store.InsertTemplateRelationship(
              core::TemplateRelationship{target_slug.slug_id, "slugPredecessor", legacy_id});
        }
        const std::string predicate =
            std::string(SlotFieldToken(subject_field)) + "SearchPrefill" +
            std::string(SlotFieldToken(object_field));
        store.InsertAddressRelationship(
            subject_slug.address_id, core::RelationshipEdge{predicate, target_slug.address_id});
        cases.push_back(
            {subject_field, object_field, predicate, target_slug.slug_id, target_slug.address_id});
      }
    }

    std::vector<std::string> headers;
    headers.reserve(4 + cases.size());
    for (const auto& subject_field : kSubjectFields) {
      const std::string& slug_id = kUseLineageHeaders
                                       ? legacy_slug_ids.at(subject_slug.slug_id)
                                       : subject_slug.slug_id;
      headers.push_back(slug_id + "-" + std::string(SlotFieldLower(subject_field)));
    }
    for (const auto& item : cases) {
      const std::string& slug_id = kUseLineageHeaders
                                       ? legacy_slug_ids.at(item.target_slug_id)
                                       : item.target_slug_id;
      headers.push_back(slug_id + "-" + std::string(SlotFieldLower(item.object)));
    }

    std::vector<std::vector<std::string>> rows;
    rows.resize(std::size(kSubjectFields));
    for (auto& row : rows) {
      row.assign(headers.size(), "");
    }

    auto set_cell = [&](std::size_t row, const std::string& header, const std::string& value) {
      auto it = std::find(headers.begin(), headers.end(), header);
      Assert(it != headers.end(), "Header missing: " + header);
      rows[row][static_cast<std::size_t>(std::distance(headers.begin(), it))] = value;
    };

    for (std::size_t row_index = 0; row_index < std::size(kSubjectFields); ++row_index) {
      const auto row_subject = kSubjectFields[row_index];
      const std::string subject_header =
          (kUseLineageHeaders ? legacy_slug_ids.at(subject_slug.slug_id) : subject_slug.slug_id) +
          "-" + std::string(SlotFieldLower(row_subject));
      set_cell(row_index, subject_header, subject_keys[row_index]);
      for (const auto& item : cases) {
        const std::string target_header =
            (kUseLineageHeaders ? legacy_slug_ids.at(item.target_slug_id) : item.target_slug_id) +
            "-" + std::string(SlotFieldLower(item.object));
        set_cell(row_index, target_header,
                 ExpectedValue(row_index, row_subject, item.object, status_values,
                               timestamp_values));
      }
    }

    {
      std::ofstream out(dataset_path);
      Assert(out.is_open(), "Failed to write dataset to " + dataset_path.string());
      for (std::size_t i = 0; i < headers.size(); ++i) {
        if (i != 0) out << ",";
        out << headers[i];
      }
      out << "\n";
      for (const auto& row : rows) {
        for (std::size_t i = 0; i < row.size(); ++i) {
          if (i != 0) out << ",";
          out << row[i];
        }
        out << "\n";
      }
    }

    AppendJsonl(summary_path,
                {{"event", "csv_prefill_setup"},
                 {"checklist", checklist},
                 {"instance_principal", instance_principal},
                 {"dataset", dataset_path.string()},
                 {"case_count", static_cast<int>(cases.size())}});

    for (std::size_t row_index = 0; row_index < std::size(kSubjectFields); ++row_index) {
      const auto row_subject = kSubjectFields[row_index];
      for (const auto& item : cases) {
        ClearTargetField(store, item);
      }

      core::SlugUpdate subject_update;
      subject_update.address_id = subject_slug.address_id;
      switch (row_subject) {
        case SlotField::kResult:
          subject_update.result = subject_keys[row_index];
          break;
        case SlotField::kStatus:
          subject_update.status = core::ParseStatus(subject_keys[row_index]);
          break;
        case SlotField::kComment:
          subject_update.comment = subject_keys[row_index];
          break;
        case SlotField::kTimestamp:
          subject_update.timestamp = subject_keys[row_index];
          break;
      }
      store.ApplyUpdate(subject_update);

      int active_count = 0;
      for (const auto& item : cases) {
        const auto target_after = store.GetSlugOrThrow(item.target_address_id);
        const bool is_active = item.subject == row_subject;
        const std::string actual_value = ReadField(target_after, item.object);
        std::string expected_value;
        if (is_active) {
          expected_value =
              ExpectedValue(row_index, row_subject, item.object, status_values, timestamp_values);
          ++active_count;
          if (item.object == SlotField::kStatus) {
            Assert(actual_value == expected_value,
                   "Status prefill mismatch for " + item.predicate);
          } else {
            Assert(actual_value == expected_value,
                   "Prefill mismatch for " + item.predicate);
          }
        } else {
          if (item.object == SlotField::kStatus) {
            Assert(actual_value == core::StatusToString(core::ChecklistStatus::kUnknown),
                   "Inactive status prefill should remain Unknown for " + item.predicate);
          } else {
            Assert(actual_value.empty(),
                   "Inactive prefill should remain empty for " + item.predicate);
          }
        }

        AppendJsonl(summary_path,
                    {{"event", "csv_prefill_case"},
                     {"subject_field", std::string(SlotFieldToken(row_subject))},
                     {"object_field", std::string(SlotFieldToken(item.object))},
                     {"predicate", item.predicate},
                     {"target_address_id", item.target_address_id},
                     {"active", is_active},
                     {"expected", expected_value},
                     {"actual", actual_value}});
      }

      AppendJsonl(summary_path,
                  {{"event", "csv_prefill_subject_complete"},
                   {"subject_field", std::string(SlotFieldToken(row_subject))},
                   {"active_count", active_count}});
    }

    RecordStep(current_step, true, "csv prefill matrix ok");

    current_step = "csv prefill self pass";
    {
      std::error_code ec;
      std::filesystem::create_directories(self_dataset_dir, ec);
    }
    const auto self_slug =
        MakeSlug(self_checklist, "CsvPrefillSelf", instance_principal, user_principal);
    store.UpsertSlug(self_slug);
    store.InsertAddressRelationship(
        self_slug.address_id,
        core::RelationshipEdge{"PassSearchPrefillResult", self_slug.address_id});

    {
      std::ofstream out(self_dataset_path);
      Assert(out.is_open(),
             "Failed to write self prefill dataset to " + self_dataset_path.string());
      out << self_slug.slug_id << "-pass," << self_slug.slug_id << "-result\n";
      out << "Pass,prefill-self-pass\n";
    }

    core::SlugUpdate self_update;
    self_update.address_id = self_slug.address_id;
    self_update.status = core::ChecklistStatus::kPass;
    store.ApplyUpdate(self_update);

    const auto self_after = store.GetSlugOrThrow(self_slug.address_id);
    Assert(self_after.result == "prefill-self-pass",
           "Self prefill should update result on Pass");
    RecordStep(current_step, true, "csv prefill self pass ok");

    current_step = "csv composed prefill chain";
    {
      std::error_code ec;
      std::filesystem::create_directories(composed_dataset_dir, ec);
    }
    const auto composed_source =
        MakeSlug(checklist, "ComposedPrefillSource", instance_principal, user_principal);
    const auto composed_target =
        MakeSlug(composed_checklist, "ComposedPrefillTarget", instance_principal, user_principal);
    store.UpsertSlug(composed_source);
    store.UpsertSlug(composed_target);
    store.InsertAddressRelationship(
        composed_source.address_id,
        core::RelationshipEdge{"ResultPropagateValidatedResult", composed_target.address_id});
    store.InsertAddressRelationship(
        composed_target.address_id,
        core::RelationshipEdge{"ResultSearchPrefillComment", composed_target.address_id});
    {
      std::ofstream out(composed_dataset_path);
      Assert(out.is_open(),
             "Failed to write composed prefill dataset to " + composed_dataset_path.string());
      out << composed_target.slug_id << "-result," << composed_target.slug_id << "-comment\n";
      out << "selector,target-owned-comment\n";
    }

    core::SlugUpdate composed_update;
    composed_update.address_id = composed_source.address_id;
    composed_update.result = "selector";
    store.ApplyUpdate(composed_update);

    const auto composed_after = store.GetSlugOrThrow(composed_target.address_id);
    Assert(composed_after.result == "selector",
           "Composed prefill should propagate the upstream selector to the target");
    Assert(composed_after.comment == "target-owned-comment",
           "Composed prefill should use the target-owned CSV on the second hop");
    RecordStep(current_step, true, "csv composed prefill chain ok");

    RemoveIfExists(dataset_path);
    RemoveIfExists(self_dataset_path);
    RemoveIfExists(composed_dataset_path);
    std::error_code ec;
    std::filesystem::remove(dataset_dir, ec);
    std::filesystem::remove(self_dataset_dir, ec);
    std::filesystem::remove(composed_dataset_dir, ec);
    RemoveIfExists(db_path);
    return 0;
  } catch (const std::exception& ex) {
    if (!current_step.empty()) {
      RecordStep(current_step, false, ex.what());
    }
    std::cerr << "[predicate-daemon-exhaustive-csv] FAIL: " << ex.what() << "\n";
    RemoveIfExists(db_path);
    return 1;
  }
}
