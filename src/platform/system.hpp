#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace platform {

bool LaunchDetached(const std::filesystem::path& executable,
                    const std::vector<std::string>& args,
                    const std::filesystem::path& working_directory,
                    const std::filesystem::path& stdout_path,
                    const std::filesystem::path& stderr_path,
                    int* pid_out,
                    std::string* error);
bool LaunchDetachedWithEnv(const std::filesystem::path& executable,
                           const std::vector<std::string>& args,
                           const std::filesystem::path& working_directory,
                           const std::filesystem::path& stdout_path,
                           const std::filesystem::path& stderr_path,
                           const std::vector<std::pair<std::string, std::string>>& env_overrides,
                           int* pid_out,
                           std::string* error);
bool RunProcess(const std::filesystem::path &executable, const std::vector<std::string> &args,
                const std::filesystem::path &working_directory, const std::filesystem::path &stdout_path,
                const std::filesystem::path &stderr_path, int *exit_code_out, std::string *error);

bool IsProcessRunning(int pid);
bool TerminateProcess(int pid, bool force, std::string* error);
int CurrentProcessId();
void OpenBrowser(const std::string& url);
std::filesystem::path WithExecutableExtension(const std::filesystem::path& path);
std::filesystem::path ResolveExecutableOnPath(const std::string& name);

}  // namespace platform
