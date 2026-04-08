#include "virtualization/logger.hpp"
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <unistd.h>

namespace virtualization::logger {

namespace {

std::mutex  g_mtx;
std::string g_pending; // partial guest console line, flushed on '\n'

// Write [data, data+n) to fd, translating bare '\n' → "\r\n".
void raw_emit(int fd, const char* data, std::size_t n) {
    const char* p   = data;
    const char* end = data + n;
    while (p < end) {
        const char* nl = static_cast<const char*>(
            std::memchr(p, '\n', static_cast<std::size_t>(end - p)));
        if (!nl) {
            ::write(fd, p, static_cast<std::size_t>(end - p));
            return;
        }
        ::write(fd, p, static_cast<std::size_t>(nl - p));
        ::write(fd, "\r\n", 2);
        p = nl + 1;
    }
}

// Flush any buffered partial guest line to stdout.
// If non-empty, close the line so a subsequent VMM log never shares it.
// Caller must hold g_mtx.
void flush_pending() {
    if (g_pending.empty()) return;
    ::write(STDOUT_FILENO, g_pending.data(), g_pending.size());
    ::write(STDOUT_FILENO, "\r\n", 2);
    g_pending.clear();
}

} // namespace

void init() noexcept {}

void shutdown() noexcept {
    std::lock_guard<std::mutex> lk(g_mtx);
    flush_pending();
    ::fsync(STDOUT_FILENO);
    ::fsync(STDERR_FILENO);
}

void log(const char* fmt, ...) noexcept {
    va_list ap1, ap2;
    va_start(ap1, fmt);
    va_copy(ap2, ap1);
    const int need = std::vsnprintf(nullptr, 0, fmt, ap1);
    va_end(ap1);
    if (need <= 0) { va_end(ap2); return; }

    std::string buf(static_cast<std::size_t>(need), '\0');
    std::vsnprintf(buf.data(), static_cast<std::size_t>(need) + 1, fmt, ap2);
    va_end(ap2);

    std::lock_guard<std::mutex> lk(g_mtx);
    // Flush any partial guest line first so stderr never splits a guest line.
    flush_pending();
    raw_emit(STDERR_FILENO, buf.data(), buf.size());
}

void write(const char* data, std::size_t len) noexcept {
    if (!data || !len) return;
    std::lock_guard<std::mutex> lk(g_mtx);
    for (std::size_t i = 0; i < len; ++i) {
        const char c = data[i];
        if (c == '\r') continue;   // strip bare CR, we add our own on '\n'
        if (c == '\n') {
            // Emit the complete line atomically to stdout.
            ::write(STDOUT_FILENO, g_pending.data(), g_pending.size());
            ::write(STDOUT_FILENO, "\r\n", 2);
            g_pending.clear();
        } else {
            g_pending += c;
        }
    }
}

} // namespace virtualization::logger