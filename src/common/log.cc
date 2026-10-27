#include "common/log.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <mutex>

#include "common/platform.h"

namespace vv {

namespace {

std::mutex gMutex;
FILE* gFile = nullptr;
std::string gPath;
size_t gMaxBytes = 5'000'000;
size_t gWritten = 0;
int gMinLevel = static_cast<int>(LogLevel::Info);
bool gStderr = std::getenv("VESKTOP_MEDIA_LOG_STDERR") != nullptr;

const char* LevelName(LogLevel level)
{
    switch (level) {
    case LogLevel::Verbose: return "V";
    case LogLevel::Info: return "I";
    case LogLevel::Warning: return "W";
    case LogLevel::Error: return "E";
    }
    return "?";
}

void Rotate()
{
    if (!gFile) return;
    fclose(gFile);
    std::error_code ec;
    std::filesystem::rename(PathFromUtf8(gPath), PathFromUtf8(gPath + ".1"), ec);
    gFile = OpenFile(gPath, "w");
    gWritten = 0;
}

void Write(const char* level, const char* tag, const char* message)
{
    using namespace std::chrono;
    auto now = system_clock::now();
    auto t = system_clock::to_time_t(now);
    int ms = static_cast<int>(duration_cast<milliseconds>(now.time_since_epoch()).count() % 1000);
    tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char ts[32];
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm);

    std::lock_guard lock(gMutex);
    if (gFile) {
        int n = fprintf(gFile, "[%s.%03d] [%s] %s: %s\n", ts, ms, level, tag, message);
        fflush(gFile);
        if (n > 0) gWritten += n;
        if (gWritten > gMaxBytes) Rotate();
    }
    if (gStderr || !gFile) fprintf(stderr, "[vesktop_media %s] %s: %s\n", level, tag, message);
}

} // namespace

void LogInit(const std::string& directory, int level, size_t maxBytes)
{
    std::lock_guard lock(gMutex);
    if (level < 0) gMinLevel = static_cast<int>(LogLevel::Error);
    else if (level < 2) gMinLevel = static_cast<int>(LogLevel::Warning);
    else if (level == 2) gMinLevel = static_cast<int>(LogLevel::Info);
    else gMinLevel = static_cast<int>(LogLevel::Verbose);
    if (maxBytes > 0) gMaxBytes = maxBytes;

    if (gFile || directory.empty()) return;
    std::error_code ec;
    std::filesystem::create_directories(PathFromUtf8(directory), ec);
    gPath = PathToUtf8(PathFromUtf8(directory) / "vesktop_media.log");
    gFile = OpenFile(gPath, "a");
    if (gFile) {
        fseek(gFile, 0, SEEK_END);
        gWritten = static_cast<size_t>(ftell(gFile));
    }
}

bool LogEnabled(LogLevel level)
{
    return static_cast<int>(level) >= gMinLevel;
}

void LogV(LogLevel level, const char* tag, const char* fmt, va_list args)
{
    if (!LogEnabled(level)) return;
    char buf[2048];
    vsnprintf(buf, sizeof(buf), fmt, args);
    Write(LevelName(level), tag, buf);
}

void Log(LogLevel level, const char* tag, const char* fmt, ...)
{
    if (!LogEnabled(level)) return;
    va_list args;
    va_start(args, fmt);
    LogV(level, tag, fmt, args);
    va_end(args);
}

void LogRaw(const std::string& level, const std::string& message)
{
    LogLevel l = LogLevel::Info;
    if (level == "error") l = LogLevel::Error;
    else if (level == "warn") l = LogLevel::Warning;
    else if (level == "trace" || level == "debug") l = LogLevel::Verbose;
    if (!LogEnabled(l)) return;
    Write(LevelName(l), "js", message.c_str());
}

} // namespace vv
