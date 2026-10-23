// Logging to <logDirectory>/vesktop_media.log (and optionally stderr).
#pragma once

#include <cstdarg>
#include <cstddef>
#include <string>

namespace vv {

enum class LogLevel { Verbose = 0, Info = 1, Warning = 2, Error = 3 };

// `level` is the value index.js passes to initialize(): -1 = errors only, 0-1 = warnings,
// 2 = info (Discord's default with debug logging on), >= 3 = verbose
void LogInit(const std::string& directory, int level, size_t maxBytes);
bool LogEnabled(LogLevel level);
void Log(LogLevel level, const char* tag, const char* fmt, ...) __attribute__((format(printf, 3, 4)));
void LogV(LogLevel level, const char* tag, const char* fmt, va_list args);
// Log lines coming from JS (consoleLog)
void LogRaw(const std::string& level, const std::string& message);

} // namespace vv

#define VV_LOGV(tag, ...) ::vv::Log(::vv::LogLevel::Verbose, tag, __VA_ARGS__)
#define VV_LOGI(tag, ...) ::vv::Log(::vv::LogLevel::Info, tag, __VA_ARGS__)
#define VV_LOGW(tag, ...) ::vv::Log(::vv::LogLevel::Warning, tag, __VA_ARGS__)
#define VV_LOGE(tag, ...) ::vv::Log(::vv::LogLevel::Error, tag, __VA_ARGS__)
