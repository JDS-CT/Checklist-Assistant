#include <iostream>
#include <string>
#include <vector>

#include "core/checklist_markdown.hpp"

namespace {

struct StepResult {
  std::string procedure;
  bool pass = false;
  std::string message;
};

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

void RecordStep(std::vector<StepResult>& steps, const std::string& procedure, bool pass,
                const std::string& message) {
  steps.push_back(StepResult{procedure, pass, message});
  std::cout << "CHAX_STEP|markdown_compat_tests|" << procedure << "|"
            << (pass ? "Pass" : "Fail") << "|" << SanitizeMessage(message) << "\n";
}

}  // namespace

int main() {
  std::vector<StepResult> steps;
  bool ok = true;

  const auto fail_step = [&](const std::string& procedure, const std::string& message) {
    RecordStep(steps, procedure, false, message);
    std::cerr << message << "\n";
    ok = false;
  };
  const auto pass_step = [&](const std::string& procedure, const std::string& message) {
    RecordStep(steps, procedure, true, message);
  };

  const std::string content = R"(---
- Checklist: <sample_intake_form>
- Version: <0.0.1>
---

# Generic Intake Form

## General Intake Section

### Organization Name
- **Action**: Record Organization Name
- **Spec**: Name of organization
- **Result**:
- **Status**:
- **Comment**:

#### Instructions
Enter the official organization name.
##### Notes
Use the legal entity name.

---

### Primary Contact
- **Action**: Record Primary Contact
- **Spec**: Primary point of contact
- **Result**:
- **Status**:
- **Comment**:

#### Instructions
Enter the name of the primary contact.
)";

  try {
    const auto parsed = core::markdown::ParseChecklistMarkdown("", content);
    if (parsed.checklist != "Generic Intake Form") {
      fail_step("checklist heading sets checklist name",
                "Unexpected checklist name: " + parsed.checklist);
    } else {
      pass_step("checklist heading sets checklist name", "checklist matched");
    }
    pass_step("front-matter mismatch is tolerated", "parse succeeded");
    if (parsed.slugs.size() != 2) {
      fail_step("parses two procedures from fixture",
                "Expected 2 slugs, got " + std::to_string(parsed.slugs.size()));
    } else {
      pass_step("parses two procedures from fixture", "slug count ok");
    }
    const auto& first = parsed.slugs.front();
    if (first.section != "General Intake Section") {
      fail_step("first procedure fields parsed", "Unexpected section: " + first.section);
    }
    if (first.procedure != "Organization Name") {
      fail_step("first procedure fields parsed", "Unexpected procedure: " + first.procedure);
    }
    if (first.action != "Record Organization Name") {
      fail_step("first procedure fields parsed", "Unexpected action: " + first.action);
    }
    if (first.spec != "Name of organization") {
      fail_step("first procedure fields parsed", "Unexpected spec: " + first.spec);
    }
    if (first.section == "General Intake Section" &&
        first.procedure == "Organization Name" && first.action == "Record Organization Name" &&
        first.spec == "Name of organization") {
      pass_step("first procedure fields parsed", "fields matched");
    }
    if (first.instructions !=
        "Enter the official organization name.\n##### Notes\nUse the legal entity name.") {
      fail_step("instructions allow nested headings",
                "Unexpected instructions: " + first.instructions);
    } else {
      pass_step("instructions allow nested headings", "instructions matched");
    }

    const auto& second = parsed.slugs[1];
    if (second.procedure != "Primary Contact") {
      fail_step("second procedure fields parsed",
                "Unexpected second procedure: " + second.procedure);
    }
    if (second.action != "Record Primary Contact") {
      fail_step("second procedure fields parsed", "Unexpected second action: " + second.action);
    }
    if (second.procedure == "Primary Contact" && second.action == "Record Primary Contact") {
      pass_step("second procedure fields parsed", "fields matched");
    }

    return ok ? 0 : 1;
  } catch (const std::exception& ex) {
    const std::string message = std::string("Parse failed: ") + ex.what();
    const std::vector<std::string> procedures = {
        "checklist heading sets checklist name",
        "front-matter mismatch is tolerated",
        "parses two procedures from fixture",
        "first procedure fields parsed",
        "instructions allow nested headings",
        "second procedure fields parsed"};
    for (const auto& proc : procedures) {
      fail_step(proc, message);
    }
    std::cerr << "markdown_compat_test failure: " << ex.what() << "\n";
    return 1;
  }
}
