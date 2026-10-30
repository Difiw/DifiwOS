// SPDX-License-Identifier: MIT
#include "parser.hpp"

#include <cstring>

namespace difiw {

ParseResult parse_line(const std::string& line) {
    ParseResult r;
    std::string cur;
    bool have = false;
    char quote = 0;

    // No escape sequences: a backslash is an ordinary character (Windows-style paths).
    for (size_t i = 0; i < line.size(); ++i) {
        const char c = line[i];
        if (quote) {
            if (c == quote) quote = 0; else cur += c;
            continue;
        }
        if (c == '\'' || c == '"') { quote = c; have = true; continue; }
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            if (have) { r.args.push_back(cur); cur.clear(); have = false; }
            continue;
        }
        if (c == '#' && !have) break;
        cur += c;
        have = true;
    }
    if (quote) {
        r.ok = false;
        r.error = "unterminated quote";
        r.args.clear();
        return r;
    }
    if (have) r.args.push_back(cur);
    return r;
}

std::string join_path(const std::string& dir, const std::string& name) {
    if (dir.empty() || dir == "/") return "/" + name;
    if (dir.back() == '/') return dir + name;
    return dir + "/" + name;
}

std::string normalize_path(const std::string& cwd, const std::string& raw,
                           const std::string& home) {
    std::string path = raw;
    for (char& ch : path)
        if (ch == '\\') ch = '/';  // Windows-style separators

    std::string full;
    if (path.empty()) {
        full = cwd;
    } else if (path == "~" || path.compare(0, 2, "~/") == 0) {
        full = home + path.substr(1);
    } else if (path[0] == '/') {
        full = path;
    } else {
        full = cwd + "/" + path;
    }

    std::vector<std::string> parts;
    size_t i = 0;
    while (i <= full.size()) {
        size_t j = full.find('/', i);
        if (j == std::string::npos) j = full.size();
        const std::string seg = full.substr(i, j - i);
        if (seg.empty() || seg == ".") {
            // skip
        } else if (seg == "..") {
            if (!parts.empty()) parts.pop_back();
        } else {
            parts.push_back(seg);
        }
        i = j + 1;
    }
    if (parts.empty()) return "/";
    std::string out;
    for (const auto& p : parts) out += "/" + p;
    return out;
}

bool is_protected_path(const std::string& p) {
    static const char* const exact[] = {"/",     "/root", "/tmp",  "/data", "/run",
                                        "/home", "/mnt",  "/init", "/linuxrc"};
    static const char* const trees[] = {"/bin", "/sbin", "/lib", "/lib64", "/usr",
                                        "/etc", "/boot", "/dev", "/proc",  "/sys"};
    for (const char* e : exact)
        if (p == e) return true;
    for (const char* t : trees) {
        const size_t n = std::strlen(t);
        if (p.compare(0, n, t) == 0 && (p.size() == n || p[n] == '/')) return true;
    }
    return false;
}

}  // namespace difiw
