#include "core/checklist_store.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "core/asset_pack.hpp"
#include "core/logging.hpp"
#include "sqlite3.h"
#include "xxhash.h"

namespace {

using core::ChecklistOwnership;
using core::ChecklistSlug;
using core::ChecklistStatus;
using core::PredicateRecord;
using core::RelationshipEdge;
using core::RelationshipGraph;
using core::SlugUpdate;
using core::TemplateRelationship;
using core::VerifyEvaluation;
using core::logging::LogError;
using core::logging::LogInfo;
using core::logging::LogWarn;

namespace fs = std::filesystem;

constexpr char kBase32Alphabet[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";  // Crockford without I/L/O/U
constexpr std::string_view kAddressSeparator = "||";
constexpr std::string_view kDefaultEntityPrincipal = "system||chax-runtime";
constexpr std::string_view kDefaultEntityKind = "system";
constexpr std::string_view kDefaultEntityDisplay = "chax-runtime";
constexpr std::string_view kPredicateDaemonPrincipal = "system||chax-predicate-daemon";
constexpr std::string_view kPredicateDaemonKind = "system";
constexpr std::string_view kPredicateDaemonDisplay = "chax-predicate-daemon";
constexpr std::int64_t kOrderGap = 1000;
constexpr std::int64_t kSectionStride = 10000000;
std::string g_entity_salt = "dev-entity-salt";

int Prepare(sqlite3* db, const std::string& sql, sqlite3_stmt** stmt);
void Finalize(sqlite3_stmt* stmt);
void StepOrThrow(sqlite3_stmt* stmt, const std::string& context);

std::string Trim(const std::string& value) {
  std::size_t start = 0;
  std::size_t end = value.size();
  while (start < value.size() &&
         std::isspace(static_cast<unsigned char>(value[start])) != 0) {
    ++start;
  }
  while (end > start && std::isspace(static_cast<unsigned char>(value[end - 1])) != 0) {
    --end;
  }
  return value.substr(start, end - start);
}

std::string CollapseWhitespace(const std::string& value) {
  std::string output;
  output.reserve(value.size());
  bool in_space = false;
  for (unsigned char ch : value) {
    if (std::isspace(ch) != 0) {
      if (!in_space) {
        output.push_back(' ');
        in_space = true;
      }
    } else {
      output.push_back(static_cast<char>(ch));
      in_space = false;
    }
  }
  return output;
}

std::string ToLower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return value;
}

std::string CanonicalizeForHash(const std::string& value) {
  return ToLower(CollapseWhitespace(Trim(value)));
}

bool IsAllowedBase32Char(char ch) {
  return ch == '0' || ch == '1' || (ch >= '2' && ch <= '9') ||
         (ch >= 'A' && ch <= 'H') || ch == 'J' || ch == 'K' || ch == 'M' || ch == 'N' ||
         ch == 'P' || ch == 'Q' || ch == 'R' || ch == 'S' || ch == 'T' || ch == 'V' ||
         ch == 'W' || ch == 'X' || ch == 'Y' || ch == 'Z';
}

std::array<uint8_t, 10> HashTo80Bits(std::string_view data) {
  const XXH128_hash_t hash = XXH3_128bits(data.data(), data.size());
  XXH128_canonical_t hash_bytes;
  XXH128_canonicalFromHash(&hash_bytes, hash);
  std::array<uint8_t, 10> truncated{};
  std::copy(hash_bytes.digest, hash_bytes.digest + truncated.size(), truncated.begin());
  return truncated;
}

std::string EncodeBase32(const std::array<uint8_t, 10>& bytes) {
  std::string output;
  output.reserve(16);

  uint32_t buffer = 0;
  int bits = 0;

  for (const auto value : bytes) {
    buffer = (buffer << 8) | value;
    bits += 8;

    while (bits >= 5) {
      bits -= 5;
      const uint32_t index = (buffer >> bits) & 0x1Fu;
      output.push_back(kBase32Alphabet[index]);
    }
  }

  if (output.size() < 16) {
    while (output.size() < 16) {
      output.push_back(kBase32Alphabet[0]);
    }
  } else if (output.size() > 16) {
    output.resize(16);
  }

  return output;
}

std::string ColumnText(sqlite3_stmt* stmt, int column) {
  const unsigned char* text = sqlite3_column_text(stmt, column);
  if (!text) {
    return {};
  }
  return reinterpret_cast<const char*>(text);
}

void EnsureEntity(sqlite3* db, const std::string& entity_id, const std::string& principal,
                  const std::string& kind, const std::string& display_name) {
  sqlite3_stmt* stmt = nullptr;
  const char* sql =
      "INSERT OR IGNORE INTO entities (entity_id, principal, kind, display_name) "
      "VALUES (?,?,?,?);";
  if (Prepare(db, sql, &stmt) != SQLITE_OK) {
    Finalize(stmt);
    throw std::runtime_error("Failed to prepare entity insert");
  }
  sqlite3_bind_text(stmt, 1, entity_id.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 2, principal.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 3, kind.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 4, display_name.c_str(), -1, SQLITE_TRANSIENT);
  StepOrThrow(stmt, "entity insert");
  Finalize(stmt);
}

std::vector<std::string> TableColumns(sqlite3* db, const std::string& table) {
  sqlite3_stmt* stmt = nullptr;
  const std::string sql = "PRAGMA table_info(" + table + ");";
  if (Prepare(db, sql, &stmt) != SQLITE_OK) {
    Finalize(stmt);
    throw std::runtime_error("Failed to inspect table schema for " + table);
  }
  std::vector<std::string> names;
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    names.push_back(ColumnText(stmt, 1));
  }
  Finalize(stmt);
  return names;
}

bool HasColumn(const std::vector<std::string>& columns, const std::string& name) {
  return std::find(columns.begin(), columns.end(), name) != columns.end();
}

bool HasOwnershipFilter(const ChecklistOwnership &ownership) {
  return !ownership.source_name.empty() || !ownership.pack.empty() || !ownership.checklist_dir.empty() ||
         !ownership.checklist.empty();
}

void AppendOwnershipClauses(const ChecklistOwnership &ownership, const std::string &alias,
                            std::vector<std::string> *clauses) {
  if (!ownership.source_name.empty()) {
    clauses->push_back(alias + ".source_name=?");
  }
  if (!ownership.pack.empty()) {
    clauses->push_back(alias + ".pack=?");
  }
  if (!ownership.checklist_dir.empty()) {
    clauses->push_back(alias + ".checklist_dir=?");
  }
  if (!ownership.checklist.empty()) {
    clauses->push_back(alias + ".checklist=?");
  }
}

void BindOwnershipFilter(sqlite3_stmt *stmt, int *bind_index, const ChecklistOwnership &ownership) {
  if (!ownership.source_name.empty()) {
    sqlite3_bind_text(stmt, (*bind_index)++, ownership.source_name.c_str(), -1, SQLITE_TRANSIENT);
  }
  if (!ownership.pack.empty()) {
    sqlite3_bind_text(stmt, (*bind_index)++, ownership.pack.c_str(), -1, SQLITE_TRANSIENT);
  }
  if (!ownership.checklist_dir.empty()) {
    sqlite3_bind_text(stmt, (*bind_index)++, ownership.checklist_dir.c_str(), -1, SQLITE_TRANSIENT);
  }
  if (!ownership.checklist.empty()) {
    sqlite3_bind_text(stmt, (*bind_index)++, ownership.checklist.c_str(), -1, SQLITE_TRANSIENT);
  }
}

ChecklistOwnership NormalizeOwnershipForWrite(const ChecklistOwnership &ownership, const ChecklistSlug &slug) {
  ChecklistOwnership normalized = ownership;
  normalized.source_name = Trim(normalized.source_name);
  if (normalized.source_name.empty()) {
    normalized.source_name = "public";
  }
  normalized.pack = Trim(normalized.pack);
  normalized.checklist_dir = Trim(normalized.checklist_dir);
  normalized.checklist = Trim(normalized.checklist);
  if (normalized.checklist.empty()) {
    normalized.checklist = slug.checklist;
  }
  if (!core::IsSafePackToken(normalized.source_name) || !core::IsSafePackToken(normalized.pack) ||
      !core::IsSafeChecklistToken(normalized.checklist_dir)) {
    throw std::invalid_argument("Checklist ownership source, pack, and checklist_dir must be plain tokens.");
  }
  if (normalized.pack.empty() || normalized.checklist_dir.empty() || normalized.checklist.empty()) {
    throw std::invalid_argument("Checklist ownership requires pack, checklist_dir, and checklist.");
  }
  return normalized;
}

ChecklistSlug BuildSlug(sqlite3_stmt* stmt) {
  ChecklistSlug slug;
  slug.address_id = ColumnText(stmt, 0);
  slug.slug_id = ColumnText(stmt, 1);
  slug.instance_id = ColumnText(stmt, 2);
  slug.instance_principal = ColumnText(stmt, 3);
  slug.checklist = ColumnText(stmt, 4);
  slug.section = ColumnText(stmt, 5);
  slug.procedure = ColumnText(stmt, 6);
  slug.action = ColumnText(stmt, 7);
  slug.spec = ColumnText(stmt, 8);
  slug.instructions = ColumnText(stmt, 9);
  slug.result = ColumnText(stmt, 10);
  slug.status = core::ParseStatus(ColumnText(stmt, 11));
  slug.comment = ColumnText(stmt, 12);
  slug.timestamp = ColumnText(stmt, 13);
  slug.entity_id = ColumnText(stmt, 14);
  slug.address_order = sqlite3_column_int64(stmt, 15);
  slug.entity_principal.clear();
  return slug;
}

struct SectionOrderInfo {
  std::int64_t section_base = 0;
  std::int64_t max_order = 0;
  std::int64_t count = 0;
};

std::optional<std::int64_t> LookupAddressOrder(sqlite3* db, const std::string& address_id) {
  sqlite3_stmt* stmt = nullptr;
  if (Prepare(db, "SELECT address_order FROM slug_order WHERE address_id=?;", &stmt) != SQLITE_OK) {
    Finalize(stmt);
    throw std::runtime_error("Failed to prepare slug_order lookup");
  }
  sqlite3_bind_text(stmt, 1, address_id.c_str(), -1, SQLITE_TRANSIENT);
  if (sqlite3_step(stmt) != SQLITE_ROW) {
    Finalize(stmt);
    return std::nullopt;
  }
  const std::int64_t value = sqlite3_column_int64(stmt, 0);
  Finalize(stmt);
  return value;
}

std::optional<std::int64_t> LookupTemplateOrder(sqlite3* db, const std::string& slug_id) {
  sqlite3_stmt* stmt = nullptr;
  const std::string template_instance_id = core::ComputeInstanceId("template||default");
  const std::string sql =
      "SELECT so.address_order FROM slugs s "
      "JOIN slug_order so ON s.address_id = so.address_id "
      "WHERE s.slug_id=? AND s.instance_id=? LIMIT 1;";
  if (Prepare(db, sql, &stmt) != SQLITE_OK) {
    Finalize(stmt);
    throw std::runtime_error("Failed to prepare template order lookup");
  }
  sqlite3_bind_text(stmt, 1, slug_id.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 2, template_instance_id.c_str(), -1, SQLITE_TRANSIENT);
  if (sqlite3_step(stmt) != SQLITE_ROW) {
    Finalize(stmt);
    return std::nullopt;
  }
  const std::int64_t value = sqlite3_column_int64(stmt, 0);
  Finalize(stmt);
  return value;
}

std::vector<TemplateRelationship> LoadTemplateRelationshipsForChecklist(
    sqlite3* db,
    const std::string& checklist) {
  std::vector<TemplateRelationship> edges;
  sqlite3_stmt* stmt = nullptr;
  const std::string sql =
      "SELECT tr.subject_slug_id, tr.predicate, tr.target_slug_id "
      "FROM template_relationships tr "
      "JOIN slugs s ON tr.subject_slug_id = s.slug_id "
      "WHERE s.checklist=?;";
  if (Prepare(db, sql, &stmt) != SQLITE_OK) {
    Finalize(stmt);
    throw std::runtime_error("Failed to prepare template relationship lookup");
  }
  sqlite3_bind_text(stmt, 1, checklist.c_str(), -1, SQLITE_TRANSIENT);

  while (sqlite3_step(stmt) == SQLITE_ROW) {
    TemplateRelationship rel;
    rel.subject_slug_id = ColumnText(stmt, 0);
    rel.predicate = ColumnText(stmt, 1);
    rel.target_slug_id = ColumnText(stmt, 2);
    edges.push_back(std::move(rel));
  }
  Finalize(stmt);
  return edges;
}

std::unordered_set<std::string> LoadAddressRelationshipKeys(sqlite3* db,
                                                            const std::string& subject_address_id) {
  std::unordered_set<std::string> keys;
  sqlite3_stmt* stmt = nullptr;
  if (Prepare(db, "SELECT predicate, target_address_id FROM address_relationships "
                  "WHERE subject_address_id=?;",
              &stmt) != SQLITE_OK) {
    Finalize(stmt);
    throw std::runtime_error("Failed to prepare address relationship lookup");
  }
  sqlite3_bind_text(stmt, 1, subject_address_id.c_str(), -1, SQLITE_TRANSIENT);
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    const std::string predicate = ColumnText(stmt, 0);
    const std::string target = ColumnText(stmt, 1);
    keys.insert(predicate + "|" + target);
  }
  Finalize(stmt);
  return keys;
}

bool HasSlugForInstanceUnlocked(sqlite3* db, const std::string& slug_id,
                                const std::string& instance_id) {
  sqlite3_stmt* stmt = nullptr;
  if (Prepare(db, "SELECT 1 FROM slugs WHERE slug_id=? AND instance_id=? LIMIT 1;", &stmt) !=
      SQLITE_OK) {
    Finalize(stmt);
    throw std::runtime_error("Failed to prepare slug existence lookup");
  }
  sqlite3_bind_text(stmt, 1, slug_id.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 2, instance_id.c_str(), -1, SQLITE_TRANSIENT);
  const bool exists = sqlite3_step(stmt) == SQLITE_ROW;
  Finalize(stmt);
  return exists;
}

std::optional<SectionOrderInfo> LookupSectionOrderInfo(sqlite3* db,
                                                       const std::string& instance_id,
                                                       const std::string& section) {
  sqlite3_stmt* stmt = nullptr;
  const std::string sql =
      "SELECT MIN(so.address_order), MAX(so.address_order), COUNT(*) "
      "FROM slugs s JOIN slug_order so ON s.address_id = so.address_id "
      "WHERE s.instance_id=? AND s.section=?;";
  if (Prepare(db, sql, &stmt) != SQLITE_OK) {
    Finalize(stmt);
    throw std::runtime_error("Failed to prepare slug_order section lookup");
  }
  sqlite3_bind_text(stmt, 1, instance_id.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 2, section.c_str(), -1, SQLITE_TRANSIENT);
  if (sqlite3_step(stmt) != SQLITE_ROW) {
    Finalize(stmt);
    return std::nullopt;
  }
  const std::int64_t count = sqlite3_column_int64(stmt, 2);
  if (count <= 0) {
    Finalize(stmt);
    return std::nullopt;
  }
  const std::int64_t min_order = sqlite3_column_int64(stmt, 0);
  const std::int64_t max_order = sqlite3_column_int64(stmt, 1);
  Finalize(stmt);
  const std::int64_t section_base = (min_order / kSectionStride) * kSectionStride;
  return SectionOrderInfo{section_base, max_order, count};
}

std::int64_t LookupMaxOrderForInstance(sqlite3* db, const std::string& instance_id) {
  sqlite3_stmt* stmt = nullptr;
  const std::string sql =
      "SELECT MAX(so.address_order) FROM slugs s "
      "JOIN slug_order so ON s.address_id = so.address_id "
      "WHERE s.instance_id=?;";
  if (Prepare(db, sql, &stmt) != SQLITE_OK) {
    Finalize(stmt);
    throw std::runtime_error("Failed to prepare slug_order max lookup");
  }
  sqlite3_bind_text(stmt, 1, instance_id.c_str(), -1, SQLITE_TRANSIENT);
  if (sqlite3_step(stmt) != SQLITE_ROW) {
    Finalize(stmt);
    return 0;
  }
  if (sqlite3_column_type(stmt, 0) == SQLITE_NULL) {
    Finalize(stmt);
    return 0;
  }
  const std::int64_t value = sqlite3_column_int64(stmt, 0);
  Finalize(stmt);
  return value;
}

void UpsertSlugOrder(sqlite3* db, const std::string& address_id, std::int64_t order) {
  sqlite3_stmt* stmt = nullptr;
  const std::string sql =
      "INSERT INTO slug_order (address_id, address_order) VALUES (?, ?) "
      "ON CONFLICT(address_id) DO UPDATE SET address_order=excluded.address_order;";
  if (Prepare(db, sql, &stmt) != SQLITE_OK) {
    Finalize(stmt);
    throw std::runtime_error("Failed to prepare slug_order upsert");
  }
  sqlite3_bind_text(stmt, 1, address_id.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(stmt, 2, order);
  StepOrThrow(stmt, "slug_order upsert");
  Finalize(stmt);
}

void InheritTemplateOwnership(sqlite3 *db, const ChecklistSlug &slug) {
  const std::string template_instance_id = core::ComputeInstanceId("template||default");
  if (slug.instance_id == template_instance_id || slug.slug_id.empty() || slug.address_id.empty()) {
    return;
  }

  sqlite3_stmt *stmt = nullptr;
  const std::string template_address = core::ComposeAddressId(slug.slug_id, template_instance_id);
  const std::string timestamp = core::CurrentTimestampIsoUtc();
  const std::string sql = "INSERT INTO address_ownership (address_id, slug_id, instance_id, checklist, "
                          "source_name, source_path, pack, checklist_dir, updated_at) "
                          "SELECT ?, ?, ?, ao.checklist, ao.source_name, ao.source_path, ao.pack, "
                          "ao.checklist_dir, ? "
                          "FROM address_ownership ao "
                          "WHERE ao.address_id=? AND ao.slug_id=? AND ao.checklist=? "
                          "ON CONFLICT(address_id, source_name, pack, checklist_dir) DO UPDATE SET "
                          "slug_id=excluded.slug_id, instance_id=excluded.instance_id, "
                          "checklist=excluded.checklist, source_path=excluded.source_path, "
                          "updated_at=excluded.updated_at;";
  if (Prepare(db, sql, &stmt) != SQLITE_OK) {
    Finalize(stmt);
    throw std::runtime_error("Failed to prepare address ownership inheritance");
  }
  sqlite3_bind_text(stmt, 1, slug.address_id.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 2, slug.slug_id.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 3, slug.instance_id.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 4, timestamp.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 5, template_address.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 6, slug.slug_id.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 7, slug.checklist.c_str(), -1, SQLITE_TRANSIENT);
  StepOrThrow(stmt, "address ownership inheritance");
  Finalize(stmt);
}

void RepackSectionOrders(sqlite3* db,
                         const std::string& instance_id,
                         const std::string& section,
                         std::int64_t section_base,
                         std::int64_t gap) {
  sqlite3_stmt* stmt = nullptr;
  const std::string sql =
      "SELECT s.address_id, so.address_order "
      "FROM slugs s JOIN slug_order so ON s.address_id = so.address_id "
      "WHERE s.instance_id=? AND s.section=? "
      "ORDER BY so.address_order, s.procedure, s.action, s.address_id;";
  if (Prepare(db, sql, &stmt) != SQLITE_OK) {
    Finalize(stmt);
    throw std::runtime_error("Failed to prepare slug_order repack query");
  }
  sqlite3_bind_text(stmt, 1, instance_id.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 2, section.c_str(), -1, SQLITE_TRANSIENT);

  std::vector<std::string> address_ids;
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    address_ids.emplace_back(ColumnText(stmt, 0));
  }
  Finalize(stmt);

  if (address_ids.empty()) {
    return;
  }

  for (std::size_t i = 0; i < address_ids.size(); ++i) {
    const std::int64_t order = section_base + gap * static_cast<std::int64_t>(i + 1);
    UpsertSlugOrder(db, address_ids[i], order);
  }
}

std::int64_t ResolveSectionOrder(sqlite3* db,
                                 const std::string& instance_id,
                                 const std::string& section) {
  const auto info = LookupSectionOrderInfo(db, instance_id, section);
  if (!info) {
    const std::int64_t max_order = LookupMaxOrderForInstance(db, instance_id);
    const std::int64_t section_index =
        max_order > 0 ? (max_order / kSectionStride) + 1 : 0;
    const std::int64_t section_base = section_index * kSectionStride;
    return section_base + kOrderGap;
  }

  const std::int64_t section_base = info->section_base;
  std::int64_t gap = kOrderGap;
  std::int64_t next_order = info->max_order + gap;
  if (next_order >= section_base + kSectionStride) {
    gap = std::max<std::int64_t>(1, kSectionStride / (info->count + 1));
    RepackSectionOrders(db, instance_id, section, section_base, gap);
    next_order = section_base + gap * (info->count + 1);
    if (next_order >= section_base + kSectionStride) {
      LogError("slug_order repack exhausted section stride for section '" + section +
               "' instance '" + instance_id + "'");
    }
  }
  return next_order;
}

std::int64_t ResolveAddressOrder(sqlite3* db,
                                 const ChecklistSlug& slug,
                                 const std::optional<std::int64_t>& existing_order) {
  if (slug.address_order > 0) {
    return slug.address_order;
  }
  if (existing_order && *existing_order > 0) {
    return *existing_order;
  }
  const auto template_order = LookupTemplateOrder(db, slug.slug_id);
  if (template_order && *template_order > 0) {
    return *template_order;
  }
  return ResolveSectionOrder(db, slug.instance_id, slug.section);
}

void BackfillSlugOrder(sqlite3* db) {
  sqlite3_stmt* stmt = nullptr;
  if (Prepare(db, "SELECT COUNT(*) FROM slug_order;", &stmt) != SQLITE_OK) {
    Finalize(stmt);
    throw std::runtime_error("Failed to prepare slug_order count query");
  }
  if (sqlite3_step(stmt) != SQLITE_ROW) {
    Finalize(stmt);
    return;
  }
  const std::int64_t order_count = sqlite3_column_int64(stmt, 0);
  Finalize(stmt);

  if (order_count == 0) {
    const std::string sql =
        "SELECT s.address_id, s.checklist, s.instance_id, s.section "
        "FROM slugs s "
        "ORDER BY s.checklist, s.instance_id, s.section, s.procedure, s.action;";
    if (Prepare(db, sql, &stmt) != SQLITE_OK) {
      Finalize(stmt);
      throw std::runtime_error("Failed to prepare slug_order backfill query");
    }

    std::string current_checklist;
    std::string current_instance;
    std::unordered_map<std::string, std::int64_t> section_index;
    std::unordered_map<std::string, std::int64_t> row_counts;
    std::int64_t next_section_index = 0;

    while (sqlite3_step(stmt) == SQLITE_ROW) {
      const std::string address_id = ColumnText(stmt, 0);
      const std::string checklist = ColumnText(stmt, 1);
      const std::string instance_id = ColumnText(stmt, 2);
      const std::string section = ColumnText(stmt, 3);
      if (checklist != current_checklist || instance_id != current_instance) {
        current_checklist = checklist;
        current_instance = instance_id;
        section_index.clear();
        row_counts.clear();
        next_section_index = 0;
      }

      auto it = section_index.find(section);
      if (it == section_index.end()) {
        section_index[section] = next_section_index++;
        row_counts[section] = 0;
        it = section_index.find(section);
      }

      std::int64_t& row_index = row_counts[section];
      const std::int64_t order =
          it->second * kSectionStride + (row_index + 1) * kOrderGap;
      ++row_index;

      UpsertSlugOrder(db, address_id, order);
    }
    Finalize(stmt);
    return;
  }

  const std::string missing_sql =
      "SELECT s.address_id, s.instance_id, s.section, s.slug_id "
      "FROM slugs s LEFT JOIN slug_order so ON s.address_id = so.address_id "
      "WHERE so.address_id IS NULL "
      "ORDER BY s.checklist, s.instance_id, s.section, s.procedure, s.action;";
  if (Prepare(db, missing_sql, &stmt) != SQLITE_OK) {
    Finalize(stmt);
    throw std::runtime_error("Failed to prepare slug_order missing backfill query");
  }
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    ChecklistSlug slug;
    slug.address_id = ColumnText(stmt, 0);
    slug.instance_id = ColumnText(stmt, 1);
    slug.section = ColumnText(stmt, 2);
    slug.slug_id = ColumnText(stmt, 3);
    const std::int64_t order = ResolveAddressOrder(db, slug, std::nullopt);
    UpsertSlugOrder(db, slug.address_id, order);
  }
  Finalize(stmt);
}

core::HistoryEntry BuildHistory(sqlite3_stmt* stmt) {
  core::HistoryEntry entry;
  entry.address_id = ColumnText(stmt, 0);
  entry.timestamp = ColumnText(stmt, 1);
  entry.result = ColumnText(stmt, 2);
  entry.status = core::ParseStatus(ColumnText(stmt, 3));
  entry.comment = ColumnText(stmt, 4);
  entry.entity_id = ColumnText(stmt, 5);
  return entry;
}

void Finalize(sqlite3_stmt* stmt) {
  if (stmt) {
    sqlite3_finalize(stmt);
  }
}

int Prepare(sqlite3* db, const std::string& sql, sqlite3_stmt** stmt) {
  return sqlite3_prepare_v2(db, sql.c_str(), -1, stmt, nullptr);
}

void StepOrThrow(sqlite3_stmt* stmt, const std::string& context) {
  const int rc = sqlite3_step(stmt);
  if (rc != SQLITE_DONE && rc != SQLITE_ROW) {
    throw std::runtime_error(context + " failed: " + std::string(sqlite3_errstr(rc)));
  }
}

std::string DefaultEntityId() {
  return core::ComputeEntityId(std::string{kDefaultEntityPrincipal});
}

struct CanonicalPredicateParts {
  std::string_view subject_state;
  std::string_view relation;
  std::string_view type;
  std::string_view object_state;
};

struct CanonicalSubjectToken {
  std::string_view token;
  std::string_view normalized_state;
};

struct CanonicalObjectToken {
  std::string_view token;
  std::string_view normalized_state;
};

enum class CanonicalGateMode {
  kNone,
  kAnd,
  kOr,
};

bool StartsWith(std::string_view value, std::string_view prefix) {
  return value.size() >= prefix.size() && value.substr(0, prefix.size()) == prefix;
}

std::optional<CanonicalPredicateParts> ParseCanonicalPredicateToken(std::string_view token) {
  constexpr CanonicalSubjectToken kSubjectStates[] = {
      {"pass", "pass"},
      {"Pass", "pass"},
      {"fail", "fail"},
      {"Fail", "fail"},
      {"other", "other"},
      {"Other", "other"},
      {"na", "na"},
      {"Na", "na"},
      {"NA", "na"},
  };
  constexpr std::string_view kRelations[] = {"Propagate", "Sync", "Verify"};
  constexpr std::string_view kTypes[] = {"Validated", "Implied", "Assumed", "AndGate", "OrGate"};
  constexpr CanonicalObjectToken kObjectStates[] = {
      {"Pass", "Pass"},
      {"Fail", "Fail"},
      {"Other", "Other"},
      {"Na", "Na"},
      {"NA", "Na"},
  };

  for (const auto& subject_state : kSubjectStates) {
    if (!StartsWith(token, subject_state.token)) continue;
    std::size_t pos = subject_state.token.size();
    for (const auto& relation : kRelations) {
      if (!StartsWith(token.substr(pos), relation)) continue;
      pos += relation.size();
      for (const auto& type : kTypes) {
        if (!StartsWith(token.substr(pos), type)) continue;
        pos += type.size();
        for (const auto& object_state : kObjectStates) {
          if (token.substr(pos) != object_state.token) continue;
          return CanonicalPredicateParts{subject_state.normalized_state, relation, type,
                                         object_state.normalized_state};
        }
        pos -= type.size();
      }
      pos -= relation.size();
    }
  }
  return std::nullopt;
}

std::optional<ChecklistStatus> SubjectStateToStatus(std::string_view subject_state) {
  if (subject_state == "pass") return ChecklistStatus::kPass;
  if (subject_state == "Pass") return ChecklistStatus::kPass;
  if (subject_state == "fail") return ChecklistStatus::kFail;
  if (subject_state == "Fail") return ChecklistStatus::kFail;
  if (subject_state == "other") return ChecklistStatus::kOther;
  if (subject_state == "Other") return ChecklistStatus::kOther;
  if (subject_state == "na") return ChecklistStatus::kNA;
  if (subject_state == "Na") return ChecklistStatus::kNA;
  if (subject_state == "NA") return ChecklistStatus::kNA;
  return std::nullopt;
}

std::optional<ChecklistStatus> ObjectStateToStatus(std::string_view object_state) {
  if (object_state == "Pass") return ChecklistStatus::kPass;
  if (object_state == "Fail") return ChecklistStatus::kFail;
  if (object_state == "Other") return ChecklistStatus::kOther;
  if (object_state == "Na") return ChecklistStatus::kNA;
  if (object_state == "NA") return ChecklistStatus::kNA;
  return std::nullopt;
}

CanonicalGateMode ParseCanonicalGateMode(std::string_view type) {
  if (type == "AndGate") {
    return CanonicalGateMode::kAnd;
  }
  if (type == "OrGate") {
    return CanonicalGateMode::kOr;
  }
  return CanonicalGateMode::kNone;
}

enum class VerifyBoolState {
  kTrue,
  kFalse,
  kIndeterminate,
};

enum class VerifyObjectKind {
  kStatusBridge,
  kCommentBridge,
  kStatusLiteral,
};

struct VerifyPredicateParts {
  std::string_view type;
  VerifyObjectKind object_kind = VerifyObjectKind::kStatusBridge;
  ChecklistStatus object_status = ChecklistStatus::kUnknown;
  std::string_view object_token;
};

struct VerifyComputation {
  VerifyBoolState state = VerifyBoolState::kIndeterminate;
  std::string reason_code;
  std::string reason;
};

struct VerifyGateComputation {
  VerifyBoolState state = VerifyBoolState::kIndeterminate;
  std::string reason_code;
  std::string reason;
  int contributor_count = 0;
  int contributor_true_count = 0;
};

struct ParsedScalar {
  double value = 0.0;
  std::string unit;
};

enum class ScalarUnitDimension {
  kUnknown,
  kPressure,
  kLength,
  kCurrent,
  kVoltage,
  kTime,
  kPower,
  kFlowRate,
  kDoseRate,
  kCurrentPerSolidAngle,
  kChargeHour,
  kChargePerArea,
  kPercent,
};

struct VerifyWritePlan {
  bool write_status = false;
  ChecklistStatus status = ChecklistStatus::kUnknown;
  bool write_comment = false;
  std::string comment;
  std::string write_decision;
};

std::string VerifyBoolToLower(VerifyBoolState state) {
  switch (state) {
    case VerifyBoolState::kTrue:
      return "true";
    case VerifyBoolState::kFalse:
      return "false";
    case VerifyBoolState::kIndeterminate:
      return "indeterminate";
  }
  return "indeterminate";
}

std::string VerifyBoolToUpper(VerifyBoolState state) {
  switch (state) {
    case VerifyBoolState::kTrue:
      return "TRUE";
    case VerifyBoolState::kFalse:
      return "FALSE";
    case VerifyBoolState::kIndeterminate:
      return "INDETERMINATE";
  }
  return "INDETERMINATE";
}

bool IsAllowedUnitChar(char ch) {
  const unsigned char byte = static_cast<unsigned char>(ch);
  return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ||
         ch == '/' || ch == '_' || ch == '-' || ch == '^' || ch == '%' || byte == 0xC2 ||
         byte == 0xB5 || byte == 0xCE || byte == 0xBC;
}

constexpr double kVerifyExactEpsilon = 1e-9;
constexpr double kVerifyPrecisionToleranceFactor = 0.25;

void ReplaceAll(std::string* value, std::string_view from, std::string_view to) {
  if (!value || from.empty()) return;
  std::size_t pos = 0;
  while ((pos = value->find(from, pos)) != std::string::npos) {
    value->replace(pos, from.size(), to);
    pos += to.size();
  }
}

std::string NormalizeUnitToken(std::string unit) {
  unit = Trim(unit);
  ReplaceAll(&unit, "\xC2\xB5", "u");  // U+00B5 MICRO SIGN
  ReplaceAll(&unit, "\xCE\xBC", "u");  // U+03BC GREEK SMALL LETTER MU
  return unit;
}

struct ScalarUnitScale {
  std::string_view token;
  std::string_view canonical;
  ScalarUnitDimension dimension;
  double factor_to_canonical;
  bool case_sensitive = true;
};

constexpr std::array<ScalarUnitScale, 46> kScalarUnitScales = {{
    {"Pa", "Pa", ScalarUnitDimension::kPressure, 1.0, true},
    {"kPa", "Pa", ScalarUnitDimension::kPressure, 1000.0, true},
    {"MPa", "Pa", ScalarUnitDimension::kPressure, 1000000.0, true},
    {"bar", "Pa", ScalarUnitDimension::kPressure, 100000.0, true},
    {"mbar", "Pa", ScalarUnitDimension::kPressure, 100.0, true},
    {"psi", "Pa", ScalarUnitDimension::kPressure, 6894.757293168, true},
    {"atm", "Pa", ScalarUnitDimension::kPressure, 101325.0, true},
    {"torr", "Pa", ScalarUnitDimension::kPressure, 133.32236842105263, true},
    {"mmHg", "Pa", ScalarUnitDimension::kPressure, 133.322387415, true},

    {"m", "m", ScalarUnitDimension::kLength, 1.0, true},
    {"cm", "m", ScalarUnitDimension::kLength, 0.01, true},
    {"mm", "m", ScalarUnitDimension::kLength, 0.001, true},
    {"um", "m", ScalarUnitDimension::kLength, 0.000001, true},
    {"nm", "m", ScalarUnitDimension::kLength, 0.000000001, true},
    {"pm", "m", ScalarUnitDimension::kLength, 0.000000000001, true},
    {"in", "m", ScalarUnitDimension::kLength, 0.0254, true},
    {"ft", "m", ScalarUnitDimension::kLength, 0.3048, true},

    {"A", "A", ScalarUnitDimension::kCurrent, 1.0, true},
    {"mA", "A", ScalarUnitDimension::kCurrent, 0.001, true},
    {"uA", "A", ScalarUnitDimension::kCurrent, 0.000001, true},
    {"nA", "A", ScalarUnitDimension::kCurrent, 0.000000001, true},
    {"pA", "A", ScalarUnitDimension::kCurrent, 0.000000000001, true},

    {"V", "V", ScalarUnitDimension::kVoltage, 1.0, true},
    {"mV", "V", ScalarUnitDimension::kVoltage, 0.001, true},
    {"kV", "V", ScalarUnitDimension::kVoltage, 1000.0, true},

    {"ns", "s", ScalarUnitDimension::kTime, 0.000000001, true},
    {"us", "s", ScalarUnitDimension::kTime, 0.000001, true},
    {"ms", "s", ScalarUnitDimension::kTime, 0.001, true},
    {"s", "s", ScalarUnitDimension::kTime, 1.0, true},
    {"min", "s", ScalarUnitDimension::kTime, 60.0, true},
    {"h", "s", ScalarUnitDimension::kTime, 3600.0, true},

    {"W", "W", ScalarUnitDimension::kPower, 1.0, true},
    {"mW", "W", ScalarUnitDimension::kPower, 0.001, true},
    {"kW", "W", ScalarUnitDimension::kPower, 1000.0, true},

    {"L/h", "L/h", ScalarUnitDimension::kFlowRate, 1.0, true},
    {"L/min", "L/h", ScalarUnitDimension::kFlowRate, 60.0, true},

    {"uSv/h", "Sv/h", ScalarUnitDimension::kDoseRate, 0.000001, true},
    {"mSv/h", "Sv/h", ScalarUnitDimension::kDoseRate, 0.001, true},
    {"Sv/h", "Sv/h", ScalarUnitDimension::kDoseRate, 1.0, true},

    {"uA/sr", "A/sr", ScalarUnitDimension::kCurrentPerSolidAngle, 0.000001, true},
    {"mA/sr", "A/sr", ScalarUnitDimension::kCurrentPerSolidAngle, 0.001, true},
    {"A/sr", "A/sr", ScalarUnitDimension::kCurrentPerSolidAngle, 1.0, true},

    {"uAh", "Ah", ScalarUnitDimension::kChargeHour, 0.000001, true},
    {"mAh", "Ah", ScalarUnitDimension::kChargeHour, 0.001, true},
    {"Ah", "Ah", ScalarUnitDimension::kChargeHour, 1.0, true},
    {"mC/cm", "C/cm", ScalarUnitDimension::kChargePerArea, 0.001, true},
}};

constexpr std::array<ScalarUnitScale, 16> kScalarUnitScalesCaseInsensitive = {{
    {"pct", "%", ScalarUnitDimension::kPercent, 1.0, false},
    {"%", "%", ScalarUnitDimension::kPercent, 1.0, false},
    {"bar", "Pa", ScalarUnitDimension::kPressure, 100000.0, false},
    {"mbar", "Pa", ScalarUnitDimension::kPressure, 100.0, false},
    {"psi", "Pa", ScalarUnitDimension::kPressure, 6894.757293168, false},
    {"atm", "Pa", ScalarUnitDimension::kPressure, 101325.0, false},
    {"torr", "Pa", ScalarUnitDimension::kPressure, 133.32236842105263, false},
    {"l/h", "L/h", ScalarUnitDimension::kFlowRate, 1.0, false},
    {"lph", "L/h", ScalarUnitDimension::kFlowRate, 1.0, false},
    {"l/min", "L/h", ScalarUnitDimension::kFlowRate, 60.0, false},
    {"lpm", "L/h", ScalarUnitDimension::kFlowRate, 60.0, false},
    {"g/h", "L/h", ScalarUnitDimension::kFlowRate, 3.785411784, false},
    {"gph", "L/h", ScalarUnitDimension::kFlowRate, 3.785411784, false},
    {"gal/h", "L/h", ScalarUnitDimension::kFlowRate, 3.785411784, false},
    {"gpm", "L/h", ScalarUnitDimension::kFlowRate, 227.12470704, false},
    {"gal/min", "L/h", ScalarUnitDimension::kFlowRate, 227.12470704, false},
}};

bool ResolveScalarUnitScale(const std::string& normalized_unit, ScalarUnitScale* out) {
  if (!out) return false;
  for (const auto& scale : kScalarUnitScales) {
    if (normalized_unit == scale.token) {
      *out = scale;
      return true;
    }
  }
  const std::string lowered = ToLower(normalized_unit);
  for (const auto& scale : kScalarUnitScalesCaseInsensitive) {
    if (lowered == ToLower(std::string(scale.token))) {
      *out = scale;
      return true;
    }
  }
  return false;
}

bool NormalizeScalarToCanonical(const ParsedScalar& input, ParsedScalar* output) {
  if (!output) return false;
  output->value = input.value;
  output->unit = NormalizeUnitToken(input.unit);
  if (output->unit.empty()) {
    return true;
  }
  ScalarUnitScale scale;
  if (!ResolveScalarUnitScale(output->unit, &scale)) {
    return false;
  }
  output->value = input.value * scale.factor_to_canonical;
  output->unit = std::string(scale.canonical);
  return true;
}

bool NormalizeScalarPairForCompare(const ParsedScalar& lhs,
                                   const ParsedScalar& rhs,
                                   ParsedScalar* lhs_out,
                                   ParsedScalar* rhs_out) {
  if (!lhs_out || !rhs_out) return false;
  ParsedScalar lhs_adjusted = lhs;
  ParsedScalar rhs_adjusted = rhs;
  lhs_adjusted.unit = NormalizeUnitToken(lhs_adjusted.unit);
  rhs_adjusted.unit = NormalizeUnitToken(rhs_adjusted.unit);
  if (lhs_adjusted.unit.empty() && rhs_adjusted.unit.empty()) {
    *lhs_out = lhs;
    *rhs_out = rhs;
    lhs_out->unit.clear();
    rhs_out->unit.clear();
    return true;
  }
  if (lhs_adjusted.unit.empty()) {
    lhs_adjusted.unit = rhs_adjusted.unit;
  } else if (rhs_adjusted.unit.empty()) {
    rhs_adjusted.unit = lhs_adjusted.unit;
  }
  if (lhs_adjusted.unit.empty() || rhs_adjusted.unit.empty()) {
    return false;
  }
  if (lhs_adjusted.unit == rhs_adjusted.unit) {
    *lhs_out = lhs_adjusted;
    *rhs_out = rhs_adjusted;
    return true;
  }
  ParsedScalar lhs_normalized;
  ParsedScalar rhs_normalized;
  if (!NormalizeScalarToCanonical(lhs_adjusted, &lhs_normalized) ||
      !NormalizeScalarToCanonical(rhs_adjusted, &rhs_normalized)) {
    return false;
  }
  if (lhs_normalized.unit != rhs_normalized.unit) {
    return false;
  }
  *lhs_out = lhs_normalized;
  *rhs_out = rhs_normalized;
  return true;
}

bool ParseScalarToken(const std::string& token, ParsedScalar* out) {
  if (!out) return false;
  const std::string trimmed = Trim(token);
  if (trimmed.empty()) {
    return false;
  }
  char* end_ptr = nullptr;
  const double parsed = std::strtod(trimmed.c_str(), &end_ptr);
  if (end_ptr == trimmed.c_str()) {
    return false;
  }
  if (!std::isfinite(parsed)) {
    return false;
  }
  std::string suffix = Trim(trimmed.substr(static_cast<std::size_t>(end_ptr - trimmed.c_str())));
  for (char ch : suffix) {
    if (!IsAllowedUnitChar(ch)) {
      return false;
    }
  }
  out->value = parsed;
  out->unit = NormalizeUnitToken(suffix);
  return true;
}

std::optional<double> ParseScalarResolutionStep(const std::string& token) {
  const std::string trimmed = Trim(token);
  if (trimmed.empty()) {
    return std::nullopt;
  }
  char* end_ptr = nullptr;
  const double parsed = std::strtod(trimmed.c_str(), &end_ptr);
  if (end_ptr == trimmed.c_str() || !std::isfinite(parsed)) {
    return std::nullopt;
  }
  const std::size_t lexeme_len = static_cast<std::size_t>(end_ptr - trimmed.c_str());
  if (lexeme_len == 0 || lexeme_len > trimmed.size()) {
    return std::nullopt;
  }
  std::string lexeme = trimmed.substr(0, lexeme_len);
  int exponent10 = 0;
  bool has_exponent = false;
  const auto exponent_pos = lexeme.find_first_of("eE");
  if (exponent_pos != std::string::npos) {
    has_exponent = true;
    std::string exponent_token = lexeme.substr(exponent_pos + 1);
    lexeme = lexeme.substr(0, exponent_pos);
    if (exponent_token.empty()) {
      return std::nullopt;
    }
    char* exp_end = nullptr;
    const long parsed_exponent = std::strtol(exponent_token.c_str(), &exp_end, 10);
    if (exp_end == exponent_token.c_str() || *exp_end != '\0') {
      return std::nullopt;
    }
    exponent10 = static_cast<int>(parsed_exponent);
  }
  int fractional_digits = 0;
  const auto dot = lexeme.find('.');
  if (dot != std::string::npos) {
    for (std::size_t idx = dot + 1; idx < lexeme.size(); ++idx) {
      const unsigned char ch = static_cast<unsigned char>(lexeme[idx]);
      if (std::isdigit(ch) != 0) {
        ++fractional_digits;
      }
    }
  }
  if (fractional_digits == 0 && !has_exponent) {
    return std::nullopt;
  }
  const int scale_exponent = exponent10 - fractional_digits;
  return std::pow(10.0, static_cast<double>(scale_exponent));
}

double ResolveScalarEqualityTolerance(const std::string& spec_scalar_token) {
  double tolerance = kVerifyExactEpsilon;
  const std::optional<double> step = ParseScalarResolutionStep(spec_scalar_token);
  if (step.has_value() && std::isfinite(*step) && *step > 0.0) {
    tolerance = std::max(tolerance, *step * kVerifyPrecisionToleranceFactor);
  }
  ParsedScalar parsed_spec;
  if (!ParseScalarToken(spec_scalar_token, &parsed_spec) || parsed_spec.unit.empty()) {
    return tolerance;
  }
  ScalarUnitScale scale;
  if (!ResolveScalarUnitScale(parsed_spec.unit, &scale) || scale.factor_to_canonical <= 0.0 ||
      !std::isfinite(scale.factor_to_canonical)) {
    return tolerance;
  }
  return std::max(kVerifyExactEpsilon, tolerance * scale.factor_to_canonical);
}

double AlignScalarToleranceToComparisonUnit(double tolerance,
                                            const std::string& spec_scalar_token,
                                            const ParsedScalar& spec_in_comparison_unit) {
  if (!std::isfinite(tolerance) || tolerance <= 0.0) {
    return kVerifyExactEpsilon;
  }
  ParsedScalar parsed_spec;
  if (!ParseScalarToken(spec_scalar_token, &parsed_spec)) {
    return tolerance;
  }
  const std::string spec_unit = NormalizeUnitToken(parsed_spec.unit);
  if (spec_unit.empty()) {
    return tolerance;
  }
  ScalarUnitScale spec_scale;
  if (!ResolveScalarUnitScale(spec_unit, &spec_scale) || spec_scale.factor_to_canonical <= 0.0 ||
      !std::isfinite(spec_scale.factor_to_canonical)) {
    return tolerance;
  }
  if (spec_in_comparison_unit.unit == spec_unit) {
    return std::max(kVerifyExactEpsilon, tolerance / spec_scale.factor_to_canonical);
  }
  return tolerance;
}

bool ParseBooleanToken(const std::string& token, bool* out) {
  if (!out) return false;
  const std::string lowered = ToLower(Trim(token));
  if (lowered == "true" || lowered == "yes" || lowered == "y" || lowered == "pass") {
    *out = true;
    return true;
  }
  if (lowered == "false" || lowered == "no" || lowered == "n" || lowered == "fail") {
    *out = false;
    return true;
  }
  return false;
}

bool ParseComparatorSpec(const std::string& spec, std::string* op, std::string* rhs) {
  if (!op || !rhs) return false;
  static constexpr std::string_view kOps[] = {"<=", ">=", "==", "!=", "<", ">", "="};
  const std::string trimmed = Trim(spec);
  for (const auto& token : kOps) {
    if (!StartsWith(trimmed, token)) {
      continue;
    }
    const std::string right = Trim(trimmed.substr(token.size()));
    if (right.empty()) {
      return false;
    }
    *op = std::string(token);
    *rhs = right;
    return true;
  }
  return false;
}

bool ParseDotDotRangeSpec(const std::string& spec, std::string* low, std::string* high) {
  if (!low || !high) return false;
  const std::string trimmed = Trim(spec);
  const auto dotdot = trimmed.find("..");
  if (dotdot == std::string::npos) {
    return false;
  }
  const std::string left = Trim(trimmed.substr(0, dotdot));
  const std::string right = Trim(trimmed.substr(dotdot + 2));
  if (left.empty() || right.empty()) {
    return false;
  }
  *low = left;
  *high = right;
  return true;
}

bool IsValidScalarUnitSuffixToken(const std::string& token) {
  const std::string trimmed = Trim(token);
  if (trimmed.empty()) {
    return false;
  }
  for (char ch : trimmed) {
    if (!IsAllowedUnitChar(ch)) {
      return false;
    }
  }
  return true;
}

std::string ApplyTrailingUnitSuffix(const std::string& scalar_token, const std::string& trailing_unit) {
  const std::string token = Trim(scalar_token);
  const std::string suffix = Trim(trailing_unit);
  if (token.empty() || suffix.empty()) {
    return token;
  }
  ParsedScalar parsed;
  if (ParseScalarToken(token, &parsed) && !parsed.unit.empty()) {
    return token;
  }
  return Trim(token + " " + suffix);
}

bool ParseBracketRangeSpec(const std::string& spec,
                           bool* include_low,
                           bool* include_high,
                           std::string* low,
                           std::string* high) {
  if (!include_low || !include_high || !low || !high) return false;
  const std::string trimmed = Trim(spec);
  if (trimmed.size() < 5) {
    return false;
  }
  const char open = trimmed.front();
  if (open != '[' && open != '(') {
    return false;
  }
  const auto close_pos = trimmed.find_last_of("])");
  if (close_pos == std::string::npos || close_pos <= 1) {
    return false;
  }
  const char close = trimmed[close_pos];
  if (close != ']' && close != ')') {
    return false;
  }
  const std::string trailing_unit = Trim(trimmed.substr(close_pos + 1));
  if (!trailing_unit.empty() && !IsValidScalarUnitSuffixToken(trailing_unit)) {
    return false;
  }
  const std::string inner = Trim(trimmed.substr(1, close_pos - 1));
  if (inner.empty()) {
    return false;
  }
  std::string left;
  std::string right;
  const auto comma = inner.find(',');
  if (comma != std::string::npos) {
    left = Trim(inner.substr(0, comma));
    right = Trim(inner.substr(comma + 1));
  } else {
    const auto hyphen = inner.find(" - ");
    if (hyphen == std::string::npos) {
      return false;
    }
    left = Trim(inner.substr(0, hyphen));
    right = Trim(inner.substr(hyphen + 3));
  }
  if (left.empty() || right.empty()) {
    return false;
  }
  *include_low = open == '[';
  *include_high = close == ']';
  *low = ApplyTrailingUnitSuffix(left, trailing_unit);
  *high = ApplyTrailingUnitSuffix(right, trailing_unit);
  return true;
}

std::string FormatScalarValue(double value) {
  if (!std::isfinite(value)) {
    return {};
  }
  std::ostringstream out;
  out << std::setprecision(12) << std::defaultfloat << value;
  std::string text = out.str();
  if (text == "-0") {
    return "0";
  }
  if (text.find('e') != std::string::npos || text.find('E') != std::string::npos) {
    return text;
  }
  const auto dot = text.find('.');
  if (dot == std::string::npos) {
    return text;
  }
  while (!text.empty() && text.back() == '0') {
    text.pop_back();
  }
  if (!text.empty() && text.back() == '.') {
    text.pop_back();
  }
  if (text == "-0") {
    return "0";
  }
  return text.empty() ? "0" : text;
}

std::optional<std::string> ParseScalarUnit(const std::string& token) {
  ParsedScalar scalar;
  if (!ParseScalarToken(token, &scalar)) {
    return std::nullopt;
  }
  const std::string unit = NormalizeUnitToken(scalar.unit);
  if (unit.empty()) {
    return std::nullopt;
  }
  return unit;
}

std::optional<std::string> SelectSpecUnit(const std::string& left, const std::string& right) {
  const auto left_unit = ParseScalarUnit(left);
  const auto right_unit = ParseScalarUnit(right);
  if (left_unit.has_value()) {
    return left_unit;
  }
  if (right_unit.has_value()) {
    return right_unit;
  }
  return std::nullopt;
}

std::optional<std::string> ResolveSpecScalarUnit(const std::string& spec) {
  std::string op;
  std::string rhs;
  if (ParseComparatorSpec(spec, &op, &rhs)) {
    return ParseScalarUnit(rhs);
  }
  std::string low;
  std::string high;
  if (ParseDotDotRangeSpec(spec, &low, &high)) {
    return SelectSpecUnit(low, high);
  }
  bool include_low = true;
  bool include_high = true;
  if (ParseBracketRangeSpec(spec, &include_low, &include_high, &low, &high)) {
    return SelectSpecUnit(low, high);
  }
  return ParseScalarUnit(spec);
}

std::string NormalizeResultToSpecUnit(const std::string& spec, const std::string& result) {
  const std::string trimmed_result = Trim(result);
  if (trimmed_result.empty()) {
    return result;
  }

  const auto spec_unit_opt = ResolveSpecScalarUnit(spec);
  if (!spec_unit_opt.has_value()) {
    return result;
  }
  const std::string spec_unit = NormalizeUnitToken(*spec_unit_opt);
  if (spec_unit.empty()) {
    return result;
  }

  ParsedScalar parsed_result;
  if (!ParseScalarToken(trimmed_result, &parsed_result)) {
    return result;
  }
  parsed_result.unit = NormalizeUnitToken(parsed_result.unit);
  if (parsed_result.unit.empty()) {
    parsed_result.unit = spec_unit;
  }

  ParsedScalar normalized_result;
  if (!NormalizeScalarToCanonical(parsed_result, &normalized_result)) {
    return result;
  }

  ParsedScalar spec_probe;
  spec_probe.value = 1.0;
  spec_probe.unit = spec_unit;
  ParsedScalar normalized_spec_probe;
  if (!NormalizeScalarToCanonical(spec_probe, &normalized_spec_probe)) {
    return result;
  }
  if (normalized_result.unit != normalized_spec_probe.unit) {
    return result;
  }

  ScalarUnitScale spec_scale;
  if (!ResolveScalarUnitScale(spec_unit, &spec_scale) || spec_scale.factor_to_canonical <= 0.0 ||
      !std::isfinite(spec_scale.factor_to_canonical)) {
    return result;
  }

  const double value_in_spec_units = normalized_result.value / spec_scale.factor_to_canonical;
  const std::string formatted = FormatScalarValue(value_in_spec_units);
  if (formatted.empty()) {
    return result;
  }
  return formatted + " " + spec_unit;
}

VerifyComputation EvaluateSpecResultVerify(const std::string& spec, const std::string& result) {
  const std::string trimmed_spec = Trim(spec);
  const std::string trimmed_result = Trim(result);
  if (trimmed_spec.empty()) {
    return VerifyComputation{VerifyBoolState::kIndeterminate, "MISSING_SPEC",
                             "Spec is empty; cannot evaluate predicate_bool."};
  }
  if (trimmed_result.empty()) {
    return VerifyComputation{VerifyBoolState::kIndeterminate, "MISSING_RESULT",
                             "Result is empty; cannot evaluate predicate_bool."};
  }

  {
    std::string op;
    std::string rhs;
    if (ParseComparatorSpec(trimmed_spec, &op, &rhs)) {
      ParsedScalar lhs_value;
      ParsedScalar rhs_value;
      if (!ParseScalarToken(trimmed_result, &lhs_value)) {
        return VerifyComputation{VerifyBoolState::kIndeterminate, "RESULT_UNPARSEABLE",
                                 "Result does not match the numeric comparator spec."};
      }
      if (!ParseScalarToken(rhs, &rhs_value)) {
        return VerifyComputation{VerifyBoolState::kIndeterminate, "SPEC_UNPARSEABLE",
                                 "Spec comparator value is not a valid scalar."};
      }
      ParsedScalar lhs_normalized;
      ParsedScalar rhs_normalized;
      if (!NormalizeScalarPairForCompare(lhs_value, rhs_value, &lhs_normalized, &rhs_normalized)) {
        return VerifyComputation{VerifyBoolState::kIndeterminate, "UNIT_MISMATCH",
                                 "Result/spec unit mismatch."};
      }
      bool matched = false;
      if (op == "<=") matched = lhs_normalized.value <= rhs_normalized.value;
      if (op == ">=") matched = lhs_normalized.value >= rhs_normalized.value;
      if (op == "<") matched = lhs_normalized.value < rhs_normalized.value;
      if (op == ">") matched = lhs_normalized.value > rhs_normalized.value;
      const double equality_tolerance = AlignScalarToleranceToComparisonUnit(
          ResolveScalarEqualityTolerance(rhs), rhs, rhs_normalized);
      if (op == "==" || op == "=")
        matched = std::abs(lhs_normalized.value - rhs_normalized.value) <= equality_tolerance;
      if (op == "!=")
        matched = std::abs(lhs_normalized.value - rhs_normalized.value) > equality_tolerance;
      return VerifyComputation{matched ? VerifyBoolState::kTrue : VerifyBoolState::kFalse,
                               matched ? "COMPARATOR_TRUE" : "COMPARATOR_FALSE",
                               matched ? "Comparator matched." : "Comparator did not match."};
    }
  }

  {
    std::string low_token;
    std::string high_token;
    if (ParseDotDotRangeSpec(trimmed_spec, &low_token, &high_token)) {
      ParsedScalar low_value;
      ParsedScalar high_value;
      ParsedScalar result_value;
      if (!ParseScalarToken(low_token, &low_value) || !ParseScalarToken(high_token, &high_value)) {
        return VerifyComputation{VerifyBoolState::kIndeterminate, "SPEC_UNPARSEABLE",
                                 "Range bounds are not valid scalars."};
      }
      if (!ParseScalarToken(trimmed_result, &result_value)) {
        return VerifyComputation{VerifyBoolState::kIndeterminate, "RESULT_UNPARSEABLE",
                                 "Result does not match range spec."};
      }
      ParsedScalar result_norm_low;
      ParsedScalar low_norm;
      ParsedScalar result_norm_high;
      ParsedScalar high_norm;
      if (!NormalizeScalarPairForCompare(result_value, low_value, &result_norm_low, &low_norm) ||
          !NormalizeScalarPairForCompare(result_value, high_value, &result_norm_high, &high_norm)) {
        return VerifyComputation{VerifyBoolState::kIndeterminate, "UNIT_MISMATCH",
                                 "Range units are not compatible."};
      }
      const bool matched =
          result_norm_low.value >= low_norm.value && result_norm_high.value <= high_norm.value;
      return VerifyComputation{matched ? VerifyBoolState::kTrue : VerifyBoolState::kFalse,
                               matched ? "RANGE_TRUE" : "RANGE_FALSE",
                               matched ? "Result is inside the inclusive range."
                                       : "Result is outside the inclusive range."};
    }
  }

  {
    bool include_low = true;
    bool include_high = true;
    std::string low_token;
    std::string high_token;
    if (ParseBracketRangeSpec(trimmed_spec, &include_low, &include_high, &low_token, &high_token)) {
      ParsedScalar low_value;
      ParsedScalar high_value;
      ParsedScalar result_value;
      if (!ParseScalarToken(low_token, &low_value) || !ParseScalarToken(high_token, &high_value)) {
        return VerifyComputation{VerifyBoolState::kIndeterminate, "SPEC_UNPARSEABLE",
                                 "Bracket range bounds are not valid scalars."};
      }
      if (!ParseScalarToken(trimmed_result, &result_value)) {
        return VerifyComputation{VerifyBoolState::kIndeterminate, "RESULT_UNPARSEABLE",
                                 "Result does not match bracket range spec."};
      }
      ParsedScalar result_norm_low;
      ParsedScalar low_norm;
      ParsedScalar result_norm_high;
      ParsedScalar high_norm;
      if (!NormalizeScalarPairForCompare(result_value, low_value, &result_norm_low, &low_norm) ||
          !NormalizeScalarPairForCompare(result_value, high_value, &result_norm_high, &high_norm)) {
        return VerifyComputation{VerifyBoolState::kIndeterminate, "UNIT_MISMATCH",
                                 "Bracket range units are not compatible."};
      }
      const bool left_ok =
          include_low ? result_norm_low.value >= low_norm.value : result_norm_low.value > low_norm.value;
      const bool right_ok = include_high ? result_norm_high.value <= high_norm.value
                                         : result_norm_high.value < high_norm.value;
      const bool matched = left_ok && right_ok;
      return VerifyComputation{matched ? VerifyBoolState::kTrue : VerifyBoolState::kFalse,
                               matched ? "RANGE_TRUE" : "RANGE_FALSE",
                               matched ? "Result is inside the bracket range."
                                       : "Result is outside the bracket range."};
    }
  }

  {
    bool spec_bool = false;
    if (ParseBooleanToken(trimmed_spec, &spec_bool)) {
      bool result_bool = false;
      if (!ParseBooleanToken(trimmed_result, &result_bool)) {
        return VerifyComputation{VerifyBoolState::kIndeterminate, "RESULT_UNPARSEABLE",
                                 "Result is not a deterministic boolean token."};
      }
      const bool matched = spec_bool == result_bool;
      return VerifyComputation{matched ? VerifyBoolState::kTrue : VerifyBoolState::kFalse,
                               matched ? "BOOLEAN_TRUE" : "BOOLEAN_FALSE",
                               matched ? "Boolean tokens match." : "Boolean tokens do not match."};
    }
  }

  {
    ParsedScalar spec_value;
    ParsedScalar result_value;
    if (ParseScalarToken(trimmed_spec, &spec_value)) {
      if (!ParseScalarToken(trimmed_result, &result_value)) {
        return VerifyComputation{VerifyBoolState::kIndeterminate, "RESULT_UNPARSEABLE",
                                 "Result is not a scalar for scalar equality."};
      }
      ParsedScalar result_normalized;
      ParsedScalar spec_normalized;
      if (!NormalizeScalarPairForCompare(result_value, spec_value, &result_normalized,
                                         &spec_normalized)) {
        return VerifyComputation{VerifyBoolState::kIndeterminate, "UNIT_MISMATCH",
                                 "Scalar equality units are not compatible."};
      }
      const double equality_tolerance = AlignScalarToleranceToComparisonUnit(
          ResolveScalarEqualityTolerance(trimmed_spec), trimmed_spec, spec_normalized);
      const bool matched =
          std::abs(result_normalized.value - spec_normalized.value) <= equality_tolerance;
      return VerifyComputation{matched ? VerifyBoolState::kTrue : VerifyBoolState::kFalse,
                               matched ? "SCALAR_TRUE" : "SCALAR_FALSE",
                               matched ? "Scalar values are equal." : "Scalar values are not equal."};
    }
  }

  if (trimmed_spec.find('<') != std::string::npos || trimmed_spec.find('>') != std::string::npos ||
      trimmed_spec.find("..") != std::string::npos ||
      ((trimmed_spec.front() == '[' || trimmed_spec.front() == '(') &&
       (trimmed_spec.back() == ']' || trimmed_spec.back() == ')'))) {
    return VerifyComputation{VerifyBoolState::kIndeterminate, "UNSUPPORTED_SPEC_GRAMMAR",
                             "Spec looks structured but is not in a supported deterministic grammar."};
  }

  const bool matched = ToLower(trimmed_result) == ToLower(trimmed_spec);
  return VerifyComputation{matched ? VerifyBoolState::kTrue : VerifyBoolState::kFalse,
                           matched ? "TEXT_TRUE" : "TEXT_FALSE",
                           matched ? "Text values match exactly." : "Text values do not match exactly."};
}

std::optional<VerifyPredicateParts> ParseVerifyPredicateToken(std::string_view token) {
  constexpr std::string_view kPrefix = "BoolVerify";
  if (!StartsWith(token, kPrefix)) {
    return std::nullopt;
  }
  constexpr std::string_view kTypes[] = {"Validated", "Implied", "Assumed", "AndGate", "OrGate"};
  struct VerifyObjectToken {
    std::string_view token;
    VerifyObjectKind kind;
    ChecklistStatus status;
  };
  constexpr VerifyObjectToken kObjects[] = {
      {"Status", VerifyObjectKind::kStatusBridge, ChecklistStatus::kUnknown},
      {"Comment", VerifyObjectKind::kCommentBridge, ChecklistStatus::kUnknown},
      {"Pass", VerifyObjectKind::kStatusLiteral, ChecklistStatus::kPass},
      {"Fail", VerifyObjectKind::kStatusLiteral, ChecklistStatus::kFail},
      {"Other", VerifyObjectKind::kStatusLiteral, ChecklistStatus::kOther},
      {"Na", VerifyObjectKind::kStatusLiteral, ChecklistStatus::kNA},
      {"NA", VerifyObjectKind::kStatusLiteral, ChecklistStatus::kNA},
  };

  std::size_t pos = kPrefix.size();
  for (const auto& type : kTypes) {
    if (!StartsWith(token.substr(pos), type)) {
      continue;
    }
    pos += type.size();
    for (const auto& object : kObjects) {
      if (token.substr(pos) != object.token) {
        continue;
      }
      return VerifyPredicateParts{type, object.kind, object.status, object.token};
    }
    pos -= type.size();
  }
  return std::nullopt;
}

VerifyGateComputation EvaluateVerifyGate(sqlite3_stmt* stmt,
                                         const std::string& target_address_id,
                                         const std::string& predicate,
                                         CanonicalGateMode gate_mode) {
  VerifyGateComputation gate;
  if (!stmt || gate_mode == CanonicalGateMode::kNone) {
    return gate;
  }
  sqlite3_reset(stmt);
  sqlite3_clear_bindings(stmt);
  sqlite3_bind_text(stmt, 1, target_address_id.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 2, predicate.c_str(), -1, SQLITE_TRANSIENT);
  int false_count = 0;
  int indeterminate_count = 0;
  int rc = SQLITE_ROW;
  while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
    const std::string source_address = ColumnText(stmt, 0);
    if (source_address.empty()) {
      continue;
    }
    ++gate.contributor_count;
    const std::string source_spec = ColumnText(stmt, 1);
    const std::string source_result = ColumnText(stmt, 2);
    const VerifyComputation eval = EvaluateSpecResultVerify(source_spec, source_result);
    if (eval.state == VerifyBoolState::kTrue) {
      ++gate.contributor_true_count;
    } else if (eval.state == VerifyBoolState::kFalse) {
      ++false_count;
    } else {
      ++indeterminate_count;
    }
  }
  if (rc != SQLITE_DONE) {
    throw std::runtime_error("Verify gate evaluation query failed");
  }

  if (gate.contributor_count <= 0) {
    gate.state = VerifyBoolState::kIndeterminate;
    gate.reason_code = "GATE_NO_CONTRIBUTORS";
    gate.reason = "No contributors found for the Verify gate.";
    return gate;
  }

  if (gate_mode == CanonicalGateMode::kAnd) {
    if (false_count > 0) {
      gate.state = VerifyBoolState::kFalse;
      gate.reason_code = "GATE_AND_FALSE";
      gate.reason = "At least one gate contributor evaluated false.";
      return gate;
    }
    if (indeterminate_count > 0) {
      gate.state = VerifyBoolState::kIndeterminate;
      gate.reason_code = "GATE_AND_INDETERMINATE";
      gate.reason = "At least one gate contributor is indeterminate.";
      return gate;
    }
    gate.state = VerifyBoolState::kTrue;
    gate.reason_code = "GATE_AND_TRUE";
    gate.reason = "All gate contributors evaluated true.";
    return gate;
  }

  if (gate.contributor_true_count > 0) {
    gate.state = VerifyBoolState::kTrue;
    gate.reason_code = "GATE_OR_TRUE";
    gate.reason = "At least one gate contributor evaluated true.";
    return gate;
  }
  if (indeterminate_count > 0) {
    gate.state = VerifyBoolState::kIndeterminate;
    gate.reason_code = "GATE_OR_INDETERMINATE";
    gate.reason = "No true contributor and at least one contributor is indeterminate.";
    return gate;
  }
  gate.state = VerifyBoolState::kFalse;
  gate.reason_code = "GATE_OR_FALSE";
  gate.reason = "All gate contributors evaluated false.";
  return gate;
}

VerifyWritePlan BuildVerifyWritePlan(const VerifyPredicateParts& parts,
                                     VerifyBoolState state,
                                     const std::string& target_comment) {
  (void)target_comment;
  VerifyWritePlan plan;
  if (parts.object_kind == VerifyObjectKind::kStatusBridge) {
    if (state == VerifyBoolState::kTrue) {
      plan.write_status = true;
      plan.status = ChecklistStatus::kPass;
      plan.write_decision = "WRITE_STATUS_PASS";
      return plan;
    }
    if (state == VerifyBoolState::kFalse) {
      plan.write_status = true;
      plan.status = ChecklistStatus::kFail;
      plan.write_decision = "WRITE_STATUS_FAIL";
      return plan;
    }
    plan.write_decision = "NO_STATUS_WRITE_INDETERMINATE";
    return plan;
  }

  if (parts.object_kind == VerifyObjectKind::kCommentBridge) {
    plan.write_comment = true;
    plan.comment = VerifyBoolToUpper(state);
    plan.write_decision = "WRITE_COMMENT_BOOL";
    return plan;
  }

  if (state == VerifyBoolState::kTrue && parts.object_status != ChecklistStatus::kUnknown) {
    plan.write_status = true;
    plan.status = parts.object_status;
    plan.write_decision = "WRITE_LITERAL_STATUS_TRUE";
    return plan;
  }
  if (state == VerifyBoolState::kFalse) {
    plan.write_decision = "NO_WRITE_FALSE";
  } else {
    plan.write_decision = "NO_WRITE_INDETERMINATE";
  }
  return plan;
}

enum class SlotField {
  kResult,
  kStatus,
  kComment,
  kSection,
  kAction,
  kSpec,
  kProcedure,
  kInstructions,
  kTimestamp
};

struct SlotPredicateParts {
  SlotField subject;
  std::string_view relation;
  std::string_view type;
  SlotField object;
  bool subject_status_filter = false;
  ChecklistStatus subject_status = ChecklistStatus::kUnknown;
};

constexpr std::pair<std::string_view, SlotField> kSlotFieldNames[] = {
    {"Result", SlotField::kResult},
    {"Status", SlotField::kStatus},
    {"Comment", SlotField::kComment},
    {"Section", SlotField::kSection},
    {"Action", SlotField::kAction},
    {"Spec", SlotField::kSpec},
    {"Procedure", SlotField::kProcedure},
    {"Instructions", SlotField::kInstructions},
    {"Timestamp", SlotField::kTimestamp},
};

struct SlotSubjectToken {
  std::string_view token;
  SlotField field;
  bool status_filter;
  ChecklistStatus status;
};

constexpr SlotSubjectToken kSlotSubjectTokens[] = {
    {"Result", SlotField::kResult, false, ChecklistStatus::kUnknown},
    {"Status", SlotField::kStatus, false, ChecklistStatus::kUnknown},
    {"Comment", SlotField::kComment, false, ChecklistStatus::kUnknown},
    {"Section", SlotField::kSection, false, ChecklistStatus::kUnknown},
    {"Action", SlotField::kAction, false, ChecklistStatus::kUnknown},
    {"Spec", SlotField::kSpec, false, ChecklistStatus::kUnknown},
    {"Procedure", SlotField::kProcedure, false, ChecklistStatus::kUnknown},
    {"Instructions", SlotField::kInstructions, false, ChecklistStatus::kUnknown},
    {"Timestamp", SlotField::kTimestamp, false, ChecklistStatus::kUnknown},
    {"pass", SlotField::kStatus, true, ChecklistStatus::kPass},
    {"Pass", SlotField::kStatus, true, ChecklistStatus::kPass},
    {"fail", SlotField::kStatus, true, ChecklistStatus::kFail},
    {"Fail", SlotField::kStatus, true, ChecklistStatus::kFail},
    {"other", SlotField::kStatus, true, ChecklistStatus::kOther},
    {"Other", SlotField::kStatus, true, ChecklistStatus::kOther},
    {"na", SlotField::kStatus, true, ChecklistStatus::kNA},
    {"Na", SlotField::kStatus, true, ChecklistStatus::kNA},
    {"NA", SlotField::kStatus, true, ChecklistStatus::kNA},
};

struct SlotRelationType {
  std::string_view relation;
  std::string_view type;
};

constexpr SlotRelationType kSlotRelationTypes[] = {
    {"Search", "Prefill"},
    {"Propagate", "Validated"},
};
constexpr std::string_view kSlugPredecessorPredicate = "slugPredecessor";
constexpr std::string_view kSlugSuccessorPredicate = "slugSuccessor";

std::optional<SlotPredicateParts> ParseSlotPredicateToken(std::string_view token) {
  for (const auto& subject_token : kSlotSubjectTokens) {
    if (!StartsWith(token, subject_token.token)) continue;
    for (const auto& relation_type : kSlotRelationTypes) {
      std::size_t pos = subject_token.token.size();
      if (!StartsWith(token.substr(pos), relation_type.relation)) {
        continue;
      }
      pos += relation_type.relation.size();
      if (!StartsWith(token.substr(pos), relation_type.type)) {
        continue;
      }
      pos += relation_type.type.size();
      for (const auto& object_pair : kSlotFieldNames) {
        if (token.substr(pos) != object_pair.first) continue;
        SlotPredicateParts parts{
            subject_token.field, relation_type.relation, relation_type.type,
            object_pair.second};
        parts.subject_status_filter = subject_token.status_filter;
        parts.subject_status = subject_token.status;
        return parts;
      }
    }
  }
  return std::nullopt;
}

std::string SlotFieldToString(SlotField field) {
  switch (field) {
    case SlotField::kResult:
      return "result";
    case SlotField::kStatus:
      return "status";
    case SlotField::kComment:
      return "comment";
    case SlotField::kSection:
      return "section";
    case SlotField::kAction:
      return "action";
    case SlotField::kSpec:
      return "spec";
    case SlotField::kProcedure:
      return "procedure";
    case SlotField::kInstructions:
      return "instructions";
    case SlotField::kTimestamp:
      return "timestamp";
  }
  return "result";
}

std::optional<SlotField> ParseSlotFieldToken(std::string_view value) {
  const std::string lowered = ToLower(std::string(value));
  if (lowered == "result") return SlotField::kResult;
  if (lowered == "status") return SlotField::kStatus;
  if (lowered == "pass" || lowered == "fail" || lowered == "other" ||
      lowered == "na" || lowered == "n/a") {
    return SlotField::kStatus;
  }
  if (lowered == "comment") return SlotField::kComment;
  if (lowered == "section") return SlotField::kSection;
  if (lowered == "action") return SlotField::kAction;
  if (lowered == "spec") return SlotField::kSpec;
  if (lowered == "procedure") return SlotField::kProcedure;
  if (lowered == "instructions") return SlotField::kInstructions;
  if (lowered == "timestamp") return SlotField::kTimestamp;
  return std::nullopt;
}

std::string ExtractSlugIdFromAddress(const std::string& address_id) {
  if (address_id.size() < 16) {
    return {};
  }
  return address_id.substr(0, 16);
}

bool IsWritableSlotField(SlotField field) {
  return field == SlotField::kResult || field == SlotField::kStatus || field == SlotField::kComment ||
         field == SlotField::kTimestamp;
}

bool IsPropagateTargetField(SlotField field) {
  return field == SlotField::kResult || field == SlotField::kStatus || field == SlotField::kComment;
}

bool FieldChanged(SlotField field,
                  bool result_changed,
                  bool status_changed,
                  bool comment_changed,
                  bool timestamp_changed) {
  switch (field) {
    case SlotField::kResult:
      return result_changed;
    case SlotField::kStatus:
      return status_changed;
    case SlotField::kComment:
      return comment_changed;
    case SlotField::kTimestamp:
      return timestamp_changed;
    case SlotField::kSection:
    case SlotField::kAction:
    case SlotField::kSpec:
    case SlotField::kProcedure:
    case SlotField::kInstructions:
      return false;
  }
  return false;
}

std::string FieldValueForSlug(const ChecklistSlug& slug, SlotField field) {
  switch (field) {
    case SlotField::kResult:
      return slug.result;
    case SlotField::kStatus:
      return core::StatusToString(slug.status);
    case SlotField::kComment:
      return slug.comment;
    case SlotField::kSection:
      return slug.section;
    case SlotField::kAction:
      return slug.action;
    case SlotField::kSpec:
      return slug.spec;
    case SlotField::kProcedure:
      return slug.procedure;
    case SlotField::kInstructions:
      return slug.instructions;
    case SlotField::kTimestamp:
      return slug.timestamp;
  }
  return {};
}

std::vector<std::string> ParseCsvLine(const std::string& line) {
  std::vector<std::string> out;
  std::string current;
  bool in_quotes = false;
  for (std::size_t i = 0; i < line.size(); ++i) {
    const char ch = line[i];
    if (in_quotes) {
      if (ch == '"') {
        if (i + 1 < line.size() && line[i + 1] == '"') {
          current.push_back('"');
          ++i;
        } else {
          in_quotes = false;
        }
      } else {
        current.push_back(ch);
      }
    } else {
      if (ch == ',') {
        out.push_back(current);
        current.clear();
      } else if (ch == '"') {
        in_quotes = true;
      } else {
        current.push_back(ch);
      }
    }
  }
  out.push_back(current);
  return out;
}

bool ReadCsv(const fs::path& path,
             std::vector<std::string>& headers,
             std::vector<std::vector<std::string>>& rows,
             std::string* error) {
  std::ifstream file(path, std::ios::binary);
  if (!file.is_open()) {
    if (error) {
      *error = "Unable to open CSV: " + path.string();
    }
    return false;
  }

  std::string line;
  bool got_header = false;
  while (std::getline(file, line)) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    if (!got_header) {
      if (Trim(line).empty()) {
        continue;
      }
      headers = ParseCsvLine(line);
      if (!headers.empty()) {
        const std::string bom = "\xEF\xBB\xBF";
        if (headers[0].rfind(bom, 0) == 0) {
          headers[0].erase(0, bom.size());
        }
      }
      got_header = true;
      continue;
    }
    if (Trim(line).empty()) {
      continue;
    }
    auto parsed = ParseCsvLine(line);
    rows.push_back(std::move(parsed));
  }

  if (!got_header || headers.empty()) {
    if (error) {
      *error = "CSV header missing: " + path.string();
    }
    return false;
  }
  return true;
}

std::string TrimColumnToken(const std::string& value) {
  const std::string trimmed = Trim(value);
  if (trimmed.empty()) {
    return {};
  }
  const auto cut = trimmed.find_first_of(" \t(");
  if (cut == std::string::npos) {
    return trimmed;
  }
  return trimmed.substr(0, cut);
}

std::optional<std::pair<std::string, SlotField>> ParseSlugFieldHeader(const std::string& header) {
  const std::string token = TrimColumnToken(header);
  if (token.empty()) {
    return std::nullopt;
  }
  std::string slug_id = token;
  std::string field_token;
  const auto dash = token.find('-');
  if (dash != std::string::npos) {
    slug_id = token.substr(0, dash);
    field_token = token.substr(dash + 1);
  }
  SlotField field = SlotField::kResult;
  if (!field_token.empty()) {
    const auto parsed = ParseSlotFieldToken(field_token);
    if (!parsed) {
      return std::nullopt;
    }
    field = *parsed;
  }
  if (slug_id.empty()) {
    return std::nullopt;
  }
  return std::make_pair(slug_id, field);
}

std::unordered_map<std::string, std::size_t> BuildSlugFieldIndex(
    const std::vector<std::string>& headers) {
  std::unordered_map<std::string, std::size_t> index;
  for (std::size_t i = 0; i < headers.size(); ++i) {
    const auto parsed = ParseSlugFieldHeader(headers[i]);
    if (!parsed) {
      continue;
    }
    const std::string key = parsed->first + "|" + SlotFieldToString(parsed->second);
    index.emplace(key, i);
  }
  return index;
}

std::optional<std::size_t> FindSlugFieldColumn(
    const std::unordered_map<std::string, std::size_t>& index,
    const std::string& slug_id,
    SlotField field) {
  const std::string key = slug_id + "|" + SlotFieldToString(field);
  const auto it = index.find(key);
  if (it == index.end()) {
    return std::nullopt;
  }
  return it->second;
}

std::optional<std::size_t> FindSlugFieldColumnWithLineage(
    const std::unordered_map<std::string, std::size_t>& index,
    const std::vector<std::string>& slug_ids,
    SlotField field,
    std::string* matched_slug_id) {
  for (const auto& slug_id : slug_ids) {
    if (slug_id.empty()) {
      continue;
    }
    const auto column = FindSlugFieldColumn(index, slug_id, field);
    if (column) {
      if (matched_slug_id) {
        *matched_slug_id = slug_id;
      }
      return column;
    }
  }
  return std::nullopt;
}

std::unordered_map<std::string, std::vector<std::string>> BuildSlugPredecessorIndex(
    const std::vector<TemplateRelationship>& relationships) {
  std::unordered_map<std::string, std::vector<std::string>> predecessors;
  predecessors.reserve(relationships.size());
  for (const auto& rel : relationships) {
    std::string successor;
    std::string predecessor;
    if (rel.predicate == kSlugSuccessorPredicate) {
      predecessor = rel.subject_slug_id;
      successor = rel.target_slug_id;
    } else if (rel.predicate == kSlugPredecessorPredicate) {
      predecessor = rel.target_slug_id;
      successor = rel.subject_slug_id;
    } else {
      continue;
    }
    if (successor.empty() || predecessor.empty() || successor == predecessor) {
      continue;
    }
    auto& entries = predecessors[successor];
    if (std::find(entries.begin(), entries.end(), predecessor) == entries.end()) {
      entries.push_back(predecessor);
    }
  }
  return predecessors;
}

std::unordered_map<std::string, std::vector<std::string>> BuildSlugSuccessorIndex(
    const std::vector<TemplateRelationship>& relationships) {
  std::unordered_map<std::string, std::vector<std::string>> successors;
  successors.reserve(relationships.size());
  for (const auto& rel : relationships) {
    std::string predecessor;
    std::string successor;
    if (rel.predicate == kSlugSuccessorPredicate) {
      predecessor = rel.subject_slug_id;
      successor = rel.target_slug_id;
    } else if (rel.predicate == kSlugPredecessorPredicate) {
      predecessor = rel.target_slug_id;
      successor = rel.subject_slug_id;
    } else {
      continue;
    }
    if (predecessor.empty() || successor.empty() || predecessor == successor) {
      continue;
    }
    auto& entries = successors[predecessor];
    if (std::find(entries.begin(), entries.end(), successor) == entries.end()) {
      entries.push_back(successor);
    }
  }
  return successors;
}

std::optional<std::string> ResolveLatestSlugId(
    const std::unordered_map<std::string, std::vector<std::string>>& successors,
    const std::string& start) {
  if (start.empty()) {
    return std::nullopt;
  }
  std::unordered_set<std::string> seen;
  std::string current = start;
  while (true) {
    if (!seen.insert(current).second) {
      return std::nullopt;
    }
    const auto it = successors.find(current);
    if (it == successors.end() || it->second.empty()) {
      return current;
    }
    if (it->second.size() > 1) {
      return std::nullopt;
    }
    current = it->second.front();
  }
}

std::vector<std::string> CollectSlugLineageCandidates(
    const std::unordered_map<std::string, std::vector<std::string>>& predecessors,
    const std::string& slug_id) {
  std::vector<std::string> candidates;
  if (slug_id.empty()) {
    return candidates;
  }
  std::unordered_set<std::string> visited;
  visited.insert(slug_id);
  candidates.push_back(slug_id);
  std::vector<std::string> stack;
  stack.push_back(slug_id);
  while (!stack.empty()) {
    const std::string current = stack.back();
    stack.pop_back();
    const auto it = predecessors.find(current);
    if (it == predecessors.end()) {
      continue;
    }
    for (const auto& pred : it->second) {
      if (pred.empty() || !visited.insert(pred).second) {
        continue;
      }
      candidates.push_back(pred);
      stack.push_back(pred);
    }
  }
  return candidates;
}

struct PrefillDatasetMatch {
  fs::path path;
  std::string mode;
  std::string matched_slug_id;
};

std::optional<PrefillDatasetMatch> FindPrefillDatasetInDataRoot(
    const fs::path& data_root, const std::string& checklist,
    const std::vector<std::string>& slug_ids, const std::string& address_id,
    std::string_view mode_prefix, bool include_checklist_fallback) {
  std::error_code ec;
  if (!fs::exists(data_root, ec) || !fs::is_directory(data_root, ec)) {
    return std::nullopt;
  }
  if (!address_id.empty()) {
    const fs::path address_specific = data_root / (address_id + ".csv");
    if (fs::exists(address_specific, ec)) {
      return PrefillDatasetMatch{address_specific, std::string(mode_prefix) + "address", {}};
    }
  }
  const std::string primary_slug = slug_ids.empty() ? "" : slug_ids.front();
  for (const auto& slug_id : slug_ids) {
    if (slug_id.empty()) {
      continue;
    }
    const bool is_predecessor = slug_id != primary_slug && !primary_slug.empty();
    const fs::path slug_specific = data_root / (slug_id + ".csv");
    if (fs::exists(slug_specific, ec)) {
      return PrefillDatasetMatch{slug_specific,
                                 std::string(mode_prefix) +
                                     (is_predecessor ? "slug_predecessor" : "slug"),
                                 slug_id};
    }
    const fs::path legacy_slug_specific = data_root / (checklist + "." + slug_id + ".csv");
    if (fs::exists(legacy_slug_specific, ec)) {
      return PrefillDatasetMatch{legacy_slug_specific,
                                 std::string(mode_prefix) +
                                     (is_predecessor ? "slug_predecessor_legacy" : "slug_legacy"),
                                 slug_id};
    }
  }
  if (include_checklist_fallback) {
    const fs::path checklist_default = data_root / (checklist + ".csv");
    if (fs::exists(checklist_default, ec)) {
      return PrefillDatasetMatch{checklist_default, std::string(mode_prefix) + "checklist", {}};
    }
  }
  return std::nullopt;
}

std::optional<PrefillDatasetMatch> FindPrefillDatasetMatch(
    const std::string& checklist, const std::vector<std::string>& slug_ids,
    const std::string& address_id, const std::optional<ChecklistOwnership>& ownership = std::nullopt) {
  if (ownership && !ownership->source_path.empty() && !ownership->pack.empty() &&
      !ownership->checklist_dir.empty()) {
    // An imported asset owns its data.  Do not fall through to a same-named checklist
    // in the staged public library when that owner's data root is absent.
    return FindPrefillDatasetInDataRoot(fs::path(ownership->source_path) / ownership->pack /
                                            ownership->checklist_dir / "data",
                                        checklist, slug_ids, address_id, "owned_", true);
  }

  const fs::path library_root = core::ResolveLibraryRoot();
  std::optional<PrefillDatasetMatch> fallback;
  for (const auto& pack_root : core::ListPackRoots(library_root)) {
    const fs::path data_root = pack_root / checklist / "data";
    if (const auto match =
            FindPrefillDatasetInDataRoot(data_root, checklist, slug_ids, address_id, "", false)) {
      return match;
    }
    if (!fallback) {
      std::error_code ec;
      const fs::path checklist_default = data_root / (checklist + ".csv");
      if (fs::exists(checklist_default, ec)) {
        fallback = PrefillDatasetMatch{checklist_default, "checklist", {}};
      }
    }
  }

  return fallback;
}

std::optional<fs::path> FindPrefillDatasetPath(const std::string& checklist,
                                               const std::vector<std::string>& slug_ids,
                                               const std::string& address_id,
                                               const std::optional<ChecklistOwnership>& ownership = std::nullopt) {
  const auto match = FindPrefillDatasetMatch(checklist, slug_ids, address_id, ownership);
  if (!match) {
    return std::nullopt;
  }
  return match->path;
}

std::optional<ChecklistOwnership> FindNewestOwnershipForAddressUnlocked(
    sqlite3* db, const std::string& address_id) {
  if (address_id.empty()) {
    return std::nullopt;
  }
  sqlite3_stmt* stmt = nullptr;
  const std::string sql =
      "SELECT source_name, COALESCE(source_path, ''), pack, checklist_dir, checklist "
      "FROM address_ownership WHERE address_id=? "
      "ORDER BY updated_at DESC, source_name, pack, checklist_dir LIMIT 1;";
  if (Prepare(db, sql, &stmt) != SQLITE_OK) {
    Finalize(stmt);
    return std::nullopt;
  }
  sqlite3_bind_text(stmt, 1, address_id.c_str(), -1, SQLITE_TRANSIENT);
  std::optional<ChecklistOwnership> ownership;
  if (sqlite3_step(stmt) == SQLITE_ROW) {
    ChecklistOwnership value;
    value.source_name = ColumnText(stmt, 0);
    value.source_path = ColumnText(stmt, 1);
    value.pack = ColumnText(stmt, 2);
    value.checklist_dir = ColumnText(stmt, 3);
    value.checklist = ColumnText(stmt, 4);
    ownership = std::move(value);
  }
  Finalize(stmt);
  return ownership;
}

bool ValuesMatch(const std::string& left, const std::string& right) {
  const std::string ltrim = Trim(left);
  const std::string rtrim = Trim(right);
  if (ltrim == rtrim) {
    return true;
  }
  return ToLower(ltrim) == ToLower(rtrim);
}

bool IsIsoTimestamp(const std::string& value) {
  std::tm tm_snapshot{};
  std::istringstream input(value);
  input >> std::get_time(&tm_snapshot, "%Y-%m-%dT%H:%M:%SZ");
  return !input.fail() && input.rdbuf()->in_avail() == 0;
}

}  // namespace

namespace core {
ChecklistStatus ParseStatus(const std::string& value) {
  const std::string lowered = ToLower(value);
  if (lowered == "pass") {
    return ChecklistStatus::kPass;
  }
  if (lowered == "fail") {
    return ChecklistStatus::kFail;
  }
  if (lowered == "na" || lowered == "n/a") {
    return ChecklistStatus::kNA;
  }
  if (lowered == "other") {
    return ChecklistStatus::kOther;
  }
  return ChecklistStatus::kUnknown;
}

std::string StatusToString(ChecklistStatus status) {
  switch (status) {
    case ChecklistStatus::kPass:
      return "Pass";
    case ChecklistStatus::kFail:
      return "Fail";
    case ChecklistStatus::kNA:
      return "NA";
    case ChecklistStatus::kOther:
      return "Other";
    case ChecklistStatus::kUnknown:
    default:
      return "Unknown";
  }
}

bool IsValidBase32Id(const std::string& value, std::size_t expected_length) {
  if (value.size() != expected_length) {
    return false;
  }
  for (char ch : value) {
    if (!IsAllowedBase32Char(ch)) {
      return false;
    }
  }
  return true;
}

std::string ComputeSlugId(const std::string& checklist, const std::string& section,
                          const std::string& procedure, const std::string& action,
                          const std::string& spec, const std::string& instructions) {
  const std::string canonical =
      CanonicalizeForHash(checklist) + "\n" + CanonicalizeForHash(section) + "\n" +
      CanonicalizeForHash(procedure) + "\n" + CanonicalizeForHash(action) + "\n" +
      CanonicalizeForHash(spec) + "\n" + CanonicalizeForHash(instructions);
  return EncodeBase32(HashTo80Bits(canonical));
}

std::string ComputeInstanceId(const std::string& instance_principal) {
  return EncodeBase32(HashTo80Bits(CanonicalizeForHash(instance_principal)));
}

std::string ComputeEntityId(const std::string& entity_principal) {
  const std::string normalized = CanonicalizeForHash(entity_principal);
  const std::string salt_normalized = CanonicalizeForHash(g_entity_salt);
  return EncodeBase32(HashTo80Bits(normalized + "\n" + salt_normalized));
}

void SetEntitySalt(const std::string& salt) {
  g_entity_salt = salt;
}

std::string ComposeAddressId(const std::string& slug_id, const std::string& instance_id,
                             bool include_separator) {
  if (include_separator) {
    return slug_id + std::string{kAddressSeparator} + instance_id;
  }
  return slug_id + instance_id;
}

std::string CurrentTimestampIsoUtc() {
  const auto now = std::chrono::system_clock::now();
  const std::time_t time = std::chrono::system_clock::to_time_t(now);
  std::tm tm_snapshot{};
#if defined(_WIN32)
  gmtime_s(&tm_snapshot, &time);
#else
  gmtime_r(&time, &tm_snapshot);
#endif
  std::ostringstream oss;
  oss << std::put_time(&tm_snapshot, "%Y-%m-%dT%H:%M:%SZ");
  return oss.str();
}

ChecklistStore::ChecklistStore(std::string db_path) : db_path_(std::move(db_path)) {}

ChecklistStore::~ChecklistStore() {
  if (db_) {
    sqlite3_close(db_);
    db_ = nullptr;
  }
}

void ChecklistStore::Initialize(bool seed_demo_data) {
  namespace fs = std::filesystem;
  const fs::path db_location(db_path_);
  if (db_location.has_parent_path()) {
    fs::create_directories(db_location.parent_path());
  }

  const int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX;
  const int rc = sqlite3_open_v2(db_path_.c_str(), &db_, flags, nullptr);
  if (rc != SQLITE_OK) {
    throw std::runtime_error("Failed to open SQLite database at " + db_path_ + ": " +
                             sqlite3_errstr(rc));
  }

  {
    char* errmsg = nullptr;
    sqlite3_exec(db_, "PRAGMA foreign_keys=ON;", nullptr, nullptr, &errmsg);
    if (errmsg) {
      std::string message = errmsg;
      sqlite3_free(errmsg);
      throw std::runtime_error("Failed to enable foreign_keys: " + message);
    }
  }

  {
    char* errmsg = nullptr;
    sqlite3_exec(db_, "PRAGMA journal_mode=WAL;", nullptr, nullptr, &errmsg);
    if (errmsg) {
      LogError(std::string{"Could not set WAL journal mode: "} + errmsg);
      sqlite3_free(errmsg);
    }
  }

  EnsureSchema();

  if (seed_demo_data && !HasAnySlugs()) {
    SeedDemoData();
  }
}

void ChecklistStore::EnsureSchema() {
  bool needs_reset = false;
  try {
    const auto columns = TableColumns(db_, "slugs");
    const bool has_expected =
        HasColumn(columns, "address_id") && HasColumn(columns, "slug_id") &&
        HasColumn(columns, "instance_id") && HasColumn(columns, "checklist") &&
        HasColumn(columns, "section") && HasColumn(columns, "procedure") &&
        HasColumn(columns, "action") && HasColumn(columns, "spec") &&
        HasColumn(columns, "instructions") && HasColumn(columns, "entity_id");
    if (!columns.empty() && !has_expected) {
      needs_reset = true;
    }
  } catch (...) {
    needs_reset = true;
  }

  if (needs_reset) {
    LogInfo("Dropping legacy schema to apply CHAX-compliant tables");
    const char *drop_sql = "DROP TABLE IF EXISTS template_relationships;"
                           "DROP TABLE IF EXISTS address_relationships;"
                           "DROP TABLE IF EXISTS predicates;"
                           "DROP TABLE IF EXISTS history;"
                           "DROP TABLE IF EXISTS address_ownership;"
                           "DROP TABLE IF EXISTS slug_ownership;"
                           "DROP TABLE IF EXISTS slugs;"
                           "DROP TABLE IF EXISTS slug_order;"
                           "DROP TABLE IF EXISTS entities;"
                           "DROP TABLE IF EXISTS instance_catalog;";
    char* errmsg = nullptr;
    if (sqlite3_exec(db_, drop_sql, nullptr, nullptr, &errmsg) != SQLITE_OK) {
      std::string message = errmsg ? errmsg : "";
      sqlite3_free(errmsg);
      throw std::runtime_error("Failed to drop legacy tables: " + message);
    }
  }

  const char *kSchema = R"sql(
    CREATE TABLE IF NOT EXISTS entities (
        entity_id    TEXT PRIMARY KEY,
        principal    TEXT NOT NULL,
        kind         TEXT NOT NULL,
        display_name TEXT,
        meta         TEXT
    );

    CREATE TABLE IF NOT EXISTS instance_catalog (
        instance_id  TEXT PRIMARY KEY,
        principal    TEXT NOT NULL,
        label        TEXT,
        meta         TEXT
    );

    CREATE TABLE IF NOT EXISTS slugs (
        address_id    TEXT PRIMARY KEY,
        slug_id       TEXT NOT NULL,
        instance_id   TEXT NOT NULL,
        checklist     TEXT NOT NULL,
        section       TEXT NOT NULL,
        procedure     TEXT NOT NULL,
        action        TEXT NOT NULL,
        spec          TEXT NOT NULL,
        instructions  TEXT NOT NULL,
        result        TEXT,
        status        TEXT CHECK (status IN ('Pass','Fail','NA','Other','Unknown')) NOT NULL,
        comment       TEXT,
        timestamp     TEXT,
        entity_id     TEXT NOT NULL,
        FOREIGN KEY(entity_id) REFERENCES entities(entity_id)
    );

    CREATE TABLE IF NOT EXISTS slug_order (
        address_id    TEXT PRIMARY KEY,
        address_order INTEGER NOT NULL,
        FOREIGN KEY(address_id) REFERENCES slugs(address_id) ON DELETE CASCADE
    );

    CREATE TABLE IF NOT EXISTS slug_ownership (
        slug_id       TEXT NOT NULL,
        checklist     TEXT NOT NULL,
        source_name   TEXT NOT NULL,
        source_path   TEXT,
        pack          TEXT NOT NULL,
        checklist_dir TEXT NOT NULL,
        updated_at    TEXT NOT NULL,
        PRIMARY KEY (slug_id, source_name, pack, checklist_dir)
    );

    CREATE TABLE IF NOT EXISTS address_ownership (
        address_id    TEXT NOT NULL,
        slug_id       TEXT NOT NULL,
        instance_id   TEXT NOT NULL,
        checklist     TEXT NOT NULL,
        source_name   TEXT NOT NULL,
        source_path   TEXT,
        pack          TEXT NOT NULL,
        checklist_dir TEXT NOT NULL,
        updated_at    TEXT NOT NULL,
        FOREIGN KEY(address_id) REFERENCES slugs(address_id) ON DELETE CASCADE,
        PRIMARY KEY (address_id, source_name, pack, checklist_dir)
    );

    CREATE TABLE IF NOT EXISTS template_relationships (
        subject_slug_id  TEXT NOT NULL,
        predicate        TEXT NOT NULL,
        target_slug_id   TEXT NOT NULL
    );

    CREATE TABLE IF NOT EXISTS address_relationships (
        subject_address_id  TEXT NOT NULL,
        predicate           TEXT NOT NULL,
        target_address_id   TEXT NOT NULL,
        FOREIGN KEY(subject_address_id) REFERENCES slugs(address_id) ON DELETE CASCADE,
        FOREIGN KEY(target_address_id)  REFERENCES slugs(address_id) ON DELETE CASCADE
    );

    CREATE TABLE IF NOT EXISTS predicates (
        name        TEXT PRIMARY KEY,
        kind        TEXT NOT NULL,
        status      TEXT NOT NULL,
        description TEXT,
        meta        TEXT
    );

    CREATE TABLE IF NOT EXISTS history (
        address_id  TEXT NOT NULL,
        timestamp   TEXT NOT NULL,
        result      TEXT,
        status      TEXT,
        comment     TEXT,
        entity_id   TEXT NOT NULL,
        FOREIGN KEY(address_id) REFERENCES slugs(address_id) ON DELETE CASCADE,
        FOREIGN KEY(entity_id)  REFERENCES entities(entity_id),
        PRIMARY KEY (address_id, timestamp)
    );

    CREATE INDEX IF NOT EXISTS idx_slugs_slug_id       ON slugs(slug_id);
    CREATE INDEX IF NOT EXISTS idx_slugs_instance_id   ON slugs(instance_id);
    CREATE INDEX IF NOT EXISTS idx_slugs_checklist     ON slugs(checklist);
    CREATE INDEX IF NOT EXISTS idx_slug_order_order    ON slug_order(address_order);
    CREATE INDEX IF NOT EXISTS idx_slug_ownership_owner
        ON slug_ownership(checklist, source_name, pack, checklist_dir);
    CREATE INDEX IF NOT EXISTS idx_address_ownership_owner
        ON address_ownership(checklist, instance_id, source_name, pack, checklist_dir);
    CREATE INDEX IF NOT EXISTS idx_address_ownership_slug
        ON address_ownership(slug_id, instance_id);
    CREATE INDEX IF NOT EXISTS idx_trel_subject        ON template_relationships(subject_slug_id);
    CREATE INDEX IF NOT EXISTS idx_trel_target         ON template_relationships(target_slug_id);
    CREATE INDEX IF NOT EXISTS idx_arel_subject        ON address_relationships(subject_address_id);
    CREATE INDEX IF NOT EXISTS idx_arel_target         ON address_relationships(target_address_id);
    CREATE INDEX IF NOT EXISTS idx_predicates_status   ON predicates(status);
    CREATE INDEX IF NOT EXISTS idx_history_entity      ON history(entity_id);
  )sql";

  char* errmsg = nullptr;
  const int rc = sqlite3_exec(db_, kSchema, nullptr, nullptr, &errmsg);
  if (rc != SQLITE_OK) {
    std::string message = errmsg ? errmsg : "";
    sqlite3_free(errmsg);
    throw std::runtime_error("Failed to initialize schema: " + message);
  }

  auto seed_default_predicates = [&]() {
    struct Seed {
      const char* name;
      const char* kind;
      const char* status;
      const char* description;
    };
      constexpr Seed kSeeds[] = {
          {"passPropagateValidatedPass", "canonical", "active",
           "When subject is Pass, propagate Pass to target (reference daemon)."},
          {"passPropagateAndGatePass", "canonical", "active",
           "When all matching incoming passPropagateAndGatePass relationships are active, "
           "propagate Pass to target (reference daemon)."},
          {"passPropagateOrGatePass", "canonical", "active",
           "When any matching incoming passPropagateOrGatePass relationship is active, "
           "propagate Pass to target (reference daemon)."},
          {"failPropagateValidatedFail", "canonical", "active",
           "When subject is Fail, propagate Fail to target (reference daemon)."},
          {"BoolVerifyValidatedStatus", "canonical", "active",
           "Verify spec/result and map predicate_bool to status (true->Pass, false->Fail)."},
          {"BoolVerifyAndGateStatus", "canonical", "active",
           "Verify gate: all contributors must evaluate true to set Pass; false drives Fail."},
          {"BoolVerifyOrGateStatus", "canonical", "active",
           "Verify gate: any true contributor sets Pass; all false drive Fail."},
          {"passSyncValidatedPass", "canonical", "active",
           "When active, enforce subject/target status equality (reference daemon)."},
          {"slugSuccessor", "extension", "active",
           "Template lineage: subject slug precedes target slug."},
          {"slugPredecessor", "extension", "active",
           "Template lineage: subject slug succeeds target slug (reverse edge)."},
          {"addressSuccessor", "extension", "active",
           "Address lineage: subject address precedes target address."},
          {"addressPredecessor", "extension", "active",
           "Address lineage: subject address succeeds target address (reverse edge)."},
      };
    sqlite3_stmt* stmt = nullptr;
    const std::string sql =
        "INSERT INTO predicates (name, kind, status, description, meta) "
        "VALUES (?, ?, ?, ?, '') ON CONFLICT(name) DO NOTHING;";
    if (Prepare(db_, sql, &stmt) != SQLITE_OK) {
      Finalize(stmt);
      throw std::runtime_error("Failed to prepare predicate seed statement");
    }
    for (const auto& seed : kSeeds) {
      sqlite3_bind_text(stmt, 1, seed.name, -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(stmt, 2, seed.kind, -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(stmt, 3, seed.status, -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(stmt, 4, seed.description, -1, SQLITE_TRANSIENT);
      StepOrThrow(stmt, "seed predicates");
      sqlite3_reset(stmt);
      sqlite3_clear_bindings(stmt);
    }
    Finalize(stmt);
  };
  seed_default_predicates();
  BackfillSlugOrder(db_);
}

bool ChecklistStore::HasAnySlugs() const {
  std::lock_guard<std::mutex> lock(mutex_);
  sqlite3_stmt* stmt = nullptr;
  if (Prepare(db_, "SELECT 1 FROM slugs LIMIT 1;", &stmt) != SQLITE_OK) {
    Finalize(stmt);
    throw std::runtime_error("Failed to prepare slug count query");
  }

  const int rc = sqlite3_step(stmt);
  Finalize(stmt);
  return rc == SQLITE_ROW;
}

void ChecklistStore::SeedDemoData() {
  LogInfo("Seeding CHAX demo checklist data");
  const std::string entity_id = DefaultEntityId();
  EnsureEntity(db_, entity_id, std::string{kDefaultEntityPrincipal},
               std::string{kDefaultEntityKind}, std::string{kDefaultEntityDisplay});

  const std::string instance_principal = "demo||instance=seed";
  const std::string instance_id = ComputeInstanceId(instance_principal);

  ChecklistSlug power_check{
      /*slug_id=*/{},
      /*instance_id=*/instance_id,
      /*instance_principal=*/instance_principal,
      /*address_id=*/{},
      /*address_order=*/0,
      /*entity_principal=*/{},
      /*checklist=*/"chax-demo",
      /*section=*/"Site Readiness",
      /*procedure=*/"Power Bring-up",
      /*action=*/"Verify rack power",
      /*spec=*/"24V DC stable",
      /*result=*/"24.1V",
      /*status=*/ChecklistStatus::kPass,
      /*comment=*/"Measured at supply taps.",
      /*timestamp=*/CurrentTimestampIsoUtc(),
      /*entity_id=*/entity_id,
      /*instructions=*/"Use calibrated multimeter on the main supply rails.",
      /*relationships=*/{}};

  ChecklistSlug uplink_check{
      /*slug_id=*/{},
      /*instance_id=*/instance_id,
      /*instance_principal=*/instance_principal,
      /*address_id=*/{},
      /*address_order=*/0,
      /*entity_principal=*/{},
      /*checklist=*/"chax-demo",
      /*section=*/"Networking",
      /*procedure=*/"Switch bring-up",
      /*action=*/"Verify uplink",
      /*spec=*/"1GbE link up",
      /*result=*/"",
      /*status=*/ChecklistStatus::kFail,
      /*comment=*/"No link LED on port 48.",
      /*timestamp=*/CurrentTimestampIsoUtc(),
      /*entity_id=*/entity_id,
      /*instructions=*/"Patch into the core switch, check SFP seating, and confirm VLAN tagging.",
      /*relationships=*/{}};

  ChecklistSlug dhcp_check{
      /*slug_id=*/{},
      /*instance_id=*/instance_id,
      /*instance_principal=*/instance_principal,
      /*address_id=*/{},
      /*address_order=*/0,
      /*entity_principal=*/{},
      /*checklist=*/"chax-demo",
      /*section=*/"Networking",
      /*procedure=*/"DHCP scope",
      /*action=*/"Request lease",
      /*spec=*/"Lease within 1s, correct gateway",
      /*result=*/"pending",
      /*status=*/ChecklistStatus::kNA,
      /*comment=*/"Waiting on uplink resolution.",
      /*timestamp=*/CurrentTimestampIsoUtc(),
      /*entity_id=*/entity_id,
      /*instructions=*/"Use iperf client once uplink is established to verify end-to-end path.",
      /*relationships=*/{}};

  power_check.slug_id = ComputeSlugId(power_check.checklist, power_check.section,
                                      power_check.procedure, power_check.action,
                                      power_check.spec, power_check.instructions);
  uplink_check.slug_id = ComputeSlugId(uplink_check.checklist, uplink_check.section,
                                       uplink_check.procedure, uplink_check.action,
                                       uplink_check.spec, uplink_check.instructions);
  dhcp_check.slug_id = ComputeSlugId(dhcp_check.checklist, dhcp_check.section,
                                     dhcp_check.procedure, dhcp_check.action,
                                     dhcp_check.spec, dhcp_check.instructions);

  power_check.address_id = ComposeAddressId(power_check.slug_id, power_check.instance_id);
  uplink_check.address_id = ComposeAddressId(uplink_check.slug_id, uplink_check.instance_id);
  dhcp_check.address_id = ComposeAddressId(dhcp_check.slug_id, dhcp_check.instance_id);

  uplink_check.relationships = {RelationshipEdge{"passVerifyValidatedPass", power_check.address_id}};
  dhcp_check.relationships = {RelationshipEdge{"passVerifyValidatedPass", uplink_check.address_id}};

  UpsertSlug(power_check);
  UpsertSlug(uplink_check);
  UpsertSlug(dhcp_check);

  ReplaceRelationships(power_check.address_id, power_check.relationships);
  ReplaceRelationships(uplink_check.address_id, uplink_check.relationships);
  ReplaceRelationships(dhcp_check.address_id, dhcp_check.relationships);
}
void ChecklistStore::UpsertSlug(const ChecklistSlug& slug) {
  std::lock_guard<std::mutex> lock(mutex_);
  UpsertSlugUnlocked(slug);
}

void ChecklistStore::UpsertSlugUnlocked(const ChecklistSlug& slug) {
  if (slug.checklist.empty() || slug.section.empty() || slug.procedure.empty() ||
      slug.action.empty() || slug.spec.empty()) {
    throw std::invalid_argument("Slug template fields must not be empty.");
  }

  ChecklistSlug normalized = slug;
  if (normalized.slug_id.empty()) {
    normalized.slug_id = ComputeSlugId(normalized.checklist, normalized.section,
                                       normalized.procedure, normalized.action,
                                       normalized.spec, normalized.instructions);
  }
  if (normalized.instance_principal.empty()) {
    normalized.instance_principal = "instance||default";
  }
  if (normalized.instance_id.empty()) {
    normalized.instance_id = ComputeInstanceId(normalized.instance_principal);
  }
  normalized.address_id = ComposeAddressId(normalized.slug_id, normalized.instance_id);
  if (!IsValidBase32Id(normalized.slug_id, 16) || !IsValidBase32Id(normalized.instance_id, 16)) {
    throw std::invalid_argument("Invalid slug_id or instance_id (expected 16-char Base32).");
  }
  if (normalized.entity_id.empty()) {
    normalized.entity_id = DefaultEntityId();
  }
  if (!normalized.entity_principal.empty() && normalized.entity_id == DefaultEntityId()) {
    normalized.entity_id = ComputeEntityId(normalized.entity_principal);
  }
  if (!normalized.entity_principal.empty() && normalized.entity_id == DefaultEntityId()) {
    normalized.entity_id = ComputeEntityId(normalized.entity_principal);
  }
  if (normalized.timestamp.empty()) {
    normalized.timestamp = CurrentTimestampIsoUtc();
  }
  normalized.result = NormalizeResultToSpecUnit(normalized.spec, normalized.result);

  EnsureInstanceRecordUnlocked(normalized.instance_principal, std::string{}, std::string{});
  EnsureEntity(db_, normalized.entity_id,
               normalized.entity_principal.empty() ? std::string{kDefaultEntityPrincipal}
                                                   : normalized.entity_principal,
               std::string{kDefaultEntityKind}, std::string{kDefaultEntityDisplay});

  const auto existing_order = LookupAddressOrder(db_, normalized.address_id);
  const bool is_new_slug = !existing_order.has_value();
  const std::int64_t resolved_order = ResolveAddressOrder(db_, normalized, existing_order);

  sqlite3_stmt* stmt = nullptr;
  const std::string sql =
      "INSERT INTO slugs (address_id, slug_id, instance_id, checklist, section, procedure, action, "
      "spec, instructions, result, status, comment, timestamp, entity_id) "
      "VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?) "
      "ON CONFLICT(address_id) DO UPDATE SET result=excluded.result, status=excluded.status, "
      "comment=excluded.comment, timestamp=excluded.timestamp, entity_id=excluded.entity_id, "
      "instructions=excluded.instructions;";

  if (Prepare(db_, sql, &stmt) != SQLITE_OK) {
    Finalize(stmt);
    throw std::runtime_error(std::string{"Failed to prepare slug upsert: "} +
                             sqlite3_errmsg(db_));
  }

  sqlite3_bind_text(stmt, 1, normalized.address_id.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 2, normalized.slug_id.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 3, normalized.instance_id.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 4, normalized.checklist.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 5, normalized.section.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 6, normalized.procedure.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 7, normalized.action.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 8, normalized.spec.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 9, normalized.instructions.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 10, normalized.result.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 11, StatusToString(normalized.status).c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 12, normalized.comment.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 13, normalized.timestamp.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 14, normalized.entity_id.c_str(), -1, SQLITE_TRANSIENT);

  StepOrThrow(stmt, "slug upsert");
  Finalize(stmt);
  InsertHistorySnapshot(normalized);
  UpsertSlugOrder(db_, normalized.address_id, resolved_order);
  InheritTemplateOwnership(db_, normalized);

  if (is_new_slug) {
    const std::string template_instance_id = ComputeInstanceId("template||default");
    if (normalized.instance_id != template_instance_id) {
      const auto template_relationships =
          LoadTemplateRelationshipsForChecklist(db_, normalized.checklist);
      if (!template_relationships.empty()) {
        const auto successors = BuildSlugSuccessorIndex(template_relationships);
        const auto resolve_for_instance =
            [&](const std::string& slug_id) -> std::optional<std::string> {
          if (!IsValidBase32Id(slug_id, 16)) {
            return std::nullopt;
          }
          if (HasSlugForInstanceUnlocked(db_, slug_id, normalized.instance_id)) {
            return slug_id;
          }
          const auto latest = ResolveLatestSlugId(successors, slug_id);
          if (!latest || *latest == slug_id) {
            return std::nullopt;
          }
          if (HasSlugForInstanceUnlocked(db_, *latest, normalized.instance_id)) {
            return *latest;
          }
          return std::nullopt;
        };
        const auto resolves_to_self = [&](const std::string& slug_id) -> bool {
          const auto latest = ResolveLatestSlugId(successors, slug_id);
          return latest && *latest == normalized.slug_id;
        };

        std::vector<TemplateRelationship> subject_rels;
        std::vector<TemplateRelationship> target_rels;
        subject_rels.reserve(template_relationships.size());
        target_rels.reserve(template_relationships.size());
        for (const auto& rel : template_relationships) {
          if (resolves_to_self(rel.subject_slug_id)) {
            subject_rels.push_back(rel);
          }
          if (resolves_to_self(rel.target_slug_id)) {
            target_rels.push_back(rel);
          }
        }

        if (!subject_rels.empty()) {
          const auto existing_keys = LoadAddressRelationshipKeys(db_, normalized.address_id);
          sqlite3_stmt* insert_stmt = nullptr;
          if (Prepare(db_,
                      "INSERT INTO address_relationships (subject_address_id, predicate, "
                      "target_address_id) VALUES (?,?,?);",
                      &insert_stmt) != SQLITE_OK) {
            Finalize(insert_stmt);
            throw std::runtime_error("Failed to prepare address relationship insert");
          }
          for (const auto& rel : subject_rels) {
            auto resolved_target = resolve_for_instance(rel.target_slug_id);
            if (!resolved_target) {
              continue;
            }
            if (*resolved_target != rel.target_slug_id) {
              LogInfo("Template relationship target lineage alias " + rel.target_slug_id + " -> " +
                      *resolved_target);
            }
            const std::string target_address =
                ComposeAddressId(*resolved_target, normalized.instance_id);
            const std::string key = rel.predicate + "|" + target_address;
            if (existing_keys.find(key) != existing_keys.end()) {
              continue;
            }
            sqlite3_reset(insert_stmt);
            sqlite3_bind_text(insert_stmt, 1, normalized.address_id.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(insert_stmt, 2, rel.predicate.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(insert_stmt, 3, target_address.c_str(), -1, SQLITE_TRANSIENT);
            StepOrThrow(insert_stmt, "address relationship insert");
          }
          Finalize(insert_stmt);
        }

        if (!target_rels.empty()) {
          sqlite3_stmt* insert_stmt = nullptr;
          if (Prepare(db_,
                      "INSERT INTO address_relationships (subject_address_id, predicate, "
                      "target_address_id) VALUES (?,?,?);",
                      &insert_stmt) != SQLITE_OK) {
            Finalize(insert_stmt);
            throw std::runtime_error("Failed to prepare address relationship insert");
          }
          std::unordered_map<std::string, std::unordered_set<std::string>> existing_by_subject;
          for (const auto& rel : target_rels) {
            auto resolved_subject = resolve_for_instance(rel.subject_slug_id);
            if (!resolved_subject) {
              continue;
            }
            if (*resolved_subject != rel.subject_slug_id) {
              LogInfo("Template relationship subject lineage alias " + rel.subject_slug_id +
                      " -> " + *resolved_subject);
            }
            const std::string subject_address =
                ComposeAddressId(*resolved_subject, normalized.instance_id);
            auto& keys = existing_by_subject[subject_address];
            if (keys.empty()) {
              keys = LoadAddressRelationshipKeys(db_, subject_address);
            }
            const std::string key = rel.predicate + "|" + normalized.address_id;
            if (keys.find(key) != keys.end()) {
              continue;
            }
            sqlite3_reset(insert_stmt);
            sqlite3_bind_text(insert_stmt, 1, subject_address.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(insert_stmt, 2, rel.predicate.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(insert_stmt, 3, normalized.address_id.c_str(), -1, SQLITE_TRANSIENT);
            StepOrThrow(insert_stmt, "address relationship insert");
            keys.insert(key);
          }
          Finalize(insert_stmt);
        }
      }
    }
  }
}

bool ChecklistStore::CreateSlugIfMissing(const ChecklistSlug& slug) {
  ChecklistSlug normalized = slug;
  if (normalized.checklist.empty() || normalized.section.empty() || normalized.procedure.empty() ||
      normalized.action.empty() || normalized.spec.empty()) {
    throw std::invalid_argument("Slug template fields must not be empty.");
  }

  if (normalized.slug_id.empty()) {
    normalized.slug_id = ComputeSlugId(normalized.checklist, normalized.section,
                                       normalized.procedure, normalized.action, normalized.spec,
                                       normalized.instructions);
  }
  if (normalized.instance_principal.empty()) {
    normalized.instance_principal = "instance||default";
  }
  if (normalized.instance_id.empty()) {
    normalized.instance_id = ComputeInstanceId(normalized.instance_principal);
  }
  normalized.address_id = ComposeAddressId(normalized.slug_id, normalized.instance_id);
  if (!IsValidBase32Id(normalized.slug_id, 16) || !IsValidBase32Id(normalized.instance_id, 16)) {
    throw std::invalid_argument("Invalid slug_id or instance_id (expected 16-char Base32).");
  }
  if (normalized.entity_id.empty()) {
    normalized.entity_id = DefaultEntityId();
  }
  if (normalized.timestamp.empty()) {
    normalized.timestamp = CurrentTimestampIsoUtc();
  }
  normalized.result = NormalizeResultToSpecUnit(normalized.spec, normalized.result);

  std::lock_guard<std::mutex> lock(mutex_);
  EnsureInstanceRecordUnlocked(normalized.instance_principal, std::string{}, std::string{});
  EnsureEntity(db_, normalized.entity_id,
               normalized.entity_principal.empty() ? std::string{kDefaultEntityPrincipal}
                                                   : normalized.entity_principal,
               std::string{kDefaultEntityKind}, std::string{kDefaultEntityDisplay});

  const auto existing_order = LookupAddressOrder(db_, normalized.address_id);
  const std::int64_t resolved_order = ResolveAddressOrder(db_, normalized, existing_order);

  sqlite3_stmt* stmt = nullptr;
  const std::string sql =
      "INSERT INTO slugs (address_id, slug_id, instance_id, checklist, section, procedure, action, "
      "spec, instructions, result, status, comment, timestamp, entity_id) "
      "VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?) "
      "ON CONFLICT(address_id) DO NOTHING;";

  if (Prepare(db_, sql, &stmt) != SQLITE_OK) {
    Finalize(stmt);
    throw std::runtime_error(std::string{"Failed to prepare slug insert: "} + sqlite3_errmsg(db_));
  }

  sqlite3_bind_text(stmt, 1, normalized.address_id.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 2, normalized.slug_id.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 3, normalized.instance_id.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 4, normalized.checklist.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 5, normalized.section.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 6, normalized.procedure.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 7, normalized.action.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 8, normalized.spec.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 9, normalized.instructions.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 10, normalized.result.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 11, StatusToString(normalized.status).c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 12, normalized.comment.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 13, normalized.timestamp.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 14, normalized.entity_id.c_str(), -1, SQLITE_TRANSIENT);

  StepOrThrow(stmt, "slug insert");
  const bool inserted = sqlite3_changes(db_) > 0;
  Finalize(stmt);
  if (inserted) {
    InsertHistorySnapshot(normalized);
  }
  if (inserted || normalized.address_order > 0 || !existing_order) {
    UpsertSlugOrder(db_, normalized.address_id, resolved_order);
  }
  InheritTemplateOwnership(db_, normalized);
  return inserted;
}

void ChecklistStore::UpdateTemplateFieldsForSlug(const ChecklistSlug& slug) {
  if (slug.slug_id.empty()) {
    throw std::invalid_argument("Slug id must not be empty.");
  }
  if (slug.checklist.empty() || slug.section.empty() || slug.procedure.empty() ||
      slug.action.empty() || slug.spec.empty()) {
    throw std::invalid_argument("Slug template fields must not be empty.");
  }

  std::lock_guard<std::mutex> lock(mutex_);

  sqlite3_stmt* stmt = nullptr;
  const std::string sql =
      "UPDATE slugs SET checklist=?, section=?, procedure=?, action=?, spec=?, instructions=? "
      "WHERE slug_id=?;";
  if (Prepare(db_, sql, &stmt) != SQLITE_OK) {
    Finalize(stmt);
    throw std::runtime_error(std::string{"Failed to prepare template field update: "} +
                             sqlite3_errmsg(db_));
  }

  sqlite3_bind_text(stmt, 1, slug.checklist.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 2, slug.section.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 3, slug.procedure.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 4, slug.action.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 5, slug.spec.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 6, slug.instructions.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 7, slug.slug_id.c_str(), -1, SQLITE_TRANSIENT);

  StepOrThrow(stmt, "template field update");
  Finalize(stmt);
}

void ChecklistStore::ReplaceRelationships(const std::string& subject_id,
                                          const std::vector<RelationshipEdge>& edges) {
  std::lock_guard<std::mutex> lock(mutex_);

  char* errmsg = nullptr;
  sqlite3_exec(db_, "BEGIN IMMEDIATE;", nullptr, nullptr, &errmsg);
  if (errmsg) {
    std::string message = errmsg;
    sqlite3_free(errmsg);
    throw std::runtime_error("Failed to begin transaction for relationships: " + message);
  }

  sqlite3_stmt* delete_stmt = nullptr;
  if (Prepare(db_, "DELETE FROM address_relationships WHERE subject_address_id=?;",
              &delete_stmt) != SQLITE_OK) {
    Finalize(delete_stmt);
    sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
    throw std::runtime_error("Failed to prepare relationship delete");
  }
  sqlite3_bind_text(delete_stmt, 1, subject_id.c_str(), -1, SQLITE_TRANSIENT);
  StepOrThrow(delete_stmt, "relationship delete");
  Finalize(delete_stmt);

  sqlite3_stmt* insert_stmt = nullptr;
  if (Prepare(db_, "INSERT INTO address_relationships (subject_address_id, predicate, "
                   "target_address_id) VALUES (?,?,?);",
              &insert_stmt) != SQLITE_OK) {
    Finalize(insert_stmt);
    sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
    throw std::runtime_error("Failed to prepare relationship insert");
  }

  for (const auto& edge : edges) {
    sqlite3_reset(insert_stmt);
    sqlite3_bind_text(insert_stmt, 1, subject_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(insert_stmt, 2, edge.predicate.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(insert_stmt, 3, edge.target.c_str(), -1, SQLITE_TRANSIENT);
    StepOrThrow(insert_stmt, "relationship insert");
  }

  Finalize(insert_stmt);
  sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr);
}

void ChecklistStore::UpsertOwnership(const ChecklistSlug &slug, const ChecklistOwnership &ownership) {
  ChecklistSlug normalized = slug;
  if (normalized.slug_id.empty()) {
    normalized.slug_id = ComputeSlugId(normalized.checklist, normalized.section, normalized.procedure,
                                       normalized.action, normalized.spec, normalized.instructions);
  }
  if (normalized.instance_id.empty()) {
    normalized.instance_id =
        ComputeInstanceId(normalized.instance_principal.empty() ? "instance||default" : normalized.instance_principal);
  }
  normalized.address_id = ComposeAddressId(normalized.slug_id, normalized.instance_id);
  const ChecklistOwnership normalized_owner = NormalizeOwnershipForWrite(ownership, normalized);
  const std::string timestamp = CurrentTimestampIsoUtc();

  std::lock_guard<std::mutex> lock(mutex_);
  sqlite3_stmt *slug_stmt = nullptr;
  sqlite3_stmt *address_stmt = nullptr;
  try {
    const std::string slug_sql = "INSERT INTO slug_ownership (slug_id, checklist, source_name, source_path, pack, "
                                 "checklist_dir, updated_at) VALUES (?,?,?,?,?,?,?) "
                                 "ON CONFLICT(slug_id, source_name, pack, checklist_dir) DO UPDATE SET "
                                 "checklist=excluded.checklist, source_path=excluded.source_path, "
                                 "updated_at=excluded.updated_at;";
    if (Prepare(db_, slug_sql, &slug_stmt) != SQLITE_OK) {
      throw std::runtime_error("Failed to prepare slug ownership upsert.");
    }
    sqlite3_bind_text(slug_stmt, 1, normalized.slug_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(slug_stmt, 2, normalized_owner.checklist.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(slug_stmt, 3, normalized_owner.source_name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(slug_stmt, 4, normalized_owner.source_path.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(slug_stmt, 5, normalized_owner.pack.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(slug_stmt, 6, normalized_owner.checklist_dir.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(slug_stmt, 7, timestamp.c_str(), -1, SQLITE_TRANSIENT);
    StepOrThrow(slug_stmt, "slug ownership upsert");
    Finalize(slug_stmt);
    slug_stmt = nullptr;

    const std::string address_sql = "INSERT INTO address_ownership (address_id, slug_id, instance_id, checklist, "
                                    "source_name, source_path, pack, checklist_dir, updated_at) "
                                    "VALUES (?,?,?,?,?,?,?,?,?) "
                                    "ON CONFLICT(address_id, source_name, pack, checklist_dir) DO UPDATE SET "
                                    "slug_id=excluded.slug_id, instance_id=excluded.instance_id, "
                                    "checklist=excluded.checklist, source_path=excluded.source_path, "
                                    "updated_at=excluded.updated_at;";
    if (Prepare(db_, address_sql, &address_stmt) != SQLITE_OK) {
      throw std::runtime_error("Failed to prepare address ownership upsert.");
    }
    sqlite3_bind_text(address_stmt, 1, normalized.address_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(address_stmt, 2, normalized.slug_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(address_stmt, 3, normalized.instance_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(address_stmt, 4, normalized_owner.checklist.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(address_stmt, 5, normalized_owner.source_name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(address_stmt, 6, normalized_owner.source_path.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(address_stmt, 7, normalized_owner.pack.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(address_stmt, 8, normalized_owner.checklist_dir.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(address_stmt, 9, timestamp.c_str(), -1, SQLITE_TRANSIENT);
    StepOrThrow(address_stmt, "address ownership upsert");
    Finalize(address_stmt);
  } catch (...) {
    Finalize(slug_stmt);
    Finalize(address_stmt);
    throw;
  }
}

void ChecklistStore::DeleteOwnedInstance(const std::string &checklist, const std::string &instance_id,
                                         const ChecklistOwnership &ownership, int *deleted_slugs) {
  if (checklist.empty() || instance_id.empty()) {
    throw std::invalid_argument("Checklist and instance_id must not be empty.");
  }
  ChecklistSlug owner_slug;
  owner_slug.checklist = checklist;
  const ChecklistOwnership normalized_owner = NormalizeOwnershipForWrite(ownership, owner_slug);

  std::lock_guard<std::mutex> lock(mutex_);
  char *errmsg = nullptr;
  sqlite3_exec(db_, "BEGIN IMMEDIATE;", nullptr, nullptr, &errmsg);
  if (errmsg) {
    std::string message = errmsg;
    sqlite3_free(errmsg);
    throw std::runtime_error("Failed to begin transaction for owned instance delete: " + message);
  }

  auto rollback = [&]() { sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr); };

  sqlite3_stmt *delete_owner = nullptr;
  sqlite3_stmt *prune_slug_owner = nullptr;
  sqlite3_stmt *delete_orphan_history = nullptr;
  sqlite3_stmt *delete_orphan_slugs = nullptr;
  int removed = 0;
  try {
    const std::string owner_delete_sql = "DELETE FROM address_ownership "
                                         "WHERE checklist=? AND instance_id=? AND source_name=? AND pack=? "
                                         "AND checklist_dir=?;";
    if (Prepare(db_, owner_delete_sql, &delete_owner) != SQLITE_OK) {
      throw std::runtime_error("Failed to prepare address ownership delete.");
    }
    sqlite3_bind_text(delete_owner, 1, checklist.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(delete_owner, 2, instance_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(delete_owner, 3, normalized_owner.source_name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(delete_owner, 4, normalized_owner.pack.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(delete_owner, 5, normalized_owner.checklist_dir.c_str(), -1, SQLITE_TRANSIENT);
    StepOrThrow(delete_owner, "address ownership delete");
    Finalize(delete_owner);
    delete_owner = nullptr;

    const std::string slug_owner_sql = "DELETE FROM slug_ownership "
                                       "WHERE checklist=? AND source_name=? AND pack=? AND checklist_dir=? "
                                       "AND NOT EXISTS (SELECT 1 FROM address_ownership ao "
                                       "WHERE ao.slug_id=slug_ownership.slug_id "
                                       "AND ao.source_name=slug_ownership.source_name "
                                       "AND ao.pack=slug_ownership.pack "
                                       "AND ao.checklist_dir=slug_ownership.checklist_dir);";
    if (Prepare(db_, slug_owner_sql, &prune_slug_owner) != SQLITE_OK) {
      throw std::runtime_error("Failed to prepare slug ownership prune.");
    }
    sqlite3_bind_text(prune_slug_owner, 1, checklist.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(prune_slug_owner, 2, normalized_owner.source_name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(prune_slug_owner, 3, normalized_owner.pack.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(prune_slug_owner, 4, normalized_owner.checklist_dir.c_str(), -1, SQLITE_TRANSIENT);
    StepOrThrow(prune_slug_owner, "slug ownership prune");
    Finalize(prune_slug_owner);
    prune_slug_owner = nullptr;

    const std::string orphan_history_sql = "DELETE FROM history WHERE address_id IN ("
                                           "SELECT s.address_id FROM slugs s "
                                           "LEFT JOIN address_ownership ao ON ao.address_id=s.address_id "
                                           "WHERE s.checklist=? AND s.instance_id=? AND ao.address_id IS NULL);";
    if (Prepare(db_, orphan_history_sql, &delete_orphan_history) != SQLITE_OK) {
      throw std::runtime_error("Failed to prepare orphan history delete.");
    }
    sqlite3_bind_text(delete_orphan_history, 1, checklist.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(delete_orphan_history, 2, instance_id.c_str(), -1, SQLITE_TRANSIENT);
    StepOrThrow(delete_orphan_history, "orphan history delete");
    Finalize(delete_orphan_history);
    delete_orphan_history = nullptr;

    const std::string orphan_slugs_sql = "DELETE FROM slugs WHERE checklist=? AND instance_id=? "
                                         "AND NOT EXISTS (SELECT 1 FROM address_ownership ao "
                                         "WHERE ao.address_id=slugs.address_id);";
    if (Prepare(db_, orphan_slugs_sql, &delete_orphan_slugs) != SQLITE_OK) {
      throw std::runtime_error("Failed to prepare orphan slug delete.");
    }
    sqlite3_bind_text(delete_orphan_slugs, 1, checklist.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(delete_orphan_slugs, 2, instance_id.c_str(), -1, SQLITE_TRANSIENT);
    StepOrThrow(delete_orphan_slugs, "orphan slug delete");
    removed = sqlite3_changes(db_);
    Finalize(delete_orphan_slugs);
    delete_orphan_slugs = nullptr;

    sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr);
  } catch (...) {
    Finalize(delete_owner);
    Finalize(prune_slug_owner);
    Finalize(delete_orphan_history);
    Finalize(delete_orphan_slugs);
    rollback();
    throw;
  }

  if (deleted_slugs)
    *deleted_slugs = removed;
}

void ChecklistStore::InsertTemplateRelationship(const TemplateRelationship& rel) {
  if (rel.subject_slug_id.empty() || rel.predicate.empty() || rel.target_slug_id.empty()) {
    throw std::invalid_argument("Template relationship fields must be non-empty.");
  }
  std::lock_guard<std::mutex> lock(mutex_);
  sqlite3_stmt* stmt = nullptr;
  const std::string sql =
      "INSERT INTO template_relationships (subject_slug_id, predicate, target_slug_id) "
      "VALUES (?,?,?);";
  if (Prepare(db_, sql, &stmt) != SQLITE_OK) {
    Finalize(stmt);
    throw std::runtime_error("Failed to prepare template relationship insert");
  }
  sqlite3_bind_text(stmt, 1, rel.subject_slug_id.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 2, rel.predicate.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 3, rel.target_slug_id.c_str(), -1, SQLITE_TRANSIENT);
  StepOrThrow(stmt, "template relationship insert");
  Finalize(stmt);
}

void ChecklistStore::InsertAddressRelationship(const std::string& subject_address_id,
                                               const RelationshipEdge& edge) {
  if (subject_address_id.empty() || edge.predicate.empty() || edge.target.empty()) {
    throw std::invalid_argument("Address relationship fields must be non-empty.");
  }
  std::lock_guard<std::mutex> lock(mutex_);
  sqlite3_stmt* stmt = nullptr;
  const std::string sql =
      "INSERT INTO address_relationships (subject_address_id, predicate, target_address_id) "
      "VALUES (?,?,?);";
  if (Prepare(db_, sql, &stmt) != SQLITE_OK) {
    Finalize(stmt);
    throw std::runtime_error("Failed to prepare address relationship insert");
  }
  sqlite3_bind_text(stmt, 1, subject_address_id.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 2, edge.predicate.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 3, edge.target.c_str(), -1, SQLITE_TRANSIENT);
  StepOrThrow(stmt, "address relationship insert");
  Finalize(stmt);
}

void ChecklistStore::ReplaceTemplateRelationships(
    const std::vector<TemplateRelationship>& relationships) {
  if (relationships.empty()) {
    return;
  }

  std::unordered_map<std::string, std::vector<TemplateRelationship>> grouped;
  for (const auto& rel : relationships) {
    if (rel.subject_slug_id.empty() || rel.predicate.empty() || rel.target_slug_id.empty()) {
      continue;
    }
    grouped[rel.subject_slug_id].push_back(rel);
  }
  if (grouped.empty()) {
    return;
  }

  std::lock_guard<std::mutex> lock(mutex_);
  char* errmsg = nullptr;
  sqlite3_exec(db_, "BEGIN IMMEDIATE;", nullptr, nullptr, &errmsg);
  if (errmsg) {
    std::string message = errmsg;
    sqlite3_free(errmsg);
    throw std::runtime_error("Failed to begin transaction for template relationships: " + message);
  }

  sqlite3_stmt* delete_stmt = nullptr;
  sqlite3_stmt* insert_stmt = nullptr;

  auto rollback = [&]() { sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr); };

  try {
    const std::string delete_sql =
        "DELETE FROM template_relationships WHERE subject_slug_id=?;";
    if (Prepare(db_, delete_sql, &delete_stmt) != SQLITE_OK) {
      throw std::runtime_error("Failed to prepare template relationship delete.");
    }
    const std::string insert_sql =
        "INSERT INTO template_relationships (subject_slug_id, predicate, target_slug_id) "
        "VALUES (?,?,?);";
    if (Prepare(db_, insert_sql, &insert_stmt) != SQLITE_OK) {
      throw std::runtime_error("Failed to prepare template relationship insert.");
    }

    for (const auto& entry : grouped) {
      sqlite3_reset(delete_stmt);
      sqlite3_bind_text(delete_stmt, 1, entry.first.c_str(), -1, SQLITE_TRANSIENT);
      StepOrThrow(delete_stmt, "template relationship delete");

      for (const auto& rel : entry.second) {
        sqlite3_reset(insert_stmt);
        sqlite3_bind_text(insert_stmt, 1, entry.first.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(insert_stmt, 2, rel.predicate.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(insert_stmt, 3, rel.target_slug_id.c_str(), -1, SQLITE_TRANSIENT);
        StepOrThrow(insert_stmt, "template relationship insert");
      }
    }

    Finalize(delete_stmt);
    Finalize(insert_stmt);
    sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr);
  } catch (...) {
    Finalize(delete_stmt);
    Finalize(insert_stmt);
    rollback();
    throw;
  }
}

void ChecklistStore::ReplaceTemplateRelationshipsForSubjects(
    const std::vector<std::string>& subject_slug_ids,
    const std::vector<TemplateRelationship>& relationships) {
  if (subject_slug_ids.empty()) {
    return;
  }

  std::unordered_set<std::string> subjects;
  subjects.reserve(subject_slug_ids.size());
  for (const auto& subject : subject_slug_ids) {
    if (!subject.empty()) {
      subjects.insert(subject);
    }
  }
  if (subjects.empty()) {
    return;
  }

  std::unordered_map<std::string, std::vector<TemplateRelationship>> grouped;
  for (const auto& rel : relationships) {
    if (rel.subject_slug_id.empty() || rel.predicate.empty() || rel.target_slug_id.empty()) {
      continue;
    }
    if (subjects.find(rel.subject_slug_id) == subjects.end()) {
      continue;
    }
    grouped[rel.subject_slug_id].push_back(rel);
  }

  std::lock_guard<std::mutex> lock(mutex_);
  char* errmsg = nullptr;
  sqlite3_exec(db_, "BEGIN IMMEDIATE;", nullptr, nullptr, &errmsg);
  if (errmsg) {
    std::string message = errmsg;
    sqlite3_free(errmsg);
    throw std::runtime_error("Failed to begin transaction for template relationships: " + message);
  }

  sqlite3_stmt* delete_stmt = nullptr;
  sqlite3_stmt* insert_stmt = nullptr;

  auto rollback = [&]() { sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr); };

  try {
    const std::string delete_sql =
        "DELETE FROM template_relationships WHERE subject_slug_id=?;";
    if (Prepare(db_, delete_sql, &delete_stmt) != SQLITE_OK) {
      throw std::runtime_error("Failed to prepare template relationship delete.");
    }
    const std::string insert_sql =
        "INSERT INTO template_relationships (subject_slug_id, predicate, target_slug_id) "
        "VALUES (?,?,?);";
    if (Prepare(db_, insert_sql, &insert_stmt) != SQLITE_OK) {
      throw std::runtime_error("Failed to prepare template relationship insert.");
    }

    for (const auto& subject : subjects) {
      sqlite3_reset(delete_stmt);
      sqlite3_bind_text(delete_stmt, 1, subject.c_str(), -1, SQLITE_TRANSIENT);
      StepOrThrow(delete_stmt, "template relationship delete");

      const auto it = grouped.find(subject);
      if (it == grouped.end()) {
        continue;
      }
      for (const auto& rel : it->second) {
        sqlite3_reset(insert_stmt);
        sqlite3_bind_text(insert_stmt, 1, subject.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(insert_stmt, 2, rel.predicate.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(insert_stmt, 3, rel.target_slug_id.c_str(), -1, SQLITE_TRANSIENT);
        StepOrThrow(insert_stmt, "template relationship insert");
      }
    }

    Finalize(delete_stmt);
    Finalize(insert_stmt);
    sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr);
  } catch (...) {
    Finalize(delete_stmt);
    Finalize(insert_stmt);
    rollback();
    throw;
  }
}

ChecklistSlug ChecklistStore::GetSlugOrThrow(const std::string& address_id) const {
  std::lock_guard<std::mutex> lock(mutex_);
  sqlite3_stmt* stmt = nullptr;
  const std::string sql =
      "SELECT s.address_id, s.slug_id, s.instance_id, COALESCE(ic.principal, ''), "
      "s.checklist, s.section, s.procedure, s.action, s.spec, "
      "s.instructions, s.result, s.status, s.comment, s.timestamp, s.entity_id, "
      "COALESCE(so.address_order, 0) "
      "FROM slugs s "
      "LEFT JOIN instance_catalog ic ON s.instance_id = ic.instance_id "
      "LEFT JOIN slug_order so ON s.address_id = so.address_id "
      "WHERE s.address_id=?;";
  if (Prepare(db_, sql, &stmt) != SQLITE_OK) {
    Finalize(stmt);
    throw std::runtime_error("Failed to prepare slug lookup");
  }
  sqlite3_bind_text(stmt, 1, address_id.c_str(), -1, SQLITE_TRANSIENT);

  if (sqlite3_step(stmt) != SQLITE_ROW) {
    Finalize(stmt);
    throw std::runtime_error("Address ID not found: " + address_id);
  }

  ChecklistSlug slug = BuildSlug(stmt);
  Finalize(stmt);
  slug.relationships = LoadOutgoingAddressEdges(address_id);
  return slug;
}

ChecklistSlug ChecklistStore::GetSlugOrThrowUnlocked(const std::string& address_id) const {
  sqlite3_stmt* stmt = nullptr;
  const std::string sql =
      "SELECT s.address_id, s.slug_id, s.instance_id, COALESCE(ic.principal, ''), "
      "s.checklist, s.section, s.procedure, s.action, s.spec, "
      "s.instructions, s.result, s.status, s.comment, s.timestamp, s.entity_id, "
      "COALESCE(so.address_order, 0) "
      "FROM slugs s "
      "LEFT JOIN instance_catalog ic ON s.instance_id = ic.instance_id "
      "LEFT JOIN slug_order so ON s.address_id = so.address_id "
      "WHERE s.address_id=?;";
  if (Prepare(db_, sql, &stmt) != SQLITE_OK) {
    Finalize(stmt);
    throw std::runtime_error("Failed to prepare slug lookup");
  }
  sqlite3_bind_text(stmt, 1, address_id.c_str(), -1, SQLITE_TRANSIENT);

  if (sqlite3_step(stmt) != SQLITE_ROW) {
    Finalize(stmt);
    throw std::runtime_error("Address ID not found: " + address_id);
  }

  ChecklistSlug slug = BuildSlug(stmt);
  Finalize(stmt);
  slug.relationships = LoadOutgoingAddressEdges(address_id);
  return slug;
}

bool ChecklistStore::HasSlugById(const std::string& slug_id) const {
  std::lock_guard<std::mutex> lock(mutex_);
  sqlite3_stmt* stmt = nullptr;
  if (Prepare(db_, "SELECT 1 FROM slugs WHERE slug_id=? LIMIT 1;", &stmt) != SQLITE_OK) {
    Finalize(stmt);
    throw std::runtime_error("Failed to prepare slug existence check");
  }
  sqlite3_bind_text(stmt, 1, slug_id.c_str(), -1, SQLITE_TRANSIENT);
  const int rc = sqlite3_step(stmt);
  Finalize(stmt);
  return rc == SQLITE_ROW;
}

std::optional<PredicateRecord> ChecklistStore::GetPredicate(const std::string& name) const {
  std::lock_guard<std::mutex> lock(mutex_);
  sqlite3_stmt* stmt = nullptr;
  const std::string sql =
      "SELECT name, kind, status, COALESCE(description, ''), COALESCE(meta, '') "
      "FROM predicates WHERE name=?;";
  if (Prepare(db_, sql, &stmt) != SQLITE_OK) {
    Finalize(stmt);
    throw std::runtime_error("Failed to prepare predicate lookup");
  }
  sqlite3_bind_text(stmt, 1, name.c_str(), -1, SQLITE_TRANSIENT);
  if (sqlite3_step(stmt) != SQLITE_ROW) {
    Finalize(stmt);
    return std::nullopt;
  }
  PredicateRecord record;
  record.name = ColumnText(stmt, 0);
  record.kind = ColumnText(stmt, 1);
  record.status = ColumnText(stmt, 2);
  record.description = ColumnText(stmt, 3);
  record.meta = ColumnText(stmt, 4);
  Finalize(stmt);
  return record;
}

std::vector<PredicateRecord> ChecklistStore::ListPredicates(std::optional<int> limit,
                                                            std::optional<int> offset) const {
  std::vector<PredicateRecord> out;
  std::lock_guard<std::mutex> lock(mutex_);
  std::string sql =
      "SELECT name, kind, status, COALESCE(description, ''), COALESCE(meta, '') "
      "FROM predicates ORDER BY name";
  if (limit && *limit > 0) {
    sql += " LIMIT ?";
  }
  if (offset && *offset >= 0) {
    sql += " OFFSET ?";
  }
  sqlite3_stmt* stmt = nullptr;
  if (Prepare(db_, sql, &stmt) != SQLITE_OK) {
    Finalize(stmt);
    throw std::runtime_error("Failed to prepare predicate list query");
  }
  int bind_idx = 1;
  if (limit && *limit > 0) {
    sqlite3_bind_int(stmt, bind_idx++, *limit);
  }
  if (offset && *offset >= 0) {
    sqlite3_bind_int(stmt, bind_idx++, *offset);
  }
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    PredicateRecord record;
    record.name = ColumnText(stmt, 0);
    record.kind = ColumnText(stmt, 1);
    record.status = ColumnText(stmt, 2);
    record.description = ColumnText(stmt, 3);
    record.meta = ColumnText(stmt, 4);
    out.push_back(std::move(record));
  }
  Finalize(stmt);
  return out;
}

void ChecklistStore::UpsertPredicate(const PredicateRecord& predicate) {
  std::lock_guard<std::mutex> lock(mutex_);
  sqlite3_stmt* stmt = nullptr;
  const std::string sql =
      "INSERT INTO predicates (name, kind, status, description, meta) "
      "VALUES (?, ?, ?, ?, ?) "
      "ON CONFLICT(name) DO UPDATE SET kind=excluded.kind, status=excluded.status, "
      "description=excluded.description, meta=excluded.meta;";
  if (Prepare(db_, sql, &stmt) != SQLITE_OK) {
    Finalize(stmt);
    throw std::runtime_error("Failed to prepare predicate upsert");
  }
  sqlite3_bind_text(stmt, 1, predicate.name.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 2, predicate.kind.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 3, predicate.status.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 4, predicate.description.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 5, predicate.meta.c_str(), -1, SQLITE_TRANSIENT);
  StepOrThrow(stmt, "upsert predicate");
  Finalize(stmt);
}

void ChecklistStore::EnsurePredicate(const std::string& name, const std::string& kind,
                                     const std::string& status) {
  std::lock_guard<std::mutex> lock(mutex_);
  sqlite3_stmt* stmt = nullptr;
  const std::string sql =
      "INSERT INTO predicates (name, kind, status, description, meta) "
      "VALUES (?, ?, ?, '', '') ON CONFLICT(name) DO NOTHING;";
  if (Prepare(db_, sql, &stmt) != SQLITE_OK) {
    Finalize(stmt);
    throw std::runtime_error("Failed to prepare predicate insert");
  }
  sqlite3_bind_text(stmt, 1, name.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 2, kind.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 3, status.c_str(), -1, SQLITE_TRANSIENT);
  StepOrThrow(stmt, "ensure predicate");
  Finalize(stmt);
}

void ChecklistStore::SetPredicateChainDepth(int depth) {
  if (depth < 0) {
    LogWarn("Predicate chain depth cannot be negative; using 0.");
    predicate_chain_depth_ = 0;
    return;
  }
  predicate_chain_depth_ = depth;
}

bool ChecklistStore::HasSlugForInstance(const std::string& slug_id,
                                        const std::string& instance_id) const {
  std::lock_guard<std::mutex> lock(mutex_);
  sqlite3_stmt* stmt = nullptr;
  const std::string sql = "SELECT 1 FROM slugs WHERE slug_id=? AND instance_id=? LIMIT 1;";
  if (Prepare(db_, sql, &stmt) != SQLITE_OK) {
    Finalize(stmt);
    throw std::runtime_error("Failed to prepare slug existence check");
  }

  sqlite3_bind_text(stmt, 1, slug_id.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 2, instance_id.c_str(), -1, SQLITE_TRANSIENT);
  const int rc = sqlite3_step(stmt);
  Finalize(stmt);
  return rc == SQLITE_ROW;
}

std::vector<ChecklistSlug> ChecklistStore::GetSlugsForChecklist(
    const std::string& checklist) const {
  return GetSlugsForChecklist(checklist, std::nullopt);
}

std::vector<ChecklistSlug>
ChecklistStore::GetSlugsForChecklist(const std::string &checklist,
                                     const std::optional<ChecklistOwnership> &ownership_filter) const {
  return QuerySlugs(std::make_optional(checklist), std::nullopt, std::nullopt, std::nullopt, std::nullopt, std::nullopt,
                    ownership_filter);
}

RelationshipGraph ChecklistStore::GetRelationships(const std::string& address_id) const {
  RelationshipGraph graph;
  std::lock_guard<std::mutex> lock(mutex_);

  sqlite3_stmt* outgoing_stmt = nullptr;
  if (Prepare(db_, "SELECT predicate, target_address_id FROM address_relationships "
                   "WHERE subject_address_id=?;",
              &outgoing_stmt) != SQLITE_OK) {
    Finalize(outgoing_stmt);
    throw std::runtime_error("Failed to prepare outgoing relationship lookup");
  }
  sqlite3_bind_text(outgoing_stmt, 1, address_id.c_str(), -1, SQLITE_TRANSIENT);
  while (sqlite3_step(outgoing_stmt) == SQLITE_ROW) {
    RelationshipEdge edge;
    edge.predicate = ColumnText(outgoing_stmt, 0);
    edge.target = ColumnText(outgoing_stmt, 1);
    graph.outgoing.push_back(edge);
  }
  Finalize(outgoing_stmt);

  sqlite3_stmt* incoming_stmt = nullptr;
  if (Prepare(db_, "SELECT subject_address_id, predicate FROM address_relationships "
                   "WHERE target_address_id=?;",
              &incoming_stmt) != SQLITE_OK) {
    Finalize(incoming_stmt);
    throw std::runtime_error("Failed to prepare incoming relationship lookup");
  }
  sqlite3_bind_text(incoming_stmt, 1, address_id.c_str(), -1, SQLITE_TRANSIENT);
  while (sqlite3_step(incoming_stmt) == SQLITE_ROW) {
    RelationshipEdge edge;
    edge.target = ColumnText(incoming_stmt, 0);
    edge.predicate = ColumnText(incoming_stmt, 1);
    graph.incoming.push_back(edge);
  }
  Finalize(incoming_stmt);

  return graph;
}

std::optional<PrefillDatasetStatus> ChecklistStore::GetPrefillDatasetStatus(
    const std::string& address_id) const {
  if (address_id.empty()) {
    return std::nullopt;
  }
  ChecklistSlug slug;
  try {
    slug = GetSlugOrThrow(address_id);
  } catch (const std::exception&) {
    return std::nullopt;
  }
  const auto template_relationships = GetTemplateRelationshipsForChecklist(slug.checklist);
  const auto predecessors = BuildSlugPredecessorIndex(template_relationships);
  const auto candidates = CollectSlugLineageCandidates(predecessors, slug.slug_id);
  std::optional<ChecklistOwnership> ownership;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    ownership = FindNewestOwnershipForAddressUnlocked(db_, address_id);
  }
  const auto match = FindPrefillDatasetMatch(slug.checklist, candidates, address_id, ownership);
  PrefillDatasetStatus status;
  if (!match) {
    status.mode = "missing";
    return status;
  }
  status.mode = match->mode;
  status.path = match->path.lexically_normal().generic_string();
  status.matched_slug_id = match->matched_slug_id;
  return status;
}

std::vector<VerifyEvaluation> ChecklistStore::EvaluateVerifyRelationships(
    const std::string& address_id) const {
  std::vector<VerifyEvaluation> evaluations;
  if (address_id.empty()) {
    return evaluations;
  }
  std::lock_guard<std::mutex> lock(mutex_);

  ChecklistSlug source;
  try {
    source = GetSlugOrThrowUnlocked(address_id);
  } catch (const std::exception&) {
    return evaluations;
  }

  const auto outgoing_edges = LoadOutgoingAddressEdges(address_id);
  if (outgoing_edges.empty()) {
    return evaluations;
  }

  sqlite3_stmt* gate_sources_stmt = nullptr;
  const std::string gate_sources_sql =
      "SELECT ar.subject_address_id, s.spec, s.result "
      "FROM address_relationships ar "
      "LEFT JOIN slugs s ON s.address_id = ar.subject_address_id "
      "WHERE ar.target_address_id=? AND ar.predicate=?;";
  if (Prepare(db_, gate_sources_sql, &gate_sources_stmt) != SQLITE_OK) {
    Finalize(gate_sources_stmt);
    throw std::runtime_error("Failed to prepare verify gate lookup");
  }

  sqlite3_stmt* target_stmt = nullptr;
  if (Prepare(db_, "SELECT status, comment FROM slugs WHERE address_id=?;", &target_stmt) != SQLITE_OK) {
    Finalize(target_stmt);
    Finalize(gate_sources_stmt);
    throw std::runtime_error("Failed to prepare verify target lookup");
  }

  for (const auto& edge : outgoing_edges) {
    const auto parts = ParseVerifyPredicateToken(edge.predicate);
    if (!parts) {
      continue;
    }
    VerifyEvaluation entry;
    entry.predicate = edge.predicate;
    entry.target_address_id = edge.target;

    VerifyComputation evaluation = EvaluateSpecResultVerify(source.spec, source.result);
    VerifyGateComputation gate;
    const auto gate_mode = ParseCanonicalGateMode(parts->type);
    entry.gate_applied = gate_mode != CanonicalGateMode::kNone;
    if (gate_mode == CanonicalGateMode::kAnd) {
      entry.gate_mode = "AndGate";
      gate = EvaluateVerifyGate(gate_sources_stmt, edge.target, edge.predicate, gate_mode);
      evaluation.state = gate.state;
      evaluation.reason_code = gate.reason_code;
      evaluation.reason = gate.reason;
    } else if (gate_mode == CanonicalGateMode::kOr) {
      entry.gate_mode = "OrGate";
      gate = EvaluateVerifyGate(gate_sources_stmt, edge.target, edge.predicate, gate_mode);
      evaluation.state = gate.state;
      evaluation.reason_code = gate.reason_code;
      evaluation.reason = gate.reason;
    } else {
      entry.gate_mode.clear();
      gate.contributor_count = 1;
      gate.contributor_true_count = evaluation.state == VerifyBoolState::kTrue ? 1 : 0;
    }
    entry.contributor_count = gate.contributor_count;
    entry.contributor_true_count = gate.contributor_true_count;
    entry.predicate_bool = VerifyBoolToLower(evaluation.state);
    entry.reason_code = evaluation.reason_code;
    entry.reason = evaluation.reason;

    sqlite3_reset(target_stmt);
    sqlite3_clear_bindings(target_stmt);
    sqlite3_bind_text(target_stmt, 1, edge.target.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(target_stmt) != SQLITE_ROW) {
      entry.would_write = false;
      entry.write_decision = "TARGET_NOT_FOUND";
      evaluations.push_back(std::move(entry));
      continue;
    }
    const std::string target_status_raw = ColumnText(target_stmt, 0);
    const std::string target_comment = ColumnText(target_stmt, 1);
    const VerifyWritePlan plan = BuildVerifyWritePlan(*parts, evaluation.state, target_comment);
    bool changed = false;
    if (plan.write_status) {
      changed = changed || ParseStatus(target_status_raw) != plan.status;
    }
    if (plan.write_comment) {
      changed = changed || target_comment != plan.comment;
    }
    entry.would_write = changed;
    if (!changed && (plan.write_status || plan.write_comment)) {
      entry.write_decision = plan.write_decision + "_NO_CHANGE";
    } else if (!changed && plan.write_decision.empty()) {
      entry.write_decision = "NO_WRITE";
    } else {
      entry.write_decision = plan.write_decision;
    }
    evaluations.push_back(std::move(entry));
  }

  Finalize(target_stmt);
  Finalize(gate_sources_stmt);
  return evaluations;
}

std::vector<AddressRelationship> ChecklistStore::ListAddressRelationships(
    const std::optional<std::string>& subject_address_id,
    const std::optional<std::string>& target_address_id,
    const std::optional<std::string>& predicate,
    std::optional<int> limit,
    std::optional<int> offset) const {
  std::vector<AddressRelationship> edges;
  std::lock_guard<std::mutex> lock(mutex_);

  std::string sql =
      "SELECT subject_address_id, predicate, target_address_id FROM address_relationships";
  std::vector<std::string> clauses;
  if (subject_address_id && !subject_address_id->empty()) clauses.push_back("subject_address_id=?");
  if (target_address_id && !target_address_id->empty()) clauses.push_back("target_address_id=?");
  if (predicate && !predicate->empty()) clauses.push_back("predicate=?");
  if (!clauses.empty()) {
    sql += " WHERE ";
    for (std::size_t i = 0; i < clauses.size(); ++i) {
      if (i > 0) sql += " AND ";
      sql += clauses[i];
    }
  }
  sql += " ORDER BY subject_address_id, predicate, target_address_id";
  if (limit && *limit > 0) {
    sql += " LIMIT ?";
  }
  if (offset && *offset >= 0) {
    sql += " OFFSET ?";
  }

  sqlite3_stmt* stmt = nullptr;
  if (Prepare(db_, sql, &stmt) != SQLITE_OK) {
    Finalize(stmt);
    throw std::runtime_error("Failed to prepare address relationship query");
  }

  int bind_idx = 1;
  auto bind_if = [&](const std::optional<std::string>& v) {
    if (v && !v->empty()) {
      sqlite3_bind_text(stmt, bind_idx++, v->c_str(), -1, SQLITE_TRANSIENT);
    }
  };
  bind_if(subject_address_id);
  bind_if(target_address_id);
  bind_if(predicate);
  if (limit && *limit > 0) {
    sqlite3_bind_int(stmt, bind_idx++, *limit);
  }
  if (offset && *offset >= 0) {
    sqlite3_bind_int(stmt, bind_idx++, *offset);
  }

  while (sqlite3_step(stmt) == SQLITE_ROW) {
    AddressRelationship rel;
    rel.subject_address_id = ColumnText(stmt, 0);
    rel.predicate = ColumnText(stmt, 1);
    rel.target_address_id = ColumnText(stmt, 2);
    edges.push_back(std::move(rel));
  }
  Finalize(stmt);
  return edges;
}

std::vector<ChecklistSlug> ChecklistStore::QuerySlugs(
    const std::optional<std::string>& checklist, const std::optional<std::string>& instance_id,
    const std::optional<std::string>& section, const std::optional<ChecklistStatus>& status,
    std::optional<int> limit, std::optional<int> offset) const {
  return QuerySlugs(checklist, instance_id, section, status, limit, offset, std::nullopt);
}

std::vector<ChecklistSlug> ChecklistStore::QuerySlugs(const std::optional<std::string> &checklist,
                                                      const std::optional<std::string> &instance_id,
                                                      const std::optional<std::string> &section,
                                                      const std::optional<ChecklistStatus> &status,
                                                      std::optional<int> limit, std::optional<int> offset,
                                                      const std::optional<ChecklistOwnership> &ownership_filter) const {
  std::vector<ChecklistSlug> slugs;
  std::lock_guard<std::mutex> lock(mutex_);

  std::string sql =
      "SELECT s.address_id, s.slug_id, s.instance_id, COALESCE(ic.principal, ''), "
      "s.checklist, s.section, s.procedure, s.action, s.spec, "
      "s.instructions, s.result, s.status, s.comment, s.timestamp, s.entity_id, "
      "COALESCE(so.address_order, 0) "
      "FROM slugs s "
      "LEFT JOIN instance_catalog ic ON s.instance_id = ic.instance_id "
      "LEFT JOIN slug_order so ON s.address_id = so.address_id";
  const bool filter_by_owner = ownership_filter && HasOwnershipFilter(*ownership_filter);
  if (filter_by_owner) {
    sql += " JOIN address_ownership ao ON ao.address_id=s.address_id";
  }
  std::vector<std::string> clauses;
  if (checklist && !checklist->empty()) {
    clauses.push_back("s.checklist=?");
  }
  if (instance_id && !instance_id->empty()) {
    clauses.push_back("s.instance_id=?");
  }
  if (section && !section->empty()) {
    clauses.push_back("s.section=?");
  }
  if (status) {
    clauses.push_back("s.status=?");
  }
  if (filter_by_owner) {
    AppendOwnershipClauses(*ownership_filter, "ao", &clauses);
  }
  if (!clauses.empty()) {
    sql += " WHERE ";
    for (std::size_t i = 0; i < clauses.size(); ++i) {
      if (i > 0) sql += " AND ";
      sql += clauses[i];
    }
  }
  sql += " ORDER BY s.checklist, s.instance_id, so.address_order IS NULL, so.address_order, "
         "s.section, s.procedure, s.action";
  if (limit && *limit > 0) {
    sql += " LIMIT ?";
  }
  if (offset && *offset >= 0) {
    sql += " OFFSET ?";
  }

  sqlite3_stmt* stmt = nullptr;
  if (Prepare(db_, sql, &stmt) != SQLITE_OK) {
    Finalize(stmt);
    throw std::runtime_error("Failed to prepare filtered slug query");
  }
  int bind_index = 1;
  if (checklist && !checklist->empty()) {
    sqlite3_bind_text(stmt, bind_index++, checklist->c_str(), -1, SQLITE_TRANSIENT);
  }
  if (instance_id && !instance_id->empty()) {
    sqlite3_bind_text(stmt, bind_index++, instance_id->c_str(), -1, SQLITE_TRANSIENT);
  }
  if (section && !section->empty()) {
    sqlite3_bind_text(stmt, bind_index++, section->c_str(), -1, SQLITE_TRANSIENT);
  }
  if (status) {
    sqlite3_bind_text(stmt, bind_index++, StatusToString(*status).c_str(), -1, SQLITE_TRANSIENT);
  }
  if (filter_by_owner) {
    BindOwnershipFilter(stmt, &bind_index, *ownership_filter);
  }
  if (limit && *limit > 0) {
    sqlite3_bind_int(stmt, bind_index++, *limit);
  }
  if (offset && *offset >= 0) {
    sqlite3_bind_int(stmt, bind_index++, *offset);
  }

  while (sqlite3_step(stmt) == SQLITE_ROW) {
    slugs.push_back(BuildSlug(stmt));
  }
  Finalize(stmt);

  for (auto& slug : slugs) {
    slug.relationships = LoadOutgoingAddressEdges(slug.address_id);
  }
  return slugs;
}

std::vector<TemplateRelationship> ChecklistStore::GetTemplateRelationshipsForChecklist(
    const std::string& checklist) const {
  return GetTemplateRelationshipsForChecklist(checklist, std::nullopt);
}

std::vector<TemplateRelationship>
ChecklistStore::GetTemplateRelationshipsForChecklist(const std::string &checklist,
                                                     const std::optional<ChecklistOwnership> &ownership_filter) const {
  std::lock_guard<std::mutex> lock(mutex_);
  if (ownership_filter && HasOwnershipFilter(*ownership_filter)) {
    ChecklistOwnership filter = *ownership_filter;
    if (filter.checklist.empty()) {
      filter.checklist = checklist;
    }
    std::vector<TemplateRelationship> edges;
    sqlite3_stmt *stmt = nullptr;
    std::vector<std::string> ownership_clauses;
    AppendOwnershipClauses(filter, "so", &ownership_clauses);

    std::string sql = "SELECT DISTINCT tr.subject_slug_id, tr.predicate, tr.target_slug_id "
                      "FROM template_relationships tr "
                      "WHERE EXISTS (SELECT 1 FROM slug_ownership so "
                      "WHERE so.slug_id=tr.subject_slug_id";
    for (const auto &clause : ownership_clauses) {
      sql += " AND " + clause;
    }
    sql += ") ORDER BY tr.subject_slug_id, tr.predicate, tr.target_slug_id;";

    if (Prepare(db_, sql, &stmt) != SQLITE_OK) {
      Finalize(stmt);
      throw std::runtime_error("Failed to prepare owned template relationship lookup");
    }
    int bind_index = 1;
    BindOwnershipFilter(stmt, &bind_index, filter);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
      TemplateRelationship rel;
      rel.subject_slug_id = ColumnText(stmt, 0);
      rel.predicate = ColumnText(stmt, 1);
      rel.target_slug_id = ColumnText(stmt, 2);
      edges.push_back(std::move(rel));
    }
    Finalize(stmt);
    return edges;
  }
  return LoadTemplateRelationshipsForChecklist(db_, checklist);
}

std::vector<TemplateRelationship> ChecklistStore::ListTemplateRelationships(
    const std::optional<std::string>& subject_slug_id,
    const std::optional<std::string>& target_slug_id,
    const std::optional<std::string>& predicate,
    std::optional<int> limit,
    std::optional<int> offset) const {
  std::vector<TemplateRelationship> edges;
  std::lock_guard<std::mutex> lock(mutex_);

  std::string sql =
      "SELECT subject_slug_id, predicate, target_slug_id FROM template_relationships";
  std::vector<std::string> clauses;
  if (subject_slug_id && !subject_slug_id->empty()) clauses.push_back("subject_slug_id=?");
  if (target_slug_id && !target_slug_id->empty()) clauses.push_back("target_slug_id=?");
  if (predicate && !predicate->empty()) clauses.push_back("predicate=?");
  if (!clauses.empty()) {
    sql += " WHERE ";
    for (std::size_t i = 0; i < clauses.size(); ++i) {
      if (i > 0) sql += " AND ";
      sql += clauses[i];
    }
  }
  sql += " ORDER BY subject_slug_id, predicate, target_slug_id";
  if (limit && *limit > 0) {
    sql += " LIMIT ?";
  }
  if (offset && *offset >= 0) {
    sql += " OFFSET ?";
  }

  sqlite3_stmt* stmt = nullptr;
  if (Prepare(db_, sql, &stmt) != SQLITE_OK) {
    Finalize(stmt);
    throw std::runtime_error("Failed to prepare template relationship query");
  }

  int bind_idx = 1;
  auto bind_if = [&](const std::optional<std::string>& v) {
    if (v && !v->empty()) {
      sqlite3_bind_text(stmt, bind_idx++, v->c_str(), -1, SQLITE_TRANSIENT);
    }
  };
  bind_if(subject_slug_id);
  bind_if(target_slug_id);
  bind_if(predicate);
  if (limit && *limit > 0) {
    sqlite3_bind_int(stmt, bind_idx++, *limit);
  }
  if (offset && *offset >= 0) {
    sqlite3_bind_int(stmt, bind_idx++, *offset);
  }

  while (sqlite3_step(stmt) == SQLITE_ROW) {
    TemplateRelationship rel;
    rel.subject_slug_id = ColumnText(stmt, 0);
    rel.predicate = ColumnText(stmt, 1);
    rel.target_slug_id = ColumnText(stmt, 2);
    edges.push_back(std::move(rel));
  }
  Finalize(stmt);
  return edges;
}

std::vector<HistoryEntry> ChecklistStore::GetHistory(const std::string& address_id) const {
  std::vector<HistoryEntry> history;
  std::lock_guard<std::mutex> lock(mutex_);
  sqlite3_stmt* stmt = nullptr;
  if (Prepare(db_, "SELECT address_id, timestamp, result, status, comment, entity_id "
                   "FROM history WHERE address_id=? ORDER BY timestamp;",
              &stmt) != SQLITE_OK) {
    Finalize(stmt);
    throw std::runtime_error("Failed to prepare history lookup");
  }
  sqlite3_bind_text(stmt, 1, address_id.c_str(), -1, SQLITE_TRANSIENT);
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    history.push_back(BuildHistory(stmt));
  }
  Finalize(stmt);
  return history;
}

std::vector<HistoryEntry> ChecklistStore::GetHistory(const std::string& address_id,
                                                     std::optional<int> limit,
                                                     std::optional<int> offset) const {
  if (!limit || *limit <= 0) {
    return GetHistory(address_id);
  }
  std::vector<HistoryEntry> history;
  std::lock_guard<std::mutex> lock(mutex_);
  sqlite3_stmt* stmt = nullptr;
  if (Prepare(db_, "SELECT address_id, timestamp, result, status, comment, entity_id "
                   "FROM history WHERE address_id=? ORDER BY timestamp LIMIT ? OFFSET ?;",
              &stmt) != SQLITE_OK) {
    Finalize(stmt);
    throw std::runtime_error("Failed to prepare history lookup");
  }
  sqlite3_bind_text(stmt, 1, address_id.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int(stmt, 2, *limit);
  sqlite3_bind_int(stmt, 3, offset.value_or(0));
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    history.push_back(BuildHistory(stmt));
  }
  Finalize(stmt);
  return history;
}

std::string ChecklistStore::EnsureEntityRecord(const std::string& principal,
                                               const std::string& kind,
                                               const std::string& display_name) {
  const std::string entity_id = ComputeEntityId(principal);
  std::lock_guard<std::mutex> lock(mutex_);
  EnsureEntity(db_, entity_id, principal, kind, display_name);
  return entity_id;
}

std::vector<std::pair<std::string, std::string>> ChecklistStore::ListEntities(
    std::optional<int> limit, std::optional<int> offset) const {
  std::vector<std::pair<std::string, std::string>> entities;
  std::lock_guard<std::mutex> lock(mutex_);

  std::string sql = "SELECT entity_id, principal FROM entities ORDER BY entity_id";
  if (limit && *limit > 0) {
    sql += " LIMIT ?";
  }
  if (offset && *offset >= 0) {
    sql += " OFFSET ?";
  }

  sqlite3_stmt* stmt = nullptr;
  if (Prepare(db_, sql, &stmt) != SQLITE_OK) {
    Finalize(stmt);
    throw std::runtime_error("Failed to prepare entity list query");
  }
  int idx = 1;
  if (limit && *limit > 0) {
    sqlite3_bind_int(stmt, idx++, *limit);
  }
  if (offset && *offset >= 0) {
    sqlite3_bind_int(stmt, idx++, *offset);
  }

  while (sqlite3_step(stmt) == SQLITE_ROW) {
    entities.emplace_back(ColumnText(stmt, 0), ColumnText(stmt, 1));
  }
  Finalize(stmt);
  return entities;
}

std::string ChecklistStore::EnsureInstanceRecord(const std::string& principal,
                                                 const std::string& label,
                                                 const std::string& meta) {
  std::lock_guard<std::mutex> lock(mutex_);
  return EnsureInstanceRecordUnlocked(principal, label, meta);
}

std::string ChecklistStore::EnsureInstanceRecordUnlocked(const std::string& principal,
                                                         const std::string& label,
                                                         const std::string& meta) {
  const std::string instance_id = ComputeInstanceId(principal);
  sqlite3_stmt* stmt = nullptr;
  const std::string sql =
      "INSERT INTO instance_catalog (instance_id, principal, label, meta) VALUES (?,?,?,?) "
      "ON CONFLICT(instance_id) DO UPDATE SET label=excluded.label, meta=excluded.meta;";
  if (Prepare(db_, sql, &stmt) != SQLITE_OK) {
    Finalize(stmt);
    throw std::runtime_error("Failed to prepare instance insert");
  }
  sqlite3_bind_text(stmt, 1, instance_id.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 2, principal.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 3, label.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 4, meta.c_str(), -1, SQLITE_TRANSIENT);
  StepOrThrow(stmt, "instance insert");
  Finalize(stmt);
  return instance_id;
}

std::vector<std::pair<std::string, std::string>> ChecklistStore::ListInstances(
    std::optional<int> limit, std::optional<int> offset) const {
  std::vector<std::pair<std::string, std::string>> instances;
  std::lock_guard<std::mutex> lock(mutex_);

  std::string sql = "SELECT instance_id, principal FROM instance_catalog ORDER BY instance_id";
  if (limit && *limit > 0) {
    sql += " LIMIT ?";
  }
  if (offset && *offset >= 0) {
    sql += " OFFSET ?";
  }

  sqlite3_stmt* stmt = nullptr;
  if (Prepare(db_, sql, &stmt) != SQLITE_OK) {
    Finalize(stmt);
    throw std::runtime_error("Failed to prepare instance list query");
  }
  int idx = 1;
  if (limit && *limit > 0) {
    sqlite3_bind_int(stmt, idx++, *limit);
  }
  if (offset && *offset >= 0) {
    sqlite3_bind_int(stmt, idx++, *offset);
  }

  while (sqlite3_step(stmt) == SQLITE_ROW) {
    instances.emplace_back(ColumnText(stmt, 0), ColumnText(stmt, 1));
  }
  Finalize(stmt);
  return instances;
}

std::vector<ChecklistOwnership> ChecklistStore::ListOwnershipsForInstance(const std::string &checklist,
                                                                          const std::string &instance_id) const {
  std::vector<ChecklistOwnership> ownerships;
  if (checklist.empty() || instance_id.empty()) {
    return ownerships;
  }

  std::lock_guard<std::mutex> lock(mutex_);
  sqlite3_stmt *stmt = nullptr;
  const std::string sql = "SELECT DISTINCT source_name, COALESCE(source_path, ''), pack, checklist_dir, checklist "
                          "FROM address_ownership "
                          "WHERE checklist=? AND instance_id=? "
                          "ORDER BY source_name, pack, checklist_dir;";
  if (Prepare(db_, sql, &stmt) != SQLITE_OK) {
    Finalize(stmt);
    throw std::runtime_error("Failed to prepare checklist ownership lookup");
  }
  sqlite3_bind_text(stmt, 1, checklist.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 2, instance_id.c_str(), -1, SQLITE_TRANSIENT);

  while (sqlite3_step(stmt) == SQLITE_ROW) {
    ChecklistOwnership ownership;
    ownership.source_name = ColumnText(stmt, 0);
    ownership.source_path = ColumnText(stmt, 1);
    ownership.pack = ColumnText(stmt, 2);
    ownership.checklist_dir = ColumnText(stmt, 3);
    ownership.checklist = ColumnText(stmt, 4);
    ownerships.push_back(std::move(ownership));
  }
  Finalize(stmt);
  return ownerships;
}

void ChecklistStore::ApplyUpdate(const SlugUpdate& update) {
  struct PendingUpdate {
    SlugUpdate update;
    int depth_remaining = 0;
  };
  std::vector<PendingUpdate> pending;
  pending.push_back(PendingUpdate{update, predicate_chain_depth_});
  std::lock_guard<std::mutex> lock(mutex_);
  const std::string predicate_daemon_principal{::kPredicateDaemonPrincipal};
  const std::string predicate_daemon_entity_id = ComputeEntityId(predicate_daemon_principal);
  EnsureEntity(db_, predicate_daemon_entity_id, predicate_daemon_principal,
               std::string{kPredicateDaemonKind}, std::string{kPredicateDaemonDisplay});
  sqlite3_stmt* get_target_stmt = nullptr;
  const std::string get_target_sql =
      "SELECT result, status, comment, timestamp FROM slugs WHERE address_id=?;";
  if (Prepare(db_, get_target_sql, &get_target_stmt) != SQLITE_OK) {
    Finalize(get_target_stmt);
    throw std::runtime_error("Failed to prepare predicate daemon target lookup");
  }
  sqlite3_stmt* get_gate_sources_stmt = nullptr;
  const std::string get_gate_sources_sql =
      "SELECT ar.subject_address_id, s.status "
      "FROM address_relationships ar "
      "LEFT JOIN slugs s ON s.address_id = ar.subject_address_id "
      "WHERE ar.target_address_id=? AND ar.predicate=?;";
  if (Prepare(db_, get_gate_sources_sql, &get_gate_sources_stmt) != SQLITE_OK) {
    Finalize(get_gate_sources_stmt);
    Finalize(get_target_stmt);
    throw std::runtime_error("Failed to prepare predicate daemon gate-source lookup");
  }
  sqlite3_stmt* get_verify_gate_sources_stmt = nullptr;
  const std::string get_verify_gate_sources_sql =
      "SELECT ar.subject_address_id, s.spec, s.result "
      "FROM address_relationships ar "
      "LEFT JOIN slugs s ON s.address_id = ar.subject_address_id "
      "WHERE ar.target_address_id=? AND ar.predicate=?;";
  if (Prepare(db_, get_verify_gate_sources_sql, &get_verify_gate_sources_stmt) != SQLITE_OK) {
    Finalize(get_verify_gate_sources_stmt);
    Finalize(get_gate_sources_stmt);
    Finalize(get_target_stmt);
    throw std::runtime_error("Failed to prepare verify gate-source lookup");
  }
  while (!pending.empty()) {
    PendingUpdate current = std::move(pending.back());
    pending.pop_back();
    ChecklistSlug existing = GetSlugOrThrowUnlocked(current.update.address_id);
    ChecklistSlug mutated = existing;
    if (current.update.result) {
      mutated.result = NormalizeResultToSpecUnit(mutated.spec, *current.update.result);
    }
    if (current.update.status) {
      mutated.status = *current.update.status;
    }
    if (current.update.comment) {
      mutated.comment = *current.update.comment;
    }
    mutated.timestamp = current.update.timestamp.value_or(CurrentTimestampIsoUtc());
    if (current.update.entity_principal_override) {
      mutated.entity_principal = *current.update.entity_principal_override;
      mutated.entity_id = ComputeEntityId(mutated.entity_principal);
      EnsureEntity(db_, mutated.entity_id, mutated.entity_principal, std::string{kDefaultEntityKind},
                   std::string{kDefaultEntityDisplay});
    } else {
      mutated.entity_id = current.update.entity_id_override.value_or(DefaultEntityId());
      EnsureEntity(db_, mutated.entity_id, std::string{kDefaultEntityPrincipal},
                   std::string{kDefaultEntityKind}, std::string{kDefaultEntityDisplay});
    }
    sqlite3_stmt* stmt = nullptr;
    const std::string sql =
        "UPDATE slugs SET result=?, status=?, comment=?, timestamp=?, entity_id=? WHERE address_id=?;";
    if (Prepare(db_, sql, &stmt) != SQLITE_OK) {
      Finalize(stmt);
      throw std::runtime_error("Failed to prepare slug update");
    }
    sqlite3_bind_text(stmt, 1, mutated.result.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, StatusToString(mutated.status).c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, mutated.comment.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, mutated.timestamp.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 5, mutated.entity_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 6, mutated.address_id.c_str(), -1, SQLITE_TRANSIENT);
    StepOrThrow(stmt, "slug update");
    Finalize(stmt);
    InsertHistorySnapshot(mutated);
    const bool status_changed = current.update.status && existing.status != mutated.status;
    const bool result_changed = current.update.result && existing.result != mutated.result;
    const bool comment_changed = current.update.comment && existing.comment != mutated.comment;
    const bool timestamp_changed =
        current.update.timestamp && existing.timestamp != mutated.timestamp;
    const bool any_changed =
        status_changed || result_changed || comment_changed || timestamp_changed;
    if (!any_changed) {
      continue;
    }
    if (current.depth_remaining <= 0) {
      continue;
    }
    const auto outgoing_edges = LoadOutgoingAddressEdges(mutated.address_id);
    if (outgoing_edges.empty()) {
      continue;
    }
    const std::string now = CurrentTimestampIsoUtc();
    if (any_changed && mutated.status != ChecklistStatus::kUnknown) {
      for (const auto& edge : outgoing_edges) {
        const auto parts = ParseCanonicalPredicateToken(edge.predicate);
        if (!parts) continue;
        if (parts->relation != "Propagate") continue;
        const auto expected_subject = SubjectStateToStatus(parts->subject_state);
        if (!expected_subject || *expected_subject != mutated.status) continue;
        const auto desired_target_status = ObjectStateToStatus(parts->object_state);
        if (!desired_target_status) continue;
        const auto gate_mode = ParseCanonicalGateMode(parts->type);
        if (gate_mode != CanonicalGateMode::kNone) {
          bool any_contributor = false;
          bool any_active = false;
          bool all_active = true;
          sqlite3_reset(get_gate_sources_stmt);
          sqlite3_clear_bindings(get_gate_sources_stmt);
          sqlite3_bind_text(get_gate_sources_stmt, 1, edge.target.c_str(), -1, SQLITE_TRANSIENT);
          sqlite3_bind_text(get_gate_sources_stmt, 2, edge.predicate.c_str(), -1, SQLITE_TRANSIENT);
          int gate_rc = SQLITE_ROW;
          while ((gate_rc = sqlite3_step(get_gate_sources_stmt)) == SQLITE_ROW) {
            const std::string source_address = ColumnText(get_gate_sources_stmt, 0);
            if (source_address.empty()) {
              continue;
            }
            any_contributor = true;
            if (sqlite3_column_type(get_gate_sources_stmt, 1) == SQLITE_NULL) {
              LogWarn("Predicate gate source missing for " + edge.predicate + " source=" +
                      source_address + " target=" + edge.target);
              all_active = false;
              continue;
            }
            const ChecklistStatus source_status =
                ParseStatus(ColumnText(get_gate_sources_stmt, 1));
            if (source_status == *expected_subject) {
              any_active = true;
            } else {
              all_active = false;
            }
          }
          if (gate_rc != SQLITE_DONE) {
            throw std::runtime_error("Predicate gate evaluation query failed");
          }
          bool gate_active = true;
          if (gate_mode == CanonicalGateMode::kAnd) {
            gate_active = any_contributor && all_active;
          } else if (gate_mode == CanonicalGateMode::kOr) {
            gate_active = any_active;
          }
          if (!gate_active) {
            continue;
          }
        }
        sqlite3_reset(get_target_stmt);
        sqlite3_clear_bindings(get_target_stmt);
        sqlite3_bind_text(get_target_stmt, 1, edge.target.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(get_target_stmt) != SQLITE_ROW) {
          continue;
        }
        const std::string target_status_raw = ColumnText(get_target_stmt, 1);
        const std::string target_comment = ColumnText(get_target_stmt, 2);
        if (edge.target == mutated.address_id) {
          continue;
        }
        const std::string new_comment =
            "Filled per relationship " + edge.predicate + " " + mutated.address_id;
        const std::string desired_status_str = StatusToString(*desired_target_status);
        if (!status_changed &&
            core::ParseStatus(target_status_raw) != ChecklistStatus::kUnknown) {
          continue;
        }
        if (target_status_raw == desired_status_str && target_comment == new_comment) {
          continue;
        }
        SlugUpdate next;
        next.address_id = edge.target;
        next.status = *desired_target_status;
        next.comment = new_comment;
        next.timestamp = now;
        next.entity_principal_override = predicate_daemon_principal;
        pending.push_back(PendingUpdate{std::move(next), current.depth_remaining - 1});
        LogInfo("PredicateDaemon propagate " + mutated.address_id + " --" + edge.predicate + "--> " +
                edge.target + " status=" + desired_status_str);
      }
    }
    if (any_changed) {
      for (const auto& edge : outgoing_edges) {
        const auto parts = ParseVerifyPredicateToken(edge.predicate);
        if (!parts) {
          continue;
        }

        VerifyComputation evaluation = EvaluateSpecResultVerify(mutated.spec, mutated.result);
        VerifyGateComputation gate;
        const auto gate_mode = ParseCanonicalGateMode(parts->type);
        if (gate_mode == CanonicalGateMode::kAnd || gate_mode == CanonicalGateMode::kOr) {
          gate = EvaluateVerifyGate(get_verify_gate_sources_stmt, edge.target, edge.predicate, gate_mode);
          evaluation.state = gate.state;
          evaluation.reason_code = gate.reason_code;
          evaluation.reason = gate.reason;
        } else {
          gate.contributor_count = 1;
          gate.contributor_true_count = evaluation.state == VerifyBoolState::kTrue ? 1 : 0;
        }

        sqlite3_reset(get_target_stmt);
        sqlite3_clear_bindings(get_target_stmt);
        sqlite3_bind_text(get_target_stmt, 1, edge.target.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(get_target_stmt) != SQLITE_ROW) {
          continue;
        }
        const std::string target_status_raw = ColumnText(get_target_stmt, 1);
        const std::string target_comment = ColumnText(get_target_stmt, 2);
        const VerifyWritePlan plan = BuildVerifyWritePlan(*parts, evaluation.state, target_comment);
        bool updated = false;
        SlugUpdate next;
        next.address_id = edge.target;
        next.entity_principal_override = predicate_daemon_principal;
        next.timestamp = now;
        if (plan.write_status && ParseStatus(target_status_raw) != plan.status) {
          next.status = plan.status;
          updated = true;
        }
        if (plan.write_comment && target_comment != plan.comment) {
          next.comment = plan.comment;
          updated = true;
        }
        if (!updated) {
          continue;
        }
        pending.push_back(PendingUpdate{std::move(next), current.depth_remaining - 1});
        LogInfo("PredicateDaemon verify " + mutated.address_id + " --" + edge.predicate + "--> " +
                edge.target + " predicate_bool=" + VerifyBoolToLower(evaluation.state) +
                " decision=" + plan.write_decision);
      }
    }
    std::vector<std::pair<RelationshipEdge, SlotPredicateParts>> propagate_edges;
    if (any_changed) {
      for (const auto& edge : outgoing_edges) {
        const auto parts = ParseSlotPredicateToken(edge.predicate);
        if (!parts) {
          continue;
        }
        if (parts->relation != "Propagate" || parts->type != "Validated") {
          continue;
        }
        if (parts->subject_status_filter && mutated.status != parts->subject_status) {
          continue;
        }
        if (parts->subject == SlotField::kInstructions) {
          LogWarn("Propagate predicate " + edge.predicate +
                  " uses Instructions source for address " + edge.target);
          continue;
        }
        if (!IsPropagateTargetField(parts->object)) {
          LogWarn("Propagate predicate " + edge.predicate + " targets non-mutable field " +
                  SlotFieldToString(parts->object) + " for address " + edge.target);
          continue;
        }
        propagate_edges.emplace_back(edge, *parts);
      }
    }
    if (!propagate_edges.empty()) {
      for (const auto& item : propagate_edges) {
        const auto& edge = item.first;
        const auto& parts = item.second;
        if (edge.target == mutated.address_id) {
          continue;
        }
        if (parts.subject == SlotField::kStatus && mutated.status == ChecklistStatus::kUnknown) {
          continue;
        }
        const std::string subject_value = FieldValueForSlug(mutated, parts.subject);
        sqlite3_reset(get_target_stmt);
        sqlite3_clear_bindings(get_target_stmt);
        sqlite3_bind_text(get_target_stmt, 1, edge.target.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(get_target_stmt) != SQLITE_ROW) {
          continue;
        }
        const std::string target_result = ColumnText(get_target_stmt, 0);
        const std::string target_status_raw = ColumnText(get_target_stmt, 1);
        const std::string target_comment = ColumnText(get_target_stmt, 2);
        std::string new_result = target_result;
        std::string new_comment = target_comment;
        std::string new_status_raw = target_status_raw;
        bool updated = false;
        SlugUpdate next;
        next.address_id = edge.target;
        next.entity_principal_override = predicate_daemon_principal;
        next.timestamp = now;
        if (parts.object == SlotField::kResult) {
          new_result = subject_value;
          updated = new_result != target_result;
          if (updated) {
            next.result = new_result;
          }
        } else if (parts.object == SlotField::kComment) {
          new_comment = subject_value;
          updated = new_comment != target_comment;
          if (updated) {
            next.comment = new_comment;
          }
        } else if (parts.object == SlotField::kStatus) {
          const auto parsed_status = core::ParseStatus(subject_value);
          if (parsed_status == ChecklistStatus::kUnknown) {
            LogWarn("Propagate predicate " + edge.predicate + " produced invalid status '" +
                    subject_value + "' for target " + edge.target);
            continue;
          }
          new_status_raw = core::StatusToString(parsed_status);
          updated = new_status_raw != target_status_raw;
          if (updated) {
            next.status = parsed_status;
          }
        }
        if (!updated) {
          continue;
        }
        pending.push_back(PendingUpdate{std::move(next), current.depth_remaining - 1});
        LogInfo("PredicateDaemon slot propagate " + mutated.address_id + " --" + edge.predicate +
                "--> " + edge.target + " field=" + SlotFieldToString(parts.object));
      }
    }
    std::vector<std::pair<RelationshipEdge, SlotPredicateParts>> prefill_edges;
    if (any_changed) {
      for (const auto& edge : outgoing_edges) {
        const auto parts = ParseSlotPredicateToken(edge.predicate);
        if (!parts) {
          continue;
        }
        if (parts->relation != "Search" || parts->type != "Prefill") {
          continue;
        }
        if (!FieldChanged(parts->subject, result_changed, status_changed, comment_changed,
                          timestamp_changed)) {
          continue;
        }
        if (parts->subject_status_filter && mutated.status != parts->subject_status) {
          continue;
        }
        if (!IsWritableSlotField(parts->object)) {
          LogWarn("Prefill predicate " + edge.predicate +
                  " targets a non-writable field for address " + edge.target);
          continue;
        }
        prefill_edges.emplace_back(edge, *parts);
      }
    }
    if (!prefill_edges.empty()) {
      const auto template_relationships =
          LoadTemplateRelationshipsForChecklist(db_, mutated.checklist);
      const auto predecessors = BuildSlugPredecessorIndex(template_relationships);
      const auto subject_candidates = CollectSlugLineageCandidates(predecessors, mutated.slug_id);
      const auto ownership = FindNewestOwnershipForAddressUnlocked(db_, mutated.address_id);
      const auto dataset_path =
          FindPrefillDatasetPath(mutated.checklist, subject_candidates, mutated.address_id, ownership);
      if (!dataset_path) {
        LogWarn("Prefill dataset not found for checklist '" + mutated.checklist + "'");
      } else {
        std::vector<std::string> headers;
        std::vector<std::vector<std::string>> rows;
        std::string csv_error;
        if (!ReadCsv(*dataset_path, headers, rows, &csv_error)) {
          LogWarn("Prefill dataset read failed: " + csv_error);
        } else {
          const auto header_index = BuildSlugFieldIndex(headers);
          const std::string subject_slug_id = mutated.slug_id;
          for (const auto& item : prefill_edges) {
            const auto& edge = item.first;
            const auto& parts = item.second;
            const std::string subject_value = FieldValueForSlug(mutated, parts.subject);
            if (parts.subject == SlotField::kStatus &&
                mutated.status == ChecklistStatus::kUnknown) {
              continue;
            }
            if (Trim(subject_value).empty()) {
              continue;
            }
            std::string subject_column_slug = subject_slug_id;
            const auto subject_column =
                FindSlugFieldColumnWithLineage(header_index, subject_candidates, parts.subject,
                                               &subject_column_slug);
            if (!subject_column) {
              LogWarn("Prefill missing subject column for slug " + subject_slug_id);
              continue;
            }
            if (subject_column_slug != subject_slug_id) {
              LogInfo("Prefill using predecessor column " + subject_column_slug + " for subject " +
                      subject_slug_id);
            }
            const std::vector<std::string>* matched_row = nullptr;
            for (const auto& row : rows) {
              if (*subject_column >= row.size()) {
                continue;
              }
              if (ValuesMatch(row[*subject_column], subject_value)) {
                matched_row = &row;
                break;
              }
            }
            if (!matched_row) {
              continue;
            }
            const std::string target_slug_id = ExtractSlugIdFromAddress(edge.target);
            if (target_slug_id.empty()) {
              continue;
            }
            const auto target_candidates = CollectSlugLineageCandidates(predecessors, target_slug_id);
            std::string target_column_slug = target_slug_id;
            const auto target_column =
                FindSlugFieldColumnWithLineage(header_index, target_candidates, parts.object,
                                               &target_column_slug);
            if (!target_column) {
              LogWarn("Prefill missing target column for slug " + target_slug_id);
              continue;
            }
            if (target_column_slug != target_slug_id) {
              LogInfo("Prefill using predecessor column " + target_column_slug + " for target " +
                      target_slug_id);
            }
            if (*target_column >= matched_row->size()) {
              continue;
            }
            const std::string raw_value = (*matched_row)[*target_column];
            if (Trim(raw_value).empty()) {
              continue;
            }
            sqlite3_reset(get_target_stmt);
            sqlite3_clear_bindings(get_target_stmt);
            sqlite3_bind_text(get_target_stmt, 1, edge.target.c_str(), -1, SQLITE_TRANSIENT);
            if (sqlite3_step(get_target_stmt) != SQLITE_ROW) {
              continue;
            }
            const std::string target_result = ColumnText(get_target_stmt, 0);
            const std::string target_status_raw = ColumnText(get_target_stmt, 1);
            const std::string target_comment = ColumnText(get_target_stmt, 2);
            const std::string target_timestamp = ColumnText(get_target_stmt, 3);
              constexpr bool kOnlyEmpty = false;
              if (kOnlyEmpty) {
                if (parts.object == SlotField::kResult && !Trim(target_result).empty()) {
                  continue;
                }
              if (parts.object == SlotField::kComment && !Trim(target_comment).empty()) {
                continue;
              }
              if (parts.object == SlotField::kTimestamp && !Trim(target_timestamp).empty()) {
                continue;
              }
              if (parts.object == SlotField::kStatus) {
                if (core::ParseStatus(target_status_raw) != ChecklistStatus::kUnknown) {
                  continue;
                }
              }
            }
            std::string new_result = target_result;
            std::string new_comment = target_comment;
            std::string new_status_raw = target_status_raw;
            std::string new_timestamp = now;
            SlugUpdate next;
            next.address_id = edge.target;
            next.entity_principal_override = predicate_daemon_principal;
            next.timestamp = now;
            bool updated = false;
            if (parts.object == SlotField::kResult) {
              new_result = raw_value;
              updated = new_result != target_result;
              if (updated) {
                next.result = new_result;
              }
            } else if (parts.object == SlotField::kComment) {
              new_comment = raw_value;
              updated = new_comment != target_comment;
              if (updated) {
                next.comment = new_comment;
              }
            } else if (parts.object == SlotField::kStatus) {
              const auto parsed_status = core::ParseStatus(raw_value);
              if (parsed_status == ChecklistStatus::kUnknown) {
                LogWarn("Prefill status value '" + raw_value + "' is invalid for target " +
                        edge.target);
                continue;
              }
              new_status_raw = core::StatusToString(parsed_status);
              updated = new_status_raw != target_status_raw;
              if (updated) {
                next.status = parsed_status;
              }
            } else if (parts.object == SlotField::kTimestamp) {
              if (!IsIsoTimestamp(raw_value)) {
                LogWarn("Prefill timestamp value '" + raw_value + "' is invalid for target " +
                        edge.target);
                continue;
              }
              new_timestamp = raw_value;
              updated = new_timestamp != target_timestamp;
              if (updated) {
                next.timestamp = new_timestamp;
              }
            }
            if (!updated) {
              continue;
            }
            pending.push_back(PendingUpdate{std::move(next), current.depth_remaining - 1});
            LogInfo("Prefill predicate " + edge.predicate + " " + mutated.address_id + " -> " +
                    edge.target);
          }
        }
      }
    }
  }
  Finalize(get_verify_gate_sources_stmt);
  Finalize(get_gate_sources_stmt);
  Finalize(get_target_stmt);
}
void ChecklistStore::DeleteChecklist(const std::string& checklist, int* deleted_slugs) {
  if (checklist.empty()) {
    throw std::invalid_argument("Checklist name must not be empty.");
  }
  std::lock_guard<std::mutex> lock(mutex_);

  char* errmsg = nullptr;
  sqlite3_exec(db_, "BEGIN IMMEDIATE;", nullptr, nullptr, &errmsg);
  if (errmsg) {
    std::string message = errmsg;
    sqlite3_free(errmsg);
    throw std::runtime_error("Failed to begin transaction for checklist delete: " + message);
  }

  auto rollback = [&]() { sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr); };

  sqlite3_stmt* select_slugs = nullptr;
  sqlite3_stmt *delete_slug_owner = nullptr;
  sqlite3_stmt *delete_address_owner = nullptr;
  sqlite3_stmt* delete_tpl_rel = nullptr;
  sqlite3_stmt* delete_history = nullptr;
  sqlite3_stmt* delete_slugs_stmt = nullptr;
  int removed = 0;
  try {
    std::vector<std::string> slug_ids;
    std::vector<std::string> address_ids;

    const std::string select_sql = "SELECT slug_id, address_id FROM slugs WHERE checklist=?;";
    if (Prepare(db_, select_sql, &select_slugs) != SQLITE_OK) {
      throw std::runtime_error("Failed to prepare checklist slug lookup for delete.");
    }
    sqlite3_bind_text(select_slugs, 1, checklist.c_str(), -1, SQLITE_TRANSIENT);
    while (sqlite3_step(select_slugs) == SQLITE_ROW) {
      slug_ids.push_back(ColumnText(select_slugs, 0));
      address_ids.push_back(ColumnText(select_slugs, 1));
    }
    Finalize(select_slugs);
    select_slugs = nullptr;

    if (!slug_ids.empty()) {
      const std::string slug_owner_delete_sql = "DELETE FROM slug_ownership WHERE checklist=?;";
      if (Prepare(db_, slug_owner_delete_sql, &delete_slug_owner) != SQLITE_OK) {
        throw std::runtime_error("Failed to prepare slug ownership delete for checklist.");
      }
      sqlite3_bind_text(delete_slug_owner, 1, checklist.c_str(), -1, SQLITE_TRANSIENT);
      StepOrThrow(delete_slug_owner, "slug ownership delete");
      Finalize(delete_slug_owner);
      delete_slug_owner = nullptr;

      const std::string address_owner_delete_sql = "DELETE FROM address_ownership WHERE checklist=?;";
      if (Prepare(db_, address_owner_delete_sql, &delete_address_owner) != SQLITE_OK) {
        throw std::runtime_error("Failed to prepare address ownership delete for checklist.");
      }
      sqlite3_bind_text(delete_address_owner, 1, checklist.c_str(), -1, SQLITE_TRANSIENT);
      StepOrThrow(delete_address_owner, "address ownership delete");
      Finalize(delete_address_owner);
      delete_address_owner = nullptr;

      const std::string tpl_delete_sql =
          "DELETE FROM template_relationships WHERE subject_slug_id=? OR target_slug_id=?;";
      if (Prepare(db_, tpl_delete_sql, &delete_tpl_rel) != SQLITE_OK) {
        throw std::runtime_error("Failed to prepare template relationship delete for checklist.");
      }
      for (const auto& id : slug_ids) {
        sqlite3_reset(delete_tpl_rel);
        sqlite3_bind_text(delete_tpl_rel, 1, id.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(delete_tpl_rel, 2, id.c_str(), -1, SQLITE_TRANSIENT);
        StepOrThrow(delete_tpl_rel, "template relationship delete");
      }
      Finalize(delete_tpl_rel);
      delete_tpl_rel = nullptr;
    }

    if (!address_ids.empty()) {
      const std::string hist_delete_sql = "DELETE FROM history WHERE address_id=?;";
      if (Prepare(db_, hist_delete_sql, &delete_history) != SQLITE_OK) {
        throw std::runtime_error("Failed to prepare history delete for checklist.");
      }
      for (const auto& addr : address_ids) {
        sqlite3_reset(delete_history);
        sqlite3_bind_text(delete_history, 1, addr.c_str(), -1, SQLITE_TRANSIENT);
        StepOrThrow(delete_history, "history delete");
      }
      Finalize(delete_history);
      delete_history = nullptr;
    }

    const std::string slugs_sql = "DELETE FROM slugs WHERE checklist=?;";
    if (Prepare(db_, slugs_sql, &delete_slugs_stmt) != SQLITE_OK) {
      throw std::runtime_error("Failed to prepare checklist slug delete.");
    }
    sqlite3_bind_text(delete_slugs_stmt, 1, checklist.c_str(), -1, SQLITE_TRANSIENT);
    StepOrThrow(delete_slugs_stmt, "slug delete");
    removed = sqlite3_changes(db_);
    Finalize(delete_slugs_stmt);
    delete_slugs_stmt = nullptr;

    sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr);
  } catch (...) {
    rollback();
    Finalize(select_slugs);
    Finalize(delete_slug_owner);
    Finalize(delete_address_owner);
    Finalize(delete_tpl_rel);
    Finalize(delete_history);
    Finalize(delete_slugs_stmt);
    throw;
  }

  if (deleted_slugs) *deleted_slugs = removed;
}

void ChecklistStore::DeleteInstance(const std::string& checklist, const std::string& instance_id,
                                    int* deleted_slugs) {
  if (checklist.empty() || instance_id.empty()) {
    throw std::invalid_argument("Checklist and instance_id must not be empty.");
  }
  std::lock_guard<std::mutex> lock(mutex_);

  char* errmsg = nullptr;
  sqlite3_exec(db_, "BEGIN IMMEDIATE;", nullptr, nullptr, &errmsg);
  if (errmsg) {
    std::string message = errmsg;
    sqlite3_free(errmsg);
    throw std::runtime_error("Failed to begin transaction for instance delete: " + message);
  }

  auto rollback = [&]() { sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr); };

  sqlite3_stmt* select_slugs = nullptr;
  sqlite3_stmt* delete_history = nullptr;
  sqlite3_stmt* delete_slugs_stmt = nullptr;
  sqlite3_stmt *prune_slug_owner = nullptr;
  int removed = 0;
  try {
    std::vector<std::string> address_ids;
    const std::string select_sql =
        "SELECT address_id FROM slugs WHERE checklist=? AND instance_id=?;";
    if (Prepare(db_, select_sql, &select_slugs) != SQLITE_OK) {
      throw std::runtime_error("Failed to prepare instance slug lookup for delete.");
    }
    sqlite3_bind_text(select_slugs, 1, checklist.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(select_slugs, 2, instance_id.c_str(), -1, SQLITE_TRANSIENT);
    while (sqlite3_step(select_slugs) == SQLITE_ROW) {
      address_ids.push_back(ColumnText(select_slugs, 0));
    }
    Finalize(select_slugs);
    select_slugs = nullptr;

    if (!address_ids.empty()) {
      const std::string hist_delete_sql = "DELETE FROM history WHERE address_id=?;";
      if (Prepare(db_, hist_delete_sql, &delete_history) != SQLITE_OK) {
        throw std::runtime_error("Failed to prepare history delete for instance.");
      }
      for (const auto& addr : address_ids) {
        sqlite3_reset(delete_history);
        sqlite3_bind_text(delete_history, 1, addr.c_str(), -1, SQLITE_TRANSIENT);
        StepOrThrow(delete_history, "history delete");
      }
      Finalize(delete_history);
      delete_history = nullptr;
    }

    const std::string slugs_sql = "DELETE FROM slugs WHERE checklist=? AND instance_id=?;";
    if (Prepare(db_, slugs_sql, &delete_slugs_stmt) != SQLITE_OK) {
      throw std::runtime_error("Failed to prepare instance slug delete.");
    }
    sqlite3_bind_text(delete_slugs_stmt, 1, checklist.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(delete_slugs_stmt, 2, instance_id.c_str(), -1, SQLITE_TRANSIENT);
    StepOrThrow(delete_slugs_stmt, "slug delete");
    removed = sqlite3_changes(db_);
    Finalize(delete_slugs_stmt);
    delete_slugs_stmt = nullptr;

    const std::string prune_sql = "DELETE FROM slug_ownership WHERE checklist=? "
                                  "AND NOT EXISTS (SELECT 1 FROM address_ownership ao "
                                  "WHERE ao.slug_id=slug_ownership.slug_id "
                                  "AND ao.source_name=slug_ownership.source_name "
                                  "AND ao.pack=slug_ownership.pack "
                                  "AND ao.checklist_dir=slug_ownership.checklist_dir);";
    if (Prepare(db_, prune_sql, &prune_slug_owner) != SQLITE_OK) {
      throw std::runtime_error("Failed to prepare stale slug ownership prune.");
    }
    sqlite3_bind_text(prune_slug_owner, 1, checklist.c_str(), -1, SQLITE_TRANSIENT);
    StepOrThrow(prune_slug_owner, "stale slug ownership prune");
    Finalize(prune_slug_owner);
    prune_slug_owner = nullptr;

    sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr);
  } catch (...) {
    rollback();
    Finalize(select_slugs);
    Finalize(delete_history);
    Finalize(delete_slugs_stmt);
    Finalize(prune_slug_owner);
    throw;
  }

  if (deleted_slugs) *deleted_slugs = removed;
}
void ChecklistStore::ReplaceChecklist(const std::string& checklist,
                                      const std::vector<ChecklistSlug>& slugs) {
  if (checklist.empty()) {
    throw std::invalid_argument("Checklist name must not be empty.");
  }
  for (const auto& slug : slugs) {
    if (slug.checklist != checklist) {
      throw std::invalid_argument("All slugs must belong to checklist '" + checklist + "'.");
    }
  }

  std::lock_guard<std::mutex> lock(mutex_);

  char* errmsg = nullptr;
  sqlite3_exec(db_, "BEGIN IMMEDIATE;", nullptr, nullptr, &errmsg);
  if (errmsg) {
    std::string message = errmsg;
    sqlite3_free(errmsg);
    throw std::runtime_error("Failed to begin transaction for checklist replace: " + message);
  }

  auto rollback = [&]() {
    sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
  };

  sqlite3_stmt* delete_slugs = nullptr;
  sqlite3_stmt* insert_rel = nullptr;

  try {
    const std::string slugs_sql = "DELETE FROM slugs WHERE checklist=?;";

    if (Prepare(db_, slugs_sql, &delete_slugs) != SQLITE_OK) {
      throw std::runtime_error(std::string{"Failed to prepare checklist cleanup statements: "} +
                               sqlite3_errmsg(db_));
    }

    sqlite3_bind_text(delete_slugs, 1, checklist.c_str(), -1, SQLITE_TRANSIENT);
    StepOrThrow(delete_slugs, "slug delete");

    Finalize(delete_slugs);
    delete_slugs = nullptr;

    std::vector<std::pair<std::string, RelationshipEdge>> pending_edges;
    for (const auto& slug : slugs) {
      UpsertSlugUnlocked(slug);
      for (const auto& edge : slug.relationships) {
        pending_edges.emplace_back(slug.address_id, edge);
      }
    }

    const std::string rel_insert_sql =
        "INSERT INTO address_relationships (subject_address_id, predicate, target_address_id) "
        "VALUES (?,?,?);";
    if (Prepare(db_, rel_insert_sql, &insert_rel) != SQLITE_OK) {
      throw std::runtime_error("Failed to prepare relationship insert for checklist replace.");
    }

    for (const auto& item : pending_edges) {
      const auto& subject = item.first;
      const auto& edge = item.second;
      sqlite3_reset(insert_rel);
      sqlite3_bind_text(insert_rel, 1, subject.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(insert_rel, 2, edge.predicate.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(insert_rel, 3, edge.target.c_str(), -1, SQLITE_TRANSIENT);
      StepOrThrow(insert_rel, "relationship insert");
    }

    Finalize(insert_rel);
    insert_rel = nullptr;
    sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr);
  } catch (...) {
    Finalize(delete_slugs);
    Finalize(insert_rel);
    rollback();
    throw;
  }
}

void ChecklistStore::ApplyBulkUpdates(const std::vector<SlugUpdate>& updates) {
  if (updates.empty()) {
    return;
  }

  char* errmsg = nullptr;
  sqlite3_exec(db_, "BEGIN IMMEDIATE;", nullptr, nullptr, &errmsg);
  if (errmsg) {
    std::string message = errmsg;
    sqlite3_free(errmsg);
    throw std::runtime_error("Failed to begin transaction for bulk update: " + message);
  }

  try {
    for (const auto& update : updates) {
      ApplyUpdate(update);
    }
    sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr);
  } catch (...) {
    sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
    throw;
  }
}

void ChecklistStore::InsertHistorySnapshot(const ChecklistSlug& slug) {
  sqlite3_stmt* stmt = nullptr;
  const std::string sql =
      "INSERT OR IGNORE INTO history (address_id, timestamp, result, status, comment, entity_id) "
      "VALUES (?,?,?,?,?,?);";
  if (Prepare(db_, sql, &stmt) != SQLITE_OK) {
    Finalize(stmt);
    throw std::runtime_error("Failed to prepare history insert");
  }

  sqlite3_bind_text(stmt, 1, slug.address_id.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 2, slug.timestamp.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 3, slug.result.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 4, StatusToString(slug.status).c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 5, slug.comment.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 6, slug.entity_id.c_str(), -1, SQLITE_TRANSIENT);

  StepOrThrow(stmt, "history insert");
  Finalize(stmt);
}

std::vector<RelationshipEdge> ChecklistStore::LoadOutgoingAddressEdges(
    const std::string& address_id) const {
  std::vector<RelationshipEdge> edges;
  sqlite3_stmt* stmt = nullptr;
  if (Prepare(db_, "SELECT predicate, target_address_id FROM address_relationships "
                   "WHERE subject_address_id=?;",
              &stmt) != SQLITE_OK) {
    Finalize(stmt);
    throw std::runtime_error("Failed to prepare outgoing relationships query");
  }

  sqlite3_bind_text(stmt, 1, address_id.c_str(), -1, SQLITE_TRANSIENT);
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    RelationshipEdge edge;
    edge.predicate = ColumnText(stmt, 0);
    edge.target = ColumnText(stmt, 1);
    edges.push_back(edge);
  }
  Finalize(stmt);
  return edges;
}

std::vector<ChecklistSlug> ChecklistStore::ExportAllSlugs() const {
  std::vector<ChecklistSlug> slugs;
  std::lock_guard<std::mutex> lock(mutex_);
  sqlite3_stmt* stmt = nullptr;
  const std::string sql =
      "SELECT s.address_id, s.slug_id, s.instance_id, COALESCE(ic.principal, ''), "
      "s.checklist, s.section, s.procedure, s.action, s.spec, "
      "s.instructions, s.result, s.status, s.comment, s.timestamp, s.entity_id, "
      "COALESCE(so.address_order, 0) "
      "FROM slugs s "
      "LEFT JOIN instance_catalog ic ON s.instance_id = ic.instance_id "
      "LEFT JOIN slug_order so ON s.address_id = so.address_id "
      "ORDER BY s.checklist, s.instance_id, so.address_order IS NULL, so.address_order, "
      "s.section, s.procedure, s.action;";

  if (Prepare(db_, sql, &stmt) != SQLITE_OK) {
    Finalize(stmt);
    throw std::runtime_error("Failed to prepare export query");
  }

  while (sqlite3_step(stmt) == SQLITE_ROW) {
    slugs.push_back(BuildSlug(stmt));
  }
  Finalize(stmt);

  for (auto& slug : slugs) {
    slug.relationships = LoadOutgoingAddressEdges(slug.address_id);
  }
  return slugs;
}

std::vector<std::string> ChecklistStore::ListChecklists() const {
  std::vector<std::string> names;
  std::lock_guard<std::mutex> lock(mutex_);
  sqlite3_stmt* stmt = nullptr;
  if (Prepare(db_, "SELECT DISTINCT checklist FROM slugs ORDER BY checklist;", &stmt) !=
      SQLITE_OK) {
    Finalize(stmt);
    throw std::runtime_error("Failed to prepare checklist listing query");
  }

  while (sqlite3_step(stmt) == SQLITE_ROW) {
    names.push_back(ColumnText(stmt, 0));
  }
  Finalize(stmt);
  return names;
}

}  // namespace core
