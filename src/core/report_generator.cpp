#include "core/report_generator.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <unordered_set>

#include <nlohmann/json.hpp>

#include "core/checklist_store.hpp"
#include "core/logging.hpp"

namespace core {
namespace {

using nlohmann::json;
using nlohmann::ordered_json;

namespace fs = std::filesystem;

fs::path ResolveDefaultRoot(const fs::path& relative) {
  std::error_code ec;
  fs::path cwd = fs::current_path(ec);
  fs::path best = cwd / relative;

  for (int depth = 0; depth <= 2; ++depth) {
    fs::path candidate = cwd;
    for (int i = 0; i < depth; ++i) {
      candidate = candidate.parent_path();
    }
    if (candidate.empty()) break;
    fs::path joined = candidate / relative;
    if (fs::exists(joined)) {
      return joined;
    }
  }
  return best;
}

fs::path FindTemplatePath(const fs::path& templates_root, const std::string& checklist,
                          const std::string& safe_checklist, const std::string& extension) {
  std::vector<fs::path> candidates;
  candidates.reserve(12);
  const bool tex = extension == ".tex";
  const bool html = extension == ".html";
  const std::string report_name = "report" + extension;

  if (tex || html) {
    const fs::path format_root = templates_root / (tex ? "tex" : "html");
    candidates.push_back(format_root / report_name);
    candidates.push_back(format_root / (checklist + extension));
    candidates.push_back(format_root / (safe_checklist + extension));
  }

  candidates.push_back(templates_root / report_name);
  candidates.push_back(templates_root / (checklist + extension));
  candidates.push_back(templates_root / (safe_checklist + extension));
  candidates.push_back(templates_root / checklist / report_name);
  candidates.push_back(templates_root / checklist / (checklist + extension));
  candidates.push_back(templates_root / safe_checklist / report_name);
  candidates.push_back(templates_root / safe_checklist / (safe_checklist + extension));

  for (const auto& candidate : candidates) {
    if (!candidate.empty() && fs::exists(candidate)) {
      return candidate;
    }
  }
  return {};
}

struct ReportOutputPaths {
  fs::path reports_root;
  fs::path templates_root;
  fs::path output_dir;
  fs::path output_path;
  std::string safe_checklist;
  std::string safe_instance;
  std::string safe_timestamp;
};

struct ResolvedTemplate {
  fs::path path;
  std::string body;
  std::unordered_set<std::string> placeholders;
  bool used = false;
};

struct PreparedReportContent {
  TemplateContext context;
  std::vector<ReportRow> rows;
  std::size_t row_count = 0;
  std::vector<std::pair<std::string, std::string>> alias_hits;
};

struct CapturedReportImage {
  fs::path preview_path;
  fs::path original_path;
  std::string preview_relative;
  std::string original_relative;
  std::string caption;
  std::string procedure;
  std::string procedure_slug_id;
  std::string captured_at;
  std::string source;
};

struct CapturedReportImages {
  std::vector<CapturedReportImage> images;
  fs::path copied_manifest_path;
};

std::string ReadFile(const fs::path& path);

std::string SanitizeToken(const std::string& value) {
  std::string output;
  output.reserve(value.size());
  for (const unsigned char ch : value) {
    if (std::isalnum(ch) || ch == '-' || ch == '_') {
      output.push_back(static_cast<char>(ch));
    } else {
      output.push_back('_');
    }
  }
  while (!output.empty() && output.front() == '_') {
    output.erase(output.begin());
  }
  if (output.empty()) {
    return "report";
  }
  return output;
}

std::string SanitizeTimestampForPath(const std::string& timestamp) {
  std::string sanitized = timestamp;
  for (auto& ch : sanitized) {
    if (ch == ':' || ch == ' ' || ch == '.' || ch == '/') {
      ch = '-';
    }
  }
  return SanitizeToken(sanitized);
}

ReportOutputPaths PrepareReportOutputPaths(const TexReportConfig& config,
                                           const std::string& checklist,
                                           const std::string& instance_id,
                                           const std::string& generated_at,
                                           const std::string& extension) {
  ReportOutputPaths paths;
  paths.safe_checklist = SanitizeToken(checklist);
  paths.safe_instance = SanitizeToken(instance_id);
  paths.safe_timestamp = SanitizeTimestampForPath(generated_at);
  paths.reports_root = config.reports_root.empty()
                           ? ResolveDefaultRoot(fs::path{"checklists"}) / "reports"
                           : config.reports_root;
  paths.templates_root = config.templates_root.empty()
                             ? ResolveDefaultRoot(fs::path{"checklists"}) / "templates"
                             : config.templates_root;

  fs::create_directories(paths.reports_root);
  fs::create_directories(paths.templates_root);

  const std::string basename = paths.safe_instance + "_" + paths.safe_timestamp;
  paths.output_dir = paths.reports_root / basename;
  fs::create_directories(paths.output_dir);
  paths.output_path = paths.output_dir / (paths.safe_checklist + extension);
  return paths;
}

std::string Trim(const std::string& value) {
  std::size_t start = 0;
  while (start < value.size() &&
         std::isspace(static_cast<unsigned char>(value[start])) != 0) {
    ++start;
  }
  std::size_t end = value.size();
  while (end > start &&
         std::isspace(static_cast<unsigned char>(value[end - 1])) != 0) {
    --end;
  }
  return value.substr(start, end - start);
}

bool IsStatusFiltered(const std::string& status) {
  const std::string trimmed = Trim(status);
  if (trimmed.empty()) return true;

  std::string lowered;
  lowered.reserve(trimmed.size());
  for (const unsigned char ch : trimmed) {
    lowered.push_back(static_cast<char>(std::tolower(ch)));
  }
  return lowered == "na" || lowered == "n/a";
}

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

bool AppendLatin1Letter(char32_t codepoint, std::string& out) {
  switch (codepoint) {
    case 0x00C0:
      out += "\\`{A}";
      return true;
    case 0x00C1:
      out += "\\'{A}";
      return true;
    case 0x00C2:
      out += "\\^{A}";
      return true;
    case 0x00C3:
      out += "\\~{A}";
      return true;
    case 0x00C4:
      out += "\\\"{A}";
      return true;
    case 0x00C5:
      out += "\\r{A}";
      return true;
    case 0x00C6:
      out += "\\AE{}";
      return true;
    case 0x00C7:
      out += "\\c{C}";
      return true;
    case 0x00C8:
      out += "\\`{E}";
      return true;
    case 0x00C9:
      out += "\\'{E}";
      return true;
    case 0x00CA:
      out += "\\^{E}";
      return true;
    case 0x00CB:
      out += "\\\"{E}";
      return true;
    case 0x00CC:
      out += "\\`{I}";
      return true;
    case 0x00CD:
      out += "\\'{I}";
      return true;
    case 0x00CE:
      out += "\\^{I}";
      return true;
    case 0x00CF:
      out += "\\\"{I}";
      return true;
    case 0x00D0:
      out += "\\DH{}";
      return true;
    case 0x00D1:
      out += "\\~{N}";
      return true;
    case 0x00D2:
      out += "\\`{O}";
      return true;
    case 0x00D3:
      out += "\\'{O}";
      return true;
    case 0x00D4:
      out += "\\^{O}";
      return true;
    case 0x00D5:
      out += "\\~{O}";
      return true;
    case 0x00D6:
      out += "\\\"{O}";
      return true;
    case 0x00D8:
      out += "\\O{}";
      return true;
    case 0x00D9:
      out += "\\`{U}";
      return true;
    case 0x00DA:
      out += "\\'{U}";
      return true;
    case 0x00DB:
      out += "\\^{U}";
      return true;
    case 0x00DC:
      out += "\\\"{U}";
      return true;
    case 0x00DD:
      out += "\\'{Y}";
      return true;
    case 0x00DE:
      out += "\\TH{}";
      return true;
    case 0x00DF:
      out += "\\ss{}";
      return true;
    case 0x00E0:
      out += "\\`{a}";
      return true;
    case 0x00E1:
      out += "\\'{a}";
      return true;
    case 0x00E2:
      out += "\\^{a}";
      return true;
    case 0x00E3:
      out += "\\~{a}";
      return true;
    case 0x00E4:
      out += "\\\"{a}";
      return true;
    case 0x00E5:
      out += "\\r{a}";
      return true;
    case 0x00E6:
      out += "\\ae{}";
      return true;
    case 0x00E7:
      out += "\\c{c}";
      return true;
    case 0x00E8:
      out += "\\`{e}";
      return true;
    case 0x00E9:
      out += "\\'{e}";
      return true;
    case 0x00EA:
      out += "\\^{e}";
      return true;
    case 0x00EB:
      out += "\\\"{e}";
      return true;
    case 0x00EC:
      out += "\\`{i}";
      return true;
    case 0x00ED:
      out += "\\'{i}";
      return true;
    case 0x00EE:
      out += "\\^{i}";
      return true;
    case 0x00EF:
      out += "\\\"{i}";
      return true;
    case 0x00F0:
      out += "\\dh{}";
      return true;
    case 0x00F1:
      out += "\\~{n}";
      return true;
    case 0x00F2:
      out += "\\`{o}";
      return true;
    case 0x00F3:
      out += "\\'{o}";
      return true;
    case 0x00F4:
      out += "\\^{o}";
      return true;
    case 0x00F5:
      out += "\\~{o}";
      return true;
    case 0x00F6:
      out += "\\\"{o}";
      return true;
    case 0x00F8:
      out += "\\o{}";
      return true;
    case 0x00F9:
      out += "\\`{u}";
      return true;
    case 0x00FA:
      out += "\\'{u}";
      return true;
    case 0x00FB:
      out += "\\^{u}";
      return true;
    case 0x00FC:
      out += "\\\"{u}";
      return true;
    case 0x00FD:
      out += "\\'{y}";
      return true;
    case 0x00FE:
      out += "\\th{}";
      return true;
    case 0x00FF:
      out += "\\\"{y}";
      return true;
    default:
      return false;
  }
}

bool AppendUnicodeLatex(char32_t codepoint, std::string& out) {
  switch (codepoint) {
    case 0x00A0:
      out.push_back(' ');
      return true;
    case 0x00A1:
      out.push_back('!');
      return true;
    case 0x00B0:
      out += "$^\\circ$";
      return true;
    case 0x00B1:
      out += "$\\pm$";
      return true;
    case 0x00B2:
      out += "$^2$";
      return true;
    case 0x00B3:
      out += "$^3$";
      return true;
    case 0x00B5:
      out += "$\\mu$";
      return true;
    case 0x00B7:
      out += "$\\cdot$";
      return true;
    case 0x00BC:
      out += "$1/4$";
      return true;
    case 0x00BD:
      out += "$1/2$";
      return true;
    case 0x00BE:
      out += "$3/4$";
      return true;
    case 0x00BF:
      out.push_back('?');
      return true;
    case 0x00D7:
      out += "$\\times$";
      return true;
    case 0x00F7:
      out += "$\\div$";
      return true;
    case 0x03BC:
      out += "$\\mu$";
      return true;
    case 0x2013:
      out += "--";
      return true;
    case 0x2014:
      out += "---";
      return true;
    case 0x2018:
      out += "`";
      return true;
    case 0x2019:
      out += "'";
      return true;
    case 0x201C:
      out += "``";
      return true;
    case 0x201D:
      out += "''";
      return true;
    case 0x2022:
      out += "$\\bullet$";
      return true;
    case 0x2026:
      out += "\\ldots{}";
      return true;
    case 0x2122:
      out += "TM";
      return true;
    case 0x2264:
      out += "$\\leq$";
      return true;
    case 0x2265:
      out += "$\\geq$";
      return true;
    default:
      return AppendLatin1Letter(codepoint, out);
  }
}

std::string EscapeLatex(const std::string& value) {
  std::string out;
  out.reserve(value.size() * 2);
  std::size_t index = 0;
  while (index < value.size()) {
    char32_t codepoint = 0;
    if (!DecodeUtf8(value, &index, &codepoint)) {
      out.push_back('?');
      continue;
    }
    if (codepoint < 0x80) {
      const char ch = static_cast<char>(codepoint);
      switch (ch) {
        case '\\':
          out += "\\textbackslash{}";
          break;
        case '{':
          out += "\\{";
          break;
        case '}':
          out += "\\}";
          break;
        case '$':
          out += "\\$";
          break;
        case '&':
          out += "\\&";
          break;
        case '#':
          out += "\\#";
          break;
        case '%':
          out += "\\%";
          break;
        case '_':
          out += "\\_";
          break;
        case '^':
          out += "\\^{}";
          break;
        case '~':
          out += "\\~{}";
          break;
        case '\n':
          out += " \\\\ ";
          break;
        case '\r':
          break;
        default:
          out.push_back(ch);
          break;
      }
      continue;
    }
    if (!AppendUnicodeLatex(codepoint, out)) {
      out.push_back('?');
    }
  }
  return out;
}

std::string SlugifyLabel(const std::string& value) {
  std::string slug;
  slug.reserve(value.size());
  bool last_dash = false;
  for (const unsigned char ch : value) {
    if (std::isalnum(ch)) {
      slug.push_back(static_cast<char>(std::tolower(ch)));
      last_dash = false;
    } else if (std::isspace(ch) || ch == '-' || ch == '_') {
      if (!slug.empty() && !last_dash) {
        slug.push_back('-');
        last_dash = true;
      }
    } else if (!slug.empty() && !last_dash) {
      slug.push_back('-');
      last_dash = true;
    }
  }
  while (!slug.empty() && slug.back() == '-') {
    slug.pop_back();
  }
  if (slug.empty()) return "section";
  return slug;
}

std::string RenderTemplate(const std::string& templ, const TemplateContext& context) {
  static const std::regex kPlaceholder(R"(\{\{\s*([A-Za-z0-9_-]+)\s*\}\})");

  std::string output;
  output.reserve(templ.size());
  std::sregex_iterator it(templ.begin(), templ.end(), kPlaceholder);
  std::sregex_iterator end;
  std::size_t last = 0;
  for (; it != end; ++it) {
    const std::size_t pos = static_cast<std::size_t>(it->position());
    const std::size_t len = static_cast<std::size_t>(it->length());
    output.append(templ, last, pos - last);
    const std::string key = (*it)[1];
    auto found = context.find(key);
    if (found != context.end()) {
      output.append(found->second);
    }
    last = pos + len;
  }
  output.append(templ, last, templ.size() - last);
  return output;
}

std::unordered_set<std::string> ExtractPlaceholders(const std::string& templ) {
  static const std::regex kPlaceholder(R"(\{\{\s*([A-Za-z0-9_-]+)\s*\}\})");
  std::unordered_set<std::string> placeholders;
  std::sregex_iterator it(templ.begin(), templ.end(), kPlaceholder);
  std::sregex_iterator end;
  for (; it != end; ++it) {
    placeholders.insert((*it)[1]);
  }
  return placeholders;
}

std::string LookupContext(const TemplateContext& context, const std::string& key,
                          const std::string& fallback = "") {
  auto it = context.find(key);
  if (it == context.end() || it->second.empty()) {
    return fallback;
  }
  return it->second;
}

ResolvedTemplate ResolveReportTemplate(const fs::path& templates_root,
                                       const std::string& checklist,
                                       const std::string& safe_checklist,
                                       const std::string& extension,
                                       const std::string& default_body) {
  ResolvedTemplate resolved;
  resolved.path = FindTemplatePath(templates_root, checklist, safe_checklist, extension);
  resolved.used = !resolved.path.empty();
  resolved.body = resolved.used ? ReadFile(resolved.path) : default_body;
  resolved.placeholders = ExtractPlaceholders(resolved.body);
  return resolved;
}

void AddDashedAliases(TemplateContext& context) {
  std::vector<std::pair<std::string, std::string>> additions;
  additions.reserve(context.size());
  for (const auto& entry : context) {
    const std::string& key = entry.first;
    if (key.find('_') == std::string::npos) {
      continue;
    }
    std::string dashed = key;
    for (auto& ch : dashed) {
      if (ch == '_') {
        ch = '-';
      }
    }
    if (context.find(dashed) == context.end()) {
      additions.emplace_back(std::move(dashed), entry.second);
    }
  }
  for (auto& addition : additions) {
    context.emplace(std::move(addition.first), std::move(addition.second));
  }
}

std::string StatusTextFor(const ChecklistSlug& slug) {
  return slug.status == ChecklistStatus::kUnknown ? std::string{} : StatusToString(slug.status);
}

PreparedReportContent PrepareReportContent(
    const std::unordered_set<std::string>& placeholders, const std::string& checklist,
    const std::string& instance_id, const std::string& instance_principal,
    const std::vector<ChecklistSlug>& slugs,
    const std::unordered_map<std::string, std::string>& slug_aliases,
    const std::string& generated_at, bool template_used, const fs::path& template_path,
    const fs::path& output_dir, const fs::path& output_path) {
  PreparedReportContent prepared;
  prepared.rows.reserve(slugs.size());

  std::unordered_map<std::string, const ChecklistSlug*> slug_index;
  slug_index.reserve(slugs.size());
  for (const auto& slug : slugs) {
    slug_index.emplace(slug.slug_id, &slug);
  }

  std::unordered_map<std::string, std::vector<std::string>> alias_index;
  if (!slug_aliases.empty()) {
    alias_index.reserve(slug_aliases.size());
    for (const auto& entry : slug_aliases) {
      if (entry.first.empty() || entry.second.empty() || entry.first == entry.second) {
        continue;
      }
      if (slug_index.find(entry.second) == slug_index.end()) {
        continue;
      }
      alias_index[entry.second].push_back(entry.first);
    }
  }

  TemplateContext base_context{
      {"checklist", checklist},
      {"title", checklist},
      {"checklist_title", checklist},
      {"instance_id", instance_id},
      {"instance_principal", instance_principal.empty() ? "unknown" : instance_principal},
      {"generated_at", generated_at},
      {"timestamp", generated_at},
      {"template_name", template_used ? template_path.filename().string() : "default"},
      {"template_used", template_used ? "true" : "false"},
      {"template_path", template_used ? template_path.string() : ""},
      {"output_dir", output_dir.string()},
      {"output_path", output_path.string()},
      {"report_filename", output_path.filename().string()},
      {"jsonl_filename", output_path.stem().string() + ".jsonl"},
  };

  const auto add_slug_field = [&](const std::string& address, const std::string& field,
                                  const std::string& value) {
    const std::string underscored = "slug_" + address + "_" + field;
    base_context[underscored] = value;
    base_context[underscored + "_keep"] = value;

    const std::string dashed = "slug-" + address + "-" + field;
    base_context[dashed] = value;
    base_context[dashed + "-keep"] = value;
  };

  const auto add_slug_fields = [&](const std::string& id, const ChecklistSlug& slug,
                                   const std::string& status_text, bool filtered) {
    if (filtered) {
      add_slug_field(id, "procedure", "");
      add_slug_field(id, "action", "");
      add_slug_field(id, "spec", "");
      add_slug_field(id, "result", "");
      add_slug_field(id, "status", "");
      add_slug_field(id, "comment", "");
      add_slug_field(id, "instructions", "");
      add_slug_field(id, "instance_principal", "");
      return;
    }
    add_slug_field(id, "procedure", slug.procedure);
    add_slug_field(id, "action", slug.action);
    add_slug_field(id, "spec", slug.spec);
    add_slug_field(id, "result", slug.result);
    add_slug_field(id, "status", status_text);
    add_slug_field(id, "comment", slug.comment);
    add_slug_field(id, "instructions", slug.instructions);
    add_slug_field(id, "instance_principal", slug.instance_principal);
  };

  std::size_t rendered_rows = 0;
  for (const auto& slug : slugs) {
    const std::string status_text = StatusTextFor(slug);
    ReportRow row{slug.section,
                  slug.procedure,
                  slug.spec,
                  slug.result,
                  status_text,
                  slug.comment,
                  slug.address_id,
                  true};

    bool used_in_template = false;
    bool keep_in_auto = false;
    std::vector<std::string> ids_to_check;
    ids_to_check.reserve(2);
    ids_to_check.push_back(slug.address_id);
    ids_to_check.push_back(slug.slug_id);

    const auto alias_it = alias_index.find(slug.slug_id);
    if (alias_it != alias_index.end()) {
      ids_to_check.reserve(ids_to_check.size() + alias_it->second.size() * 2);
      for (const auto& alias_slug_id : alias_it->second) {
        if (alias_slug_id.empty()) {
          continue;
        }
        ids_to_check.push_back(alias_slug_id);
        ids_to_check.push_back(ComposeAddressId(alias_slug_id, instance_id));
      }
    }

    const auto mark_usage = [&](const std::string& field) {
      for (const auto& id : ids_to_check) {
        if (id.empty()) {
          continue;
        }
        const std::string base = "slug_" + id + "_" + field;
        const std::string base_keep = base + "_keep";
        const std::string dashed = "slug-" + id + "-" + field;
        const std::string dashed_keep = dashed + "-keep";
        if (placeholders.find(base) != placeholders.end() ||
            placeholders.find(dashed) != placeholders.end()) {
          used_in_template = true;
        }
        if (placeholders.find(base_keep) != placeholders.end() ||
            placeholders.find(dashed_keep) != placeholders.end()) {
          used_in_template = true;
          keep_in_auto = true;
        }
      }
    };

    const bool filtered = IsStatusFiltered(row.status);
    add_slug_fields(slug.address_id, slug, row.status, filtered);
    add_slug_fields(slug.slug_id, slug, row.status, filtered);

    mark_usage("procedure");
    mark_usage("action");
    mark_usage("spec");
    mark_usage("result");
    mark_usage("status");
    mark_usage("comment");
    mark_usage("instructions");
    mark_usage("instance_principal");

    if (used_in_template && !keep_in_auto) {
      row.include_in_auto = false;
    }

    if (!IsStatusFiltered(row.status) && row.include_in_auto) {
      ++rendered_rows;
    }
    prepared.rows.push_back(std::move(row));
  }

  if (!slug_aliases.empty()) {
    for (const auto& entry : slug_aliases) {
      const std::string& alias_slug_id = entry.first;
      const std::string& target_slug_id = entry.second;
      if (alias_slug_id.empty() || target_slug_id.empty() || alias_slug_id == target_slug_id) {
        continue;
      }
      const auto it = slug_index.find(target_slug_id);
      if (it == slug_index.end()) {
        continue;
      }
      const ChecklistSlug& slug = *it->second;
      const std::string status_text = StatusTextFor(slug);
      const bool filtered = IsStatusFiltered(status_text);
      add_slug_fields(alias_slug_id, slug, status_text, filtered);
      add_slug_fields(ComposeAddressId(alias_slug_id, instance_id), slug, status_text, filtered);
    }
  }

  if (!slug_aliases.empty()) {
    static const std::array<std::string_view, 8> kFields{
        "procedure", "action", "spec", "result",
        "status", "comment", "instructions", "instance_principal"};
    const auto has_placeholder_for_id = [&](const std::string& id) {
      if (id.empty()) {
        return false;
      }
      for (const auto field : kFields) {
        const std::string field_str(field);
        const std::string base = "slug_" + id + "_" + field_str;
        const std::string base_keep = base + "_keep";
        const std::string dashed = "slug-" + id + "-" + field_str;
        const std::string dashed_keep = dashed + "-keep";
        if (placeholders.find(base) != placeholders.end() ||
            placeholders.find(base_keep) != placeholders.end() ||
            placeholders.find(dashed) != placeholders.end() ||
            placeholders.find(dashed_keep) != placeholders.end()) {
          return true;
        }
      }
      return false;
    };

    for (const auto& entry : slug_aliases) {
      const std::string& alias_slug_id = entry.first;
      const std::string& target_slug_id = entry.second;
      if (alias_slug_id.empty() || target_slug_id.empty() || alias_slug_id == target_slug_id) {
        continue;
      }
      const std::string alias_address_id = ComposeAddressId(alias_slug_id, instance_id);
      if (has_placeholder_for_id(alias_slug_id) || has_placeholder_for_id(alias_address_id)) {
        prepared.alias_hits.emplace_back(alias_slug_id, target_slug_id);
      }
    }
  }

  prepared.row_count = rendered_rows;
  base_context["row_count"] = std::to_string(prepared.row_count);
  AddDashedAliases(base_context);
  prepared.context = std::move(base_context);
  return prepared;
}

std::string BuildReportHeader(const TemplateContext& context) {
  std::ostringstream header;
  header << "% CHAX-REPORT\n";
  header << "% checklist: " << LookupContext(context, "checklist", "unknown") << "\n";
  header << "% instance_id: " << LookupContext(context, "instance_id", "unknown") << "\n";
  header << "% instance_principal: " << LookupContext(context, "instance_principal", "unknown")
         << "\n";
  const std::string timestamp_fallback =
      LookupContext(context, "generated_at", "unknown");
  header << "% timestamp: " << LookupContext(context, "timestamp", timestamp_fallback) << "\n";
  header << "% generated_at: "
         << LookupContext(context, "generated_at", LookupContext(context, "timestamp", "unknown"))
         << "\n";
  header << "% row_count: " << LookupContext(context, "row_count", "0") << "\n";
  header << "% template_used: " << LookupContext(context, "template_used", "false") << "\n";
  header << "% template_name: " << LookupContext(context, "template_name", "default") << "\n";
  const std::string template_path = LookupContext(context, "template_path");
  if (!template_path.empty()) {
    header << "% template_path: " << template_path << "\n";
  }
  const std::string output_path = LookupContext(context, "output_path");
  if (!output_path.empty()) {
    header << "% output_path: " << output_path << "\n";
  }
  header << "% ---\n\n";
  return header.str();
}

std::string BuildHtmlReportHeader(const TemplateContext& context) {
  std::ostringstream header;
  header << "<!-- CHAX-REPORT\n";
  header << "checklist: " << LookupContext(context, "checklist", "unknown") << "\n";
  header << "instance_id: " << LookupContext(context, "instance_id", "unknown") << "\n";
  header << "instance_principal: "
         << LookupContext(context, "instance_principal", "unknown") << "\n";
  const std::string timestamp_fallback = LookupContext(context, "generated_at", "unknown");
  header << "timestamp: " << LookupContext(context, "timestamp", timestamp_fallback) << "\n";
  header << "generated_at: "
         << LookupContext(context, "generated_at", LookupContext(context, "timestamp", "unknown"))
         << "\n";
  header << "row_count: " << LookupContext(context, "row_count", "0") << "\n";
  header << "template_used: " << LookupContext(context, "template_used", "false") << "\n";
  header << "template_name: " << LookupContext(context, "template_name", "default") << "\n";
  const std::string template_path = LookupContext(context, "template_path");
  if (!template_path.empty()) {
    header << "template_path: " << template_path << "\n";
  }
  const std::string output_path = LookupContext(context, "output_path");
  if (!output_path.empty()) {
    header << "output_path: " << output_path << "\n";
  }
  header << "--- -->\n";
  return header.str();
}

std::string RenderAutoTables(const std::vector<ReportRow>& rows) {
  constexpr int kMaxTableVisualRows = 25;

  struct SectionGroup {
    std::string name;
    std::vector<const ReportRow*> items;
  };

  std::vector<SectionGroup> sections;
  std::unordered_map<std::string, std::size_t> index_by_section;

  for (const auto& row : rows) {
    if (!row.include_in_auto) continue;
    auto it = index_by_section.find(row.section);
    if (it == index_by_section.end()) {
      index_by_section[row.section] = sections.size();
      sections.push_back(SectionGroup{row.section, {}});
      it = index_by_section.find(row.section);
    }
    if (IsStatusFiltered(row.status)) continue;
    sections[it->second].items.push_back(&row);
  }

  std::ostringstream out;
  bool first = true;
  for (const auto& section : sections) {
    if (section.items.empty()) continue;
    const std::string label_base = SlugifyLabel(section.name);
    std::size_t chunk_start = 0;
    int part = 1;
    while (chunk_start < section.items.size()) {
      std::size_t chunk_end = chunk_start;
      int budget_used = 0;
      while (chunk_end < section.items.size()) {
        const auto* row_ptr = section.items[chunk_end];
        const int row_cost = Trim(row_ptr->comment).empty() ? 1 : 2;
        if (chunk_end > chunk_start && budget_used + row_cost > kMaxTableVisualRows) {
          break;
        }
        budget_used += row_cost;
        ++chunk_end;
      }

      if (!first) out << "\n";
      first = false;

      out << "\\refstepcounter{tablecount}\n";
      out << "\\begin{table}[H]\n";
      out << "\\caption{" << EscapeLatex(section.name);
      if (part > 1) {
        out << " (Part " << part << ")";
      }
      out << "}\n";
      out << "\\label{tab:" << label_base;
      if (part > 1) {
        out << "-" << part;
      }
      out << "}\n";
      out << "\\begin{tabular}{\n"
          << "    @{}>{\\raggedright\\arraybackslash}p{0.08\\linewidth}\n"
          << "    >{\\raggedright\\arraybackslash}p{0.26\\linewidth}\n"
          << "    >{\\raggedright\\arraybackslash}p{0.24\\linewidth}\n"
          << "    >{\\raggedright\\arraybackslash}p{0.24\\linewidth}\n"
          << "    >{\\raggedright\\arraybackslash}p{0.12\\linewidth}@{}}\n";
      out << "    \\textbf{No.} & \\textbf{Procedure}  & \\textbf{Spec} & \\textbf{Result} & "
             "\\textbf{Status} \\\\\n";
      out << "    \\hline\n";

      for (std::size_t i = chunk_start; i < chunk_end; ++i) {
        const auto* row_ptr = section.items[i];
        const int number = static_cast<int>(i + 1);
        const std::string trimmed_comment = Trim(row_ptr->comment);
        out << "    " << number << " & " << EscapeLatex(row_ptr->procedure) << " & "
            << EscapeLatex(row_ptr->spec) << " & " << EscapeLatex(row_ptr->result) << " & "
            << EscapeLatex(row_ptr->status) << " \\\\\n";
        if (!trimmed_comment.empty()) {
          out << "    \\arrayrulecolor[gray]{0.8} \\hline\n";
          out << "    \\multicolumn{5}{p{0.95\\linewidth}}{\\textbf{Comment: }"
              << EscapeLatex(trimmed_comment) << "} \\\\\n";
          out << "    \\arrayrulecolor{black}\n";
          out << "    \\hline\n";
        } else {
          out << "    \\hline\n";
        }
      }

      out << "\\end{tabular}\n";
      out << "\\end{table}\n";
      chunk_start = chunk_end;
      ++part;
    }
  }

  return out.str();
}

std::string EscapeHtml(const std::string& value) {
  std::string out;
  out.reserve(value.size() * 2);
  for (const unsigned char ch : value) {
    switch (ch) {
      case '&':
        out += "&amp;";
        break;
      case '<':
        out += "&lt;";
        break;
      case '>':
        out += "&gt;";
        break;
      case '"':
        out += "&quot;";
        break;
      case '\'':
        out += "&#39;";
        break;
      default:
        out.push_back(static_cast<char>(ch));
        break;
    }
  }
  return out;
}

std::string ToLower(const std::string& value) {
  std::string out;
  out.reserve(value.size());
  for (const unsigned char ch : value) {
    out.push_back(static_cast<char>(std::tolower(ch)));
  }
  return out;
}

std::string HtmlStatusClass(const std::string& status) {
  const std::string lowered = ToLower(Trim(status));
  if (lowered == "pass") return "status-pass";
  if (lowered == "fail") return "status-fail";
  if (lowered == "indeterminate") return "status-indeterminate";
  if (lowered == "warning") return "status-warning";
  if (lowered == "other") return "status-other";
  if (lowered == "na" || lowered == "n/a") return "status-na";
  return lowered.empty() ? "status-unknown" : ("status-" + SlugifyLabel(lowered));
}

std::string RenderHtmlAutoTables(const std::vector<ReportRow>& rows) {
  struct SectionGroup {
    std::string name;
    std::vector<const ReportRow*> items;
  };

  std::vector<SectionGroup> sections;
  std::unordered_map<std::string, std::size_t> index_by_section;

  for (const auto& row : rows) {
    if (!row.include_in_auto || IsStatusFiltered(row.status)) {
      continue;
    }
    auto it = index_by_section.find(row.section);
    if (it == index_by_section.end()) {
      index_by_section[row.section] = sections.size();
      sections.push_back(SectionGroup{row.section, {}});
      it = index_by_section.find(row.section);
    }
    sections[it->second].items.push_back(&row);
  }

  std::ostringstream out;
  for (const auto& section : sections) {
    if (section.items.empty()) {
      continue;
    }
    out << "<section class=\"table-section\" id=\"section-" << EscapeHtml(SlugifyLabel(section.name))
        << "\">";
    out << "<div class=\"section-head\">";
    out << "<h2>" << EscapeHtml(section.name) << "</h2>";
    out << "<span class=\"section-count\">" << section.items.size() << " row";
    if (section.items.size() != 1) {
      out << "s";
    }
    out << "</span></div>";
    out << "<div class=\"table-wrap\"><table class=\"report-table\">";
    out << "<thead><tr><th>No.</th><th>Procedure</th><th>Spec</th><th>Result</th><th>Status</th></tr></thead>";
    out << "<tbody>";
    for (std::size_t i = 0; i < section.items.size(); ++i) {
      const auto* row_ptr = section.items[i];
      const std::string status_class = HtmlStatusClass(row_ptr->status);
      out << "<tr>";
      out << "<td class=\"col-no\">" << (i + 1) << "</td>";
      out << "<td>" << EscapeHtml(row_ptr->procedure) << "</td>";
      out << "<td>" << EscapeHtml(row_ptr->spec) << "</td>";
      out << "<td>" << EscapeHtml(row_ptr->result) << "</td>";
      out << "<td><span class=\"status-pill " << status_class << "\">"
          << EscapeHtml(row_ptr->status) << "</span></td>";
      out << "</tr>";

      const std::string trimmed_comment = Trim(row_ptr->comment);
      if (!trimmed_comment.empty()) {
        out << "<tr class=\"comment-row\"><td colspan=\"5\">";
        out << "<span class=\"comment-label\">Comment</span>";
        out << "<div class=\"comment-body\">" << EscapeHtml(trimmed_comment) << "</div>";
        out << "</td></tr>";
      }
    }
    out << "</tbody></table></div></section>";
  }

  if (out.str().empty()) {
    return "<section class=\"empty-state\"><p>No report rows were eligible for AutoTables.</p></section>";
  }
  return out.str();
}

std::string ReadFile(const fs::path& path) {
  std::ifstream file(path);
  if (!file.is_open()) {
    throw std::runtime_error("Failed to open template: " + path.string());
  }
  std::ostringstream buffer;
  buffer << file.rdbuf();
  return buffer.str();
}

bool IsSafeEvidenceRelativePath(const std::string& value) {
  if (value.empty()) {
    return false;
  }
  const fs::path path(value);
  if (path.empty() || path.is_absolute() || path.has_root_name() || path.has_root_directory()) {
    return false;
  }
  for (const auto& component : path) {
    if (component == "." || component == "..") {
      return false;
    }
  }
  return true;
}

std::string ReadRequiredManifestString(const json& object, const char* field,
                                       const fs::path& manifest_path) {
  if (!object.contains(field) || !object.at(field).is_string()) {
    throw std::runtime_error("Report image manifest requires string field '" + std::string(field) +
                             "': " + manifest_path.string());
  }
  const std::string value = object.at(field).get<std::string>();
  if (Trim(value).empty()) {
    throw std::runtime_error("Report image manifest field '" + std::string(field) +
                             "' must not be blank: " + manifest_path.string());
  }
  return value;
}

std::string ReadOptionalManifestString(const json& object, const char* field,
                                       const fs::path& manifest_path) {
  if (!object.contains(field) || object.at(field).is_null()) {
    return {};
  }
  if (!object.at(field).is_string()) {
    throw std::runtime_error("Report image manifest field '" + std::string(field) +
                             "' must be a string: " + manifest_path.string());
  }
  return object.at(field).get<std::string>();
}

void RequirePreviewImageFormat(const fs::path& preview, const fs::path& manifest_path) {
  const std::string extension = ToLower(preview.extension().string());
  if (extension == ".png" || extension == ".jpg" || extension == ".jpeg") {
    return;
  }
  throw std::runtime_error(
      "Report image manifest preview must use PNG or JPEG so HTML and LaTeX can render it: " +
      preview.string() + " (" + manifest_path.string() + ")");
}

void CopyEvidenceFile(const fs::path& source_root, const fs::path& output_root,
                      const std::string& relative, const fs::path& manifest_path,
                      std::unordered_set<std::string>& copied_paths) {
  if (!IsSafeEvidenceRelativePath(relative)) {
    throw std::runtime_error("Report image manifest path must be a safe relative path: " + relative +
                             " (" + manifest_path.string() + ")");
  }
  if (!copied_paths.insert(relative).second) {
    return;
  }

  const fs::path source = source_root / fs::path(relative);
  std::error_code ec;
  if (!fs::is_regular_file(source, ec) || ec) {
    throw std::runtime_error("Report image manifest references a missing file: " + source.string());
  }
  const fs::path destination = output_root / fs::path(relative);
  fs::create_directories(destination.parent_path(), ec);
  if (ec) {
    throw std::runtime_error("Failed to create report image output directory: " +
                             destination.parent_path().string());
  }
  if (!fs::copy_file(source, destination, fs::copy_options::overwrite_existing, ec) || ec) {
    throw std::runtime_error("Failed to copy report image evidence: " + source.string() + " -> " +
                             destination.string());
  }
}

CapturedReportImages PrepareCapturedReportImages(const ReportOutputPaths& paths,
                                                 const std::string& instance_id) {
  CapturedReportImages captured;
  const fs::path source_root = paths.reports_root / SanitizeToken(instance_id) / "images";
  const fs::path manifest_path = source_root / "manifest.json";
  std::error_code ec;
  if (!fs::exists(manifest_path, ec) || ec) {
    return captured;
  }
  if (!fs::is_regular_file(manifest_path, ec) || ec) {
    throw std::runtime_error("Report image manifest is not a regular file: " + manifest_path.string());
  }

  json manifest;
  try {
    std::ifstream input(manifest_path, std::ios::binary);
    if (!input.is_open()) {
      throw std::runtime_error("Failed to open report image manifest");
    }
    input >> manifest;
  } catch (const std::exception& error) {
    throw std::runtime_error("Failed to parse report image manifest " + manifest_path.string() +
                             ": " + error.what());
  }

  if (!manifest.is_object() || !manifest.contains("schema") || !manifest.at("schema").is_string() ||
      manifest.at("schema").get<std::string>() != "chax-report-images-v1") {
    throw std::runtime_error("Report image manifest must declare schema 'chax-report-images-v1': " +
                             manifest_path.string());
  }
  if (!manifest.contains("images") || !manifest.at("images").is_array()) {
    throw std::runtime_error("Report image manifest requires an images array: " +
                             manifest_path.string());
  }

  const fs::path output_root = paths.output_dir / "images";
  fs::create_directories(output_root, ec);
  if (ec) {
    throw std::runtime_error("Failed to create report image output directory: " + output_root.string());
  }

  std::unordered_set<std::string> copied_paths;
  captured.images.reserve(manifest.at("images").size());
  for (const auto& item : manifest.at("images")) {
    if (!item.is_object()) {
      throw std::runtime_error("Each report image manifest entry must be an object: " +
                               manifest_path.string());
    }
    CapturedReportImage image;
    image.preview_relative = ReadRequiredManifestString(item, "preview", manifest_path);
    if (!IsSafeEvidenceRelativePath(image.preview_relative)) {
      throw std::runtime_error("Report image preview must be a safe relative path: " +
                               image.preview_relative + " (" + manifest_path.string() + ")");
    }
    image.preview_path = fs::path(image.preview_relative);
    RequirePreviewImageFormat(image.preview_path, manifest_path);
    image.original_relative = ReadOptionalManifestString(item, "original", manifest_path);
    if (!image.original_relative.empty() && !IsSafeEvidenceRelativePath(image.original_relative)) {
      throw std::runtime_error("Report image original must be a safe relative path: " +
                               image.original_relative + " (" + manifest_path.string() + ")");
    }
    image.original_path = fs::path(image.original_relative);
    image.caption = ReadOptionalManifestString(item, "caption", manifest_path);
    image.procedure = ReadOptionalManifestString(item, "procedure", manifest_path);
    image.procedure_slug_id = ReadOptionalManifestString(item, "procedure_slug_id", manifest_path);
    image.captured_at = ReadOptionalManifestString(item, "captured_at", manifest_path);
    image.source = ReadOptionalManifestString(item, "source", manifest_path);

    CopyEvidenceFile(source_root, output_root, image.preview_relative, manifest_path, copied_paths);
    if (!image.original_relative.empty()) {
      CopyEvidenceFile(source_root, output_root, image.original_relative, manifest_path, copied_paths);
    }
    captured.images.push_back(std::move(image));
  }

  if (manifest.contains("attachments")) {
    if (!manifest.at("attachments").is_array()) {
      throw std::runtime_error("Report image manifest attachments must be an array: " +
                               manifest_path.string());
    }
    for (const auto& item : manifest.at("attachments")) {
      if (!item.is_object()) {
        throw std::runtime_error("Each report attachment manifest entry must be an object: " +
                                 manifest_path.string());
      }
      const std::string attachment_relative =
          ReadRequiredManifestString(item, "path", manifest_path);
      CopyEvidenceFile(source_root, output_root, attachment_relative, manifest_path, copied_paths);
    }
  }

  const fs::path copied_manifest = output_root / "manifest.json";
  if (!fs::copy_file(manifest_path, copied_manifest, fs::copy_options::overwrite_existing, ec) || ec) {
    throw std::runtime_error("Failed to copy report image manifest: " + manifest_path.string());
  }
  captured.copied_manifest_path = copied_manifest;
  return captured;
}

std::string EvidenceOutputRelativePath(const std::string& relative) {
  return (fs::path("images") / fs::path(relative)).generic_string();
}

std::string BuildCapturedImagesHtml(const std::vector<CapturedReportImage>& images) {
  if (images.empty()) {
    return {};
  }

  std::ostringstream out;
  out << "<section class=\"captured-images\">"
      << "<header class=\"captured-images-head\"><h2>Captured Evidence</h2>"
      << "<p>Images captured during this checklist run. Full-resolution source files are retained "
         "beside this report.</p></header>";
  for (std::size_t page_start = 0; page_start < images.size(); page_start += 6) {
    const std::size_t page_end = std::min(page_start + 6, images.size());
    out << "<div class=\"captured-image-page\">";
    for (std::size_t index = page_start; index < page_end; ++index) {
      const auto& image = images[index];
      const std::string preview = EvidenceOutputRelativePath(image.preview_relative);
      const std::string original = image.original_relative.empty()
                                       ? std::string{}
                                       : EvidenceOutputRelativePath(image.original_relative);
      const std::string label = image.procedure.empty() ? "Checklist evidence" : image.procedure;
      out << "<figure class=\"captured-image-card\">";
      if (!original.empty()) {
        out << "<a class=\"captured-image-link\" href=\"" << EscapeHtml(original)
            << "\" title=\"Open the full-resolution source file\">";
      }
      out << "<img src=\"" << EscapeHtml(preview) << "\" alt=\""
          << EscapeHtml(label) << "\" loading=\"lazy\">";
      if (!original.empty()) {
        out << "</a>";
      }
      out << "<figcaption><strong>" << EscapeHtml(label) << "</strong>";
      if (!image.captured_at.empty()) {
        out << "<span>Captured: " << EscapeHtml(image.captured_at) << "</span>";
      }
      if (!image.caption.empty()) {
        out << "<span>" << EscapeHtml(image.caption) << "</span>";
      }
      if (!image.source.empty()) {
        out << "<span>Source: " << EscapeHtml(image.source) << "</span>";
      }
      out << "</figcaption></figure>";
    }
    out << "</div>";
  }
  out << "</section>";
  return out.str();
}

std::string BuildCapturedImagesLatex(const std::vector<CapturedReportImage>& images) {
  if (images.empty()) {
    return {};
  }

  const auto render_card = [](const CapturedReportImage& image) {
    const std::string label = image.procedure.empty() ? "Checklist evidence" : image.procedure;
    const std::string preview = EvidenceOutputRelativePath(image.preview_relative);
    std::ostringstream card;
    card << "\\begin{minipage}[t]{0.48\\linewidth}\\centering\n"
         << "\\includegraphics[width=\\linewidth,height=0.19\\textheight,keepaspectratio]"
         << "{\\detokenize{" << preview << "}}\\\\\n"
         << "\\footnotesize\\textbf{" << EscapeLatex(label) << "}\\\\\n";
    if (!image.captured_at.empty()) {
      card << "\\scriptsize Captured: " << EscapeLatex(image.captured_at) << "\\\\\n";
    }
    if (!image.caption.empty()) {
      card << "\\scriptsize " << EscapeLatex(image.caption) << "\\\\\n";
    }
    if (!image.source.empty()) {
      card << "\\scriptsize Source: " << EscapeLatex(image.source) << "\\\\\n";
    }
    card << "\\end{minipage}";
    return card.str();
  };

  std::ostringstream out;
  for (std::size_t page_start = 0; page_start < images.size(); page_start += 6) {
    const std::size_t page_end = std::min(page_start + 6, images.size());
    out << "\\clearpage\n\\section*{Captured Evidence}\n\\begin{center}\n";
    for (std::size_t row_start = page_start; row_start < page_end; row_start += 2) {
      out << render_card(images[row_start]);
      if (row_start + 1 < page_end) {
        out << "\\hfill\n" << render_card(images[row_start + 1]);
      }
      out << "\\par\\vspace{4mm}\n";
    }
    out << "\\end{center}\n";
  }
  return out.str();
}

void AddCapturedImagesContext(PreparedReportContent& prepared,
                              const CapturedReportImages& captured) {
  prepared.context["captured_images_count"] = std::to_string(captured.images.size());
  prepared.context["images_manifest_filename"] =
      captured.copied_manifest_path.empty() ? "" : "images/manifest.json";
  prepared.context["CapturedImages"] = BuildCapturedImagesHtml(captured.images);
  prepared.context["CapturedImageFigures"] = BuildCapturedImagesLatex(captured.images);
  AddDashedAliases(prepared.context);
}

std::string EscapeFdfString(const std::string& value) {
  std::string out;
  out.reserve(value.size() * 2);
  for (const char ch : value) {
    switch (ch) {
      case '\\':
      case '(':
      case ')':
        out.push_back('\\');
        out.push_back(ch);
        break;
      case '\r':
      case '\n':
        out.append("\\r");
        break;
      default:
        out.push_back(ch);
        break;
    }
  }
  return out;
}

std::vector<std::string> ExtractFdfFieldNames(const std::string& templ) {
  std::vector<std::string> fields;
  std::unordered_set<std::string> seen;
  std::size_t pos = 0;
  while (pos < templ.size()) {
    const std::size_t start = templ.find("/T(", pos);
    if (start == std::string::npos) {
      break;
    }
    std::size_t idx = start + 3;
    std::string name;
    bool escaped = false;
    for (; idx < templ.size(); ++idx) {
      const char ch = templ[idx];
      if (escaped) {
        name.push_back(ch);
        escaped = false;
        continue;
      }
      if (ch == '\\') {
        escaped = true;
        continue;
      }
      if (ch == ')') {
        break;
      }
      name.push_back(ch);
    }
    if (!name.empty() && seen.insert(name).second) {
      fields.push_back(name);
    }
    pos = idx < templ.size() ? idx + 1 : templ.size();
  }
  return fields;
}

std::string BuildFdfPayload(const TemplateContext& context,
                            const std::vector<std::string>& fields,
                            const std::string& pdf_filename) {
  std::vector<std::pair<std::string, std::string>> items;
  std::unordered_set<std::string> seen;
  if (!fields.empty()) {
    items.reserve(fields.size() + context.size());
    for (const auto& key : fields) {
      if (key.empty()) {
        continue;
      }
      const auto it = context.find(key);
      items.emplace_back(key, it == context.end() ? std::string{} : it->second);
      seen.insert(key);
    }
  } else {
    items.reserve(context.size());
  }

  std::vector<std::pair<std::string, std::string>> remainder;
  remainder.reserve(context.size());
  for (const auto& entry : context) {
    if (entry.first.empty() || seen.find(entry.first) != seen.end()) {
      continue;
    }
    remainder.emplace_back(entry.first, entry.second);
  }
  std::sort(remainder.begin(), remainder.end(),
            [](const auto& lhs, const auto& rhs) { return lhs.first < rhs.first; });
  items.insert(items.end(), remainder.begin(), remainder.end());

  std::ostringstream out;
  out << "%FDF-1.2\n";
  out << "1 0 obj\n";
  out << "<<\n";
  out << "/FDF << ";
  if (!pdf_filename.empty()) {
    out << "/F (" << EscapeFdfString(pdf_filename) << ") ";
  }
  out << "/Fields [\n";
  for (const auto& entry : items) {
    out << "<< /T (" << EscapeFdfString(entry.first) << ") /V ("
        << EscapeFdfString(entry.second) << ") >>\n";
  }
  out << "] >>\n";
  out << ">>\n";
  out << "endobj\n";
  out << "trailer\n";
  out << "<< /Root 1 0 R >>\n";
  out << "%%EOF\n";
  return out.str();
}

std::string DefaultTemplate() {
  return R"(\documentclass{article}
\usepackage[margin=0.9in]{geometry}
\usepackage{array}
\usepackage[table]{xcolor}
\usepackage{float}
\usepackage{graphicx}
\usepackage{fancyhdr}
\usepackage{textcomp}
\newcounter{tablecount}
\setcounter{tablecount}{0}

% ---------- Default footer (only used when no template is provided) ----------
\newcommand{\SidecarName}{\jobname.jsonl}
\newcommand{\AppName}{Checklist Assistant\texttrademark{}}
\newcommand{\CompanyName}{CVMewt Inc.\texttrademark{}}
\newcommand{\LogoFile}{../../img/logo.png}

\newcommand{\FooterInternal}{%
  \scriptsize\sffamily
  \begin{tabular}{@{}c@{}}
    \CompanyName\;---\;Generated by \AppName%
    \IfFileExists{\LogoFile}{\;\raisebox{-0.15\height}{\includegraphics[height=3.5mm]{\LogoFile}}}{} \\
    Machine-readable sidecar: \texttt{\SidecarName}
  \end{tabular}%
}

\newcommand{\UseFooter}[1]{%
  \pagestyle{fancy}%
  \fancyhf{}%
  \renewcommand{\headrulewidth}{0pt}%
  \renewcommand{\footrulewidth}{0.2pt}%
  \fancyfoot[C]{#1}%
}
\begin{document}
\UseFooter{\FooterInternal}
\section*{ {{checklist}} }

\subsection*{Report Metadata}
\begin{tabular}{@{}ll@{}}
Checklist & {{checklist}} \\
Instance ID & {{instance-id}} \\
Instance Principal & {{instance-principal}} \\
Timestamp (UTC) & {{timestamp}} \\
Rendered Rows & {{row-count}} \\
Captured Images & {{captured-images-count}} \\
Template & {{template-name}} \\
\end{tabular}

{{AutoTables}}
{{CapturedImageFigures}}
\end{document}
)";
}

std::string DefaultHtmlTemplate() {
  return R"(<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>{{checklist_title}}</title>
  <style>
    :root {
      --page-bg: linear-gradient(180deg, #eef3f8 0%, #dde8f2 100%);
      --surface: #ffffff;
      --surface-alt: #f8fbfd;
      --border: #ced8e2;
      --text: #15202b;
      --muted: #52606d;
      --accent: #0f5b85;
      --accent-soft: #dff0fb;
      --pass-bg: #e7f8ef;
      --pass-text: #0f6b3e;
      --fail-bg: #fdeaea;
      --fail-text: #9f1d1d;
      --indeterminate-bg: #fff4dd;
      --indeterminate-text: #9a5a00;
      --other-bg: #eceef6;
      --other-text: #384152;
      --shadow: 0 20px 45px rgba(19, 35, 52, 0.12);
      --radius: 18px;
      --font-sans: "Segoe UI", "Helvetica Neue", Arial, sans-serif;
      --font-display: "Georgia", "Times New Roman", serif;
    }

    * { box-sizing: border-box; }

    html, body {
      margin: 0;
      padding: 0;
      background: #dfe7ef;
      color: var(--text);
      font-family: var(--font-sans);
    }

    body {
      background-image: var(--page-bg);
      min-height: 100vh;
      padding: 32px 20px 48px;
    }

    .report-shell {
      width: min(1120px, 100%);
      margin: 0 auto;
      display: grid;
      gap: 24px;
    }

    .hero {
      background:
        radial-gradient(circle at top right, rgba(15, 91, 133, 0.12), transparent 32%),
        linear-gradient(135deg, #0f2133 0%, #16364f 56%, #0f5b85 100%);
      color: #f7fbff;
      border-radius: calc(var(--radius) + 8px);
      padding: 32px;
      box-shadow: var(--shadow);
      overflow: hidden;
      position: relative;
    }

    .hero::after {
      content: "";
      position: absolute;
      inset: auto -40px -40px auto;
      width: 220px;
      height: 220px;
      border-radius: 50%;
      background: rgba(255, 255, 255, 0.08);
      filter: blur(4px);
    }

    .hero-grid {
      display: grid;
      grid-template-columns: minmax(0, 1.4fr) minmax(300px, 0.9fr);
      gap: 24px;
      position: relative;
      z-index: 1;
    }

    .eyebrow {
      margin: 0 0 12px;
      font-size: 0.82rem;
      letter-spacing: 0.14em;
      text-transform: uppercase;
      color: rgba(247, 251, 255, 0.72);
    }

    h1 {
      margin: 0 0 14px;
      font-family: var(--font-display);
      font-size: clamp(2rem, 4vw, 3.2rem);
      line-height: 1;
      max-width: 14ch;
    }

    .hero-copy {
      margin: 0;
      max-width: 56ch;
      color: rgba(247, 251, 255, 0.82);
      line-height: 1.55;
    }

    .meta-card {
      background: rgba(255, 255, 255, 0.1);
      border: 1px solid rgba(255, 255, 255, 0.16);
      border-radius: var(--radius);
      padding: 18px;
      backdrop-filter: blur(4px);
    }

    .meta-card h2 {
      margin: 0 0 14px;
      font-size: 0.92rem;
      letter-spacing: 0.1em;
      text-transform: uppercase;
      color: rgba(247, 251, 255, 0.7);
    }

    .meta-grid {
      display: grid;
      grid-template-columns: repeat(2, minmax(0, 1fr));
      gap: 12px;
    }

    .meta-item {
      min-width: 0;
    }

    .meta-label {
      display: block;
      font-size: 0.78rem;
      color: rgba(247, 251, 255, 0.66);
      margin-bottom: 4px;
      text-transform: uppercase;
      letter-spacing: 0.08em;
    }

    .meta-value {
      display: block;
      font-size: 0.98rem;
      line-height: 1.35;
      word-break: break-word;
    }

    .summary-grid {
      display: grid;
      grid-template-columns: repeat(4, minmax(0, 1fr));
      gap: 16px;
    }

    .summary-card {
      background: var(--surface);
      border: 1px solid var(--border);
      border-radius: var(--radius);
      padding: 18px;
      box-shadow: 0 8px 20px rgba(19, 35, 52, 0.07);
    }

    .summary-card .summary-label {
      display: block;
      margin-bottom: 8px;
      font-size: 0.8rem;
      text-transform: uppercase;
      letter-spacing: 0.08em;
      color: var(--muted);
    }

    .summary-card .summary-value {
      display: block;
      font-size: 1.1rem;
      font-weight: 700;
      line-height: 1.25;
      word-break: break-word;
    }

    .tables-panel {
      background: rgba(255, 255, 255, 0.64);
      border: 1px solid rgba(206, 216, 226, 0.75);
      border-radius: calc(var(--radius) + 6px);
      padding: 24px;
      box-shadow: var(--shadow);
      backdrop-filter: blur(6px);
    }

    .panel-head {
      display: flex;
      justify-content: space-between;
      gap: 16px;
      align-items: flex-end;
      margin-bottom: 18px;
    }

    .panel-head h2 {
      margin: 0;
      font-family: var(--font-display);
      font-size: 1.8rem;
      line-height: 1.05;
    }

    .panel-head p {
      margin: 6px 0 0;
      max-width: 58ch;
      color: var(--muted);
    }

    .table-section + .table-section {
      margin-top: 20px;
    }

    .section-head {
      display: flex;
      justify-content: space-between;
      gap: 12px;
      align-items: baseline;
      margin-bottom: 10px;
    }

    .section-head h2 {
      margin: 0;
      font-size: 1.2rem;
      font-family: var(--font-display);
    }

    .section-count {
      color: var(--muted);
      font-size: 0.88rem;
      white-space: nowrap;
    }

    .table-wrap {
      overflow-x: auto;
      border-radius: 14px;
      border: 1px solid var(--border);
      background: var(--surface);
    }

    .report-table {
      width: 100%;
      border-collapse: collapse;
      min-width: 720px;
    }

    .report-table th,
    .report-table td {
      padding: 13px 14px;
      text-align: left;
      vertical-align: top;
      border-bottom: 1px solid var(--border);
    }

    .report-table thead th {
      position: sticky;
      top: 0;
      z-index: 1;
      background: var(--surface-alt);
      font-size: 0.82rem;
      text-transform: uppercase;
      letter-spacing: 0.08em;
      color: var(--muted);
    }

    .report-table tbody tr:last-child td {
      border-bottom: 0;
    }

    .col-no {
      width: 70px;
      color: var(--muted);
      font-variant-numeric: tabular-nums;
    }

    .status-pill {
      display: inline-flex;
      align-items: center;
      gap: 8px;
      border-radius: 999px;
      padding: 6px 11px;
      font-size: 0.88rem;
      font-weight: 700;
      background: #eef2f7;
      color: var(--other-text);
    }

    .status-pass {
      background: var(--pass-bg);
      color: var(--pass-text);
    }

    .status-fail {
      background: var(--fail-bg);
      color: var(--fail-text);
    }

    .status-indeterminate {
      background: var(--indeterminate-bg);
      color: var(--indeterminate-text);
    }

    .status-other,
    .status-warning,
    .status-unknown,
    .status-na {
      background: var(--other-bg);
      color: var(--other-text);
    }

    .comment-row td {
      background: #fbfcfe;
    }

    .comment-label {
      display: inline-block;
      margin-bottom: 6px;
      font-size: 0.76rem;
      text-transform: uppercase;
      letter-spacing: 0.08em;
      color: var(--accent);
      font-weight: 700;
    }

    .comment-body {
      color: var(--text);
      white-space: pre-wrap;
      line-height: 1.5;
    }

    .empty-state {
      background: var(--surface);
      border: 1px dashed var(--border);
      border-radius: 14px;
      padding: 20px;
      color: var(--muted);
    }

    .captured-images {
      background: var(--surface);
      border: 1px solid var(--border);
      border-radius: calc(var(--radius) + 6px);
      padding: 24px;
      box-shadow: var(--shadow);
    }

    .captured-images-head {
      margin-bottom: 18px;
    }

    .captured-images-head h2 {
      margin: 0;
      font-family: var(--font-display);
      font-size: 1.8rem;
    }

    .captured-images-head p {
      margin: 6px 0 0;
      color: var(--muted);
    }

    .captured-image-page {
      display: grid;
      grid-template-columns: repeat(2, minmax(0, 1fr));
      gap: 18px;
    }

    .captured-image-page + .captured-image-page {
      margin-top: 22px;
    }

    .captured-image-card {
      margin: 0;
      border: 1px solid var(--border);
      border-radius: 12px;
      overflow: hidden;
      background: var(--surface-alt);
      break-inside: avoid;
    }

    .captured-image-link {
      display: block;
      background: #101820;
    }

    .captured-image-card img {
      display: block;
      width: 100%;
      height: auto;
      max-height: 420px;
      object-fit: contain;
      margin: 0 auto;
    }

    .captured-image-card figcaption {
      display: grid;
      gap: 3px;
      padding: 10px 12px 12px;
      color: var(--muted);
      font-size: 0.86rem;
      line-height: 1.35;
    }

    .captured-image-card figcaption strong {
      color: var(--text);
      font-size: 0.92rem;
    }

    @media (max-width: 900px) {
      .hero-grid,
      .summary-grid,
      .meta-grid,
      .captured-image-page {
        grid-template-columns: 1fr;
      }

      body {
        padding: 18px 12px 28px;
      }

      .hero,
      .tables-panel {
        padding: 20px;
      }
    }

    @media print {
      @page {
        size: auto;
        margin: 12mm;
      }

      body {
        padding: 0;
        background: #fff;
      }

      .report-shell {
        width: 100%;
        max-width: none;
      }

      .hero,
      .tables-panel,
      .summary-card,
      .table-wrap,
      .captured-images {
        box-shadow: none;
      }

      .hero,
      .tables-panel,
      .table-section,
      .captured-image-card {
        break-inside: avoid;
      }

      .captured-images {
        break-before: page;
        page-break-before: always;
        border: 0;
        padding: 0;
      }

      .captured-image-page {
        grid-template-columns: repeat(2, minmax(0, 1fr));
        gap: 6mm;
        break-after: page;
        page-break-after: always;
      }

      .captured-image-page:last-child {
        break-after: auto;
        page-break-after: auto;
      }

      .captured-image-card img {
        max-height: 40mm;
      }
    }
  </style>
</head>
<body>
  <div class="report-shell">
    <section class="hero">
      <div class="hero-grid">
        <div>
          <p class="eyebrow">Checklist Assistant HTML Report</p>
          <h1>{{checklist_title}}</h1>
          <p class="hero-copy">Browser-first checklist reporting with deterministic slug injection, template fallback, and a machine-readable JSONL sidecar for downstream tooling.</p>
        </div>
        <aside class="meta-card">
          <h2>Run Metadata</h2>
          <div class="meta-grid">
            <div class="meta-item">
              <span class="meta-label">Checklist</span>
              <span class="meta-value">{{checklist}}</span>
            </div>
            <div class="meta-item">
              <span class="meta-label">Instance ID</span>
              <span class="meta-value">{{instance_id}}</span>
            </div>
            <div class="meta-item">
              <span class="meta-label">Principal</span>
              <span class="meta-value">{{instance_principal}}</span>
            </div>
            <div class="meta-item">
              <span class="meta-label">Generated (UTC)</span>
              <span class="meta-value">{{timestamp}}</span>
            </div>
            <div class="meta-item">
              <span class="meta-label">Rendered Rows</span>
              <span class="meta-value">{{row_count}}</span>
            </div>
            <div class="meta-item">
              <span class="meta-label">Template</span>
              <span class="meta-value">{{template_name}}</span>
            </div>
          </div>
        </aside>
      </div>
    </section>

    <section class="summary-grid">
      <article class="summary-card">
        <span class="summary-label">Report File</span>
        <span class="summary-value">{{report_filename}}</span>
      </article>
      <article class="summary-card">
        <span class="summary-label">JSONL Sidecar</span>
        <span class="summary-value">{{jsonl_filename}}</span>
      </article>
      <article class="summary-card">
        <span class="summary-label">Template Used</span>
        <span class="summary-value">{{template_used}}</span>
      </article>
      <article class="summary-card">
        <span class="summary-label">Output Directory</span>
        <span class="summary-value">{{output_dir}}</span>
      </article>
    </section>

    <section class="tables-panel">
      <div class="panel-head">
        <div>
          <h2>Checklist Data</h2>
          <p>Rows injected directly into a custom template are removed from AutoTables unless the template uses the `_keep` form.</p>
        </div>
      </div>
      {{AutoTables}}
    </section>
    {{CapturedImages}}
  </div>
</body>
</html>
)";
}

ordered_json SlugToJson(const ChecklistSlug& slug, const ReportJsonlOptions& options) {
  ordered_json row = ordered_json::object();
  if (options.include_address_id) {
    row["address_id"] = slug.address_id;
  }
  if (options.include_address_order) {
    row["address_order"] = slug.address_order;
  }
  if (options.include_checklist) {
    row["checklist"] = slug.checklist;
  }
  if (options.include_section) {
    row["section"] = slug.section;
  }
  if (options.include_procedure) {
    row["procedure"] = slug.procedure;
  }
  if (options.include_entity_id) {
    row["entity_id"] = slug.entity_id;
  }
  if (options.include_entity_principal) {
    row["entity_principal"] = slug.entity_principal;
  }
  if (options.include_instance_id) {
    row["instance_id"] = slug.instance_id;
  }
  if (options.include_instance_principal) {
    row["instance_principal"] = slug.instance_principal;
  }
  if (options.include_slug_id) {
    row["slug_id"] = slug.slug_id;
  }
  if (options.include_instructions) {
    row["instructions"] = slug.instructions;
  }
  if (options.include_relationships) {
    ordered_json relationships = ordered_json::array();
    for (const auto& edge : slug.relationships) {
      relationships.push_back({{"predicate", edge.predicate}, {"target", edge.target}});
    }
    row["relationships"] = std::move(relationships);
  }
  if (options.include_timestamp) {
    row["timestamp"] = slug.timestamp;
  }
  if (options.include_action) {
    row["action"] = slug.action;
  }
  if (options.include_spec) {
    row["spec"] = slug.spec;
  }
  if (options.include_result) {
    row["result"] = slug.result;
  }
  if (options.include_status) {
    row["status"] = StatusToString(slug.status);
  }
  if (options.include_comment) {
    row["comment"] = slug.comment;
  }
  return row;
}

bool ShouldOmitJsonlRow(const ChecklistSlug& slug, const ReportJsonlOptions& options) {
  if (!options.omit_unknown_and_na) {
    return false;
  }
  return slug.status == ChecklistStatus::kUnknown || slug.status == ChecklistStatus::kNA;
}

bool WriteJsonlSnapshot(const fs::path& path, const std::vector<ChecklistSlug>& slugs,
                        const ReportJsonlOptions& options, std::string* error_out) {
  std::ofstream jsonl(path, std::ios::binary);
  if (!jsonl.is_open()) {
    if (error_out) {
      *error_out = "Failed to open JSONL path for writing: " + path.string();
    }
    return false;
  }
  bool wrote_row = false;
  for (const auto& slug : slugs) {
    if (ShouldOmitJsonlRow(slug, options)) {
      continue;
    }
    if (wrote_row) {
      jsonl << "\n";
    }
    jsonl << SlugToJson(slug, options).dump();
    wrote_row = true;
  }
  if (!jsonl) {
    if (error_out) {
      *error_out = "Failed to write JSONL snapshot: " + path.string();
    }
    return false;
  }
  return true;
}

}  // namespace

ReportJsonlOptions ReportJsonlOptions::Report() {
  ReportJsonlOptions options;
  options.omit_unknown_and_na = true;
  return options;
}

ReportJsonlOptions ReportJsonlOptions::Minimal() {
  ReportJsonlOptions options;
  options.include_checklist = false;
  options.include_section = false;
  options.include_procedure = false;
  options.include_action = false;
  options.include_spec = false;
  options.include_result = true;
  options.include_status = true;
  options.include_comment = true;
  options.include_timestamp = false;
  options.include_address_order = false;
  options.include_entity_id = false;
  options.include_instance_id = false;
  options.omit_unknown_and_na = true;
  return options;
}

ReportJsonlOptions ReportJsonlOptions::Full() {
  ReportJsonlOptions options;
  options.include_checklist = true;
  options.include_section = true;
  options.include_procedure = true;
  options.include_action = true;
  options.include_spec = true;
  options.include_result = true;
  options.include_status = true;
  options.include_comment = true;
  options.include_timestamp = true;
  options.include_address_order = true;
  options.include_entity_id = true;
  options.include_entity_principal = false;
  options.include_instance_id = false;
  options.include_instance_principal = false;
  options.include_slug_id = false;
  options.include_instructions = true;
  options.include_relationships = true;
  options.omit_unknown_and_na = false;
  return options;
}

std::string RenderLatexReport(const std::string& template_body, const TemplateContext& context,
                              const std::vector<ReportRow>& rows) {
  TemplateContext safe_context;
  safe_context.reserve(context.size() + 2);
  for (const auto& entry : context) {
    const std::string& key = entry.first;
    if (key == "AutoTables" || key == "AutoGeneratedTablePages" ||
        key == "CapturedImageFigures") {
      safe_context[key] = entry.second;
    } else {
      safe_context[key] = EscapeLatex(entry.second);
    }
  }

  const std::string auto_tables = RenderAutoTables(rows);
  safe_context["AutoTables"] = auto_tables;
  safe_context["AutoGeneratedTablePages"] = auto_tables;

  const std::string metadata_header = BuildReportHeader(context);
  return RenderTemplate(metadata_header + template_body, safe_context);
}

std::string RenderHtmlReport(const std::string& template_body, const TemplateContext& context,
                             const std::vector<ReportRow>& rows) {
  TemplateContext safe_context;
  safe_context.reserve(context.size() + 2);
  for (const auto& entry : context) {
    const std::string& key = entry.first;
    if (key == "AutoTables" || key == "AutoGeneratedTablePages" ||
        key == "CapturedImages") {
      safe_context[key] = entry.second;
    } else {
      safe_context[key] = EscapeHtml(entry.second);
    }
  }

  const std::string auto_tables = RenderHtmlAutoTables(rows);
  safe_context["AutoTables"] = auto_tables;
  safe_context["AutoGeneratedTablePages"] = auto_tables;

  const std::string metadata_header = BuildHtmlReportHeader(context);
  return RenderTemplate(metadata_header + template_body, safe_context);
}

TexReportResult GenerateTexReport(const TexReportConfig& config, const std::string& checklist,
                                  const std::string& instance_id,
                                  const std::string& instance_principal,
                                  const std::vector<ChecklistSlug>& slugs,
                                  const std::unordered_map<std::string, std::string>& slug_aliases,
                                  const ReportJsonlOptions& jsonl_options,
                                  TemplateContext* context_out) {
  if (slugs.empty()) {
    throw std::invalid_argument("Cannot render a report with no slugs.");
  }

  TexReportResult result;
  result.generated_at = CurrentTimestampIsoUtc();

  const ReportOutputPaths paths =
      PrepareReportOutputPaths(config, checklist, instance_id, result.generated_at, ".tex");
  result.output_dir = paths.output_dir;
  result.output_path = paths.output_path;

  const ResolvedTemplate resolved_template = ResolveReportTemplate(
      paths.templates_root, checklist, paths.safe_checklist, ".tex", DefaultTemplate());
  result.template_used = resolved_template.used;
  if (result.template_used) {
    result.template_path = resolved_template.path;
  }

  const CapturedReportImages captured_images = PrepareCapturedReportImages(paths, instance_id);
  result.image_count = captured_images.images.size();
  result.images_manifest_path = captured_images.copied_manifest_path;

  PreparedReportContent prepared = PrepareReportContent(
      resolved_template.placeholders, checklist, instance_id, instance_principal, slugs,
      slug_aliases, result.generated_at, result.template_used, result.template_path,
      result.output_dir, result.output_path);
  AddCapturedImagesContext(prepared, captured_images);
  result.row_count = prepared.row_count;
  result.alias_hits = prepared.alias_hits;
  if (context_out) {
    *context_out = prepared.context;
  }

  const std::string rendered =
      RenderLatexReport(resolved_template.body, prepared.context, prepared.rows);

  std::ofstream output(result.output_path, std::ios::binary);
  if (!output.is_open()) {
    throw std::runtime_error("Failed to open report path for writing: " +
                             result.output_path.string());
  }
  output << rendered;

  const fs::path jsonl_path = result.output_dir / (paths.safe_checklist + ".jsonl");
  std::string jsonl_error;
  if (!WriteJsonlSnapshot(jsonl_path, slugs, jsonl_options, &jsonl_error)) {
    throw std::runtime_error(jsonl_error);
  }
  result.jsonl_path = jsonl_path;
  result.jsonl_written = true;

  return result;
}

HtmlReportResult GenerateHtmlReport(const HtmlReportConfig& config, const std::string& checklist,
                                    const std::string& instance_id,
                                    const std::string& instance_principal,
                                    const std::vector<ChecklistSlug>& slugs,
                                    const std::unordered_map<std::string, std::string>& slug_aliases,
                                    const ReportJsonlOptions& jsonl_options,
                                    TemplateContext* context_out) {
  if (slugs.empty()) {
    throw std::invalid_argument("Cannot render a report with no slugs.");
  }

  HtmlReportResult result;
  result.generated_at = CurrentTimestampIsoUtc();

  const ReportOutputPaths paths =
      PrepareReportOutputPaths(config, checklist, instance_id, result.generated_at, ".html");
  result.output_dir = paths.output_dir;
  result.output_path = paths.output_path;

  const ResolvedTemplate resolved_template = ResolveReportTemplate(
      paths.templates_root, checklist, paths.safe_checklist, ".html", DefaultHtmlTemplate());
  result.template_used = resolved_template.used;
  if (result.template_used) {
    result.template_path = resolved_template.path;
  }

  const CapturedReportImages captured_images = PrepareCapturedReportImages(paths, instance_id);
  result.image_count = captured_images.images.size();
  result.images_manifest_path = captured_images.copied_manifest_path;

  PreparedReportContent prepared = PrepareReportContent(
      resolved_template.placeholders, checklist, instance_id, instance_principal, slugs,
      slug_aliases, result.generated_at, result.template_used, result.template_path,
      result.output_dir, result.output_path);
  AddCapturedImagesContext(prepared, captured_images);
  result.row_count = prepared.row_count;
  result.alias_hits = prepared.alias_hits;
  if (context_out) {
    *context_out = prepared.context;
  }

  const std::string rendered =
      RenderHtmlReport(resolved_template.body, prepared.context, prepared.rows);

  std::ofstream output(result.output_path, std::ios::binary);
  if (!output.is_open()) {
    throw std::runtime_error("Failed to open report path for writing: " +
                             result.output_path.string());
  }
  output << rendered;

  const fs::path jsonl_path = result.output_dir / (paths.safe_checklist + ".jsonl");
  std::string jsonl_error;
  if (!WriteJsonlSnapshot(jsonl_path, slugs, jsonl_options, &jsonl_error)) {
    throw std::runtime_error(jsonl_error);
  }
  result.jsonl_path = jsonl_path;
  result.jsonl_written = true;

  return result;
}

FillableReportResult GenerateFillableReport(const TexReportConfig& config,
                                            const std::string& checklist,
                                            const std::string& instance_id,
                                            const std::string& generated_at,
                                            const TemplateContext& context,
                                            const std::vector<ChecklistSlug>& slugs,
                                            const ReportJsonlOptions& jsonl_options) {
  FillableReportResult result;
  const fs::path reports_root = config.reports_root.empty()
                                    ? ResolveDefaultRoot(fs::path{"checklists"}) / "reports"
                                    : config.reports_root;
  const fs::path templates_root =
      config.templates_root.empty()
          ? ResolveDefaultRoot(fs::path{"checklists"}) / "templates"
          : config.templates_root;
  const std::string safe_checklist = SanitizeToken(checklist);
  const std::string safe_instance = SanitizeToken(instance_id);
  const std::string safe_timestamp = SanitizeTimestampForPath(generated_at);
  result.fdf_template_path = FindTemplatePath(templates_root, checklist, safe_checklist, ".fdf");
  if (result.fdf_template_path.empty()) {
    return result;
  }
  result.template_used = true;
  result.pdf_template_path = FindTemplatePath(templates_root, checklist, safe_checklist, ".pdf");
  result.output_dir = reports_root / (safe_instance + "_" + safe_timestamp);

  std::error_code ec;
  fs::create_directories(result.output_dir, ec);
  if (ec) {
    result.error = "Failed to create fillable output directory: " + result.output_dir.string();
    core::logging::LogWarn("Fillable report export failed: " + result.error);
    return result;
  }

  std::string template_body;
  try {
    template_body = ReadFile(result.fdf_template_path);
  } catch (const std::exception& ex) {
    result.error = ex.what();
    core::logging::LogWarn("Fillable report export failed: " + result.error);
    return result;
  }

  const std::vector<std::string> fields = ExtractFdfFieldNames(template_body);
  const std::string pdf_filename =
      result.pdf_template_path.empty() ? std::string{} : (safe_checklist + ".pdf");

  result.fdf_output_path = result.output_dir / (safe_checklist + ".fdf");
  try {
    std::ofstream fdf_file(result.fdf_output_path, std::ios::binary);
    if (!fdf_file.is_open()) {
      result.error = "Failed to open FDF path for writing: " + result.fdf_output_path.string();
      core::logging::LogWarn("Fillable report export failed: " + result.error);
      return result;
    }
    fdf_file << BuildFdfPayload(context, fields, pdf_filename);
    if (!fdf_file) {
      result.error = "Failed to write FDF output: " + result.fdf_output_path.string();
      core::logging::LogWarn("Fillable report export failed: " + result.error);
      return result;
    }
  } catch (const std::exception& ex) {
    result.error = ex.what();
    core::logging::LogWarn("Fillable report export failed: " + result.error);
    return result;
  }
  result.fdf_written = true;

  if (!result.pdf_template_path.empty()) {
    result.pdf_copy_path = result.output_dir / (safe_checklist + ".pdf");
    std::error_code copy_ec;
    fs::copy_file(result.pdf_template_path, result.pdf_copy_path,
                  fs::copy_options::overwrite_existing, copy_ec);
    if (copy_ec) {
      core::logging::LogWarn("Fillable report export could not copy PDF template: " +
                             result.pdf_template_path.string());
    } else {
      result.pdf_copied = true;
    }
  }

  result.jsonl_path = result.output_dir / (safe_checklist + ".jsonl");
  std::string jsonl_error;
  if (!WriteJsonlSnapshot(result.jsonl_path, slugs, jsonl_options, &jsonl_error)) {
    result.error = jsonl_error;
    core::logging::LogWarn("Fillable report export failed: " + result.error);
    return result;
  }
  result.jsonl_written = true;

  return result;
}

}  // namespace core
