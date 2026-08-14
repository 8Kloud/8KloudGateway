// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include <cstdarg>

namespace kg {

enum class LogLevel { Debug, Info, Warning, Error };

void setLogLevel(LogLevel level);
void log(LogLevel level, const char* format, ...)
    __attribute__((format(printf, 2, 3)));

}  // namespace kg

#define KG_DEBUG(...) ::kg::log(::kg::LogLevel::Debug, __VA_ARGS__)
#define KG_INFO(...) ::kg::log(::kg::LogLevel::Info, __VA_ARGS__)
#define KG_WARN(...) ::kg::log(::kg::LogLevel::Warning, __VA_ARGS__)
#define KG_ERROR(...) ::kg::log(::kg::LogLevel::Error, __VA_ARGS__)

