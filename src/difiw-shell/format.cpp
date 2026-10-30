// SPDX-License-Identifier: MIT
#include "format.hpp"

#include <cstdio>
#include <limits>

namespace difiw {

std::string human_size(std::uint64_t bytes) {
    static const char* const units[] = {"B", "KiB", "MiB", "GiB", "TiB", "PiB", "EiB"};
    if (bytes < 1024) return std::to_string(bytes) + " B";
    double v = static_cast<double>(bytes);
    int u = 0;
    while (v >= 1024.0 && u < 6) { v /= 1024.0; ++u; }
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.1f %s", v, units[u]);
    return buf;
}

std::string size_with_bytes(std::uint64_t bytes) {
    if (bytes < 1024) return human_size(bytes);
    return human_size(bytes) + " (" + std::to_string(bytes) + " B)";
}

std::string format_duration(std::uint64_t s) {
    char buf[48];
    std::snprintf(buf, sizeof buf, "%lluh %02llum %02llus",
                  static_cast<unsigned long long>(s / 3600),
                  static_cast<unsigned long long>((s / 60) % 60),
                  static_cast<unsigned long long>(s % 60));
    return buf;
}

unsigned percent(std::uint64_t part, std::uint64_t total) {
    if (total == 0) return 0;
    if (part > total) part = total;
    return static_cast<unsigned>((static_cast<long double>(part) * 100.0L) / total + 0.5L);
}

std::uint64_t sat_add(std::uint64_t a, std::uint64_t b, bool& overflow) {
    const std::uint64_t max = std::numeric_limits<std::uint64_t>::max();
    if (a > max - b) { overflow = true; return max; }
    return a + b;
}

}  // namespace difiw
