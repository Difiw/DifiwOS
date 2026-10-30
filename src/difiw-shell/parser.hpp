// Difiw Shell - line parser and path helpers (pure functions, host-testable).
// SPDX-License-Identifier: MIT
#pragma once

#include <string>
#include <vector>

namespace difiw {

struct ParseResult {
    bool ok = true;
    std::vector<std::string> args;
    std::string error;
};

// Splits a command line into arguments.
// Supported: whitespace separation, 'single quotes' (literal),
// "double quotes" (with \" and \\ escapes), backslash escape outside quotes,
// '#' at the start of a word begins a comment.
// Not supported: pipes, redirects, variables, globbing (documented limitation).
ParseResult parse_line(const std::string& line);

// Lexically normalizes `path` against `cwd` (resolves ".", ".." and "//").
// A leading "~" expands to `home`. Empty `path` yields `cwd`.
std::string normalize_path(const std::string& cwd, const std::string& path,
                           const std::string& home = "/root");

// True for paths that `del` must never remove: the root, mount points and
// critical system trees. Expects a normalized absolute path.
bool is_protected_path(const std::string& normalized);

// "dir/name" join that avoids a double slash.
std::string join_path(const std::string& dir, const std::string& name);

}  // namespace difiw
