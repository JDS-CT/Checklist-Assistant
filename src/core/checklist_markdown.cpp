#include "core/checklist_markdown.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <unordered_map>
#include <utility>

#include "core/logging.hpp"

namespace {

using core::ChecklistSlug;
using core::ChecklistStatus;
using core::AddressRelationship;
using core::RelationshipEdge;
using core::TemplateRelationship;
using core::markdown::ParsedChecklist;
using UnicodeWarning = core::markdown::ParsedChecklist::UnicodeWarning;

struct ProcedureBuilder {
  std::string section;
  std::string procedure;
  std::string action;
  std::string spec;
  std::string result;
  std::string status_text;
  std::string comment;
  std::vector<std::string> instruction_lines;
  bool relationships_seen = false;
  bool relationships_source_seen = false;
  std::string relationships_source_line;
  std::vector<std::string> relationship_lines;
  bool instructions_seen = false;
  int bullet_index = 0;
};

struct BulletField {
  std::string name;
  std::string value;
};

enum class RelationshipScope { kTemplate, kAddress };

constexpr std::string_view kSlugSuccessorPredicate = "slugSuccessor";
constexpr std::string_view kSlugPredecessorPredicate = "slugPredecessor";

struct RelationshipSection {
  std::string subject_slug_id;
  std::string source_line;
  bool source_line_present = false;
  std::vector<std::string> lines;
};

struct SourceIdentity {
  bool valid = false;
  bool is_address = false;
  std::string slug_id;
  std::string address_id;
  std::string instance_id;
};

enum class TargetKind { kInvalid, kSlug, kAddress, kSectionProcedure };

struct TargetSpec {
  TargetKind kind = TargetKind::kInvalid;
  std::string slug_id;
  std::string address_id;
  std::string section;
  std::string procedure;
};

bool DecodeUtf8(const std::string& input, std::size_t* index, char32_t* codepoint) {
  if (*index >= input.size()) {
    return false;
  }
  const unsigned char lead = static_cast<unsigned char>(input[*index]);
  if (lead < 0x80) {
    *codepoint = lead;
    ++(*index);
    return true;
  }
  if (lead < 0xC2) {
    ++(*index);
    return false;
  }
  const std::size_t remaining = input.size() - *index;
  if ((lead & 0xE0) == 0xC0) {
    if (remaining < 2) {
      ++(*index);
      return false;
    }
    const unsigned char b1 = static_cast<unsigned char>(input[*index + 1]);
    if ((b1 & 0xC0) != 0x80) {
      ++(*index);
      return false;
    }
    const char32_t cp = (static_cast<char32_t>(lead & 0x1F) << 6) |
                        static_cast<char32_t>(b1 & 0x3F);
    if (cp < 0x80) {
      ++(*index);
      return false;
    }
    *codepoint = cp;
    *index += 2;
    return true;
  }
  if ((lead & 0xF0) == 0xE0) {
    if (remaining < 3) {
      ++(*index);
      return false;
    }
    const unsigned char b1 = static_cast<unsigned char>(input[*index + 1]);
    const unsigned char b2 = static_cast<unsigned char>(input[*index + 2]);
    if ((b1 & 0xC0) != 0x80 || (b2 & 0xC0) != 0x80) {
      ++(*index);
      return false;
    }
    const char32_t cp = (static_cast<char32_t>(lead & 0x0F) << 12) |
                        (static_cast<char32_t>(b1 & 0x3F) << 6) |
                        static_cast<char32_t>(b2 & 0x3F);
    if (cp < 0x800 || (cp >= 0xD800 && cp <= 0xDFFF)) {
      ++(*index);
      return false;
    }
    *codepoint = cp;
    *index += 3;
    return true;
  }
  if ((lead & 0xF8) == 0xF0) {
    if (remaining < 4) {
      ++(*index);
      return false;
    }
    const unsigned char b1 = static_cast<unsigned char>(input[*index + 1]);
    const unsigned char b2 = static_cast<unsigned char>(input[*index + 2]);
    const unsigned char b3 = static_cast<unsigned char>(input[*index + 3]);
    if ((b1 & 0xC0) != 0x80 || (b2 & 0xC0) != 0x80 || (b3 & 0xC0) != 0x80) {
      ++(*index);
      return false;
    }
    const char32_t cp = (static_cast<char32_t>(lead & 0x07) << 18) |
                        (static_cast<char32_t>(b1 & 0x3F) << 12) |
                        (static_cast<char32_t>(b2 & 0x3F) << 6) |
                        static_cast<char32_t>(b3 & 0x3F);
    if (cp < 0x10000 || cp > 0x10FFFF) {
      ++(*index);
      return false;
    }
    *codepoint = cp;
    *index += 4;
    return true;
  }
  ++(*index);
  return false;
}

bool IsLatexSupportedLatin1(char32_t codepoint) {
  switch (codepoint) {
    case 0x00C0:
    case 0x00C1:
    case 0x00C2:
    case 0x00C3:
    case 0x00C4:
    case 0x00C5:
    case 0x00C6:
    case 0x00C7:
    case 0x00C8:
    case 0x00C9:
    case 0x00CA:
    case 0x00CB:
    case 0x00CC:
    case 0x00CD:
    case 0x00CE:
    case 0x00CF:
    case 0x00D0:
    case 0x00D1:
    case 0x00D2:
    case 0x00D3:
    case 0x00D4:
    case 0x00D5:
    case 0x00D6:
    case 0x00D8:
    case 0x00D9:
    case 0x00DA:
    case 0x00DB:
    case 0x00DC:
    case 0x00DD:
    case 0x00DE:
    case 0x00DF:
    case 0x00E0:
    case 0x00E1:
    case 0x00E2:
    case 0x00E3:
    case 0x00E4:
    case 0x00E5:
    case 0x00E6:
    case 0x00E7:
    case 0x00E8:
    case 0x00E9:
    case 0x00EA:
    case 0x00EB:
    case 0x00EC:
    case 0x00ED:
    case 0x00EE:
    case 0x00EF:
    case 0x00F0:
    case 0x00F1:
    case 0x00F2:
    case 0x00F3:
    case 0x00F4:
    case 0x00F5:
    case 0x00F6:
    case 0x00F8:
    case 0x00F9:
    case 0x00FA:
    case 0x00FB:
    case 0x00FC:
    case 0x00FD:
    case 0x00FE:
    case 0x00FF:
      return true;
    default:
      return false;
  }
}

bool IsLatexSupportedCodepoint(char32_t codepoint) {
  if (codepoint < 0x80) {
    return true;
  }
  switch (codepoint) {
    case 0x00A0:
    case 0x00A1:
    case 0x00B0:
    case 0x00B1:
    case 0x00B2:
    case 0x00B3:
    case 0x00B5:
    case 0x00B7:
    case 0x00BC:
    case 0x00BD:
    case 0x00BE:
    case 0x00BF:
    case 0x00D7:
    case 0x00F7:
    case 0x03BC:
    case 0x2013:
    case 0x2014:
    case 0x2018:
    case 0x2019:
    case 0x201C:
    case 0x201D:
    case 0x2022:
    case 0x2026:
    case 0x2122:
    case 0x2264:
    case 0x2265:
      return true;
    default:
      return IsLatexSupportedLatin1(codepoint);
  }
}

std::vector<UnicodeWarning> ScanUnsupportedLatexUnicode(const std::vector<std::string>& lines) {
  std::unordered_map<char32_t, UnicodeWarning> seen;
  for (std::size_t line_index = 0; line_index < lines.size(); ++line_index) {
    const std::string& line = lines[line_index];
    std::size_t index = 0;
    std::size_t column = 1;
    while (index < line.size()) {
      char32_t codepoint = 0;
      const std::size_t before = index;
      const bool decoded = DecodeUtf8(line, &index, &codepoint);
      if (!decoded) {
        codepoint = 0xFFFD;
      }
      if (!IsLatexSupportedCodepoint(codepoint)) {
        auto& entry = seen[codepoint];
        if (entry.count == 0) {
          entry.codepoint = static_cast<std::uint32_t>(codepoint);
          entry.line = line_index + 1;
          entry.column = column;
        }
        ++entry.count;
      }
      if (index == before) {
        ++index;
      }
      ++column;
    }
  }
  std::vector<UnicodeWarning> warnings;
  warnings.reserve(seen.size());
  for (const auto& item : seen) {
    warnings.push_back(item.second);
  }
  return warnings;
}

bool IsValidPredicateToken(const std::string& value) {
  constexpr std::size_t kMaxLen = 128;
  if (value.empty() || value.size() > kMaxLen) return false;

  const auto is_ascii_alpha = [](unsigned char ch) {
    return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z');
  };
  const auto is_ascii_digit = [](unsigned char ch) { return (ch >= '0' && ch <= '9'); };

  const unsigned char first = static_cast<unsigned char>(value.front());
  if (!is_ascii_alpha(first)) return false;

  for (const unsigned char ch : value) {
    if (ch >= 0x80) return false;
    if (is_ascii_alpha(ch) || is_ascii_digit(ch) || ch == '_') continue;
    return false;
  }
  return true;
}

std::optional<std::string> NormalizeAddressId(const std::string& value) {
  if (core::IsValidBase32Id(value, 32)) {
    return value;
  }
  constexpr std::string_view kSep = "||";
  if (value.size() != 34 || value.find(kSep) != 16) {
    return std::nullopt;
  }
  const std::string slug = value.substr(0, 16);
  const std::string inst = value.substr(18);
  if (!core::IsValidBase32Id(slug, 16) || !core::IsValidBase32Id(inst, 16)) {
    return std::nullopt;
  }
  return slug + inst;
}

std::optional<std::pair<std::string, std::string>> SplitAddressId(const std::string& value) {
  const auto normalized = NormalizeAddressId(value);
  if (!normalized) {
    return std::nullopt;
  }
  return std::make_pair(normalized->substr(0, 16), normalized->substr(16, 16));
}

std::vector<std::string> SplitTupleFields(const std::string& inner) {
  std::vector<std::string> fields;
  std::string current;
  int depth = 0;
  for (const char ch : inner) {
    if (ch == '(') {
      ++depth;
      current.push_back(ch);
      continue;
    }
    if (ch == ')' && depth > 0) {
      --depth;
      current.push_back(ch);
      continue;
    }
    if (ch == ',' && depth == 0) {
      fields.push_back(core::markdown::Trim(current));
      current.clear();
      continue;
    }
    current.push_back(ch);
  }
  fields.push_back(core::markdown::Trim(current));
  return fields;
}

std::optional<std::vector<std::string>> ParseTupleFields(const std::string& value) {
  const std::string trimmed = core::markdown::Trim(value);
  if (trimmed.size() < 2 || trimmed.front() != '(' || trimmed.back() != ')') {
    return std::nullopt;
  }
  const std::string inner = core::markdown::Trim(trimmed.substr(1, trimmed.size() - 2));
  if (inner.empty()) {
    return std::nullopt;
  }
  return SplitTupleFields(inner);
}

SourceIdentity ParseSourceIdentityLine(const std::string& line) {
  SourceIdentity source;
  const std::string trimmed = core::markdown::Trim(line);
  if (trimmed.empty()) {
    return source;
  }
  if (const auto split = SplitAddressId(trimmed)) {
    source.valid = true;
    source.is_address = true;
    source.slug_id = split->first;
    source.instance_id = split->second;
    source.address_id = split->first + split->second;
    return source;
  }
  if (core::IsValidBase32Id(trimmed, 16)) {
    source.valid = true;
    source.slug_id = trimmed;
    return source;
  }
  return source;
}

std::optional<std::string> ParseLegacySlugId(const std::string& value, bool* was_address) {
  if (was_address) {
    *was_address = false;
  }
  const std::string trimmed = core::markdown::Trim(value);
  if (trimmed.empty()) {
    return std::nullopt;
  }
  if (const auto split = SplitAddressId(trimmed)) {
    if (was_address) {
      *was_address = true;
    }
    return split->first;
  }
  if (core::IsValidBase32Id(trimmed, 16)) {
    return trimmed;
  }
  return std::nullopt;
}

TargetSpec ParseTargetSpec(const std::string& value) {
  TargetSpec spec;
  const std::string trimmed = core::markdown::Trim(value);
  if (trimmed.empty()) {
    return spec;
  }
  if (const auto split = SplitAddressId(trimmed)) {
    spec.kind = TargetKind::kAddress;
    spec.address_id = split->first + split->second;
    return spec;
  }
  if (core::IsValidBase32Id(trimmed, 16)) {
    spec.kind = TargetKind::kSlug;
    spec.slug_id = trimmed;
    return spec;
  }
  const auto tuple_fields = ParseTupleFields(trimmed);
  if (!tuple_fields) {
    return spec;
  }
  if (tuple_fields->size() == 2) {
    if (tuple_fields->at(0).empty() || tuple_fields->at(1).empty()) {
      return spec;
    }
    spec.kind = TargetKind::kSectionProcedure;
    spec.section = tuple_fields->at(0);
    spec.procedure = tuple_fields->at(1);
    return spec;
  }
  return spec;
}

std::string StripMarkdownEmphasis(const std::string& value) {
  if (value.size() >= 4) {
    if (value.substr(0, 2) == "**" && value.substr(value.size() - 2) == "**") {
      return value.substr(2, value.size() - 4);
    }
    if (value.substr(0, 2) == "__" && value.substr(value.size() - 2) == "__") {
      return value.substr(2, value.size() - 4);
    }
  }
  return value;
}

std::optional<BulletField> ParseBulletFieldLine(const std::string& trimmed) {
  if (trimmed.empty() || trimmed.front() != '-') {
    return std::nullopt;
  }
  std::size_t pos = 1;
  while (pos < trimmed.size() && std::isspace(static_cast<unsigned char>(trimmed[pos])) != 0) {
    ++pos;
  }
  if (pos >= trimmed.size()) {
    return std::nullopt;
  }
  const std::string rest = trimmed.substr(pos);
  const auto colon_pos = rest.find(':');
  if (colon_pos == std::string::npos) {
    return std::nullopt;
  }
  std::string field = core::markdown::Trim(rest.substr(0, colon_pos));
  std::string value = core::markdown::Trim(rest.substr(colon_pos + 1));
  field = core::markdown::Trim(StripMarkdownEmphasis(field));
  if (field.empty()) {
    return std::nullopt;
  }
  return BulletField{field, value};
}

bool IsHorizontalRule(const std::string& trimmed) {
  if (trimmed.size() < 3) {
    return false;
  }
  for (const char ch : trimmed) {
    if (ch != '-') {
      return false;
    }
  }
  return true;
}

std::string NormalizeFrontMatterKey(const std::string& key) {
  std::string normalized = core::markdown::Trim(key);
  if (!normalized.empty() && normalized.front() == '-') {
    normalized = core::markdown::Trim(normalized.substr(1));
  }
  return normalized;
}

std::string StripUtf8Bom(const std::string& value) {
  if (value.size() >= 3 && static_cast<unsigned char>(value[0]) == 0xEF &&
      static_cast<unsigned char>(value[1]) == 0xBB &&
      static_cast<unsigned char>(value[2]) == 0xBF) {
    return value.substr(3);
  }
  return value;
}

std::string JoinLines(const std::vector<std::string>& lines) {
  std::ostringstream out;
  for (std::size_t i = 0; i < lines.size(); ++i) {
    if (i > 0) {
      out << "\n";
    }
    out << lines[i];
  }
  return out.str();
}

ChecklistSlug FinalizeSlug(const std::string& checklist, ProcedureBuilder builder) {
  core::markdown::Require(!builder.section.empty(), "Section (H2) is required before procedures.");
  core::markdown::Require(!builder.procedure.empty(), "Procedure (H3) is required.");
  core::markdown::Require(builder.bullet_index == 5,
                          "Procedure requires Action/Spec/Result/Status/Comment bullets in order.");
  core::markdown::Require(builder.instructions_seen,
                          "Procedure block is missing '#### Instructions' section.");

  while (!builder.instruction_lines.empty() && builder.instruction_lines.back().empty()) {
    builder.instruction_lines.pop_back();
  }

  ChecklistSlug slug;
  slug.checklist = checklist;
  slug.section = builder.section;
  slug.procedure = builder.procedure;
  slug.action = builder.action;
  slug.spec = builder.spec;
  slug.result = builder.result;
  slug.comment = builder.comment;
  slug.instructions = JoinLines(builder.instruction_lines);
  if (!builder.status_text.empty()) {
    slug.status = core::ParseStatus(builder.status_text);
    core::markdown::Require(slug.status != ChecklistStatus::kUnknown,
                            "Status must be Pass, Fail, NA, or Other.");
  } else {
    slug.status = ChecklistStatus::kUnknown;
  }

  slug.slug_id = core::ComputeSlugId(slug.checklist, slug.section, slug.procedure, slug.action,
                                     slug.spec, slug.instructions);
  return slug;
}

}  // namespace

namespace core::markdown {

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

bool StartsWith(const std::string& value, const std::string& prefix) {
  return value.compare(0, prefix.size(), prefix) == 0;
}

std::string ToLower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return value;
}

std::string StripPrefix(const std::string& value, const std::string& prefix) {
  if (StartsWith(value, prefix)) {
    return value.substr(prefix.size());
  }
  return value;
}

void Require(bool condition, const std::string& message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

ParsedChecklist ParseChecklistMarkdown(const std::string& checklist_name,
                                       const std::string& content) {
  ParsedChecklist parsed;
  parsed.checklist = checklist_name;
  std::vector<RelationshipSection> relationship_sections;

  std::vector<std::string> lines;
  {
    const std::string normalized = StripUtf8Bom(content);
    std::string current;
    std::istringstream stream(normalized);
    while (std::getline(stream, current)) {
      if (!current.empty() && current.back() == '\r') {
        current.pop_back();
      }
      lines.push_back(current);
    }
  }
  parsed.unicode_warnings = ScanUnsupportedLatexUnicode(lines);

  std::string current_section;
  ProcedureBuilder builder;
  bool checklist_seen = false;
  bool in_front_matter = false;
  std::string front_matter_checklist;
  bool front_matter_checklist_list_item = false;
  bool in_instructions = false;
  bool in_relationships = false;

  constexpr std::array<std::string_view, 5> kFieldOrder = {"Action", "Spec", "Result", "Status",
                                                           "Comment"};

  auto flush = [&]() {
    if (builder.procedure.empty()) {
      return;
    }
    auto slug = FinalizeSlug(parsed.checklist, builder);
    if (builder.relationships_seen) {
      RelationshipSection section;
      section.subject_slug_id = slug.slug_id;
      section.source_line = builder.relationships_source_line;
      section.source_line_present = builder.relationships_source_seen;
      section.lines = builder.relationship_lines;
      relationship_sections.push_back(std::move(section));
    }
    parsed.slugs.push_back(std::move(slug));
    builder = ProcedureBuilder{};
    builder.section = current_section;
    in_instructions = false;
    in_relationships = false;
  };

  const auto next_non_empty_line = [&](std::size_t start) -> std::string {
    for (std::size_t idx = start; idx < lines.size(); ++idx) {
      const std::string candidate = Trim(lines[idx]);
      if (!candidate.empty()) {
        return candidate;
      }
    }
    return std::string{};
  };
  const auto is_heading_boundary = [](const std::string& line) -> bool {
    return StartsWith(line, "# ") || StartsWith(line, "## ") || StartsWith(line, "### ") ||
           StartsWith(line, "#### ");
  };

  for (std::size_t line_index = 0; line_index < lines.size(); ++line_index) {
    const auto& raw_line = lines[line_index];
    const std::string trimmed = Trim(raw_line);

    if (!checklist_seen && builder.procedure.empty() && trimmed == "---") {
      in_front_matter = !in_front_matter;
      continue;
    }
    if (in_front_matter) {
      if (trimmed == "---") {
        in_front_matter = false;
        continue;
      }
      const auto colon_pos = trimmed.find(':');
      if (colon_pos != std::string::npos) {
        const bool is_list_item = trimmed.front() == '-';
        const std::string key = NormalizeFrontMatterKey(trimmed.substr(0, colon_pos));
        const std::string value = Trim(trimmed.substr(colon_pos + 1));
        if (key == "Checklist") {
          front_matter_checklist = value;
          front_matter_checklist_list_item = is_list_item;
        }
      }
      continue;
    }

    if (trimmed.empty()) {
      if (in_instructions) {
        builder.instruction_lines.push_back(std::string{});
      }
      continue;
    }

    if (IsHorizontalRule(trimmed)) {
      if (in_relationships) {
        continue;
      }
      if (!in_instructions) {
        continue;
      }
      const std::string next_line = next_non_empty_line(line_index + 1);
      if (is_heading_boundary(next_line)) {
        in_instructions = false;
        continue;
      }
    }

    if (in_instructions) {
      if (StartsWith(trimmed, "#### ")) {
        in_instructions = false;
        in_relationships = Trim(trimmed.substr(5)) == "Relationships";
      } else if (StartsWith(trimmed, "### ") || StartsWith(trimmed, "## ") ||
                 StartsWith(trimmed, "# ")) {
        in_instructions = false;
      } else {
        builder.instruction_lines.push_back(raw_line);
        continue;
      }
    }

    if (StartsWith(trimmed, "# Checklist:")) {
      flush();
      const std::string checklist = Trim(trimmed.substr(std::string{"# Checklist:"}.size()));
      Require(!checklist.empty(), "Checklist heading must include a name.");
      if (parsed.checklist.empty()) {
        parsed.checklist = checklist;
      } else if (parsed.checklist != checklist) {
        throw std::runtime_error("Checklist heading '" + checklist +
                                 "' does not match expected checklist '" + parsed.checklist + "'.");
      }
      checklist_seen = true;
      current_section.clear();
      builder = ProcedureBuilder{};
      continue;
    }

    if (StartsWith(trimmed, "# ")) {
      flush();
      const std::string checklist = Trim(trimmed.substr(2));
      Require(!checklist.empty(), "Checklist heading must include a name.");
      Require(!checklist_seen, "Checklist heading already defined.");
      if (parsed.checklist.empty()) {
        parsed.checklist = checklist;
      } else if (parsed.checklist != checklist) {
        throw std::runtime_error("Checklist heading '" + checklist +
                                 "' does not match expected checklist '" + parsed.checklist + "'.");
      }
      checklist_seen = true;
      current_section.clear();
      builder = ProcedureBuilder{};
      continue;
    }

    if (StartsWith(trimmed, "## Section:")) {
      Require(checklist_seen, "Section encountered before checklist heading.");
      flush();
      current_section = Trim(trimmed.substr(std::string{"## Section:"}.size()));
      Require(!current_section.empty(), "Section heading must include a name.");
      builder.section = current_section;
      continue;
    }

    if (StartsWith(trimmed, "## ")) {
      Require(checklist_seen, "Section encountered before checklist heading.");
      flush();
      current_section = Trim(trimmed.substr(3));
      Require(!current_section.empty(), "Section heading must include a name.");
      builder.section = current_section;
      continue;
    }

    if (StartsWith(trimmed, "### Procedure:")) {
      Require(checklist_seen, "Procedure encountered before checklist heading.");
      flush();
      builder = ProcedureBuilder{};
      builder.section = current_section;
      builder.procedure = Trim(trimmed.substr(std::string{"### Procedure:"}.size()));
      Require(!builder.section.empty(), "Procedure encountered before any Section heading.");
      Require(!builder.procedure.empty(), "Procedure heading must include a name.");
      continue;
    }

    if (StartsWith(trimmed, "### ")) {
      Require(checklist_seen, "Procedure encountered before checklist heading.");
      flush();
      builder = ProcedureBuilder{};
      builder.section = current_section;
      builder.procedure = Trim(trimmed.substr(4));
      Require(!builder.section.empty(), "Procedure encountered before any Section heading.");
      Require(!builder.procedure.empty(), "Procedure heading must include a name.");
      continue;
    }

    if (StartsWith(trimmed, "#### ")) {
      Require(!builder.procedure.empty(),
              "Subsection encountered before a procedure heading was defined.");
      const std::string header = Trim(trimmed.substr(5));
      if (header == "Instructions") {
        Require(builder.bullet_index == static_cast<int>(kFieldOrder.size()),
                "Instructions must follow the Action/Spec/Result/Status/Comment bullets.");
        in_instructions = true;
        in_relationships = false;
        builder.instructions_seen = true;
      } else if (header == "Relationships") {
        Require(builder.instructions_seen,
                "Relationships must follow the Instructions section for each procedure.");
        in_relationships = true;
        in_instructions = false;
        builder.relationships_seen = true;
        builder.relationships_source_seen = false;
        builder.relationships_source_line.clear();
        builder.relationship_lines.clear();
      } else {
        throw std::runtime_error("Unknown subsection heading under procedure: " + header);
      }
      continue;
    }

    if (in_relationships) {
      if (StartsWith(trimmed, "##### ")) {
        continue;
      }
      if (!StartsWith(trimmed, "-")) {
        core::logging::LogWarn("Skipping malformed relationship line: " + trimmed);
        continue;
      }
      const auto edge_text = Trim(trimmed.substr(1));
      if (edge_text.empty()) {
        core::logging::LogWarn("Skipping empty relationship bullet line.");
        continue;
      }
      if (!builder.relationships_source_seen) {
        builder.relationships_source_seen = true;
        builder.relationships_source_line = edge_text;
        continue;
      }
      builder.relationship_lines.push_back(edge_text);
      continue;
    }

    if (StartsWith(trimmed, "-")) {
      Require(!builder.procedure.empty(), "Bullets must appear under a procedure heading.");
      if (builder.bullet_index >= static_cast<int>(kFieldOrder.size())) {
        throw std::runtime_error("Unexpected bullet after Comment for procedure '" +
                                 builder.procedure + "'.");
      }
      const auto expected = std::string{kFieldOrder[builder.bullet_index]};
      const auto parsed_field = ParseBulletFieldLine(trimmed);
      Require(parsed_field.has_value(),
              "Expected '" + expected + "' bullet for procedure '" + builder.procedure + "'.");
      Require(parsed_field->name == expected,
              "Expected '" + expected + "' bullet for procedure '" + builder.procedure + "'.");
      const std::string value = parsed_field->value;
      switch (builder.bullet_index) {
        case 0:
          Require(!value.empty(), "Action bullet must be non-empty.");
          builder.action = value;
          break;
        case 1:
          Require(!value.empty(), "Spec bullet must be non-empty.");
          builder.spec = value;
          break;
        case 2:
          builder.result = value;
          break;
        case 3:
          builder.status_text = value;
          break;
        case 4:
          builder.comment = value;
          break;
        default:
          break;
      }
      builder.bullet_index++;
      continue;
    }

    throw std::runtime_error("Unrecognized line in checklist Markdown: " + trimmed);
  }

  Require(!in_front_matter, "Front-matter block must be closed with '---'.");
  flush();

  Require(checklist_seen, "Checklist Markdown must include a checklist heading.");
  if (!front_matter_checklist.empty() && parsed.checklist != front_matter_checklist) {
    if (front_matter_checklist_list_item) {
      core::logging::LogWarn("Front-matter Checklist '" + front_matter_checklist +
                             "' does not match heading '" + parsed.checklist +
                             "'. Ignoring front-matter value.");
    } else {
      throw std::runtime_error("Front-matter Checklist '" + front_matter_checklist +
                               "' does not match heading '" + parsed.checklist + "'.");
    }
  }
  Require(!parsed.slugs.empty(), "No checklist procedures were parsed from Markdown.");

  std::unordered_map<std::string, std::vector<std::string>> slugs_by_section_proc;
  for (const auto& slug : parsed.slugs) {
    const std::string key = slug.section + "\n" + slug.procedure;
    slugs_by_section_proc[key].push_back(slug.slug_id);
  }

  const auto resolve_section_proc = [&](const TargetSpec& target) -> std::vector<std::string> {
    std::vector<std::string> matches;
    const std::string key = target.section + "\n" + target.procedure;
    const auto it = slugs_by_section_proc.find(key);
    if (it == slugs_by_section_proc.end()) {
      core::logging::LogWarn("No slug found for relationship tuple (" + target.section + ", " +
                             target.procedure + ").");
      return matches;
    }
    matches = it->second;
    if (matches.size() > 1) {
      core::logging::LogWarn("Ambiguous relationship tuple (" + target.section + ", " +
                             target.procedure + "); attaching to all matches.");
    }
    return matches;
  };

  for (const auto& section : relationship_sections) {
    std::vector<std::string> lines = section.lines;
    SourceIdentity source;
    bool default_source = false;
    if (section.source_line_present) {
      source = ParseSourceIdentityLine(section.source_line);
      if (!source.valid) {
        core::logging::LogWarn("Invalid relationships source identity: " + section.source_line +
                               "; using computed slug_id.");
        default_source = true;
        if (!section.source_line.empty()) {
          lines.insert(lines.begin(), section.source_line);
        }
      }
    } else {
      core::logging::LogWarn("Relationships section missing source identity line for slug_id " +
                             section.subject_slug_id + "; using computed slug_id.");
      default_source = true;
    }
    if (default_source) {
      source.valid = true;
      source.is_address = false;
      source.slug_id = section.subject_slug_id;
    } else {
      if (!source.is_address && !source.slug_id.empty() &&
          source.slug_id != section.subject_slug_id) {
        core::logging::LogWarn(
            "Relationships source slug_id does not match procedure slug_id; "
            "using computed slug_id for template relationships.");
      }
      if (source.is_address && !source.slug_id.empty() &&
          source.slug_id != section.subject_slug_id) {
        core::logging::LogWarn(
            "Relationships source address_id does not match procedure slug_id; "
            "using explicit address_id.");
      }
    }

    if (section.source_line_present) {
      bool legacy_address = false;
      const auto legacy_slug = ParseLegacySlugId(section.source_line, &legacy_address);
      if (legacy_slug && *legacy_slug != section.subject_slug_id) {
        parsed.template_relationships.push_back(
            TemplateRelationship{*legacy_slug, std::string{kSlugSuccessorPredicate},
                                 section.subject_slug_id});
        parsed.template_relationships.push_back(
            TemplateRelationship{section.subject_slug_id, std::string{kSlugPredecessorPredicate},
                                 *legacy_slug});
        if (legacy_address) {
          core::logging::LogWarn(
              "Legacy relationship address_id detected; stripped instance_id and linked slugSuccessor.");
        } else {
          core::logging::LogWarn(
              "Legacy relationship slug_id detected; linked slugSuccessor to current slug.");
        }
      }
    }

    if (lines.empty()) {
      continue;
    }

    for (const auto& line : lines) {
      std::string predicate;
      TargetSpec target;
      const auto space_pos = line.find(' ');
      if (space_pos == std::string::npos) {
        bool legacy_address = false;
        const auto legacy_slug = ParseLegacySlugId(line, &legacy_address);
        if (legacy_slug && *legacy_slug != section.subject_slug_id) {
          parsed.template_relationships.push_back(
              TemplateRelationship{*legacy_slug, std::string{kSlugSuccessorPredicate},
                                   section.subject_slug_id});
          parsed.template_relationships.push_back(
              TemplateRelationship{section.subject_slug_id, std::string{kSlugPredecessorPredicate},
                                   *legacy_slug});
          if (legacy_address) {
            core::logging::LogWarn(
                "Legacy relationship address_id detected; stripped instance_id and linked slugSuccessor.");
          } else {
            core::logging::LogWarn(
                "Legacy relationship slug_id detected; linked slugSuccessor to current slug.");
          }
        } else {
          core::logging::LogWarn("Skipping relationship without target: " + line);
        }
        continue;
      }
      predicate = Trim(line.substr(0, space_pos));
      const std::string target_text = Trim(line.substr(space_pos + 1));
      if (predicate.empty() || target_text.empty()) {
        core::logging::LogWarn("Skipping relationship with empty predicate/target: " + line);
        continue;
      }
      if (!IsValidPredicateToken(predicate)) {
        core::logging::LogWarn("Skipping relationship with invalid predicate token: " + predicate);
        continue;
      }
      target = ParseTargetSpec(target_text);
      if (target.kind == TargetKind::kInvalid) {
        core::logging::LogWarn("Skipping relationship with invalid target: " + target_text);
        continue;
      }

      if (!source.is_address) {
        std::vector<std::string> targets;
        switch (target.kind) {
          case TargetKind::kSlug:
            targets.push_back(target.slug_id);
            break;
          case TargetKind::kAddress:
            targets.push_back(target.address_id.substr(0, 16));
            break;
          case TargetKind::kSectionProcedure:
            targets = resolve_section_proc(target);
            break;
          case TargetKind::kInvalid:
            break;
        }
        for (const auto& slug_id : targets) {
          parsed.template_relationships.push_back(
              TemplateRelationship{section.subject_slug_id, predicate, slug_id});
        }
        continue;
      }

      if (source.instance_id.empty()) {
        core::logging::LogWarn("Skipping address relationship without instance_id: " + line);
        continue;
      }

      std::vector<std::string> targets;
      switch (target.kind) {
        case TargetKind::kAddress:
          targets.push_back(target.address_id);
          break;
        case TargetKind::kSlug:
          targets.push_back(core::ComposeAddressId(target.slug_id, source.instance_id));
          break;
        case TargetKind::kSectionProcedure: {
          const auto slug_ids = resolve_section_proc(target);
          for (const auto& slug_id : slug_ids) {
            targets.push_back(core::ComposeAddressId(slug_id, source.instance_id));
          }
          break;
        }
        case TargetKind::kInvalid:
          break;
      }
      for (const auto& address_id : targets) {
        parsed.address_relationships.push_back(
            AddressRelationship{source.address_id, predicate, address_id});
      }
    }
  }

  return parsed;
}

std::string ExportChecklistMarkdown(const std::string& checklist_name,
                                    const std::vector<ChecklistSlug>& slugs,
                                    const std::vector<TemplateRelationship>& relationships,
                                    RelationshipExportMode relationship_mode,
                                    RelationshipIdentityFormat identity_format) {
  if (slugs.empty()) {
    throw std::runtime_error("Checklist contains no slugs to export.");
  }
  static_cast<void>(identity_format);

  const bool use_address_relationships = relationship_mode == RelationshipExportMode::kAddress;
  auto ordered = slugs;
  std::sort(ordered.begin(), ordered.end(), [](const ChecklistSlug& a, const ChecklistSlug& b) {
    const bool a_has_order = a.address_order > 0;
    const bool b_has_order = b.address_order > 0;
    if (a_has_order != b_has_order) {
      return a_has_order;
    }
    if (a_has_order && b_has_order && a.address_order != b.address_order) {
      return a.address_order < b.address_order;
    }
    if (a.section != b.section) return a.section < b.section;
    if (a.procedure != b.procedure) return a.procedure < b.procedure;
    return a.action < b.action;
  });

  std::unordered_map<std::string, std::vector<RelationshipEdge>> rels_by_slug;
  if (!use_address_relationships) {
    for (const auto& rel : relationships) {
      rels_by_slug[rel.subject_slug_id].push_back({rel.predicate, rel.target_slug_id});
    }
    for (auto& entry : rels_by_slug) {
      auto& edges = entry.second;
      std::sort(edges.begin(), edges.end(), [](const RelationshipEdge& a, const RelationshipEdge& b) {
        if (a.predicate != b.predicate) return a.predicate < b.predicate;
        return a.target < b.target;
      });
      edges.erase(std::unique(edges.begin(), edges.end(),
                              [](const RelationshipEdge& a, const RelationshipEdge& b) {
                                return a.predicate == b.predicate && a.target == b.target;
                              }),
                  edges.end());
    }
  }

  std::ostringstream out;
  std::string current_section;

  out << "# Checklist: " << checklist_name << "\n\n";
  for (std::size_t idx = 0; idx < ordered.size(); ++idx) {
    const auto& slug = ordered[idx];
    if (slug.section != current_section) {
      current_section = slug.section;
      out << "## Section: " << current_section << "\n\n";
    }

    out << "### Procedure: " << slug.procedure << "\n";
    out << "- Action: " << slug.action << "\n";
    out << "- Spec: " << slug.spec << "\n";
    out << "- Result: " << slug.result << "\n";
    const std::string status_text =
        slug.status == core::ChecklistStatus::kUnknown ? std::string{} : core::StatusToString(slug.status);
    out << "- Status: " << status_text << "\n";
    out << "- Comment: " << slug.comment << "\n";
    out << "\n";
    out << "#### Instructions\n";
    out << slug.instructions << "\n\n";

    out << "#### Relationships\n";
    std::vector<RelationshipEdge> edges;
    if (use_address_relationships) {
      edges = slug.relationships;
    } else {
      const auto it = rels_by_slug.find(slug.slug_id);
      if (it != rels_by_slug.end()) {
        edges = it->second;
      }
    }
    if (use_address_relationships && !slug.address_id.empty()) {
      out << "- " << slug.address_id << "\n";
    } else {
      out << "- " << slug.slug_id << "\n";
    }
    if (!edges.empty()) {
      std::sort(edges.begin(), edges.end(), [](const RelationshipEdge& a, const RelationshipEdge& b) {
        if (a.predicate != b.predicate) return a.predicate < b.predicate;
        return a.target < b.target;
      });
      edges.erase(std::unique(edges.begin(), edges.end(),
                              [](const RelationshipEdge& a, const RelationshipEdge& b) {
                                return a.predicate == b.predicate && a.target == b.target;
                              }),
                  edges.end());
      for (const auto& edge : edges) {
        out << "- " << edge.predicate << " " << edge.target << "\n";
      }
    }
    out << "\n";

    out << "\n";
    if (idx + 1 < ordered.size()) {
      out << "---\n\n";
    }
  }

  return out.str();
}

}  // namespace core::markdown
