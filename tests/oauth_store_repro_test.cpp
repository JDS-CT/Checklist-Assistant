#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include "core/logging.hpp"
#include "core/oauth.hpp"

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
  std::cout << "CHAX_STEP|oauth_store_repro_test|" << procedure << "|"
            << (pass ? "Pass" : "Fail") << "|" << SanitizeMessage(message) << "\n";
}

void RemoveIfExists(const std::filesystem::path& path) {
  std::error_code ec;
  std::filesystem::remove(path, ec);
}

}  // namespace

int main() {
  core::logging::SetLogLevel(core::logging::LogLevel::kDebug);
  std::vector<StepResult> steps;
  bool ok = true;

  const auto db_path =
      (std::filesystem::temp_directory_path() / "chax-oauth-repro.db").string();
  RemoveIfExists(db_path);

  core::OAuthStore store(db_path);
  store.Initialize();

  core::OAuthClientConfig client;
  client.client_id = "repro-client";
  client.secret_hash = core::HashSecret("repro-secret");
  client.redirect_uris = {"http://localhost/repro"};
  client.allowed_scopes = {"checklist:read"};

  std::cout << "Upserting client" << std::endl;
  store.UpsertClient(client);
  RecordStep(steps, "upsert oauth client", true, "client upserted");

  std::cout << "GetClient first call" << std::endl;
  const auto first = store.GetClient(client.client_id);
  std::cout << "First call complete: " << (first ? "found" : "missing") << std::endl;
  if (!first) {
    RecordStep(steps, "get client first", false, "client missing on first call");
    ok = false;
  } else {
    RecordStep(steps, "get client first", true, "client found");
  }

  std::cout << "GetClient second call" << std::endl;
  const auto second = store.GetClient(client.client_id);
  std::cout << "Second call complete: " << (second ? "found" : "missing") << std::endl;
  if (!second) {
    RecordStep(steps, "get client second", false, "client missing on second call");
    ok = false;
  } else {
    RecordStep(steps, "get client second", true, "client found");
  }

  return ok ? 0 : 1;
}
