#include "core/logging.hpp"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cctype>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string_view>

namespace {

using core::logging::LogLevel;

std::atomic<LogLevel> g_log_level{LogLevel::kInfo};
std::mutex g_log_mutex;

std::string CurrentTimestamp() {
  const auto now = std::chrono::system_clock::now();
  const std::time_t now_time = std::chrono::system_clock::to_time_t(now);
  std::tm tm_snapshot;
#if defined(_WIN32)
  localtime_s(&tm_snapshot, &now_time);
#else
  localtime_r(&now_time, &tm_snapshot);
#endif
  std::ostringstream oss;
  oss << std::put_time(&tm_snapshot, "%Y-%m-%d %H:%M:%S");
  return oss.str();
}

std::string_view ToString(LogLevel level) {
  switch (level) {
    case LogLevel::kError:
      return "ERROR";
    case LogLevel::kWarn:
      return "WARN";
    case LogLevel::kInfo:
      return "INFO";
    case LogLevel::kDebug:
      return "DEBUG";
  }
  return "INFO";
}

LogLevel ParseLevel(std::string value) {
  for (auto& ch : value) {
    ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  }
  if (value == "error") {
    return LogLevel::kError;
  }
  if (value == "warn" || value == "warning") {
    return LogLevel::kWarn;
  }
  if (value == "debug") {
    return LogLevel::kDebug;
  }
  return LogLevel::kInfo;
}

}  // namespace

namespace core::logging {

void InitializeFromEnvironment() {
  if (const char* env = std::getenv("CHAX_LOG_LEVEL")) {
    SetLogLevel(ParseLevel(env));
  }
}

void SetLogLevel(LogLevel level) { g_log_level.store(level); }

LogLevel GetLogLevel() { return g_log_level.load(); }

bool IsDebugEnabled() { return GetLogLevel() == LogLevel::kDebug; }

void Log(LogLevel level, const std::string& message) {
  const auto current_level = g_log_level.load();
  if (static_cast<int>(level) > static_cast<int>(current_level)) {
    return;
  }

  const std::string timestamp = CurrentTimestamp();
  std::lock_guard<std::mutex> lock(g_log_mutex);
  std::ostream& stream =
      (level == LogLevel::kError || level == LogLevel::kWarn) ? std::cerr : std::cout;
  stream << timestamp << " [" << ToString(level) << "] " << message << std::endl;
}

void LogInfo(const std::string& message) { Log(LogLevel::kInfo, message); }

void LogWarn(const std::string& message) { Log(LogLevel::kWarn, message); }

void LogError(const std::string& message) { Log(LogLevel::kError, message); }

void LogDebug(const std::string& message) { Log(LogLevel::kDebug, message); }

}  // namespace core::logging
