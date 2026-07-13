#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/checklist_store.hpp"
#include "core/report_generator.hpp"

namespace {

bool KeepArtifacts() {
  const char* env = std::getenv("CHAX_KEEP_TEST_ARTIFACTS");
  return env != nullptr && env[0] != '\0';
}

bool CleanArtifactsOnExit() {
  const char* env = std::getenv("CHAX_CLEAN_TEST_ARTIFACTS");
  return env != nullptr && env[0] != '\0';
}

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
  std::cout << "CHAX_STEP|report_generation_test|" << procedure << "|"
            << (pass ? "Pass" : "Fail") << "|" << SanitizeMessage(message) << "\n";
}

int CountOccurrences(const std::string& haystack, const std::string& needle) {
  int count = 0;
  std::size_t pos = haystack.find(needle);
  while (pos != std::string::npos) {
    ++count;
    pos = haystack.find(needle, pos + needle.size());
  }
  return count;
}

struct RowDef {
  std::string section;
  std::string procedure;
  std::string spec;
  std::string result;
  std::string status;
  std::string comment;
  std::string slug_id;
};

const std::vector<RowDef> kRows = {
    {"Alpha", "Pass One", "Spec-A1", "Result-A1", "Pass", "Pass comment", "SLUGAAAAAAA00001"},
    {"Alpha", "Pass Two", "Spec-A2", "Result-A2", "Pass", "", "SLUGAAAAAAA00002"},
    {"Beta", "Fail One", "Spec-B1", "Result-B1", "Fail", "Fail comment", "SLUGAAAAAAA00003"},
    {"Beta", "Fail Two", "Spec-B2", "Result-B2", "Fail", "", "SLUGAAAAAAA00004"},
    {"Gamma", "NA One", "Spec-C1", "Result-C1", "NA", "NA comment", "SLUGAAAAAAA00005"},
    {"Gamma", "NA Two", "Spec-C2", "Result-C2", "NA", "", "SLUGAAAAAAA00006"},
    {"Delta", "Other One", "Spec-D1", "Result-D1", "Other", "Other comment", "SLUGAAAAAAA00007"},
    {"Delta", "Other Two", "Spec-D2", "Result-D2", "Other", "", "SLUGAAAAAAA00008"},
    {"Epsilon", "Blank One", "Spec-E1", "Result-E1", "", "Blank comment", "SLUGAAAAAAA00009"},
    {"Epsilon", "Blank Two", "Spec-E2", "Result-E2", "", "", "SLUGAAAAAAA00010"},
};

core::ChecklistSlug MakeSlug(const RowDef& def, const std::string& checklist,
                             const std::string& instance_id) {
  core::ChecklistSlug slug;
  slug.checklist = checklist;
  slug.instance_id = instance_id;
  slug.instance_principal = "instance||" + instance_id;
  slug.section = def.section;
  slug.procedure = def.procedure;
  slug.action = def.procedure;
  slug.spec = def.spec;
  slug.result = def.result;
  slug.status = core::ParseStatus(def.status);
  slug.comment = def.comment;
  slug.slug_id = def.slug_id;
  slug.address_id = slug.slug_id + instance_id;
  return slug;
}

std::vector<core::ChecklistSlug> BuildSlugs(const std::string& checklist,
                                            const std::string& instance_id) {
  std::vector<core::ChecklistSlug> slugs;
  slugs.reserve(kRows.size());
  for (const auto& row : kRows) {
    slugs.push_back(MakeSlug(row, checklist, instance_id));
  }
  return slugs;
}

std::string ReadFile(const std::filesystem::path& path) {
  std::ifstream file(path);
  std::stringstream buffer;
  buffer << file.rdbuf();
  return buffer.str();
}

std::string AddressFor(const std::vector<core::ChecklistSlug>& slugs, const std::string& procedure) {
  for (const auto& slug : slugs) {
    if (slug.procedure == procedure) return slug.address_id;
  }
  return {};
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

}  // namespace

int main() {
  std::string current_step;
  try {
    const auto tmp_root = ResolveRepoRoot() / ".chax" / "test-artifacts" / "report-generation";
    std::error_code ec;
    std::filesystem::remove_all(tmp_root, ec);
    const auto reports_root = tmp_root / "reports";
    const auto templates_root = tmp_root / "templates";
    std::filesystem::create_directories(reports_root);
    std::filesystem::create_directories(templates_root);

    const std::string auto_preamble = R"(\documentclass{article}
\usepackage{array}
\usepackage[table]{xcolor}
\usepackage{float}
\newcounter{tablecount}
\setcounter{tablecount}{0}
\begin{document}
)";

    const std::string instance_id = "INSTINSTANCE0001";

    // Template 1: auto tables only (default behavior).
    {
      current_step = "auto tables";
      const std::string checklist = "auto-only";
      auto slugs = BuildSlugs(checklist, instance_id);
      const auto report = core::GenerateTexReport(
          core::TexReportConfig{reports_root, templates_root}, checklist, instance_id,
          "instance||" + instance_id, slugs);
      const std::string content = ReadFile(report.output_path);
      Assert(content.find("NA One") == std::string::npos, "NA rows should be omitted from AutoTables");
      Assert(content.find("Blank One") == std::string::npos,
             "Blank status rows should be omitted from AutoTables");
      Assert(CountOccurrences(content, "Pass One") == 1 && CountOccurrences(content, "Fail One") == 1 &&
                 CountOccurrences(content, "Other One") == 1,
             "Included statuses should appear once in AutoTables");
      Assert(report.jsonl_written, "Auto-only report should emit JSONL");
      RecordStep(current_step, true, "auto tables ok");
    }

    // Template 1b: auto tables split large sections into multiple tables.
    {
      current_step = "auto table pagination";
      const std::string checklist = "auto-paginate";
      std::vector<core::ChecklistSlug> slugs;
      slugs.reserve(64);
      for (int i = 1; i <= 64; ++i) {
        core::ChecklistSlug slug;
        slug.checklist = checklist;
        slug.instance_id = instance_id;
        slug.instance_principal = "instance||" + instance_id;
        slug.section = "Long Section";
        slug.procedure = "Procedure " + std::to_string(i);
        slug.action = slug.procedure;
        slug.spec = "Spec " + std::to_string(i);
        slug.result = "Result " + std::to_string(i);
        slug.status = core::ChecklistStatus::kPass;
        slug.comment = (i % 5 == 0) ? ("Comment " + std::to_string(i)) : "";
        slug.slug_id = "SLUGLONGSECTION" + std::to_string(1000 + i);
        slug.address_id = slug.slug_id + instance_id;
        slugs.push_back(std::move(slug));
      }
      const auto report = core::GenerateTexReport(
          core::TexReportConfig{reports_root, templates_root}, checklist, instance_id,
          "instance||" + instance_id, slugs);
      const std::string content = ReadFile(report.output_path);
      Assert(CountOccurrences(content, "\\begin{table}[H]") >= 2,
             "Large section should split into multiple table blocks");
      Assert(content.find("Long Section (Part 2)") != std::string::npos,
             "Large section should include part captions after the first table");
      Assert(content.find("\\label{tab:long-section-2}") != std::string::npos,
             "Large section split tables should use unique labels");
      RecordStep(current_step, true, "auto table pagination ok");
    }

    // Build baseline slugs for remaining templates and map addresses.
    const std::string checklist_template = "template-only";
    auto template_slugs = BuildSlugs(checklist_template, instance_id);
    const std::string addr_pass_one = AddressFor(template_slugs, "Pass One");
    const std::string addr_fail_one = AddressFor(template_slugs, "Fail One");
    const std::string addr_other_one = AddressFor(template_slugs, "Other One");
    const std::string addr_blank_one = AddressFor(template_slugs, "Blank One");

    // Template 2: template-only, injects slug fields and suppresses auto tables entirely.
    {
      current_step = "template only";
      std::ofstream templ(templates_root / (checklist_template + ".tex"));
      templ << R"(\documentclass{article}
\begin{document}
Pass status: {{slug_)" << addr_pass_one << R"(_status}}
Fail comment: {{slug_)" << addr_fail_one << R"(_comment}}
\end{document}
)";
      templ.close();
      const auto report = core::GenerateTexReport(
          core::TexReportConfig{reports_root, templates_root}, checklist_template, instance_id,
          "instance||" + instance_id, template_slugs);
      const std::string content = ReadFile(report.output_path);
      Assert(content.find("Pass status: Pass") != std::string::npos,
             "Template-only report should inject status");
      Assert(content.find("Fail comment: Fail comment") != std::string::npos,
             "Template-only report should inject comment");
      Assert(content.find("tab:") == std::string::npos, "Template-only report should not render tables");
      RecordStep(current_step, true, "template only ok");
    }

    // Template 2b: lineage aliasing replaces legacy slug tokens.
    {
      current_step = "lineage aliases";
      const std::string checklist = "lineage-alias";
      auto slugs = BuildSlugs(checklist, instance_id);
      std::unordered_map<std::string, std::string> aliases;
      aliases.emplace(slugs[0].slug_id, slugs[1].slug_id);
      std::ofstream templ(templates_root / (checklist + ".tex"));
      templ << R"(\documentclass{article}
\begin{document}
Alias procedure: {{slug_)" << slugs[0].slug_id << R"(_procedure}}
\end{document}
)";
      templ.close();
      const auto report = core::GenerateTexReport(
          core::TexReportConfig{reports_root, templates_root}, checklist, instance_id,
          "instance||" + instance_id, slugs, aliases);
      const std::string content = ReadFile(report.output_path);
      Assert(content.find("Alias procedure: Pass Two") != std::string::npos,
             "Slug lineage alias should resolve to latest slug values");
      RecordStep(current_step, true, "lineage aliases ok");
    }

    // Template 2c: lineage aliasing omits auto tables without keep.
    {
      current_step = "lineage alias omit autotable";
      const std::string checklist = "lineage-alias-omit";
      auto slugs = BuildSlugs(checklist, instance_id);
      std::unordered_map<std::string, std::string> aliases;
      aliases.emplace(slugs[0].slug_id, slugs[1].slug_id);
      std::ofstream templ(templates_root / (checklist + ".tex"));
      templ << auto_preamble << R"(
Alias result: {{slug_)" << slugs[0].slug_id << R"(_result}}
{{AutoTables}}
\end{document}
)";
      templ.close();
      const auto report = core::GenerateTexReport(
          core::TexReportConfig{reports_root, templates_root}, checklist, instance_id,
          "instance||" + instance_id, slugs, aliases);
      const std::string content = ReadFile(report.output_path);
      Assert(content.find("Alias result: Result-A2") != std::string::npos,
             "Alias token should resolve to latest slug values");
      Assert(content.find("Pass Two") == std::string::npos,
             "Alias-used slug without _keep should be omitted from AutoTables");
      RecordStep(current_step, true, "lineage alias omit ok");
    }

    // Template 2d: lineage aliasing keeps auto tables with keep.
    {
      current_step = "lineage alias keep autotable";
      const std::string checklist = "lineage-alias-keep";
      auto slugs = BuildSlugs(checklist, instance_id);
      std::unordered_map<std::string, std::string> aliases;
      aliases.emplace(slugs[0].slug_id, slugs[1].slug_id);
      std::ofstream templ(templates_root / (checklist + ".tex"));
      templ << auto_preamble << R"(
Alias result: {{slug_)" << slugs[0].slug_id << R"(_result_keep}}
{{AutoTables}}
\end{document}
)";
      templ.close();
      const auto report = core::GenerateTexReport(
          core::TexReportConfig{reports_root, templates_root}, checklist, instance_id,
          "instance||" + instance_id, slugs, aliases);
      const std::string content = ReadFile(report.output_path);
      Assert(content.find("Alias result: Result-A2") != std::string::npos,
             "Alias token should resolve to latest slug values");
      Assert(content.find("Pass Two") != std::string::npos,
             "Alias-used slug with _keep should remain in AutoTables");
      RecordStep(current_step, true, "lineage alias keep ok");
    }

    // Template 3: template + auto tables, keep one injected row.
    {
      current_step = "template + tables";
      const std::string checklist = "template-auto";
      auto slugs = BuildSlugs(checklist, instance_id);
      const std::string addr = AddressFor(slugs, "Pass Two");
      std::ofstream templ(templates_root / (checklist + ".tex"));
      templ << auto_preamble << R"(
Injected pass: {{slug_)" << addr << R"(_result_keep}}
{{AutoTables}}
\end{document}
)";
      templ.close();
      const auto report = core::GenerateTexReport(
          core::TexReportConfig{reports_root, templates_root}, checklist, instance_id,
          "instance||" + instance_id, slugs);
      const std::string content = ReadFile(report.output_path);
      Assert(content.find("Injected pass: Result-A2") != std::string::npos,
             "Template should inject kept field");
      Assert(content.find("Pass Two") != std::string::npos,
             "Kept slug should remain in AutoTables");
      RecordStep(current_step, true, "template + tables ok");
    }

    // Template 4: repeat a slug field multiple times.
    {
      current_step = "repeat fields";
      const std::string checklist = "repeat-fields";
      auto slugs = BuildSlugs(checklist, instance_id);
      const std::string addr = AddressFor(slugs, "Fail One");
      std::ofstream templ(templates_root / (checklist + ".tex"));
      templ << auto_preamble << R"(
Repeated: {{slug_)" << addr << R"(_procedure}} / {{slug_)" << addr << R"(_procedure}}
\end{document}
)";
      templ.close();
      const auto report = core::GenerateTexReport(
          core::TexReportConfig{reports_root, templates_root}, checklist, instance_id,
          "instance||" + instance_id, slugs);
      const std::string content = ReadFile(report.output_path);
      Assert(CountOccurrences(content, "Fail One") == 2,
             "Slug field should be usable multiple times");
      RecordStep(current_step, true, "repeat fields ok");
    }

    // Template 5: inject and omit from auto tables.
    {
      current_step = "omit autotable";
      const std::string checklist = "omit-autotable";
      auto slugs = BuildSlugs(checklist, instance_id);
      const std::string addr = AddressFor(slugs, "Other Two");
      std::ofstream templ(templates_root / (checklist + ".tex"));
      templ << auto_preamble << R"(
Omit status: {{slug_)" << addr << R"(_status}}
{{AutoTables}}
\end{document}
)";
      templ.close();
      const auto report = core::GenerateTexReport(
          core::TexReportConfig{reports_root, templates_root}, checklist, instance_id,
          "instance||" + instance_id, slugs);
      const std::string content = ReadFile(report.output_path);
      Assert(content.find("Omit status: Other") != std::string::npos,
             "Template should inject field");
      Assert(content.find("Other Two") == std::string::npos,
             "Injected slug without _keep should be omitted from AutoTables");
      RecordStep(current_step, true, "omit autotable ok");
    }

    // Template 6: inject and retain in auto tables.
    {
      current_step = "keep autotable";
      const std::string checklist = "keep-autotable";
      auto slugs = BuildSlugs(checklist, instance_id);
      const std::string addr = AddressFor(slugs, "Other One");
      std::ofstream templ(templates_root / (checklist + ".tex"));
      templ << auto_preamble << R"(
Retain comment: {{slug_)" << addr << R"(_comment_keep}}
{{AutoTables}}
\end{document}
)";
      templ.close();
      const auto report = core::GenerateTexReport(
          core::TexReportConfig{reports_root, templates_root}, checklist, instance_id,
          "instance||" + instance_id, slugs);
      const std::string content = ReadFile(report.output_path);
      Assert(content.find("Retain comment: Other comment") != std::string::npos,
             "Template should inject kept field");
      Assert(content.find("Other One") != std::string::npos,
             "Kept slug should remain in AutoTables even after template use");
      RecordStep(current_step, true, "keep autotable ok");
    }

    // Template 6b: escape common Unicode values into LaTeX-safe output.
    {
      current_step = "latex escaping";
      const std::string checklist = "latex-escape";
      auto slugs = BuildSlugs(checklist, instance_id);
      const std::string special_value =
          std::string("\xC2\xA1 15W during warm-up. \xE2\x89\xA4 5 s \xC2\xB1 2 \xC2\xB0") +
          "C";
      slugs[0].result = special_value;
      const std::string addr = AddressFor(slugs, "Pass One");
      std::ofstream templ(templates_root / (checklist + ".tex"));
      templ << R"(\documentclass{article}
\begin{document}
Value: {{slug_)" << addr << R"(_result}}
\end{document}
)";
      templ.close();
      const auto report = core::GenerateTexReport(
          core::TexReportConfig{reports_root, templates_root}, checklist, instance_id,
          "instance||" + instance_id, slugs);
      const std::string content = ReadFile(report.output_path);
      Assert(content.find("Value: ! 15W during warm-up.") != std::string::npos,
             "LaTeX escaping should map inverted punctuation safely");
      Assert(content.find("$\\leq$") != std::string::npos,
             "LaTeX escaping should map <= to math mode");
      Assert(content.find("$\\pm$") != std::string::npos,
             "LaTeX escaping should map plus-minus to math mode");
      Assert(content.find("$^\\circ$C") != std::string::npos,
             "LaTeX escaping should map degrees to math mode");
      RecordStep(current_step, true, "latex escaping ok");
    }

    // Template 6c: HTML default template fallback.
    {
      current_step = "html default";
      const std::string checklist = "html-default";
      auto slugs = BuildSlugs(checklist, instance_id);
      slugs[0].procedure = "Inspect & Clean <Assembly>";
      const auto report = core::GenerateHtmlReport(
          core::HtmlReportConfig{reports_root, templates_root}, checklist, instance_id,
          "instance||" + instance_id, slugs);
      const std::string content = ReadFile(report.output_path);
      Assert(report.output_path.extension() == ".html",
             "HTML report should use the .html extension");
      Assert(content.find("<!doctype html>") != std::string::npos,
             "HTML default report should emit an HTML document");
      Assert(content.find("Inspect &amp; Clean &lt;Assembly&gt;") != std::string::npos,
             "HTML output should escape injected text");
      Assert(content.find("NA One") == std::string::npos,
             "HTML AutoTables should omit NA rows");
      Assert(report.jsonl_written, "HTML default report should emit JSONL");
      RecordStep(current_step, true, "html default ok");
    }

    // Template 6d: HTML custom templates use the same slug placeholders and keep logic.
    {
      current_step = "html template keep/omit";
      const std::string checklist = "html-template";
      auto slugs = BuildSlugs(checklist, instance_id);
      const std::string addr_fail = AddressFor(slugs, "Fail One");
      const std::string addr_other = AddressFor(slugs, "Other One");
      std::filesystem::create_directories(templates_root / "html");
      std::ofstream templ(templates_root / "html" / (checklist + ".html"));
      templ << R"(<!doctype html>
<html lang="en"><body>
<div>Injected omit: {{slug-)" << addr_fail << R"(-result}}</div>
<div>Injected keep: {{slug_)" << addr_other << R"(_comment_keep}}</div>
<section>{{AutoTables}}</section>
</body></html>
)";
      templ.close();
      const auto report = core::GenerateHtmlReport(
          core::HtmlReportConfig{reports_root, templates_root}, checklist, instance_id,
          "instance||" + instance_id, slugs);
      const std::string content = ReadFile(report.output_path);
      Assert(report.template_used, "HTML checklist template should be detected");
      Assert(report.template_path.filename() == (checklist + ".html"),
             "HTML report should resolve templates from templates/html");
      Assert(content.find("Injected omit: Result-B1") != std::string::npos,
             "HTML template should inject slug fields");
      Assert(content.find("Injected keep: Other comment") != std::string::npos,
             "HTML template should inject kept slug fields");
      Assert(content.find("Fail One") == std::string::npos,
             "HTML template rows without _keep should be omitted from AutoTables");
      Assert(content.find("Other One") != std::string::npos,
             "HTML template rows with _keep should remain in AutoTables");
      RecordStep(current_step, true, "html template keep/omit ok");
    }

    // Template 6e: captured image evidence is copied into a self-contained report bundle and
    // rendered as four print-safe cards per page in HTML and LaTeX.
    {
      current_step = "captured report images";
      const std::string checklist = "image-evidence";
      const std::string image_instance_id = "IMAGES0000000001";
      const auto slugs = BuildSlugs(checklist, image_instance_id);
      const auto image_root = reports_root / image_instance_id / "images" / "evidence";
      std::filesystem::create_directories(image_root);
      for (int image_number = 1; image_number <= 4; ++image_number) {
        std::ofstream preview(image_root / ("capture-" + std::to_string(image_number) + ".png"),
                              std::ios::binary);
        preview << "PNG test preview " << image_number;
        std::ofstream original(image_root / ("capture-" + std::to_string(image_number) + ".tif"),
                               std::ios::binary);
        original << "TIFF test original " << image_number;
      }
      std::ofstream manifest(reports_root / image_instance_id / "images" / "manifest.json");
      manifest << R"({
  "schema": "chax-report-images-v1",
  "images": [
    {"preview": "evidence/capture-1.png", "original": "evidence/capture-1.tif", "procedure": "Image One", "caption": "Caption & one", "captured_at": "2026-01-01T00:00:00Z", "source": "Test collector"},
    {"preview": "evidence/capture-2.png", "original": "evidence/capture-2.tif", "procedure": "Image Two"},
    {"preview": "evidence/capture-3.png", "original": "evidence/capture-3.tif", "procedure": "Image Three"},
    {"preview": "evidence/capture-4.png", "original": "evidence/capture-4.tif", "procedure": "Image Four"}
  ]
})";
      manifest.close();

      std::filesystem::create_directories(templates_root / "html");
      std::filesystem::create_directories(templates_root / "tex");
      std::ofstream html_template(templates_root / "html" / (checklist + ".html"));
      html_template << "<!doctype html><html><body>{{CapturedImages}}</body></html>\n";
      html_template.close();
      std::ofstream tex_template(templates_root / "tex" / (checklist + ".tex"));
      tex_template << "\\documentclass{article}\\usepackage{graphicx}\\begin{document}\n"
                   << "{{CapturedImageFigures}}\n\\end{document}\n";
      tex_template.close();

      const auto html_report = core::GenerateHtmlReport(
          core::HtmlReportConfig{reports_root, templates_root}, checklist, image_instance_id,
          "instance||" + image_instance_id, slugs);
      const std::string html_content = ReadFile(html_report.output_path);
      Assert(html_report.image_count == 4, "HTML report should count copied image evidence");
      Assert(std::filesystem::exists(html_report.images_manifest_path),
             "HTML report should retain a copied image manifest");
      Assert(std::filesystem::exists(html_report.output_dir / "images" / "evidence" / "capture-1.png"),
             "HTML report should retain the preview image");
      Assert(std::filesystem::exists(html_report.output_dir / "images" / "evidence" / "capture-1.tif"),
             "HTML report should retain the original image");
      Assert(CountOccurrences(html_content, "captured-image-card") == 4,
             "HTML report should render four image cards");
      Assert(html_content.find("Caption &amp; one") != std::string::npos,
             "HTML report should escape manifest captions");
      Assert(html_content.find("images/evidence/capture-1.tif") != std::string::npos,
             "HTML report should link each preview to its original evidence file");

      const auto tex_report = core::GenerateTexReport(
          core::TexReportConfig{reports_root, templates_root}, checklist, image_instance_id,
          "instance||" + image_instance_id, slugs);
      const std::string tex_content = ReadFile(tex_report.output_path);
      Assert(tex_report.image_count == 4, "LaTeX report should count copied image evidence");
      Assert(CountOccurrences(tex_content, "\\includegraphics") == 4,
             "LaTeX report should render every evidence preview");
      Assert(tex_content.find("\\detokenize{images/evidence/capture-1.png}") != std::string::npos,
             "LaTeX report should use report-local preview paths");
      RecordStep(current_step, true, "captured report images ok");
    }

    // Template 6f: manifests must not traverse outside the checklist-local evidence folder.
    {
      current_step = "captured report image path safety";
      const std::string checklist = "image-evidence-invalid";
      const std::string image_instance_id = "BADIMAGE000000001";
      const auto slugs = BuildSlugs(checklist, image_instance_id);
      const auto image_root = reports_root / image_instance_id / "images";
      std::filesystem::create_directories(image_root);
      std::ofstream manifest(image_root / "manifest.json");
      manifest << R"({"schema":"chax-report-images-v1","images":[{"preview":"../outside.png"}]})";
      manifest.close();
      bool rejected = false;
      try {
        (void)core::GenerateHtmlReport(core::HtmlReportConfig{reports_root, templates_root},
                                       checklist, image_instance_id,
                                       "instance||" + image_instance_id, slugs);
      } catch (const std::runtime_error&) {
        rejected = true;
      }
      Assert(rejected, "Report image manifests must reject traversal paths");
      RecordStep(current_step, true, "captured report image path safety ok");
    }

    // Template 7: fillable FDF output.
    {
      current_step = "fillable fdf";
      const std::string checklist = "fillable-demo";
      auto slugs = BuildSlugs(checklist, instance_id);
      const std::string address = AddressFor(slugs, "Pass One");
      const std::string generated_at = "2026-01-01T00:00:00Z";

      std::ofstream fdf_template(templates_root / (checklist + ".fdf"));
      fdf_template << "%FDF-1.2\n";
      fdf_template << "1 0 obj\n";
      fdf_template << "<< /FDF << /Fields [\n";
      fdf_template << "<< /T (slug-" << address << "-procedure) /V () >>\n";
      fdf_template << "] >> >>\n";
      fdf_template << "endobj\n";
      fdf_template << "trailer\n";
      fdf_template << "<< /Root 1 0 R >>\n";
      fdf_template << "%%EOF\n";
      fdf_template.close();

      std::ofstream pdf_template(templates_root / (checklist + ".pdf"), std::ios::binary);
      pdf_template << "%PDF-1.4\n%PDF form placeholder\n1 0 obj\n<<>>\nendobj\ntrailer\n<<>>\n%%EOF\n";
      pdf_template.close();

      core::TemplateContext context{
          {"checklist", checklist},
          {"instance_id", instance_id},
          {"generated_at", generated_at},
          {"slug-" + address + "-procedure", "Pass One"},
      };

      const auto fillable = core::GenerateFillableReport(
          core::TexReportConfig{reports_root, templates_root}, checklist, instance_id, generated_at,
          context, slugs);
      Assert(fillable.template_used, "Fillable template should be detected");
      Assert(fillable.fdf_written, "Fillable FDF should be written");
      Assert(std::filesystem::exists(fillable.fdf_output_path), "Fillable FDF should exist");
      if (fillable.pdf_copied) {
        Assert(std::filesystem::exists(fillable.pdf_copy_path), "Fillable PDF copy should exist");
      }
      const std::string fdf_content = ReadFile(fillable.fdf_output_path);
      Assert(fdf_content.find("Pass One") != std::string::npos,
             "Fillable FDF should include injected field values");
      RecordStep(current_step, true, "fillable fdf ok");
    }

    if (KeepArtifacts()) {
      std::cout << "Report generation artifacts preserved under: " << tmp_root << std::endl;
    }

    if (CleanArtifactsOnExit()) {
      std::filesystem::remove_all(tmp_root, ec);
    }
    return 0;
  } catch (const std::exception& ex) {
    if (!current_step.empty()) {
      RecordStep(current_step, false, ex.what());
    }
    std::cerr << "report_generation_test failure: " << ex.what() << std::endl;
    return 1;
  }
}
