#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

#include "nlohmann/json.hpp"
#include "platform/system.hpp"

namespace {

using nlohmann::json;

struct LaunchOptions {
  std::vector<std::string> child_args = {"start"};
  std::optional<std::string> host;
  std::optional<int> port;
  bool serve_ui = true;
  bool open_browser = true;
};

std::string BuildUiUrlFromBase(const std::string& base_url) {
  std::string base = base_url;
  if (base.empty()) {
    base = "http://127.0.0.1:8080";
  }
  while (!base.empty() && base.back() == '/') {
    base.pop_back();
  }
  return base + "/ui/";
}

bool HasRuntimeAssets(const std::filesystem::path& dir) {
  if (dir.empty()) {
    return false;
  }
  std::error_code ec;
  const auto ui_root = dir / "CHAX-CLIENT" / "web";
  if (std::filesystem::exists(ui_root, ec)) {
    return true;
  }
  ec.clear();
  const auto vui_root = dir / "CHAX-CLIENT" / "vui";
  return std::filesystem::exists(vui_root, ec);
}

std::filesystem::path ResolveRuntimeRoot(const std::filesystem::path& launcher_dir) {
  if (HasRuntimeAssets(launcher_dir)) {
    return launcher_dir;
  }
  if (launcher_dir.has_parent_path()) {
    const auto parent = launcher_dir.parent_path();
    if (HasRuntimeAssets(parent)) {
      return parent;
    }
  }
  return launcher_dir;
}

std::optional<int> ParsePort(const std::string& value) {
  try {
    const int parsed = std::stoi(value);
    if (parsed > 0 && parsed <= 65535) {
      return parsed;
    }
  } catch (...) {
  }
  return std::nullopt;
}

std::filesystem::path ResolveServerPath(const std::filesystem::path& launcher_path) {
  const auto launcher_dir = launcher_path.parent_path();
  const auto local_candidate =
      platform::WithExecutableExtension(launcher_dir / "checklist_assistant_server");
  std::error_code ec;
  if (std::filesystem::exists(local_candidate, ec)) {
    return local_candidate;
  }
  const auto path_candidate = platform::ResolveExecutableOnPath("checklist_assistant_server");
  if (!path_candidate.empty()) {
    return path_candidate;
  }
  return local_candidate;
}

void WriteLauncherError(const std::filesystem::path& launcher_path, const std::string& message) {
  std::error_code ec;
  const auto launcher_dir = launcher_path.parent_path();
  const auto working_dir = ResolveRuntimeRoot(launcher_dir);
  const auto log_dir = working_dir / ".chax" / "logs";
  std::filesystem::create_directories(log_dir, ec);
  std::ofstream output(log_dir / "checklist_assistant_launcher.err", std::ios::app);
  if (!output.is_open()) {
    return;
  }
  output << message << "\n";
}

void CleanupStaleServerState(const std::filesystem::path& runtime_root) {
  std::error_code ec;
  const auto state_file = runtime_root / ".chax" / "run" / "server.json";
  if (!std::filesystem::exists(state_file, ec)) {
    return;
  }
  std::ifstream input(state_file);
  if (!input.is_open()) {
    return;
  }
  json parsed;
  try {
    input >> parsed;
  } catch (...) {
    std::filesystem::remove(state_file, ec);
    return;
  }
  const int pid = parsed.value("pid", 0);
  if (pid <= 0 || !platform::IsProcessRunning(pid)) {
    std::filesystem::remove(state_file, ec);
  }
}

std::optional<std::string> GetRunningBaseUrl(const std::filesystem::path& runtime_root) {
  std::error_code ec;
  const auto state_file = runtime_root / ".chax" / "run" / "server.json";
  if (!std::filesystem::exists(state_file, ec)) {
    return std::nullopt;
  }
  std::ifstream input(state_file);
  if (!input.is_open()) {
    return std::nullopt;
  }
  json parsed;
  try {
    input >> parsed;
  } catch (...) {
    return std::nullopt;
  }
  const int pid = parsed.value("pid", 0);
  if (pid <= 0 || !platform::IsProcessRunning(pid)) {
    return std::nullopt;
  }
  return parsed.value("base_url", "");
}

bool ParseArgs(int argc, char** argv, LaunchOptions* opts, std::string* error) {
  if (!opts) {
    if (error) {
      *error = "Internal launcher error.";
    }
    return false;
  }
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--host") {
      if (i + 1 >= argc) {
        if (error) {
          *error = "Missing value for --host.";
        }
        return false;
      }
      const std::string value = argv[++i];
      opts->host = value;
      opts->child_args.push_back("--host");
      opts->child_args.push_back(value);
      continue;
    }
    if (arg == "--port") {
      if (i + 1 >= argc) {
        if (error) {
          *error = "Missing value for --port.";
        }
        return false;
      }
      const std::string value = argv[++i];
      const auto parsed = ParsePort(value);
      if (!parsed) {
        if (error) {
          *error = "Invalid port value.";
        }
        return false;
      }
      opts->port = *parsed;
      opts->child_args.push_back("--port");
      opts->child_args.push_back(value);
      continue;
    }
    if (arg == "--no-ui") {
      opts->serve_ui = false;
      opts->open_browser = false;
      opts->child_args.push_back(arg);
      continue;
    }
    if (arg == "--ui") {
      opts->serve_ui = true;
      opts->child_args.push_back(arg);
      continue;
    }
    if (arg == "--no-browser") {
      opts->open_browser = false;
      continue;
    }
    if (arg == "--open-browser") {
      opts->open_browser = true;
      continue;
    }
    opts->child_args.push_back(arg);
  }
  return true;
}

int RunLauncher(int argc, char** argv) {
  std::filesystem::path launcher_path = argv[0];
  std::error_code ec;
  const auto canonical = std::filesystem::weakly_canonical(launcher_path, ec);
  if (!ec) {
    launcher_path = canonical;
  }

  LaunchOptions opts;
  std::string parse_error;
  if (!ParseArgs(argc, argv, &opts, &parse_error)) {
    WriteLauncherError(launcher_path, parse_error);
    return 1;
  }

  const auto server_path = ResolveServerPath(launcher_path);
  if (!std::filesystem::exists(server_path, ec)) {
    WriteLauncherError(launcher_path,
                       "Unable to find checklist_assistant_server executable near " +
                           launcher_path.string());
    return 1;
  }

  const auto runtime_root = ResolveRuntimeRoot(server_path.parent_path());
  CleanupStaleServerState(runtime_root);
  if (const auto running_base_url = GetRunningBaseUrl(runtime_root)) {
    if (opts.serve_ui && opts.open_browser) {
      platform::OpenBrowser(BuildUiUrlFromBase(*running_base_url));
    }
    return 0;
  }
  if (opts.open_browser) {
    opts.child_args.push_back("--open-browser");
  } else {
    opts.child_args.push_back("--no-browser");
  }

  int child_pid = 0;
  std::string launch_error;
  const bool launched =
      platform::LaunchDetached(server_path, opts.child_args, runtime_root, {}, {}, &child_pid,
                               &launch_error);
  if (!launched) {
    WriteLauncherError(launcher_path, "Failed to launch server bootstrap: " + launch_error);
    return 1;
  }

  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  return RunLauncher(argc, argv);
}
