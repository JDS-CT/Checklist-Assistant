#pragma once

#include <string>

namespace core::logging {

enum class LogLevel { kError = 0, kWarn, kInfo, kDebug };

void InitializeFromEnvironment();
void SetLogLevel(LogLevel level);
LogLevel GetLogLevel();
bool IsDebugEnabled();

void Log(LogLevel level, const std::string& message);
void LogInfo(const std::string& message);
void LogWarn(const std::string& message);
void LogError(const std::string& message);
void LogDebug(const std::string& message);

}  // namespace core::logging
