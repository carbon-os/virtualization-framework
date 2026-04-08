#pragma once
#include <cstddef>

namespace virtualization::logger {

void init()     noexcept;
void shutdown() noexcept;

/// VMM structured log line → stderr  (printf-style)
void log(const char* fmt, ...) noexcept
    __attribute__((format(printf, 1, 2)));

/// Guest console output → stdout  (line-buffered, \n flushes atomically)
void write(const char* data, std::size_t len) noexcept;

} // namespace virtualization::logger