// Difiw Shell - output formatting helpers (pure functions, host-testable).
// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <string>

namespace difiw {

// 0 -> "0 B", 1536 -> "1.5 KiB", 1073741824 -> "1.0 GiB"
std::string human_size(std::uint64_t bytes);

// 1536 -> "1.5 KiB (1536 B)"; plain "512 B" below 1 KiB
std::string size_with_bytes(std::uint64_t bytes);

// 3725 -> "1h 02m 05s"
std::string format_duration(std::uint64_t seconds);

// percent(1,4) -> 25; safe for total == 0 (returns 0)
unsigned percent(std::uint64_t part, std::uint64_t total);

// Saturating add, sets `overflow` instead of wrapping.
std::uint64_t sat_add(std::uint64_t a, std::uint64_t b, bool& overflow);

}  // namespace difiw
