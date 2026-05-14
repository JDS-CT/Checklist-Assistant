#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

#include "core/checklist_store.hpp"

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
  std::cout << "CHAX_STEP|integration_schema_test|" << procedure << "|"
            << (pass ? "Pass" : "Fail") << "|" << SanitizeMessage(message) << "\n";
}

std::string TempDbPath() {
  auto dir = std::filesystem::temp_directory_path() / "apim-schema-test.db";
  return dir.string();
}

void RemoveIfExists(const std::string& path) {
  std::error_code ec;
  std::filesystem::remove(path, ec);
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

  const auto db_path = TempDbPath();
  RemoveIfExists(db_path);

  try {
    core::ChecklistStore store(db_path);
    store.Initialize(/*seed_demo_data=*/false);

    core::ChecklistSlug slug;
    slug.checklist = "integration-checklist";
    slug.section = "Section A";
    slug.procedure = "Proc 1";
    slug.action = "Do thing";
    slug.spec = "Expected value";
    slug.result = "pending";
    slug.status = core::ChecklistStatus::kNA;
    slug.comment = "";
    slug.timestamp = core::CurrentTimestampIsoUtc();
    slug.instructions = "Steps go here";
    slug.slug_id = core::ComputeSlugId(slug.checklist, slug.section, slug.procedure, slug.action,
                                       slug.spec, slug.instructions);
    slug.instance_id = core::ComputeInstanceId("test||instance=integration");
    slug.address_id = core::ComposeAddressId(slug.slug_id, slug.instance_id);

    const bool created = store.CreateSlugIfMissing(slug);
    if (!created) {
      fail_step("create primary slug", "Slug was not created in empty test store");
    } else {
      pass_step("create primary slug", "slug created");
    }

    // Second slug to exercise filtering
    core::ChecklistSlug slug2;
    slug2.checklist = "integration-checklist";
    slug2.section = "Section B";
    slug2.procedure = "Proc 2";
    slug2.action = "Do other thing";
    slug2.spec = "Expected value 2";
    slug2.result = "pending";
    slug2.status = core::ChecklistStatus::kFail;
    slug2.comment = "fail";
    slug2.timestamp = core::CurrentTimestampIsoUtc();
    slug2.instructions = "Steps go elsewhere";
    slug2.slug_id = core::ComputeSlugId(slug2.checklist, slug2.section, slug2.procedure,
                                        slug2.action, slug2.spec, slug2.instructions);
    slug2.instance_id = slug.instance_id;
    slug2.address_id = core::ComposeAddressId(slug2.slug_id, slug2.instance_id);
    if (!store.CreateSlugIfMissing(slug2)) {
      fail_step("create second slug", "Second slug was not created");
    } else {
      pass_step("create second slug", "slug created");
    }

    const auto checklists = store.ListChecklists();
    if (checklists.empty() || checklists.front() != slug.checklist) {
      fail_step("list checklists", "Checklist list did not return expected name");
    } else {
      pass_step("list checklists", "checklist listed");
    }

    const auto slugs = store.GetSlugsForChecklist(slug.checklist);
    if (slugs.size() != 2) {
      fail_step("list slugs for checklist",
                "Expected 2 slugs, got " + std::to_string(slugs.size()));
    }
    const auto& fetched = slugs.front();
    const bool first_matches = fetched.address_id == slug.address_id;
    const bool second_matches = slugs.back().address_id == slug2.address_id;
    if (!first_matches || !second_matches) {
      fail_step("list slugs for checklist", "Fetched slugs do not match inserted slugs");
    }
    if (slugs.size() == 2 && first_matches && second_matches) {
      pass_step("list slugs for checklist", "slugs matched");
    }
    if (slugs.size() == 2) {
      if (slugs[0].address_order <= 0 || slugs[1].address_order <= 0) {
        fail_step("address order assigned", "address_order was not set on fetched slugs");
      } else {
        pass_step("address order assigned", "address_order assigned");
      }
    }

    const auto filtered =
        store.QuerySlugs(std::make_optional(slug.checklist), std::nullopt, std::nullopt,
                         std::make_optional(core::ChecklistStatus::kFail), std::nullopt,
                         std::nullopt);
    if (filtered.size() != 1 || filtered.front().address_id != slug2.address_id) {
      fail_step("filter slugs by status", "Filtered slug query did not return expected result");
    } else {
      pass_step("filter slugs by status", "filtered query matched");
    }

    // history limit should work and include at least the creation snapshot
    core::SlugUpdate update;
    update.address_id = slug.address_id;
    update.comment = std::string{"update comment"};
    store.ApplyUpdate(update);
    const auto history_limited = store.GetHistory(slug.address_id, 1);
    if (history_limited.empty()) {
      fail_step("history limit returns entries", "History limit did not return any entries");
    } else {
      pass_step("history limit returns entries", "history returned entries");
    }

    RemoveIfExists(db_path);
    return ok ? 0 : 1;
  } catch (const std::exception& ex) {
    const std::string message = std::string("integration_schema_test failure: ") + ex.what();
    const std::vector<std::string> procedures = {
        "create primary slug",
        "create second slug",
        "list checklists",
        "list slugs for checklist",
        "address order assigned",
        "filter slugs by status",
        "history limit returns entries"};
    for (const auto& proc : procedures) {
      fail_step(proc, message);
    }
    std::cerr << "integration_schema_test failure: " << ex.what() << "\n";
    RemoveIfExists(db_path);
    return 1;
  }
}
