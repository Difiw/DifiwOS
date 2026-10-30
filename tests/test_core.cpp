// Host unit tests for parser and formatting helpers.
// SPDX-License-Identifier: MIT
#include <cstdio>
#include <limits>

#include "format.hpp"
#include "parser.hpp"

static int g_fail = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++g_fail; } \
    } while (0)

using namespace difiw;

int main() {
    // parser
    auto r = parse_line("  ls   -a  /tmp ");
    CHECK(r.ok && r.args.size() == 3 && r.args[0] == "ls" && r.args[2] == "/tmp");
    r = parse_line("touch \"my file.txt\" 'a b'");
    CHECK(r.ok && r.args.size() == 3 && r.args[1] == "my file.txt" && r.args[2] == "a b");
    r = parse_line("echo 'say \"hi\"'");
    CHECK(r.ok && r.args.size() == 2 && r.args[1] == "say \"hi\"");
    r = parse_line("cd \\tmp\\x");  // backslash is an ordinary character
    CHECK(r.ok && r.args.size() == 2 && r.args[1] == "\\tmp\\x");
    r = parse_line("echo \"привет мир\"");
    CHECK(r.ok && r.args.size() == 2 && r.args[1] == "привет мир");
    r = parse_line("echo \"\"");
    CHECK(r.ok && r.args.size() == 2 && r.args[1].empty());
    r = parse_line("echo \"open");
    CHECK(!r.ok);
    r = parse_line("cd\\");
    CHECK(r.ok && r.args.size() == 1 && r.args[0] == "cd\\");
    r = parse_line("");
    CHECK(r.ok && r.args.empty());
    r = parse_line("   # comment only");
    CHECK(r.ok && r.args.empty());

    // paths
    CHECK(normalize_path("/root", "a/b") == "/root/a/b");
    CHECK(normalize_path("/root", "../tmp/./x//y") == "/tmp/x/y");
    CHECK(normalize_path("/", "..") == "/");
    CHECK(normalize_path("/a", "") == "/a");
    CHECK(normalize_path("/a", "~/x", "/root") == "/root/x");
    CHECK(normalize_path("/a", "/../../etc") == "/etc");
    CHECK(normalize_path("/root", "..\\tmp\\x") == "/tmp/x");
    CHECK(normalize_path("/root", "\\etc\\..\\tmp") == "/tmp");
    CHECK(join_path("/", "x") == "/x");
    CHECK(join_path("/a/", "x") == "/a/x");

    // protection
    CHECK(is_protected_path("/"));
    CHECK(is_protected_path("/etc"));
    CHECK(is_protected_path("/etc/passwd"));
    CHECK(is_protected_path("/usr/bin/x"));
    CHECK(is_protected_path("/root"));
    CHECK(is_protected_path("/tmp"));
    CHECK(!is_protected_path("/tmp/file"));
    CHECK(!is_protected_path("/root/notes.txt"));
    CHECK(!is_protected_path("/etcetera"));
    CHECK(!is_protected_path("/data/file"));

    // formatting
    CHECK(human_size(0) == "0 B");
    CHECK(human_size(1023) == "1023 B");
    CHECK(human_size(1536) == "1.5 KiB");
    CHECK(human_size(1073741824ULL) == "1.0 GiB");
    CHECK(size_with_bytes(2048) == "2.0 KiB (2048 B)");
    CHECK(format_duration(3725) == "1h 02m 05s");
    CHECK(percent(1, 4) == 25);
    CHECK(percent(5, 0) == 0);
    CHECK(percent(9, 4) == 100);
    bool ov = false;
    const auto mx = std::numeric_limits<std::uint64_t>::max();
    CHECK(sat_add(mx, 1, ov) == mx && ov);

    if (g_fail == 0) std::printf("all tests passed\n");
    return g_fail == 0 ? 0 : 1;
}
