#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iomanip>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "core/app.hpp"
#include "core/checklist_store.hpp"
#include "core/logging.hpp"
#include "core/oauth.hpp"
#include "nlohmann/json.hpp"
#include "platform/http_client.hpp"
#include "platform/http_server.hpp"
#include "platform/system.hpp"

namespace {

using core::logging::LogError;
using core::logging::LogInfo;
using core::logging::LogWarn;
using nlohmann::json;

enum class Command { kUsage, kHelp, kRun, kStart, kStatus, kStop };

struct RuntimePaths {
  std::filesystem::path exe_path;
  std::filesystem::path working_dir;
  std::filesystem::path runtime_dir;
  std::filesystem::path run_dir;
  std::filesystem::path log_dir;
  std::filesystem::path state_file;
};

struct RuntimeState {
  int pid = 0;
  std::string host;
  int port = 0;
  std::string base_url;
  std::string shutdown_token;
  std::string stdout_log;
  std::string stderr_log;
  int whisper_pid = 0;
  std::string whisper_stdout;
  std::string whisper_stderr;
  struct BackgroundProcessInfo {
    std::string name;
    int pid = 0;
    std::string stdout_log;
    std::string stderr_log;
  };
  std::vector<BackgroundProcessInfo> background_processes;
  std::string started_at;
};

struct CliOptions {
  Command command = Command::kUsage;
  std::optional<std::string> host;
  std::optional<int> port;
  bool serve_ui = true;
  bool open_browser = true;
  std::optional<bool> whisper_autostart;
  std::optional<std::string> whisper_host;
  std::optional<int> whisper_port;
  std::optional<std::string> whisper_model;
  std::optional<std::string> whisper_server;
  std::filesystem::path state_file;
  std::optional<std::string> shutdown_token;
  std::optional<std::filesystem::path> stdout_log;
  std::optional<std::filesystem::path> stderr_log;
  std::string error;
};

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
  if (std::filesystem::exists(vui_root, ec)) {
    return true;
  }
  return false;
}

RuntimePaths ResolvePaths(const std::filesystem::path& exe_path) {
  RuntimePaths paths;
  paths.exe_path = exe_path;
  const auto exe_dir = exe_path.parent_path();
  paths.working_dir = exe_dir;
  std::filesystem::path candidate = exe_dir;
  for (int depth = 0; depth <= 4 && !candidate.empty(); ++depth) {
    if (HasRuntimeAssets(candidate)) {
      paths.working_dir = candidate;
      break;
    }
    const auto parent = candidate.parent_path();
    if (parent == candidate) {
      break;
    }
    candidate = parent;
  }
  paths.runtime_dir = paths.working_dir / ".chax";
  paths.run_dir = paths.runtime_dir / "run";
  paths.log_dir = paths.runtime_dir / "logs";
  paths.state_file = paths.run_dir / "server.json";
  return paths;
}

std::string TimestampForFilename() {
  const auto now = std::chrono::system_clock::now();
  const std::time_t now_time = std::chrono::system_clock::to_time_t(now);
  std::tm tm_snapshot;
#if defined(_WIN32)
  localtime_s(&tm_snapshot, &now_time);
#else
  localtime_r(&now_time, &tm_snapshot);
#endif
  std::ostringstream oss;
  oss << std::put_time(&tm_snapshot, "%Y%m%d-%H%M%S");
  return oss.str();
}

std::string TimestampIso8601() {
  const auto now = std::chrono::system_clock::now();
  const std::time_t now_time = std::chrono::system_clock::to_time_t(now);
  std::tm tm_snapshot;
#if defined(_WIN32)
  localtime_s(&tm_snapshot, &now_time);
#else
  localtime_r(&now_time, &tm_snapshot);
#endif
  std::ostringstream oss;
  oss << std::put_time(&tm_snapshot, "%Y-%m-%dT%H:%M:%S");
  return oss.str();
}

void PrintUsage(const std::filesystem::path& exe_path) {
  const auto name = exe_path.filename().string();
  const auto paths = ResolvePaths(exe_path);
  std::cout << "Checklist Assistant Server\n";
  std::cout << "Usage:\n";
  std::cout << "  " << name << " --help\n";
  std::cout << "  " << name << "\n";
  std::cout << "  " << name << " run [--host <host>] [--port <port>] [--ui|--no-ui]\n";
  std::cout << "       [--open-browser|--no-browser] [--whisper|--no-whisper]\n";
  std::cout << "       [--whisper-host <host>] [--whisper-port <port>]\n";
  std::cout << "       [--whisper-server <path>] [--whisper-model <path>]\n";
  std::cout << "  " << name << " start [--host <host>] [--port <port>] [--ui|--no-ui]\n";
  std::cout << "       [--open-browser|--no-browser] [--whisper|--no-whisper]\n";
  std::cout << "       [--whisper-host <host>] [--whisper-port <port>]\n";
  std::cout << "       [--whisper-server <path>] [--whisper-model <path>]\n";
  std::cout << "  " << name << " status [--state-file <path>]\n";
  std::cout << "  " << name << " stop [--state-file <path>]\n";
  std::cout << "\n";
  std::cout << "Commands:\n";
  std::cout << "  run     Start the server in the foreground (blocks until stopped)\n";
  std::cout << "  start   Launch the server in the background and return immediately\n";
  std::cout << "  status  Report whether a managed server is running\n";
  std::cout << "  stop    Stop the running managed server\n";
  std::cout << "\n";
  std::cout << "Options:\n";
  std::cout << "  --state-file <path>       Runtime state file (default: "
            << paths.state_file.string() << ")\n";
  std::cout << "  --shutdown-token <token>  Use a fixed shutdown token for managed start\n";
  std::cout << "  --log-file <path>         Redirect server stdout for managed start\n";
  std::cout << "  --error-log-file <path>   Redirect server stderr for managed start\n";
  std::cout << "  --whisper                 Enable whisper autostart (disabled by default)\n";
  std::cout << "  --no-whisper              Disable whisper autostart\n";
  std::cout << "\n";
  std::cout << "Configuration:\n";
  std::cout << "  No config file by default; set defaults with environment variables.\n";
  std::cout << "  Core: CHAX_HOST, CHAX_PORT, CHAX_DB, CHAX_OPEN_BROWSER, CHAX_PREDICATE_CHAIN_DEPTH\n";
  std::cout << "  VUI: CHAX_SERVE_VUI, CHAX_VUI_ROOT\n";
  std::cout << "  Whisper (opt-in): CHAX_WHISPER_AUTOSTART, CHAX_WHISPER_HOST, CHAX_WHISPER_PORT\n";
  std::cout << "           CHAX_WHISPER_SERVER, CHAX_WHISPER_MODEL, CHAX_WHISPER_MODEL_FALLBACK\n";
  std::cout << "  Background: CHAX_BACKGROUND_PROCESSES, CHAX_BACKGROUND_PROCESSES_ROOT\n";
  std::cout << "  Auth (local): CHAX_LOCALHOST_NOAUTH, OAUTH_AUTHORIZE_SIMPLE, GUEST_NAME,\n";
  std::cout << "               GUEST_PROVIDER, CHAX_ENTITY_SALT\n";
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

CliOptions ParseCli(int argc, char** argv, const RuntimePaths& paths) {
  CliOptions opts;
  opts.state_file = paths.state_file;

  if (argc <= 1) {
    return opts;
  }

  std::string cmd = argv[1];
  if (cmd == "--help" || cmd == "-h") {
    opts.command = Command::kHelp;
    return opts;
  }

  if (cmd == "run") {
    opts.command = Command::kRun;
  } else if (cmd == "start") {
    opts.command = Command::kStart;
    opts.open_browser = false;
  } else if (cmd == "status") {
    opts.command = Command::kStatus;
  } else if (cmd == "stop") {
    opts.command = Command::kStop;
  } else {
    opts.error = "Unknown command: " + cmd;
    return opts;
  }

  bool browser_set = opts.command == Command::kStart;  // default off for start unless overridden

  for (int i = 2; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--host" && i + 1 < argc) {
      opts.host = argv[++i];
    } else if (arg == "--port" && i + 1 < argc) {
      const auto parsed = ParsePort(argv[++i]);
      if (!parsed) {
        opts.error = "Invalid port value.";
        return opts;
      }
      opts.port = *parsed;
    } else if (arg == "--ui") {
      opts.serve_ui = true;
    } else if (arg == "--no-ui") {
      opts.serve_ui = false;
      opts.open_browser = false;
    } else if (arg == "--no-browser") {
      opts.open_browser = false;
      browser_set = true;
    } else if (arg == "--open-browser") {
      opts.open_browser = true;
      browser_set = true;
    } else if (arg == "--whisper") {
      opts.whisper_autostart = true;
    } else if (arg == "--no-whisper") {
      opts.whisper_autostart = false;
    } else if (arg == "--whisper-host" && i + 1 < argc) {
      opts.whisper_host = argv[++i];
    } else if (arg == "--whisper-port" && i + 1 < argc) {
      const auto parsed = ParsePort(argv[++i]);
      if (!parsed) {
        opts.error = "Invalid whisper port value.";
        return opts;
      }
      opts.whisper_port = *parsed;
    } else if (arg == "--whisper-server" && i + 1 < argc) {
      opts.whisper_server = argv[++i];
    } else if (arg == "--whisper-model" && i + 1 < argc) {
      opts.whisper_model = argv[++i];
    } else if (arg == "--state-file" && i + 1 < argc) {
      opts.state_file = argv[++i];
    } else if (arg == "--shutdown-token" && i + 1 < argc) {
      opts.shutdown_token = argv[++i];
    } else if (arg == "--log-file" && i + 1 < argc) {
      opts.stdout_log = std::filesystem::path(argv[++i]);
    } else if (arg == "--error-log-file" && i + 1 < argc) {
      opts.stderr_log = std::filesystem::path(argv[++i]);
    } else {
      opts.error = "Unknown or incomplete option: " + arg;
      return opts;
    }
  }

  if (!browser_set && opts.command != Command::kStart) {
    opts.open_browser = true;
  }

  return opts;
}

json ToJson(const RuntimeState& state) {
  json j;
  j["pid"] = state.pid;
  j["host"] = state.host;
  j["port"] = state.port;
  j["base_url"] = state.base_url;
  j["shutdown_token"] = state.shutdown_token;
  j["stdout_log"] = state.stdout_log;
  j["stderr_log"] = state.stderr_log;
  if (state.whisper_pid > 0) {
    j["whisper_pid"] = state.whisper_pid;
  }
  if (!state.whisper_stdout.empty()) {
    j["whisper_stdout"] = state.whisper_stdout;
  }
  if (!state.whisper_stderr.empty()) {
    j["whisper_stderr"] = state.whisper_stderr;
  }
  if (!state.background_processes.empty()) {
    json processes = json::array();
    for (const auto& proc : state.background_processes) {
      processes.push_back({{"name", proc.name},
                           {"pid", proc.pid},
                           {"stdout_log", proc.stdout_log},
                           {"stderr_log", proc.stderr_log}});
    }
    j["background_processes"] = processes;
  }
  j["started_at"] = state.started_at;
  return j;
}

std::optional<RuntimeState> ReadState(const std::filesystem::path& path, std::string* error) {
  if (!std::filesystem::exists(path)) {
    return std::nullopt;
  }
  try {
    std::ifstream input(path);
    json parsed;
    input >> parsed;
    RuntimeState state;
    state.pid = parsed.value("pid", 0);
    state.host = parsed.value("host", "");
    state.port = parsed.value("port", 0);
    state.base_url = parsed.value("base_url", "");
    state.shutdown_token = parsed.value("shutdown_token", "");
    state.stdout_log = parsed.value("stdout_log", "");
    state.stderr_log = parsed.value("stderr_log", "");
    state.whisper_pid = parsed.value("whisper_pid", 0);
    state.whisper_stdout = parsed.value("whisper_stdout", "");
    state.whisper_stderr = parsed.value("whisper_stderr", "");
    if (const auto it = parsed.find("background_processes"); it != parsed.end() && it->is_array()) {
      for (const auto& item : *it) {
        if (!item.is_object()) {
          continue;
        }
        RuntimeState::BackgroundProcessInfo info;
        info.name = item.value("name", "");
        info.pid = item.value("pid", 0);
        info.stdout_log = item.value("stdout_log", "");
        info.stderr_log = item.value("stderr_log", "");
        if (info.pid > 0) {
          state.background_processes.push_back(std::move(info));
        }
      }
    }
    state.started_at = parsed.value("started_at", "");
    return state;
  } catch (const std::exception& ex) {
    if (error) {
      *error = ex.what();
    }
  }
  return std::nullopt;
}

class StateFileGuard {
 public:
 explicit StateFileGuard(std::filesystem::path path) : path_(std::move(path)) {}
  void Write(const RuntimeState& state) {
    const auto parent = path_.parent_path();
    if (!parent.empty()) {
      std::filesystem::create_directories(parent);
    }
    std::ofstream output(path_);
    output << ToJson(state).dump(2);
  }
  void Remove() const {
    std::error_code ec;
    std::filesystem::remove(path_, ec);
  }
  ~StateFileGuard() { Remove(); }

 private:
  std::filesystem::path path_;
};

std::string BaseUrlForState(const RuntimeState& state) {
  if (!state.base_url.empty()) {
    return state.base_url;
  }
  if (!state.host.empty() && state.port > 0) {
    return "http://" + state.host + ":" + std::to_string(state.port);
  }
  return {};
}

struct WhisperLaunch {
  int pid = 0;
  std::filesystem::path stdout_log;
  std::filesystem::path stderr_log;
};

std::filesystem::path ResolveWhisperPath(const std::filesystem::path& base_dir,
                                         const std::filesystem::path& candidate) {
  if (candidate.is_absolute()) {
    return candidate;
  }
  return base_dir / candidate;
}

std::optional<std::filesystem::path> ResolveWhisperModel(
    const core::ServerConfig& config, const std::filesystem::path& base_dir, std::string* note) {
  std::error_code ec;
  std::filesystem::path primary = ResolveWhisperPath(base_dir, config.whisper_model_path);
  if (!primary.empty() && std::filesystem::exists(primary, ec)) {
    return primary;
  }
  std::filesystem::path fallback = ResolveWhisperPath(base_dir, config.whisper_model_fallback_path);
  if (!fallback.empty() && std::filesystem::exists(fallback, ec)) {
    if (note) {
      *note = fallback.string();
    }
    return fallback;
  }
  return std::nullopt;
}

std::optional<WhisperLaunch> StartWhisperServer(const core::ServerConfig& config,
                                                const RuntimePaths& paths) {
  if (!config.whisper_autostart) {
    return std::nullopt;
  }
  if (config.whisper_port == config.port) {
    LogWarn("Whisper port matches API port; set CHAX_WHISPER_PORT to avoid conflict.");
    return std::nullopt;
  }

  std::error_code ec;
  std::filesystem::path server_path =
      ResolveWhisperPath(paths.working_dir, config.whisper_server_path);
  if (server_path.empty() || !std::filesystem::exists(server_path, ec)) {
    LogWarn("Whisper server not found at " + server_path.string());
    return std::nullopt;
  }

  std::string model_note;
  auto model_path = ResolveWhisperModel(config, paths.working_dir, &model_note);
  if (!model_path) {
    LogWarn("Whisper model not found; expected " + config.whisper_model_path +
            " (or fallback " + config.whisper_model_fallback_path + ")");
    return std::nullopt;
  }

  std::filesystem::create_directories(paths.log_dir);
  const auto timestamp = TimestampForFilename();
  const auto stdout_log = paths.log_dir / ("whisper-" + timestamp + ".log");
  const auto stderr_log = paths.log_dir / ("whisper-" + timestamp + ".err");

  std::vector<std::string> args = {"--host", config.whisper_host,
                                   "--port", std::to_string(config.whisper_port),
                                   "--model", model_path->string(),
                                   "--language", "en",
                                   "--inference-path", "/inference"};
  int pid = 0;
  std::string launch_error;
  const bool launched =
      platform::LaunchDetached(server_path, args, server_path.parent_path(),
                               stdout_log, stderr_log, &pid, &launch_error);
  if (!launched) {
    LogWarn("Failed to start whisper server: " + launch_error);
    return std::nullopt;
  }
  LogInfo("Whisper server started (PID " + std::to_string(pid) + ") at " +
          config.whisper_host + ":" + std::to_string(config.whisper_port));
  if (!model_note.empty()) {
    LogInfo("Whisper model fallback selected: " + model_note);
  }
  return WhisperLaunch{pid, stdout_log, stderr_log};
}

struct BackgroundProcessSpec {
  std::string name;
  std::filesystem::path command;
  std::filesystem::path working_dir;
  std::vector<std::string> args;
};

std::filesystem::path ResolveBackgroundRoot(const core::ServerConfig& config,
                                            const RuntimePaths& paths) {
  std::filesystem::path root = config.background_processes_root;
  if (root.is_relative()) {
    root = paths.working_dir / root;
  }
  return root;
}

std::optional<BackgroundProcessSpec> ResolveBackgroundProcessSpec(
    const std::filesystem::path& directory, const std::string& name) {
  std::error_code ec;
  const auto cmd = directory / "start.cmd";
  if (std::filesystem::exists(cmd, ec)) {
    auto runner = platform::ResolveExecutableOnPath("cmd");
    if (!runner.empty()) {
      return BackgroundProcessSpec{name, runner, directory, {"/c", cmd.string()}};
    }
    LogError("Background process " + name + " has start.cmd but no cmd runner found.");
  }

  const auto bat = directory / "start.bat";
  if (std::filesystem::exists(bat, ec)) {
    auto runner = platform::ResolveExecutableOnPath("cmd");
    if (!runner.empty()) {
      return BackgroundProcessSpec{name, runner, directory, {"/c", bat.string()}};
    }
    LogError("Background process " + name + " has start.bat but no cmd runner found.");
  }

  const auto ps1 = directory / "start.ps1";
  if (std::filesystem::exists(ps1, ec)) {
    auto runner = platform::ResolveExecutableOnPath("pwsh");
    if (runner.empty()) {
      runner = platform::ResolveExecutableOnPath("powershell");
    }
    if (!runner.empty()) {
      return BackgroundProcessSpec{name,
                                   runner,
                                   directory,
                                   {"-NoProfile", "-ExecutionPolicy", "Bypass", "-File",
                                    ps1.string()}};
    }
    LogError("Background process " + name + " has start.ps1 but no PowerShell runner found.");
  }

  const auto sh = directory / "start.sh";
  if (std::filesystem::exists(sh, ec)) {
    auto runner = platform::ResolveExecutableOnPath("sh");
    if (!runner.empty()) {
      return BackgroundProcessSpec{name, runner, directory, {sh.string()}};
    }
    LogError("Background process " + name + " has start.sh but no sh runner found.");
  }

  const auto start_exec = platform::WithExecutableExtension(directory / "start");
  if (std::filesystem::exists(start_exec, ec)) {
    return BackgroundProcessSpec{name, start_exec, directory, {}};
  }

  return std::nullopt;
}

std::vector<RuntimeState::BackgroundProcessInfo> StartBackgroundProcesses(
    const core::ServerConfig& config, const RuntimePaths& paths) {
  if (!config.background_processes_enabled) {
    LogInfo("Background processes disabled.");
    return {};
  }

  const auto root = ResolveBackgroundRoot(config, paths);
  std::error_code ec;
  if (!std::filesystem::exists(root, ec)) {
    LogInfo("Background processes folder not found at " + root.string());
    return {};
  }

  std::filesystem::create_directories(paths.log_dir);
  std::vector<RuntimeState::BackgroundProcessInfo> launched;
  for (const auto& entry : std::filesystem::directory_iterator(root, ec)) {
    if (ec) {
      LogWarn("Failed to scan background processes directory: " + root.string());
      break;
    }
    if (!entry.is_directory()) {
      continue;
    }
    const auto name = entry.path().filename().string();
    if (!name.empty() && name.front() == '.') {
      continue;
    }
    const auto spec = ResolveBackgroundProcessSpec(entry.path(), name);
    if (!spec) {
      continue;
    }
    const auto timestamp = TimestampForFilename();
    const auto stdout_log = paths.log_dir / ("background-" + name + "-" + timestamp + ".log");
    const auto stderr_log = paths.log_dir / ("background-" + name + "-" + timestamp + ".err");
    std::vector<std::pair<std::string, std::string>> env_overrides;
    if (!config.base_url.empty()) {
      env_overrides.emplace_back("CHAX_BASE_URL", config.base_url);
    }
    if (!config.admin_password_plain.empty()) {
      env_overrides.emplace_back("CHAX_ADMIN_PASSWORD", config.admin_password_plain);
    }
    int pid = 0;
    std::string launch_error;
    const bool started =
        platform::LaunchDetachedWithEnv(spec->command, spec->args, spec->working_dir, stdout_log,
                                        stderr_log, env_overrides, &pid, &launch_error);
    if (!started) {
      LogWarn("Failed to start background process " + name + ": " + launch_error);
      continue;
    }
    LogInfo("Background process started: " + name + " (PID " + std::to_string(pid) + ")");
    RuntimeState::BackgroundProcessInfo info;
    info.name = name;
    info.pid = pid;
    info.stdout_log = stdout_log.string();
    info.stderr_log = stderr_log.string();
    launched.push_back(std::move(info));
  }

  return launched;
}

bool WaitForServerReady(const std::string& base_url,
                        std::atomic<bool>& stop_flag,
                        std::chrono::seconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  platform::HttpClient client(base_url);
  while (!stop_flag.load()) {
    try {
      const auto resp = client.Get("/api/v1/health");
      if (resp.status >= 200 && resp.status < 300) {
        return true;
      }
    } catch (const std::exception&) {
    }
    if (std::chrono::steady_clock::now() >= deadline) {
      return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
  }
  return false;
}

int CommandStatus(const CliOptions& opts) {
  std::string error;
  const auto state = ReadState(opts.state_file, &error);
  if (!state) {
    std::cout << "Server not running (no state at " << opts.state_file.string() << ").\n";
    return 0;
  }

  const bool running = platform::IsProcessRunning(state->pid);
  if (!running) {
    std::cout << "Server not running (stale PID " << state->pid << ").\n";
    std::error_code ec;
    std::filesystem::remove(opts.state_file, ec);
    return 0;
  }

  const std::string url = BaseUrlForState(*state);
  std::cout << "Server running (PID " << state->pid << ")";
  if (!url.empty()) {
    std::cout << " at " << url;
  }
  std::cout << ".\n";
  if (!state->stdout_log.empty()) {
    std::cout << "  stdout: " << state->stdout_log << "\n";
  }
  if (!state->stderr_log.empty()) {
    std::cout << "  stderr: " << state->stderr_log << "\n";
  }
  if (state->whisper_pid > 0) {
    const bool whisper_running = platform::IsProcessRunning(state->whisper_pid);
    std::cout << "  whisper: PID " << state->whisper_pid
              << (whisper_running ? " (running)" : " (stale)") << "\n";
    if (!state->whisper_stdout.empty()) {
      std::cout << "    stdout: " << state->whisper_stdout << "\n";
    }
    if (!state->whisper_stderr.empty()) {
      std::cout << "    stderr: " << state->whisper_stderr << "\n";
    }
  }
  for (const auto& proc : state->background_processes) {
    const bool running_proc = platform::IsProcessRunning(proc.pid);
    std::cout << "  background: " << proc.name << " PID " << proc.pid
              << (running_proc ? " (running)" : " (stale)") << "\n";
    if (!proc.stdout_log.empty()) {
      std::cout << "    stdout: " << proc.stdout_log << "\n";
    }
    if (!proc.stderr_log.empty()) {
      std::cout << "    stderr: " << proc.stderr_log << "\n";
    }
  }
  return 0;
}

int CommandStop(const CliOptions& opts) {
  std::string error;
  const auto state = ReadState(opts.state_file, &error);
  if (!state) {
    std::cout << "Server not running (no state at " << opts.state_file.string() << ").\n";
    return 0;
  }

  if (!platform::IsProcessRunning(state->pid)) {
    std::cout << "Server not running (stale PID " << state->pid << "). Cleaning up state.\n";
    std::error_code ec;
    std::filesystem::remove(opts.state_file, ec);
    return 0;
  }

  const std::string base_url = BaseUrlForState(*state);
  bool stop_sent = false;
  bool forced = false;
  if (!base_url.empty() && !state->shutdown_token.empty()) {
    try {
      platform::HttpClient client(base_url);
      client.Post("/api/v1/admin/shutdown", "", {}, "application/json",
                  {{"X-CHAX-SHUTDOWN-TOKEN", state->shutdown_token}});
      stop_sent = true;
    } catch (const std::exception& ex) {
      std::cout << "Failed to call shutdown endpoint: " << ex.what() << "\n";
    }
  } else {
    std::cout << "Shutdown token missing; skipping HTTP shutdown and attempting PID stop.\n";
  }

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (platform::IsProcessRunning(state->pid) &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }

  if (platform::IsProcessRunning(state->pid)) {
    std::string kill_error;
    platform::TerminateProcess(state->pid, false, &kill_error);
    const auto kill_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (platform::IsProcessRunning(state->pid) &&
           std::chrono::steady_clock::now() < kill_deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (platform::IsProcessRunning(state->pid) &&
        !platform::TerminateProcess(state->pid, true, &kill_error)) {
      std::cout << "Failed to terminate PID " << state->pid << ": " << kill_error << "\n";
      return 1;
    }
    forced = true;
  }

  std::error_code ec;
  std::filesystem::remove(opts.state_file, ec);
  if (forced && stop_sent) {
    std::cout << "Server process " << state->pid << " terminated after shutdown request.\n";
  } else if (forced) {
    std::cout << "Server process " << state->pid << " terminated.\n";
  } else if (stop_sent) {
    std::cout << "Server stopped via /api/v1/admin/shutdown.\n";
  } else {
    std::cout << "Server stopped.\n";
  }

  if (state->whisper_pid > 0 && platform::IsProcessRunning(state->whisper_pid)) {
    std::string whisper_error;
    if (platform::TerminateProcess(state->whisper_pid, true, &whisper_error)) {
      std::cout << "Whisper server process " << state->whisper_pid << " terminated.\n";
    } else {
      std::cout << "Failed to terminate whisper PID " << state->whisper_pid << ": "
                << whisper_error << "\n";
    }
  }
  for (const auto& proc : state->background_processes) {
    if (proc.pid <= 0 || !platform::IsProcessRunning(proc.pid)) {
      continue;
    }
    std::string proc_error;
    if (platform::TerminateProcess(proc.pid, true, &proc_error)) {
      std::cout << "Background process " << proc.name << " PID " << proc.pid << " terminated.\n";
    } else {
      std::cout << "Failed to terminate background process " << proc.name << " PID " << proc.pid
                << ": " << proc_error << "\n";
    }
  }
  return 0;
}

int CommandStart(const CliOptions& opts, const RuntimePaths& paths) {
  std::filesystem::create_directories(paths.run_dir);
  std::filesystem::create_directories(paths.log_dir);

  std::string error;
  const auto existing_state = ReadState(opts.state_file, &error);
  if (existing_state && platform::IsProcessRunning(existing_state->pid)) {
    std::cout << "Server already running (PID " << existing_state->pid << "). Stop it first.\n";
    return 1;
  }
  if (existing_state) {
    std::error_code ec;
    std::filesystem::remove(opts.state_file, ec);
  }

  const auto timestamp = TimestampForFilename();
  const auto stdout_log =
      opts.stdout_log.value_or(paths.log_dir / ("server-" + timestamp + ".log"));
  const auto stderr_log =
      opts.stderr_log.value_or(paths.log_dir / ("server-" + timestamp + ".err"));
  const std::string shutdown_token =
      opts.shutdown_token.value_or(core::GenerateTokenString(48));

  std::vector<std::string> child_args = {"run"};
  if (opts.host) {
    child_args.push_back("--host");
    child_args.push_back(*opts.host);
  }
  if (opts.port) {
    child_args.push_back("--port");
    child_args.push_back(std::to_string(*opts.port));
  }
  if (!opts.serve_ui) {
    child_args.push_back("--no-ui");
  }
  if (!opts.open_browser) {
    child_args.push_back("--no-browser");
  }
  if (opts.whisper_autostart.has_value()) {
    child_args.push_back(*opts.whisper_autostart ? "--whisper" : "--no-whisper");
  }
  if (opts.whisper_host) {
    child_args.push_back("--whisper-host");
    child_args.push_back(*opts.whisper_host);
  }
  if (opts.whisper_port) {
    child_args.push_back("--whisper-port");
    child_args.push_back(std::to_string(*opts.whisper_port));
  }
  if (opts.whisper_server) {
    child_args.push_back("--whisper-server");
    child_args.push_back(*opts.whisper_server);
  }
  if (opts.whisper_model) {
    child_args.push_back("--whisper-model");
    child_args.push_back(*opts.whisper_model);
  }
  child_args.push_back("--state-file");
  child_args.push_back(opts.state_file.string());
  child_args.push_back("--shutdown-token");
  child_args.push_back(shutdown_token);
  child_args.push_back("--log-file");
  child_args.push_back(stdout_log.string());
  child_args.push_back("--error-log-file");
  child_args.push_back(stderr_log.string());

  int pid = 0;
  std::string launch_error;
  const bool launched = platform::LaunchDetached(paths.exe_path, child_args, paths.working_dir,
                                                 stdout_log, stderr_log, &pid, &launch_error);
  if (!launched) {
    std::cout << "Failed to start server: " << launch_error << "\n";
    return 1;
  }

  std::cout << "Server started (PID " << pid << "). Logs at " << stdout_log.string() << "\n";
  return 0;
}

core::ServerConfig ApplyCliOverrides(core::ServerConfig config, const CliOptions& opts) {
  if (opts.host) {
    config.host = *opts.host;
  }
  if (opts.port) {
    config.port = *opts.port;
  }
  config.serve_ui = opts.serve_ui;
  if (!config.serve_ui) {
    config.open_browser = false;
  } else {
    config.open_browser = opts.open_browser;
  }
  if (opts.shutdown_token) {
    config.shutdown_token = *opts.shutdown_token;
  }
  if (config.shutdown_token.empty()) {
    config.shutdown_token = core::GenerateTokenString(48);
  }
  if (opts.whisper_autostart.has_value()) {
    config.whisper_autostart = *opts.whisper_autostart;
  }
  if (opts.whisper_host) {
    config.whisper_host = *opts.whisper_host;
  }
  if (opts.whisper_port) {
    config.whisper_port = *opts.whisper_port;
  }
  if (opts.whisper_model) {
    config.whisper_model_path = *opts.whisper_model;
  }
  if (opts.whisper_server) {
    config.whisper_server_path = *opts.whisper_server;
  }

  config.base_url = "http://" + config.host + ":" + std::to_string(config.port);
  const std::string default_redirect = config.base_url + "/oauth/callback";
  const std::string ui_redirect = config.base_url + "/ui/oauth_callback.html";
  auto ensure_uri = [&](const std::string& uri) {
    if (std::find(config.oauth_redirect_uris.begin(), config.oauth_redirect_uris.end(), uri) ==
        config.oauth_redirect_uris.end()) {
      config.oauth_redirect_uris.push_back(uri);
    }
  };
  ensure_uri(default_redirect);
  if (config.serve_ui) {
    ensure_uri(ui_redirect);
  }

  return config;
}

bool IsLoopbackHost(const std::string& host) {
  if (host == "127.0.0.1" || host == "::1" || host == "localhost") {
    return true;
  }
  if (host.rfind("127.", 0) == 0) {
    return true;
  }
  if (host.rfind("::ffff:127.", 0) == 0) {
    return true;
  }
  return false;
}

int CommandRun(const CliOptions& opts, const RuntimePaths& paths) {
  std::error_code ec;
  std::filesystem::current_path(paths.working_dir, ec);
  if (ec) {
    std::cout << "Warning: failed to switch working directory to " << paths.working_dir.string()
              << ": " << ec.message() << "\n";
  }
  core::logging::InitializeFromEnvironment();
  auto config = ApplyCliOverrides(core::LoadServerConfig(), opts);
  if (IsLoopbackHost(config.host)) {
    if (!config.localhost_noauth_overridden) {
      config.localhost_noauth = true;
    }
    if (!config.simple_authorize_overridden) {
      config.simple_authorize = true;
    }
  }
  if (config.serve_ui && config.oauth_client_generated) {
    config.oauth_client_id = "chax-ui-client";
    config.oauth_client_secret = "chax-ui-secret";
    config.oauth_client_generated = false;
    LogInfo("UI mode: using default OAuth client chax-ui-client / chax-ui-secret");
  }

  core::logging::LogInfo("Opening runtime store at " + config.database_path +
                         (config.seed_demo_data ? " (seed enabled)" : " (seed disabled)"));
  core::ChecklistStore store(config.database_path);
  store.Initialize(config.seed_demo_data);
  store.SetPredicateChainDepth(config.predicate_chain_depth);

  core::OAuthStore oauth_store(config.database_path);
  oauth_store.Initialize();

  platform::HttpServer server;
  core::ConfigureServer(server, store, oauth_store, config);

  auto whisper = StartWhisperServer(config, paths);
  std::vector<RuntimeState::BackgroundProcessInfo> background_processes;
  std::mutex state_mutex;
  std::atomic<bool> background_stop{false};

  RuntimeState state;
  state.pid = platform::CurrentProcessId();
  state.host = config.host;
  state.port = config.port;
  state.base_url = config.base_url;
  state.shutdown_token = config.shutdown_token;
  state.started_at = TimestampIso8601();
  if (opts.stdout_log) {
    state.stdout_log = opts.stdout_log->string();
  }
  if (opts.stderr_log) {
    state.stderr_log = opts.stderr_log->string();
  }
  if (whisper) {
    state.whisper_pid = whisper->pid;
    state.whisper_stdout = whisper->stdout_log.string();
    state.whisper_stderr = whisper->stderr_log.string();
  }
  StateFileGuard state_guard(opts.state_file);
  state_guard.Write(state);

  core::logging::LogInfo("Starting Checklist Assistant server on " + config.host + ":" +
                         std::to_string(config.port));

  std::thread background_thread;
  if (config.background_processes_enabled) {
    background_thread = std::thread([&]() {
      const bool ready = WaitForServerReady(config.base_url, background_stop,
                                            std::chrono::seconds(30));
      if (!ready) {
        LogWarn("Background processes not started; server readiness timed out.");
        return;
      }
      auto launched = StartBackgroundProcesses(config, paths);
      if (launched.empty()) {
        return;
      }
      {
        std::lock_guard<std::mutex> guard(state_mutex);
        background_processes = launched;
        RuntimeState updated = state;
        updated.background_processes = launched;
        state_guard.Write(updated);
      }
    });
  }

  if (config.serve_ui && config.open_browser) {
    const std::string ui_url = config.base_url + "/ui/";
    core::logging::LogInfo("UI available at " + ui_url);
    platform::OpenBrowser(ui_url);
  } else if (config.serve_ui) {
    const std::string ui_url = config.base_url + "/ui/";
    core::logging::LogInfo("UI available at " + ui_url + " (browser launch disabled)");
  }
  if (config.serve_vui) {
    std::error_code ec;
    if (std::filesystem::exists(config.vui_root, ec)) {
      const std::string vui_url = config.base_url + "/vui/";
      core::logging::LogInfo("VUI available at " + vui_url);
    }
  }

  try {
    server.Start(config.host, config.port);
  } catch (const std::exception& ex) {
    LogError(std::string{"Server terminated with error: "} + ex.what());
    background_stop = true;
    if (background_thread.joinable()) {
      background_thread.join();
    }
    return 1;
  }

  background_stop = true;
  if (background_thread.joinable()) {
    background_thread.join();
  }

  if (whisper && platform::IsProcessRunning(whisper->pid)) {
    std::string whisper_error;
    if (!platform::TerminateProcess(whisper->pid, true, &whisper_error)) {
      LogWarn("Failed to terminate whisper PID " + std::to_string(whisper->pid) + ": " +
              whisper_error);
    }
  }
  {
    std::lock_guard<std::mutex> guard(state_mutex);
    for (const auto& proc : background_processes) {
      if (!platform::IsProcessRunning(proc.pid)) {
        continue;
      }
      std::string proc_error;
      if (!platform::TerminateProcess(proc.pid, true, &proc_error)) {
        LogWarn("Failed to terminate background process " + proc.name + " PID " +
                std::to_string(proc.pid) + ": " + proc_error);
      }
    }
  }
  LogInfo("Server shut down gracefully.");
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  std::filesystem::path exe_path = argv[0];
  std::error_code ec;
  const auto canonical = std::filesystem::weakly_canonical(exe_path, ec);
  if (!ec) {
    exe_path = canonical;
  }
  const auto paths = ResolvePaths(exe_path);
  const auto cli = ParseCli(argc, argv, paths);
  if (!cli.error.empty()) {
    std::cout << cli.error << "\n";
    PrintUsage(exe_path);
    return 1;
  }

  if (cli.command == Command::kHelp || cli.command == Command::kUsage) {
    PrintUsage(exe_path);
    return 0;
  }

  switch (cli.command) {
    case Command::kRun:
      return CommandRun(cli, paths);
    case Command::kStart:
      return CommandStart(cli, paths);
    case Command::kStatus:
      return CommandStatus(cli);
    case Command::kStop:
      return CommandStop(cli);
    default:
      PrintUsage(exe_path);
      return 0;
  }
}
