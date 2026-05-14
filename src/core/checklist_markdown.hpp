#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "core/checklist_store.hpp"

namespace core::markdown {

struct ParsedChecklist {
  std::string checklist;
  std::vector<ChecklistSlug> slugs;
  std::vector<TemplateRelationship> template_relationships;
  std::vector<AddressRelationship> address_relationships;
  struct UnicodeWarning {
    std::uint32_t codepoint = 0;
    std::size_t line = 0;
    std::size_t column = 0;
    std::size_t count = 0;
  };
  std::vector<UnicodeWarning> unicode_warnings;
};

enum class RelationshipExportMode { kTemplate, kAddress };
enum class RelationshipIdentityFormat { kId };

std::string Trim(const std::string& value);
bool StartsWith(const std::string& value, const std::string& prefix);
std::string ToLower(std::string value);
std::string StripPrefix(const std::string& value, const std::string& prefix);
void Require(bool condition, const std::string& message);

ParsedChecklist ParseChecklistMarkdown(const std::string& checklist_name,
                                       const std::string& content);

std::string ExportChecklistMarkdown(const std::string& checklist_name,
                                    const std::vector<ChecklistSlug>& slugs,
                                    const std::vector<TemplateRelationship>& relationships,
                                    RelationshipExportMode relationship_mode,
                                    RelationshipIdentityFormat identity_format);

}  // namespace core::markdown
