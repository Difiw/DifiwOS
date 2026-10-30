// Difiw Shell - built-in commands.
// SPDX-License-Identifier: MIT
#pragma once

#include <string>
#include <vector>

namespace difiw {

constexpr const char* kVersion = "0.1.0";
constexpr const char* kEdition = "Core";

// Returned by `exit` to leave the shell loop.
constexpr int kExitShell = 1000;

void init_state();
std::string current_dir();
void print_banner();

// Runs a built-in command, or falls back to an external program from PATH.
// Returns the exit status (0 = success), or kExitShell.
int run_command(const std::vector<std::string>& args);

}  // namespace difiw
