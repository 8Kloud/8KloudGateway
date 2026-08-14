// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "core/log.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <mutex>

namespace kg {
namespace {
std::atomic<LogLevel> g_level{LogLevel::Info};
std::mutex g_mutex;
}

void setLogLevel(LogLevel level) { g_level = level; }

void log(LogLevel level, const char* format, ...) {
    if (level < g_level.load(std::memory_order_relaxed)) return;
    static const char* names[] = {"debug", "info", "warn", "error"};
    const auto now = std::chrono::system_clock::now();
    const std::time_t time = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    localtime_r(&time, &tm);
    char stamp[32]{};
    std::strftime(stamp, sizeof stamp, "%Y-%m-%d %H:%M:%S", &tm);

    std::lock_guard lock(g_mutex);
    std::fprintf(stderr, "%s %-5s ", stamp, names[static_cast<int>(level)]);
    va_list args;
    va_start(args, format);
    std::vfprintf(stderr, format, args);
    va_end(args);
    std::fputc('\n', stderr);
    std::fflush(stderr);
}
}  // namespace kg

