#pragma once

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

struct sqlite3;

namespace core {

enum class ChecklistStatus { kUnknown = 0, kPass, kFail, kNA, kOther };

struct RelationshipEdge {
  std::string predicate;
  std::string target;
};

struct TemplateRelationship {
  std::string subject_slug_id;
  std::string predicate;
  std::string target_slug_id;
};

struct AddressRelationship {
  std::string subject_address_id;
  std::string predicate;
  std::string target_address_id;
};

struct PredicateRecord {
  std::string name;
  std::string kind;
  std::string status;
  std::string description;
  std::string meta;
};

struct HistoryEntry {
  std::string address_id;
  std::string timestamp;
  std::string result;
  ChecklistStatus status = ChecklistStatus::kUnknown;
  std::string comment;
  std::string entity_id;
};

struct PrefillDatasetStatus {
  std::string mode;
  std::string path;
  std::string matched_slug_id;
};

struct RelationshipGraph {
  std::vector<RelationshipEdge> outgoing;
  std::vector<RelationshipEdge> incoming;
};

struct VerifyEvaluation {
  std::string predicate;
  std::string target_address_id;
  std::string predicate_bool;
  std::string reason_code;
  std::string reason;
  bool gate_applied = false;
  std::string gate_mode;
  int contributor_count = 0;
  int contributor_true_count = 0;
  bool would_write = false;
  std::string write_decision;
};

struct ChecklistSlug {
  std::string slug_id;
  std::string instance_id;
  std::string instance_principal;
  std::string address_id;
  std::int64_t address_order = 0;
  std::string entity_principal;
  std::string checklist;
  std::string section;
  std::string procedure;
  std::string action;
  std::string spec;

  std::string result;
  ChecklistStatus status = ChecklistStatus::kUnknown;
  std::string comment;
  std::string timestamp;
  std::string entity_id;

  std::string instructions;
  std::vector<RelationshipEdge> relationships;
};

struct ChecklistOwnership {
  std::string source_name;
  std::string source_path;
  std::string pack;
  std::string checklist_dir;
  std::string checklist;
};

struct SlugUpdate {
  std::string address_id;
  std::optional<std::string> result;
  std::optional<ChecklistStatus> status;
  std::optional<std::string> comment;
  std::optional<std::string> timestamp;
  std::optional<std::string> entity_id_override;
  std::optional<std::string> entity_principal_override;
};

class ChecklistStore {
 public:
  explicit ChecklistStore(std::string db_path);
  ~ChecklistStore();

  ChecklistStore(const ChecklistStore&) = delete;
  ChecklistStore& operator=(const ChecklistStore&) = delete;

  void Initialize(bool seed_demo_data);
  ChecklistSlug GetSlugOrThrow(const std::string& address_id) const;
  std::vector<ChecklistSlug> GetSlugsForChecklist(const std::string& checklist) const;
  std::vector<ChecklistSlug> GetSlugsForChecklist(const std::string &checklist,
                                                  const std::optional<ChecklistOwnership> &ownership_filter) const;
  std::vector<ChecklistSlug> QuerySlugs(const std::optional<std::string>& checklist,
                                        const std::optional<std::string>& instance_id,
                                        const std::optional<std::string>& section,
                                        const std::optional<ChecklistStatus>& status,
                                        std::optional<int> limit,
                                        std::optional<int> offset) const;
  std::vector<ChecklistSlug> QuerySlugs(const std::optional<std::string> &checklist,
                                        const std::optional<std::string> &instance_id,
                                        const std::optional<std::string> &section,
                                        const std::optional<ChecklistStatus> &status, std::optional<int> limit,
                                        std::optional<int> offset,
                                        const std::optional<ChecklistOwnership> &ownership_filter) const;
  bool HasSlugById(const std::string& slug_id) const;
  RelationshipGraph GetRelationships(const std::string& address_id) const;
  std::optional<PrefillDatasetStatus> GetPrefillDatasetStatus(
      const std::string& address_id) const;
  std::vector<VerifyEvaluation> EvaluateVerifyRelationships(
      const std::string& address_id) const;
  std::optional<PredicateRecord> GetPredicate(const std::string& name) const;
  std::vector<PredicateRecord> ListPredicates(std::optional<int> limit,
                                              std::optional<int> offset) const;
  void UpsertPredicate(const PredicateRecord& predicate);
  void EnsurePredicate(const std::string& name, const std::string& kind,
                       const std::string& status);
  void SetPredicateChainDepth(int depth);
  std::vector<HistoryEntry> GetHistory(const std::string& address_id) const;
  std::vector<HistoryEntry> GetHistory(const std::string& address_id, std::optional<int> limit,
                                       std::optional<int> offset = std::nullopt) const;
  void ApplyUpdate(const SlugUpdate& update);
  void ApplyBulkUpdates(const std::vector<SlugUpdate>& updates);
  void ReplaceChecklist(const std::string& checklist, const std::vector<ChecklistSlug>& slugs);
  std::vector<ChecklistSlug> ExportAllSlugs() const;
  std::vector<std::string> ListChecklists() const;
  void UpsertSlug(const ChecklistSlug& slug);
  void ReplaceRelationships(const std::string& subject_id,
                            const std::vector<RelationshipEdge>& edges);
  void UpsertOwnership(const ChecklistSlug &slug, const ChecklistOwnership &ownership);
  void DeleteOwnedInstance(const std::string &checklist, const std::string &instance_id,
                           const ChecklistOwnership &ownership, int *deleted_slugs = nullptr);
  bool CreateSlugIfMissing(const ChecklistSlug& slug);
  void UpdateTemplateFieldsForSlug(const ChecklistSlug& slug);
  void DeleteChecklist(const std::string& checklist, int* deleted_slugs = nullptr);
  void DeleteInstance(const std::string& checklist, const std::string& instance_id,
                      int* deleted_slugs = nullptr);
  void ReplaceTemplateRelationships(const std::vector<TemplateRelationship>& relationships);
  void ReplaceTemplateRelationshipsForSubjects(const std::vector<std::string>& subject_slug_ids,
                                              const std::vector<TemplateRelationship>& relationships);
  std::vector<TemplateRelationship> GetTemplateRelationshipsForChecklist(
      const std::string& checklist) const;
  std::vector<TemplateRelationship>
  GetTemplateRelationshipsForChecklist(const std::string &checklist,
                                       const std::optional<ChecklistOwnership> &ownership_filter) const;
  std::vector<TemplateRelationship> ListTemplateRelationships(
      const std::optional<std::string>& subject_slug_id,
      const std::optional<std::string>& target_slug_id,
      const std::optional<std::string>& predicate,
      std::optional<int> limit,
      std::optional<int> offset) const;
  std::vector<AddressRelationship> ListAddressRelationships(
      const std::optional<std::string>& subject_address_id,
      const std::optional<std::string>& target_address_id,
      const std::optional<std::string>& predicate,
      std::optional<int> limit,
      std::optional<int> offset) const;
  void InsertTemplateRelationship(const TemplateRelationship& rel);
  void InsertAddressRelationship(const std::string& subject_address_id,
                                 const RelationshipEdge& edge);
  bool HasSlugForInstance(const std::string& slug_id, const std::string& instance_id) const;
  std::string EnsureEntityRecord(const std::string& principal, const std::string& kind,
                                 const std::string& display_name);
  std::vector<std::pair<std::string, std::string>> ListEntities(std::optional<int> limit,
                                                                std::optional<int> offset) const;
  std::string EnsureInstanceRecord(const std::string& principal, const std::string& label,
                                   const std::string& meta);
  std::vector<std::pair<std::string, std::string>> ListInstances(std::optional<int> limit,
                                                                 std::optional<int> offset) const;
  std::vector<ChecklistOwnership> ListOwnershipsForInstance(const std::string &checklist,
                                                            const std::string &instance_id) const;

private:
  void EnsureSchema();
  bool HasAnySlugs() const;
  void SeedDemoData();
  void UpsertSlugUnlocked(const ChecklistSlug& slug);
  std::string EnsureInstanceRecordUnlocked(const std::string& principal,
                                           const std::string& label,
                                           const std::string& meta);
  void InsertHistorySnapshot(const ChecklistSlug& slug);
  std::vector<RelationshipEdge> LoadOutgoingAddressEdges(const std::string& address_id) const;
  ChecklistSlug GetSlugOrThrowUnlocked(const std::string& address_id) const;

  sqlite3* db_ = nullptr;
  std::string db_path_;
  mutable std::mutex mutex_;
  int predicate_chain_depth_ = 1;
};

ChecklistStatus ParseStatus(const std::string& value);
std::string StatusToString(ChecklistStatus status);
bool IsValidBase32Id(const std::string& value, std::size_t expected_length);
std::string ComputeSlugId(const std::string& checklist, const std::string& section,
                          const std::string& procedure, const std::string& action,
                          const std::string& spec, const std::string& instructions);
std::string ComputeInstanceId(const std::string& instance_principal);
std::string ComputeEntityId(const std::string& entity_principal);
void SetEntitySalt(const std::string& salt);
std::string ComposeAddressId(const std::string& slug_id, const std::string& instance_id,
                             bool include_separator = false);
std::string CurrentTimestampIsoUtc();

}  // namespace core
