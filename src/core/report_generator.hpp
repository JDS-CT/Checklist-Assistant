#pragma once

#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/checklist_store.hpp"

namespace core {

struct ReportRow {
  std::string section;
  std::string procedure;
  std::string spec;
  std::string result;
  std::string status;
  std::string comment;
  std::string address_id;
  bool include_in_auto = true;
};

using TemplateContext = std::unordered_map<std::string, std::string>;

struct ReportJsonlOptions {
  bool include_checklist = true;
  bool include_section = true;
  bool include_procedure = true;
  bool include_action = true;
  bool include_spec = true;
  bool include_result = true;
  bool include_status = true;
  bool include_comment = true;
  bool include_timestamp = true;
  bool include_address_id = true;
  bool include_address_order = true;
  bool include_entity_id = true;
  bool include_entity_principal = false;
  bool include_instance_id = false;
  bool include_instance_principal = false;
  bool include_slug_id = false;
  bool include_instructions = false;
  bool include_relationships = false;
  bool omit_unknown_and_na = false;

  static ReportJsonlOptions Report();
  static ReportJsonlOptions Minimal();
  static ReportJsonlOptions Full();
};

struct TexReportConfig {
  std::filesystem::path reports_root;
  std::filesystem::path templates_root;
};

struct TexReportResult {
  std::filesystem::path output_path;
  std::filesystem::path output_dir;
  std::filesystem::path jsonl_path;
  std::filesystem::path images_manifest_path;
  bool template_used = false;
  bool jsonl_written = false;
  std::filesystem::path template_path;
  std::string generated_at;
  std::size_t row_count = 0;
  std::size_t image_count = 0;
  std::vector<std::pair<std::string, std::string>> alias_hits;
};

using HtmlReportConfig = TexReportConfig;

struct HtmlReportResult {
  std::filesystem::path output_path;
  std::filesystem::path output_dir;
  std::filesystem::path jsonl_path;
  std::filesystem::path images_manifest_path;
  bool template_used = false;
  bool jsonl_written = false;
  std::filesystem::path template_path;
  std::string generated_at;
  std::size_t row_count = 0;
  std::size_t image_count = 0;
  std::vector<std::pair<std::string, std::string>> alias_hits;
};

struct FillableReportResult {
  std::filesystem::path output_dir;
  std::filesystem::path fdf_output_path;
  std::filesystem::path jsonl_path;
  std::filesystem::path fdf_template_path;
  std::filesystem::path pdf_template_path;
  std::filesystem::path pdf_copy_path;
  bool template_used = false;
  bool fdf_written = false;
  bool jsonl_written = false;
  bool pdf_copied = false;
  std::string error;
};

std::string RenderLatexReport(const std::string& template_body, const TemplateContext& context,
                              const std::vector<ReportRow>& rows);

std::string RenderHtmlReport(const std::string& template_body, const TemplateContext& context,
                             const std::vector<ReportRow>& rows);

TexReportResult GenerateTexReport(const TexReportConfig& config, const std::string& checklist,
                                  const std::string& instance_id,
                                  const std::string& instance_principal,
                                  const std::vector<ChecklistSlug>& slugs,
                                  const std::unordered_map<std::string, std::string>& slug_aliases = {},
                                  const ReportJsonlOptions& jsonl_options = ReportJsonlOptions{},
                                  TemplateContext* context_out = nullptr);

HtmlReportResult GenerateHtmlReport(
    const HtmlReportConfig& config, const std::string& checklist, const std::string& instance_id,
    const std::string& instance_principal, const std::vector<ChecklistSlug>& slugs,
    const std::unordered_map<std::string, std::string>& slug_aliases = {},
    const ReportJsonlOptions& jsonl_options = ReportJsonlOptions{},
    TemplateContext* context_out = nullptr);

FillableReportResult GenerateFillableReport(const TexReportConfig& config,
                                            const std::string& checklist,
                                            const std::string& instance_id,
                                            const std::string& generated_at,
                                            const TemplateContext& context,
                                            const std::vector<ChecklistSlug>& slugs,
                                            const ReportJsonlOptions& jsonl_options = ReportJsonlOptions{});

}  // namespace core
