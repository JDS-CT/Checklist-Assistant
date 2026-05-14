#include "platform/system.hpp"

#include <cstdlib>
#include <cwctype>
#include <map>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <Windows.h>
#include <shellapi.h>
#else
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {

#if defined(_WIN32)

std::wstring Widen(const std::string& input) {
  return std::wstring(input.begin(), input.end());
}

std::wstring QuoteForWindows(const std::string& input) {
  std::wstring widened = Widen(input);
  if (widened.find_first_of(L" \t\"") == std::wstring::npos) {
    return widened;
  }
  std::wstring quoted;
  quoted.push_back(L'"');
  for (wchar_t ch : widened) {
    if (ch == L'"') {
      quoted.push_back(L'\\');
    }
    quoted.push_back(ch);
  }
  quoted.push_back(L'"');
  return quoted;
}

std::wstring BuildCommandLine(const std::filesystem::path& executable,
                              const std::vector<std::string>& args) {
  std::wstring command = QuoteForWindows(executable.string());
  for (const auto& arg : args) {
    command.push_back(L' ');
    command.append(QuoteForWindows(arg));
  }
  return command;
}

std::string DescribeWindowsError(DWORD code) {
  LPWSTR buffer = nullptr;
  const DWORD length = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                                          FORMAT_MESSAGE_IGNORE_INSERTS,
                                      nullptr, code, 0, reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);
  if (length == 0 || buffer == nullptr) {
    return "Unknown Windows error";
  }
  std::wstring message(buffer, length);
  LocalFree(buffer);
  return std::string(message.begin(), message.end());
}

HANDLE OpenLogHandle(const std::filesystem::path& path, std::string* error) {
  SECURITY_ATTRIBUTES attrs{};
  attrs.nLength = sizeof(attrs);
  attrs.lpSecurityDescriptor = nullptr;
  attrs.bInheritHandle = TRUE;

  const auto handle = CreateFileW(path.wstring().c_str(), FILE_APPEND_DATA,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE, &attrs, OPEN_ALWAYS,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    if (error) {
      *error = "Failed to open log file " + path.string() + ": " +
               DescribeWindowsError(GetLastError());
    }
    return INVALID_HANDLE_VALUE;
  }
  return handle;
}

HANDLE OpenNullInputHandle(std::string* error) {
  SECURITY_ATTRIBUTES attrs{};
  attrs.nLength = sizeof(attrs);
  attrs.lpSecurityDescriptor = nullptr;
  attrs.bInheritHandle = TRUE;

  const auto handle =
      CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &attrs,
                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    if (error) {
      *error = "Failed to open NUL stdin handle: " + DescribeWindowsError(GetLastError());
    }
    return INVALID_HANDLE_VALUE;
  }
  return handle;
}

std::wstring ToLowerW(std::wstring value) {
  for (auto& ch : value) {
    ch = static_cast<wchar_t>(towlower(ch));
  }
  return value;
}

std::vector<wchar_t> BuildEnvironmentBlock(
    const std::vector<std::pair<std::string, std::string>>& overrides,
    std::string* error) {
  if (overrides.empty()) {
    return {};
  }
  LPWCH env = GetEnvironmentStringsW();
  if (!env) {
    if (error) {
      *error = "Failed to read process environment.";
    }
    return {};
  }

  std::map<std::wstring, std::wstring> env_map;
  std::vector<std::wstring> raw_entries;
  for (LPWCH current = env; *current; ) {
    std::wstring entry(current);
    current += entry.size() + 1;
    const auto pos = entry.find(L'=');
    if (pos == std::wstring::npos) {
      continue;
    }
    if (pos == 0) {
      raw_entries.push_back(entry);
      continue;
    }
    const std::wstring key = entry.substr(0, pos);
    env_map[ToLowerW(key)] = entry;
  }
  FreeEnvironmentStringsW(env);

  for (const auto& pair : overrides) {
    if (pair.first.empty()) {
      continue;
    }
    const std::wstring key = Widen(pair.first);
    const std::wstring entry = key + L"=" + Widen(pair.second);
    env_map[ToLowerW(key)] = entry;
  }

  std::vector<wchar_t> block;
  for (const auto& entry : raw_entries) {
    block.insert(block.end(), entry.begin(), entry.end());
    block.push_back(L'\0');
  }
  for (const auto& kv : env_map) {
    const auto& entry = kv.second;
    block.insert(block.end(), entry.begin(), entry.end());
    block.push_back(L'\0');
  }
  block.push_back(L'\0');
  return block;
}

#else

int OpenLogFile(const std::filesystem::path& path, std::string* error) {
  const int fd = ::open(path.string().c_str(), O_CREAT | O_WRONLY | O_APPEND, 0644);
  if (fd < 0 && error) {
    *error = "Failed to open log file " + path.string();
  }
  return fd;
}

#endif

}  // namespace

namespace platform {

bool LaunchDetached(const std::filesystem::path& executable,
                    const std::vector<std::string>& args,
                    const std::filesystem::path& working_directory,
                    const std::filesystem::path& stdout_path,
                    const std::filesystem::path& stderr_path,
                    int* pid_out,
                    std::string* error) {
#if defined(_WIN32)
  HANDLE stdin_handle = INVALID_HANDLE_VALUE;
  HANDLE stdout_handle = INVALID_HANDLE_VALUE;
  HANDLE stderr_handle = INVALID_HANDLE_VALUE;

  stdin_handle = OpenNullInputHandle(error);
  if (stdin_handle == INVALID_HANDLE_VALUE) {
    return false;
  }
  if (!stdout_path.empty()) {
    stdout_handle = OpenLogHandle(stdout_path, error);
    if (stdout_handle == INVALID_HANDLE_VALUE) {
      CloseHandle(stdin_handle);
      return false;
    }
  }
  if (!stderr_path.empty()) {
    stderr_handle = OpenLogHandle(stderr_path, error);
    if (stderr_handle == INVALID_HANDLE_VALUE) {
      CloseHandle(stdin_handle);
      if (stdout_handle != INVALID_HANDLE_VALUE) {
        CloseHandle(stdout_handle);
      }
      return false;
    }
  }

  const std::wstring command_line = BuildCommandLine(executable, args);
  STARTUPINFOW startup{};
  PROCESS_INFORMATION proc{};
  startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESTDHANDLES;
  startup.hStdInput = stdin_handle;
  startup.hStdOutput =
      stdout_handle == INVALID_HANDLE_VALUE ? GetStdHandle(STD_OUTPUT_HANDLE) : stdout_handle;
  startup.hStdError =
      stderr_handle == INVALID_HANDLE_VALUE ? GetStdHandle(STD_ERROR_HANDLE) : stderr_handle;

  SetHandleInformation(stdin_handle, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
  if (stdout_handle != INVALID_HANDLE_VALUE) {
    SetHandleInformation(stdout_handle, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
  }
  if (stderr_handle != INVALID_HANDLE_VALUE) {
    SetHandleInformation(stderr_handle, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
  }

  std::wstring mutable_command = command_line;
  const DWORD flags = CREATE_NEW_PROCESS_GROUP | CREATE_NO_WINDOW;
  const BOOL ok = CreateProcessW(
      nullptr, mutable_command.empty() ? nullptr : mutable_command.data(), nullptr, nullptr, TRUE,
      flags, nullptr,
      working_directory.empty() ? nullptr : working_directory.wstring().c_str(), &startup, &proc);

  CloseHandle(stdin_handle);
  if (stdout_handle != INVALID_HANDLE_VALUE) {
    CloseHandle(stdout_handle);
  }
  if (stderr_handle != INVALID_HANDLE_VALUE) {
    CloseHandle(stderr_handle);
  }

  if (!ok) {
    if (error) {
      *error = "CreateProcess failed: " + DescribeWindowsError(GetLastError());
    }
    return false;
  }

  if (pid_out) {
    *pid_out = static_cast<int>(proc.dwProcessId);
  }

  CloseHandle(proc.hProcess);
  CloseHandle(proc.hThread);
  return true;
#else
  const pid_t pid = ::fork();
  if (pid < 0) {
    if (error) {
      *error = "fork() failed";
    }
    return false;
  }
  if (pid == 0) {
    if (!working_directory.empty()) {
      ::chdir(working_directory.string().c_str());
    }
    ::setsid();

    if (!stdout_path.empty()) {
      const int fd = OpenLogFile(stdout_path, nullptr);
      if (fd >= 0) {
        ::dup2(fd, STDOUT_FILENO);
        ::close(fd);
      }
    }
    if (!stderr_path.empty()) {
      const int fd = OpenLogFile(stderr_path, nullptr);
      if (fd >= 0) {
        ::dup2(fd, STDERR_FILENO);
        ::close(fd);
      }
    }

    std::vector<std::string> argv_storage;
    argv_storage.reserve(args.size() + 1);
    argv_storage.push_back(executable.string());
    for (const auto& arg : args) {
      argv_storage.push_back(arg);
    }

    std::vector<char*> argv;
    argv.reserve(argv_storage.size() + 1);
    for (auto& entry : argv_storage) {
      argv.push_back(entry.data());
    }
    argv.push_back(nullptr);
    ::execv(executable.string().c_str(), argv.data());
    _exit(127);
  }

  if (pid_out) {
    *pid_out = static_cast<int>(pid);
  }
  return true;
#endif
}

bool LaunchDetachedWithEnv(const std::filesystem::path& executable,
                           const std::vector<std::string>& args,
                           const std::filesystem::path& working_directory,
                           const std::filesystem::path& stdout_path,
                           const std::filesystem::path& stderr_path,
                           const std::vector<std::pair<std::string, std::string>>& env_overrides,
                           int* pid_out,
                           std::string* error) {
#if defined(_WIN32)
  HANDLE stdin_handle = INVALID_HANDLE_VALUE;
  HANDLE stdout_handle = INVALID_HANDLE_VALUE;
  HANDLE stderr_handle = INVALID_HANDLE_VALUE;

  stdin_handle = OpenNullInputHandle(error);
  if (stdin_handle == INVALID_HANDLE_VALUE) {
    return false;
  }
  if (!stdout_path.empty()) {
    stdout_handle = OpenLogHandle(stdout_path, error);
    if (stdout_handle == INVALID_HANDLE_VALUE) {
      CloseHandle(stdin_handle);
      return false;
    }
  }
  if (!stderr_path.empty()) {
    stderr_handle = OpenLogHandle(stderr_path, error);
    if (stderr_handle == INVALID_HANDLE_VALUE) {
      CloseHandle(stdin_handle);
      if (stdout_handle != INVALID_HANDLE_VALUE) {
        CloseHandle(stdout_handle);
      }
      return false;
    }
  }

  const std::wstring command_line = BuildCommandLine(executable, args);
  STARTUPINFOW startup{};
  PROCESS_INFORMATION proc{};
  startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESTDHANDLES;
  startup.hStdInput = stdin_handle;
  startup.hStdOutput =
      stdout_handle == INVALID_HANDLE_VALUE ? GetStdHandle(STD_OUTPUT_HANDLE) : stdout_handle;
  startup.hStdError =
      stderr_handle == INVALID_HANDLE_VALUE ? GetStdHandle(STD_ERROR_HANDLE) : stderr_handle;

  SetHandleInformation(stdin_handle, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
  if (stdout_handle != INVALID_HANDLE_VALUE) {
    SetHandleInformation(stdout_handle, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
  }
  if (stderr_handle != INVALID_HANDLE_VALUE) {
    SetHandleInformation(stderr_handle, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
  }

  std::wstring mutable_command = command_line;
  DWORD flags = CREATE_NEW_PROCESS_GROUP | CREATE_NO_WINDOW;
  std::vector<wchar_t> env_block = BuildEnvironmentBlock(env_overrides, error);
  LPVOID env_ptr = env_block.empty() ? nullptr : env_block.data();
  if (env_ptr != nullptr) {
    flags |= CREATE_UNICODE_ENVIRONMENT;
  }
  const BOOL ok = CreateProcessW(
      nullptr, mutable_command.empty() ? nullptr : mutable_command.data(), nullptr, nullptr, TRUE,
      flags, env_ptr,
      working_directory.empty() ? nullptr : working_directory.wstring().c_str(), &startup, &proc);

  CloseHandle(stdin_handle);
  if (stdout_handle != INVALID_HANDLE_VALUE) {
    CloseHandle(stdout_handle);
  }
  if (stderr_handle != INVALID_HANDLE_VALUE) {
    CloseHandle(stderr_handle);
  }

  if (!ok) {
    if (error) {
      *error = "CreateProcess failed: " + DescribeWindowsError(GetLastError());
    }
    return false;
  }

  if (pid_out) {
    *pid_out = static_cast<int>(proc.dwProcessId);
  }

  CloseHandle(proc.hProcess);
  CloseHandle(proc.hThread);
  return true;
#else
  if (env_overrides.empty()) {
    return LaunchDetached(executable, args, working_directory, stdout_path, stderr_path, pid_out,
                          error);
  }

  const pid_t pid = ::fork();
  if (pid < 0) {
    if (error) {
      *error = "fork() failed";
    }
    return false;
  }
  if (pid == 0) {
    if (!working_directory.empty()) {
      ::chdir(working_directory.string().c_str());
    }
    ::setsid();

    if (!stdout_path.empty()) {
      const int fd = OpenLogFile(stdout_path, nullptr);
      if (fd >= 0) {
        ::dup2(fd, STDOUT_FILENO);
        ::close(fd);
      }
    }
    if (!stderr_path.empty()) {
      const int fd = OpenLogFile(stderr_path, nullptr);
      if (fd >= 0) {
        ::dup2(fd, STDERR_FILENO);
        ::close(fd);
      }
    }

    for (const auto& pair : env_overrides) {
      if (pair.first.empty()) {
        continue;
      }
      ::setenv(pair.first.c_str(), pair.second.c_str(), 1);
    }

    std::vector<std::string> argv_storage;
    argv_storage.reserve(args.size() + 1);
    argv_storage.push_back(executable.string());
    for (const auto& arg : args) {
      argv_storage.push_back(arg);
    }

    std::vector<char*> argv;
    argv.reserve(argv_storage.size() + 1);
    for (auto& entry : argv_storage) {
      argv.push_back(entry.data());
    }
    argv.push_back(nullptr);
    ::execv(executable.string().c_str(), argv.data());
    _exit(127);
  }

  if (pid_out) {
    *pid_out = static_cast<int>(pid);
  }
  return true;
#endif
}

bool RunProcess(const std::filesystem::path &executable, const std::vector<std::string> &args,
                const std::filesystem::path &working_directory, const std::filesystem::path &stdout_path,
                const std::filesystem::path &stderr_path, int *exit_code_out, std::string *error) {
  if (exit_code_out) {
    *exit_code_out = -1;
  }
#if defined(_WIN32)
  HANDLE stdin_handle = INVALID_HANDLE_VALUE;
  HANDLE stdout_handle = INVALID_HANDLE_VALUE;
  HANDLE stderr_handle = INVALID_HANDLE_VALUE;

  stdin_handle = OpenNullInputHandle(error);
  if (stdin_handle == INVALID_HANDLE_VALUE) {
    return false;
  }
  if (!stdout_path.empty()) {
    stdout_handle = OpenLogHandle(stdout_path, error);
    if (stdout_handle == INVALID_HANDLE_VALUE) {
      CloseHandle(stdin_handle);
      return false;
    }
  }
  if (!stderr_path.empty()) {
    stderr_handle = OpenLogHandle(stderr_path, error);
    if (stderr_handle == INVALID_HANDLE_VALUE) {
      CloseHandle(stdin_handle);
      if (stdout_handle != INVALID_HANDLE_VALUE) {
        CloseHandle(stdout_handle);
      }
      return false;
    }
  }

  const std::wstring command_line = BuildCommandLine(executable, args);
  STARTUPINFOW startup{};
  PROCESS_INFORMATION proc{};
  startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESTDHANDLES;
  startup.hStdInput = stdin_handle;
  startup.hStdOutput = stdout_handle == INVALID_HANDLE_VALUE ? GetStdHandle(STD_OUTPUT_HANDLE) : stdout_handle;
  startup.hStdError = stderr_handle == INVALID_HANDLE_VALUE ? GetStdHandle(STD_ERROR_HANDLE) : stderr_handle;

  SetHandleInformation(stdin_handle, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
  if (stdout_handle != INVALID_HANDLE_VALUE) {
    SetHandleInformation(stdout_handle, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
  }
  if (stderr_handle != INVALID_HANDLE_VALUE) {
    SetHandleInformation(stderr_handle, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
  }

  std::wstring mutable_command = command_line;
  const DWORD flags = CREATE_NO_WINDOW;
  const BOOL ok = CreateProcessW(
      nullptr, mutable_command.empty() ? nullptr : mutable_command.data(), nullptr, nullptr, TRUE, flags, nullptr,
      working_directory.empty() ? nullptr : working_directory.wstring().c_str(), &startup, &proc);

  CloseHandle(stdin_handle);
  if (stdout_handle != INVALID_HANDLE_VALUE) {
    CloseHandle(stdout_handle);
  }
  if (stderr_handle != INVALID_HANDLE_VALUE) {
    CloseHandle(stderr_handle);
  }

  if (!ok) {
    if (error) {
      *error = "CreateProcess failed: " + DescribeWindowsError(GetLastError());
    }
    return false;
  }

  WaitForSingleObject(proc.hProcess, INFINITE);
  DWORD exit_code = 1;
  const BOOL got_exit_code = GetExitCodeProcess(proc.hProcess, &exit_code);
  CloseHandle(proc.hProcess);
  CloseHandle(proc.hThread);
  if (!got_exit_code) {
    if (error) {
      *error = "GetExitCodeProcess failed: " + DescribeWindowsError(GetLastError());
    }
    return false;
  }
  if (exit_code_out) {
    *exit_code_out = static_cast<int>(exit_code);
  }
  return true;
#else
  const pid_t pid = ::fork();
  if (pid < 0) {
    if (error) {
      *error = "fork() failed";
    }
    return false;
  }
  if (pid == 0) {
    if (!working_directory.empty()) {
      ::chdir(working_directory.string().c_str());
    }
    if (!stdout_path.empty()) {
      const int fd = OpenLogFile(stdout_path, nullptr);
      if (fd >= 0) {
        ::dup2(fd, STDOUT_FILENO);
        ::close(fd);
      }
    }
    if (!stderr_path.empty()) {
      const int fd = OpenLogFile(stderr_path, nullptr);
      if (fd >= 0) {
        ::dup2(fd, STDERR_FILENO);
        ::close(fd);
      }
    }

    std::vector<std::string> argv_storage;
    argv_storage.reserve(args.size() + 1);
    argv_storage.push_back(executable.string());
    for (const auto &arg : args) {
      argv_storage.push_back(arg);
    }

    std::vector<char *> argv;
    argv.reserve(argv_storage.size() + 1);
    for (auto &entry : argv_storage) {
      argv.push_back(entry.data());
    }
    argv.push_back(nullptr);
    ::execv(executable.string().c_str(), argv.data());
    _exit(127);
  }

  int status = 0;
  while (::waitpid(pid, &status, 0) < 0) {
    if (errno == EINTR) {
      continue;
    }
    if (error) {
      *error = "waitpid() failed";
    }
    return false;
  }
  int exit_code = 1;
  if (WIFEXITED(status)) {
    exit_code = WEXITSTATUS(status);
  } else if (WIFSIGNALED(status)) {
    exit_code = 128 + WTERMSIG(status);
  }
  if (exit_code_out) {
    *exit_code_out = exit_code;
  }
  return true;
#endif
}

bool IsProcessRunning(int pid) {
  if (pid <= 0) {
    return false;
  }
#if defined(_WIN32)
  HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
  if (!process) {
    return false;
  }
  DWORD exit_code = 0;
  const BOOL ok = GetExitCodeProcess(process, &exit_code);
  CloseHandle(process);
  return ok && exit_code == STILL_ACTIVE;
#else
  const int result = kill(static_cast<pid_t>(pid), 0);
  if (result == 0) {
    return true;
  }
  return errno == EPERM;
#endif
}

bool TerminateProcess(int pid, bool force, std::string* error) {
  if (pid <= 0) {
    if (error) {
      *error = "Invalid PID";
    }
    return false;
  }
#if defined(_WIN32)
  (void)force;
  HANDLE process = OpenProcess(PROCESS_TERMINATE, FALSE, static_cast<DWORD>(pid));
  if (!process) {
    if (error) {
      *error = "OpenProcess failed: " + DescribeWindowsError(GetLastError());
    }
    return false;
  }
  const BOOL ok = ::TerminateProcess(process, 1);
  if (!ok && error) {
    *error = "TerminateProcess failed: " + DescribeWindowsError(GetLastError());
  }
  CloseHandle(process);
  return ok == TRUE;
#else
  const int signal = force ? SIGKILL : SIGTERM;
  const int result = kill(static_cast<pid_t>(pid), signal);
  if (result != 0 && error) {
    *error = "Failed to send signal to process";
  }
  return result == 0;
#endif
}

int CurrentProcessId() {
#if defined(_WIN32)
  return static_cast<int>(GetCurrentProcessId());
#else
  return static_cast<int>(getpid());
#endif
}

void OpenBrowser(const std::string& url) {
#if defined(_WIN32)
  const std::wstring wide_url = Widen(url);
  const HINSTANCE result =
      ShellExecuteW(nullptr, L"open", wide_url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
  if (reinterpret_cast<INT_PTR>(result) <= 32) {
    const std::string command = "start \"\" \"" + url + "\"";
    std::system(command.c_str());
  }
#else
  const std::string command = "xdg-open \"" + url + "\"";
  std::thread([command]() { std::system(command.c_str()); }).detach();
#endif
}

std::filesystem::path WithExecutableExtension(const std::filesystem::path& path) {
#if defined(_WIN32)
  if (path.extension() == ".exe") {
    return path;
  }
  std::filesystem::path with_ext = path;
  if (!with_ext.has_extension()) {
    with_ext += ".exe";
  }
  return with_ext;
#else
  return path;
#endif
}

std::filesystem::path ResolveExecutableOnPath(const std::string& name) {
  if (name.empty()) {
    return {};
  }

  std::filesystem::path candidate(name);
  std::error_code ec;
  if (candidate.is_absolute()) {
    if (std::filesystem::exists(candidate, ec)) {
      return candidate;
    }
    return {};
  }
  if (candidate.has_parent_path()) {
    auto full = std::filesystem::current_path(ec) / candidate;
    if (!ec && std::filesystem::exists(full, ec)) {
      return full;
    }
  }

  const char* path_env = std::getenv("PATH");
  if (!path_env) {
    return {};
  }
  const std::string raw_path = path_env;
#if defined(_WIN32)
  const char delimiter = ';';
  std::vector<std::string> extensions;
  if (candidate.has_extension()) {
    extensions = {""};
  } else {
    extensions = {".exe", ".cmd", ".bat"};
  }
#else
  const char delimiter = ':';
  std::vector<std::string> extensions = {""};
#endif

  std::size_t start = 0;
  while (start <= raw_path.size()) {
    const auto end = raw_path.find(delimiter, start);
    const auto token = raw_path.substr(start, end == std::string::npos ? raw_path.size() - start
                                                                       : end - start);
    if (!token.empty()) {
      const std::filesystem::path dir = token;
      for (const auto& ext : extensions) {
        std::filesystem::path full = dir / (name + ext);
        if (std::filesystem::exists(full, ec)) {
          return full;
        }
      }
    }
    if (end == std::string::npos) {
      break;
    }
    start = end + 1;
  }

  return {};
}

}  // namespace platform
