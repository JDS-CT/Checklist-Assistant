#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

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
  std::cout << "CHAX_STEP|mcp_voice_helper_test|" << procedure << "|"
            << (pass ? "Pass" : "Fail") << "|" << SanitizeMessage(message) << "\n";
}

std::string ReadFile(const std::filesystem::path& path) {
  std::ifstream stream(path, std::ios::binary);
  Assert(stream.good(), "Missing file: " + path.string());
  std::string content((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
  return content;
}

}  // namespace

int main() {
  std::string current_step;
  try {
    const auto root = std::filesystem::path(__FILE__).parent_path().parent_path();
    const auto prompts_dir = root / "CHAX-CLIENT" / "mcp-voice-assistant" / "prompts";

    current_step = "onboarding prompt";
    const auto onboarding = ReadFile(prompts_dir / "onboarding.md");
    Assert(onboarding.find("chax.update_slug") != std::string::npos,
           "Onboarding prompt must reference update flow.");
    Assert(onboarding.find("chax.evaluate_graph") != std::string::npos,
           "Onboarding prompt must mention graph evaluation.");
    Assert(onboarding.find("address_id") != std::string::npos,
           "Onboarding prompt must mention address_id handling.");
    RecordStep(current_step, true, "onboarding ok");

    current_step = "tool calls prompt";
    const auto tool_calls = ReadFile(prompts_dir / "tool_calls.md");
    Assert(tool_calls.find("chax.list_slugs") != std::string::npos,
           "Tool call guide must include list_slugs.");
    Assert(tool_calls.find("chax.relationships") != std::string::npos,
           "Tool call guide must include relationships navigation.");
    Assert(tool_calls.find("chax.create_entity") != std::string::npos,
           "Tool call guide must include catalog helpers.");
    RecordStep(current_step, true, "tool calls ok");

    current_step = "utterances prompt";
    const auto utterances = ReadFile(prompts_dir / "utterances.md");
    Assert(!utterances.empty(), "Utterance cheatsheet must not be empty.");
    Assert(utterances.find("update_slug") != std::string::npos,
           "Utterance cheatsheet must mention update guidance.");
    RecordStep(current_step, true, "utterances ok");

    current_step = "voice helper readme";
    const auto readme = ReadFile(root / "CHAX-CLIENT" / "mcp-voice-assistant" / "README.md");
    Assert(readme.find("Voice") != std::string::npos,
           "Voice helper README must mention voice usage.");
    Assert(readme.find("MCP") != std::string::npos,
           "Voice helper README must mention MCP wiring.");
    RecordStep(current_step, true, "readme ok");

    return 0;
  } catch (const std::exception& ex) {
    if (!current_step.empty()) {
      RecordStep(current_step, false, ex.what());
    }
    std::cerr << "mcp_voice_helper_test failure: " << ex.what() << "\n";
    return 1;
  }
}
