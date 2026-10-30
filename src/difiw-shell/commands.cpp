// SPDX-License-Identifier: MIT
#include "commands.hpp"

#include <dirent.h>
#include <fcntl.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/sysinfo.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>
#include <signal.h>

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <set>
#include <utility>

#include "format.hpp"
#include "parser.hpp"

namespace difiw {

namespace {

constexpr std::size_t kReadLimit = 64 * 1024;  // `read` shows at most 64 KiB
constexpr int kMaxDepth = 64;                  // recursion limit for size/del -r

std::string g_cwd = "/";

std::string home_dir() {
    const char* h = std::getenv("HOME");
    return (h && *h == '/') ? h : "/root";
}

std::string resolve(const std::string& p) { return normalize_path(g_cwd, p, home_dir()); }

int fail(const char* cmd, const std::string& what, int err) {
    std::fprintf(stderr, "difiw: %s: %s: %s\n", cmd, what.c_str(), std::strerror(err));
    return 1;
}

int bad_usage(const char* cmd, const char* usage) {
    std::fprintf(stderr, "difiw: %s: invalid arguments\nusage: %s\n", cmd, usage);
    return 2;
}

// Splits leading "-x" flags from args[1..]. Returns false (and prints) on an
// unknown flag. `allowed` lists accepted single-letter flags.
bool take_flags(const char* cmd, const std::vector<std::string>& args, const char* allowed,
                std::set<char>& flags, std::vector<std::string>& rest) {
    std::size_t i = 1;
    for (; i < args.size(); ++i) {
        const std::string& a = args[i];
        if (a == "--") { ++i; break; }
        if (a.size() < 2 || a[0] != '-') break;
        for (std::size_t k = 1; k < a.size(); ++k) {
            if (!std::strchr(allowed, a[k])) {
                std::fprintf(stderr, "difiw: %s: unknown option '-%c'\n", cmd, a[k]);
                return false;
            }
            flags.insert(a[k]);
        }
    }
    for (; i < args.size(); ++i) rest.push_back(args[i]);
    return true;
}

bool is_mountpoint_used(const char* mp) {
    FILE* f = std::fopen("/proc/mounts", "r");
    if (!f) return false;
    char line[512];
    bool found = false;
    const std::string needle = std::string(" ") + mp + " ";
    while (std::fgets(line, sizeof line, f))
        if (std::strstr(line, needle.c_str())) { found = true; break; }
    std::fclose(f);
    return found;
}

std::uint64_t meminfo_kb(const char* key) {
    FILE* f = std::fopen("/proc/meminfo", "r");
    if (!f) return 0;
    char line[256];
    std::uint64_t val = 0;
    const std::size_t n = std::strlen(key);
    while (std::fgets(line, sizeof line, f)) {
        if (std::strncmp(line, key, n) == 0 && line[n] == ':') {
            val = std::strtoull(line + n + 1, nullptr, 10);
            break;
        }
    }
    std::fclose(f);
    return val;
}

// ---------------------------------------------------------------- commands

int cmd_help(const std::vector<std::string>& args);

int cmd_time(const std::vector<std::string>& args) {
    if (args.size() != 1) return bad_usage("time", "time");
    const std::time_t t = std::time(nullptr);
    struct tm tm;
    localtime_r(&t, &tm);
    char buf[32];
    std::strftime(buf, sizeof buf, "%H:%M:%S", &tm);
    const long off = tm.tm_gmtoff;
    std::printf("%s (UTC%c%02ld:%02ld)\n", buf, off < 0 ? '-' : '+', std::labs(off) / 3600,
                (std::labs(off) / 60) % 60);
    return 0;
}

int cmd_date(const std::vector<std::string>& args) {
    if (args.size() != 1) return bad_usage("date", "date");
    const std::time_t t = std::time(nullptr);
    struct tm tm;
    localtime_r(&t, &tm);
    char buf[64];
    std::strftime(buf, sizeof buf, "%Y-%m-%d, %A", &tm);
    std::printf("%s\n", buf);
    return 0;
}

int cmd_info(const std::vector<std::string>& args) {
    if (args.size() != 1) return bad_usage("info", "info");
    struct utsname u;
    struct sysinfo si;
    if (uname(&u) != 0) return fail("info", "uname", errno);
    const bool have_si = sysinfo(&si) == 0;
    const char* tty = ttyname(STDIN_FILENO);
    std::printf("Difiw OS %s %s\n", kVersion, kEdition);
    std::printf("  Kernel:       %s %s (%s)\n", u.sysname, u.release, u.machine);
    std::printf("  Hostname:     %s\n", u.nodename);
    if (have_si) std::printf("  Uptime:       %s\n", format_duration(si.uptime).c_str());
    std::printf("  Shell:        Difiw Shell %s\n", kVersion);
    std::printf("  Terminal:     %s\n", tty ? tty : "(not a terminal)");
    std::printf("  Root FS:      RAM (initramfs); changes are lost on reboot\n");
    if (is_mountpoint_used("/data"))
        std::printf("  Persistence:  /data is mounted (disk labelled DIFIWDATA)\n");
    else
        std::printf("  Persistence:  none (no DIFIWDATA disk attached)\n");
    return 0;
}

int cmd_mem(const std::vector<std::string>& args) {
    if (args.size() != 1) return bad_usage("mem", "mem");
    struct sysinfo si;
    if (sysinfo(&si) != 0) return fail("mem", "sysinfo", errno);
    const std::uint64_t unit = si.mem_unit ? si.mem_unit : 1;
    const std::uint64_t total = si.totalram * unit;
    std::uint64_t avail = meminfo_kb("MemAvailable") * 1024ULL;
    if (avail == 0) avail = (si.freeram + si.bufferram) * unit;
    if (avail > total) avail = total;
    const std::uint64_t used = total - avail;
    std::printf("Memory\n");
    std::printf("  Total:      %s\n", human_size(total).c_str());
    std::printf("  Used:       %s (%u%%)\n", human_size(used).c_str(), percent(used, total));
    std::printf("  Available:  %s\n", human_size(avail).c_str());
    std::printf("  Free:       %s\n", human_size(si.freeram * unit).c_str());
    return 0;
}

int cmd_disk(const std::vector<std::string>& args) {
    if (args.size() > 2) return bad_usage("disk", "disk [path]");
    const std::string path = args.size() == 2 ? resolve(args[1]) : g_cwd;
    struct statvfs sv;
    if (statvfs(path.c_str(), &sv) != 0) return fail("disk", path, errno);
    const std::uint64_t total = static_cast<std::uint64_t>(sv.f_blocks) * sv.f_frsize;
    const std::uint64_t freeb = static_cast<std::uint64_t>(sv.f_bavail) * sv.f_frsize;
    if (total == 0) {
        std::printf("Filesystem of %s\n  No fixed capacity is reported (RAM-backed filesystem).\n",
                    path.c_str());
        return 0;
    }
    const std::uint64_t used = total - std::min<std::uint64_t>(total, static_cast<std::uint64_t>(sv.f_bfree) * sv.f_frsize);
    std::printf("Filesystem of %s\n", path.c_str());
    std::printf("  Total:  %s\n", human_size(total).c_str());
    std::printf("  Used:   %s (%u%%)\n", human_size(used).c_str(), percent(used, total));
    std::printf("  Free:   %s\n", human_size(freeb).c_str());
    return 0;
}

struct SizeStats {
    std::uint64_t bytes = 0, files = 0, dirs = 0, links = 0, errors = 0;
    bool overflow = false;
};

void walk_size(const std::string& path, SizeStats& st, int depth,
               std::set<std::pair<dev_t, ino_t>>& seen) {
    struct stat sb;
    if (lstat(path.c_str(), &sb) != 0) {
        std::fprintf(stderr, "difiw: size: %s: %s\n", path.c_str(), std::strerror(errno));
        ++st.errors;
        return;
    }
    if (S_ISLNK(sb.st_mode)) { ++st.links; return; }  // never followed
    if (!S_ISDIR(sb.st_mode)) {
        ++st.files;
        st.bytes = sat_add(st.bytes, static_cast<std::uint64_t>(sb.st_size), st.overflow);
        return;
    }
    ++st.dirs;
    if (depth >= kMaxDepth) {
        std::fprintf(stderr, "difiw: size: %s: too deep, skipped\n", path.c_str());
        ++st.errors;
        return;
    }
    if (!seen.insert({sb.st_dev, sb.st_ino}).second) return;  // loop guard
    DIR* d = opendir(path.c_str());
    if (!d) {
        std::fprintf(stderr, "difiw: size: %s: %s\n", path.c_str(), std::strerror(errno));
        ++st.errors;
        return;
    }
    while (struct dirent* e = readdir(d)) {
        if (!std::strcmp(e->d_name, ".") || !std::strcmp(e->d_name, "..")) continue;
        walk_size(join_path(path, e->d_name), st, depth + 1, seen);
    }
    closedir(d);
}

int cmd_size(const std::vector<std::string>& args) {
    if (args.size() != 2) return bad_usage("size", "size <path>");
    const std::string path = resolve(args[1]);
    struct stat sb;
    if (lstat(path.c_str(), &sb) != 0) return fail("size", path, errno);
    SizeStats st;
    std::set<std::pair<dev_t, ino_t>> seen;
    walk_size(path, st, 0, seen);
    if (S_ISDIR(sb.st_mode)) {
        std::printf("%s: %s%s in %llu files, %llu directories", path.c_str(),
                    st.overflow ? ">= " : "", size_with_bytes(st.bytes).c_str(),
                    static_cast<unsigned long long>(st.files),
                    static_cast<unsigned long long>(st.dirs > 0 ? st.dirs - 1 : 0));
        if (st.links) std::printf(", %llu symlinks (not followed)", static_cast<unsigned long long>(st.links));
        std::printf("\n");
    } else {
        std::printf("%s: %s\n", path.c_str(), size_with_bytes(st.bytes).c_str());
    }
    if (st.errors) {
        std::fprintf(stderr, "difiw: size: %llu objects could not be read\n",
                     static_cast<unsigned long long>(st.errors));
        return 1;
    }
    return 0;
}

void print_ls_entry(const std::string& full, const std::string& name) {
    struct stat sb;
    if (lstat(full.c_str(), &sb) != 0) {
        std::printf("?  %10s  %s\n", "-", name.c_str());
        return;
    }
    if (S_ISDIR(sb.st_mode)) {
        std::printf("d  %10s  %s/\n", "-", name.c_str());
    } else if (S_ISLNK(sb.st_mode)) {
        char target[PATH_MAX];
        const ssize_t n = readlink(full.c_str(), target, sizeof target - 1);
        if (n >= 0) target[n] = 0; else target[0] = 0;
        std::printf("l  %10s  %s -> %s\n", "-", name.c_str(), target);
    } else {
        const char t = S_ISREG(sb.st_mode) ? '-' : 'o';
        std::printf("%c  %10s  %s\n", t, human_size(static_cast<std::uint64_t>(sb.st_size)).c_str(), name.c_str());
    }
}

int cmd_ls(const std::vector<std::string>& args) {
    std::set<char> flags;
    std::vector<std::string> rest;
    if (!take_flags("ls", args, "a", flags, rest)) return 2;
    if (rest.size() > 1) return bad_usage("ls", "ls [-a] [path]");
    const std::string path = rest.empty() ? g_cwd : resolve(rest[0]);
    struct stat sb;
    if (lstat(path.c_str(), &sb) != 0) return fail("ls", path, errno);
    if (!S_ISDIR(sb.st_mode)) {
        print_ls_entry(path, path);
        return 0;
    }
    DIR* d = opendir(path.c_str());
    if (!d) return fail("ls", path, errno);
    std::vector<std::string> names;
    while (struct dirent* e = readdir(d)) {
        if (!std::strcmp(e->d_name, ".") || !std::strcmp(e->d_name, "..")) continue;
        if (e->d_name[0] == '.' && !flags.count('a')) continue;
        names.push_back(e->d_name);
    }
    closedir(d);
    std::sort(names.begin(), names.end());
    for (const auto& n : names) print_ls_entry(join_path(path, n), n);
    if (names.empty()) std::printf("(empty)\n");
    return 0;
}

int cmd_cd(const std::vector<std::string>& args) {
    if (args.size() > 2) return bad_usage("cd", "cd [path]");
    const std::string target = args.size() == 2 ? resolve(args[1]) : home_dir();
    if (chdir(target.c_str()) != 0) return fail("cd", target, errno);
    char buf[PATH_MAX];
    if (getcwd(buf, sizeof buf)) g_cwd = buf; else g_cwd = target;
    return 0;
}

int cmd_pwd(const std::vector<std::string>& args) {
    if (args.size() != 1) return bad_usage("pwd", "pwd");
    std::printf("%s\n", g_cwd.c_str());
    return 0;
}

int cmd_read(const std::vector<std::string>& args) {
    if (args.size() != 2) return bad_usage("read", "read <file>");
    const std::string path = resolve(args[1]);
    struct stat sb;
    if (stat(path.c_str(), &sb) != 0) return fail("read", path, errno);
    if (S_ISDIR(sb.st_mode)) return fail("read", path, EISDIR);
    if (!S_ISREG(sb.st_mode)) {
        std::fprintf(stderr, "difiw: read: %s: not a regular file\n", path.c_str());
        return 1;
    }
    const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return fail("read", path, errno);
    std::vector<char> buf(kReadLimit + 1);
    std::size_t got = 0;
    while (got < buf.size()) {
        const ssize_t n = ::read(fd, buf.data() + got, buf.size() - got);
        if (n < 0) {
            if (errno == EINTR) continue;
            const int e = errno;
            close(fd);
            return fail("read", path, e);
        }
        if (n == 0) break;
        got += static_cast<std::size_t>(n);
    }
    close(fd);
    const bool truncated = got > kReadLimit;
    if (truncated) got = kReadLimit;
    if (std::memchr(buf.data(), 0, std::min<std::size_t>(got, 4096))) {
        std::fprintf(stderr, "difiw: read: %s: looks like a binary file, not shown\n", path.c_str());
        return 1;
    }
    std::fwrite(buf.data(), 1, got, stdout);
    if (got && buf[got - 1] != '\n') std::fputc('\n', stdout);
    if (truncated)
        std::printf("[output truncated at %zu bytes; file is %s]\n", kReadLimit,
                    size_with_bytes(static_cast<std::uint64_t>(sb.st_size)).c_str());
    return 0;
}

int cmd_clear(const std::vector<std::string>& args) {
    if (args.size() != 1) return bad_usage("clear", "clear");
    std::fputs("\033[H\033[2J\033[3J", stdout);
    return 0;
}

int cmd_mkdir(const std::vector<std::string>& args) {
    std::set<char> flags;
    std::vector<std::string> rest;
    if (!take_flags("mkdir", args, "p", flags, rest)) return 2;
    if (rest.empty()) return bad_usage("mkdir", "mkdir [-p] <path>...");
    int rc = 0;
    for (const auto& a : rest) {
        const std::string path = resolve(a);
        if (flags.count('p')) {
            std::string cur;
            bool ok = true;
            std::size_t i = 1;
            while (ok && i <= path.size()) {
                std::size_t j = path.find('/', i);
                if (j == std::string::npos) j = path.size();
                cur = path.substr(0, j);
                if (mkdir(cur.c_str(), 0755) != 0 && errno != EEXIST) {
                    rc = fail("mkdir", cur, errno);
                    ok = false;
                }
                i = j + 1;
            }
        } else if (mkdir(path.c_str(), 0755) != 0) {
            rc = fail("mkdir", path, errno);
        }
    }
    return rc;
}

int cmd_touch(const std::vector<std::string>& args) {
    if (args.size() < 2) return bad_usage("touch", "touch <path>...");
    int rc = 0;
    for (std::size_t i = 1; i < args.size(); ++i) {
        const std::string path = resolve(args[i]);
        const int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_CLOEXEC | O_NOCTTY, 0644);
        if (fd < 0) {
            // A directory cannot be opened for writing; update its times instead.
            if (errno == EISDIR && utimensat(AT_FDCWD, path.c_str(), nullptr, 0) == 0) continue;
            rc = fail("touch", path, errno);
            continue;
        }
        if (futimens(fd, nullptr) != 0) rc = fail("touch", path, errno);
        close(fd);
    }
    return rc;
}

// Real-path check: the parent directory may be a symlink into a protected tree.
bool protected_by_realpath(const std::string& path) {
    const std::size_t slash = path.rfind('/');
    const std::string parent = slash == 0 ? "/" : path.substr(0, slash);
    char buf[PATH_MAX];
    if (!realpath(parent.c_str(), buf)) return false;
    return is_protected_path(join_path(buf, path.substr(slash + 1)));
}

struct DelStats { std::uint64_t removed = 0, errors = 0; };

void remove_tree(const std::string& path, int depth, DelStats& st) {
    struct stat sb;
    if (lstat(path.c_str(), &sb) != 0) { ++st.errors; return; }
    if (S_ISDIR(sb.st_mode)) {
        if (depth >= kMaxDepth) {
            std::fprintf(stderr, "difiw: del: %s: too deep, skipped\n", path.c_str());
            ++st.errors;
            return;
        }
        DIR* d = opendir(path.c_str());
        if (!d) {
            std::fprintf(stderr, "difiw: del: %s: %s\n", path.c_str(), std::strerror(errno));
            ++st.errors;
            return;
        }
        std::vector<std::string> kids;
        while (struct dirent* e = readdir(d)) {
            if (!std::strcmp(e->d_name, ".") || !std::strcmp(e->d_name, "..")) continue;
            kids.push_back(e->d_name);
        }
        closedir(d);
        for (const auto& k : kids) remove_tree(join_path(path, k), depth + 1, st);
        if (rmdir(path.c_str()) == 0) ++st.removed;
        else { std::fprintf(stderr, "difiw: del: %s: %s\n", path.c_str(), std::strerror(errno)); ++st.errors; }
    } else if (unlink(path.c_str()) == 0) {
        ++st.removed;
    } else {
        std::fprintf(stderr, "difiw: del: %s: %s\n", path.c_str(), std::strerror(errno));
        ++st.errors;
    }
}

int cmd_del(const std::vector<std::string>& args) {
    std::set<char> flags;
    std::vector<std::string> rest;
    if (!take_flags("del", args, "ry", flags, rest)) return 2;
    if (rest.size() != 1) return bad_usage("del", "del [-r [-y]] <path>");
    if (rest[0].empty()) {
        std::fprintf(stderr, "difiw: del: empty path refused\n");
        return 2;
    }
    const std::string path = resolve(rest[0]);
    if (is_protected_path(path) || protected_by_realpath(path)) {
        std::fprintf(stderr, "difiw: del: %s: protected system path, refusing to delete\n", path.c_str());
        return 3;
    }
    if (g_cwd == path || g_cwd.compare(0, path.size() + 1, path + "/") == 0) {
        std::fprintf(stderr, "difiw: del: %s: this is the current directory (or contains it); cd elsewhere first\n", path.c_str());
        return 3;
    }
    struct stat sb;
    if (lstat(path.c_str(), &sb) != 0) return fail("del", path, errno);
    if (!S_ISDIR(sb.st_mode)) {
        if (unlink(path.c_str()) != 0) return fail("del", path, errno);
        std::printf("deleted %s\n", path.c_str());
        return 0;
    }
    if (!flags.count('r')) {
        if (rmdir(path.c_str()) != 0) {
            if (errno == ENOTEMPTY)
                std::fprintf(stderr, "difiw: del: %s: directory not empty (use 'del -r' to delete it with contents)\n", path.c_str());
            else
                return fail("del", path, errno);
            return 1;
        }
        std::printf("deleted %s\n", path.c_str());
        return 0;
    }
    SizeStats count;
    std::set<std::pair<dev_t, ino_t>> seen;
    walk_size(path, count, 0, seen);
    if (!flags.count('y')) {
        if (!isatty(STDIN_FILENO)) {
            std::fprintf(stderr, "difiw: del: -r needs confirmation; add -y in non-interactive use\n");
            return 3;
        }
        std::printf("Delete %s with %llu files and %llu subdirectories? [y/N] ", path.c_str(),
                    static_cast<unsigned long long>(count.files),
                    static_cast<unsigned long long>(count.dirs > 0 ? count.dirs - 1 : 0));
        std::fflush(stdout);
        char ans[16];
        if (!std::fgets(ans, sizeof ans, stdin) || (ans[0] != 'y' && ans[0] != 'Y')) {
            std::printf("cancelled\n");
            return 1;
        }
    }
    DelStats st;
    remove_tree(path, 0, st);
    std::printf("deleted %llu objects%s\n", static_cast<unsigned long long>(st.removed),
                st.errors ? " (with errors)" : "");
    return st.errors ? 1 : 0;
}

int power_action(const char* cmd, int what, const char* msg) {
    if (geteuid() != 0) {
        std::fprintf(stderr, "difiw: %s: permission denied (root required)\n", cmd);
        return 1;
    }
    std::printf("%s\n", msg);
    std::fflush(stdout);
    sync();
    if (reboot(what) != 0) return fail(cmd, "reboot syscall", errno);
    return 0;
}

int cmd_reboot(const std::vector<std::string>& args) {
    if (args.size() != 1) return bad_usage("reboot", "reboot");
    return power_action("reboot", RB_AUTOBOOT, "Syncing disks... rebooting.");
}

int cmd_poweroff(const std::vector<std::string>& args) {
    if (args.size() != 1) return bad_usage("poweroff", "poweroff");
    return power_action("poweroff", RB_POWER_OFF, "Syncing disks... powering off.");
}

int cmd_echo(const std::vector<std::string>& args) {
    for (std::size_t i = 1; i < args.size(); ++i) std::printf("%s%s", i > 1 ? " " : "", args[i].c_str());
    std::printf("\n");
    return 0;
}

int cmd_exit(const std::vector<std::string>&) { return kExitShell; }

struct Builtin {
    const char* name;
    const char* usage;
    const char* summary;
    int (*fn)(const std::vector<std::string>&);
};

const Builtin kBuiltins[] = {
    {"help", "help [command]", "show this list or details of one command", cmd_help},
    {"time", "time", "current time, e.g. 14:03:22 (UTC+00:00)", cmd_time},
    {"date", "date", "current date, e.g. 2026-10-30, Friday", cmd_date},
    {"info", "info", "system, kernel, uptime and storage mode", cmd_info},
    {"mem", "mem", "RAM total / used / available", cmd_mem},
    {"disk", "disk [path]", "filesystem capacity of a path (default: current dir)", cmd_disk},
    {"size", "size <path>", "size of a file or recursive size of a directory", cmd_size},
    {"ls", "ls [-a] [path]", "list a directory (-a shows hidden files)", cmd_ls},
    {"cd", "cd [path]", "change directory (no argument: home)", cmd_cd},
    {"pwd", "pwd", "print the current directory", cmd_pwd},
    {"read", "read <file>", "print a text file (max 64 KiB, no binaries)", cmd_read},
    {"clear", "clear", "clear the terminal", cmd_clear},
    {"mkdir", "mkdir [-p] <path>...", "create directories", cmd_mkdir},
    {"touch", "touch <path>...", "create files or update their time", cmd_touch},
    {"del", "del [-r [-y]] <path>", "delete a file or empty directory (-r: with contents)", cmd_del},
    {"echo", "echo [text...]", "print text", cmd_echo},
    {"reboot", "reboot", "sync and restart the machine", cmd_reboot},
    {"poweroff", "poweroff", "sync and power off the machine", cmd_poweroff},
    {"exit", "exit", "close this session (a new one starts)", cmd_exit},
};

int cmd_help(const std::vector<std::string>& args) {
    if (args.size() > 2) return bad_usage("help", "help [command]");
    if (args.size() == 2) {
        for (const auto& b : kBuiltins) {
            if (args[1] == b.name) {
                std::printf("%s\n  usage: %s\n", b.summary, b.usage);
                return 0;
            }
        }
        std::fprintf(stderr, "difiw: help: no such command: %s\n", args[1].c_str());
        return 1;
    }
    std::printf("Difiw Shell %s - built-in commands:\n", kVersion);
    for (const auto& b : kBuiltins) std::printf("  %-22s %s\n", b.usage, b.summary);
    std::printf("Other programs from PATH are started as external commands.\n");
    std::printf("Quotes: 'single' and \"double\"; no pipes or redirects in 0.1.0.\n");
    return 0;
}

int run_external(const std::vector<std::string>& args) {
    std::vector<char*> argv;
    for (const auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);
    std::fflush(stdout);
    const pid_t pid = fork();
    if (pid < 0) return fail(args[0].c_str(), "fork", errno);
    if (pid == 0) {
        signal(SIGINT, SIG_DFL);
        signal(SIGQUIT, SIG_DFL);
        signal(SIGTSTP, SIG_DFL);
        execvp(argv[0], argv.data());
        const int e = errno;
        if (e == ENOENT)
            std::fprintf(stderr, "difiw: %s: command not found (type 'help')\n", args[0].c_str());
        else
            std::fprintf(stderr, "difiw: %s: %s\n", args[0].c_str(), std::strerror(e));
        _exit(e == ENOENT ? 127 : 126);
    }
    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) return fail(args[0].c_str(), "waitpid", errno);
    }
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return 1;
}

}  // namespace

void init_state() {
    char buf[PATH_MAX];
    if (getcwd(buf, sizeof buf)) g_cwd = buf;
    else g_cwd = "/";
}

std::string current_dir() { return g_cwd; }

void print_banner() {
    struct utsname u;
    struct sysinfo si;
    const bool ok = uname(&u) == 0;
    std::printf("\n");
    std::printf("  ____  _  __ _        ___  ____\n");
    std::printf(" |  _ \\(_)/ _(_)_    _/ _ \\/ ___|\n");
    std::printf(" | | | | | |_| \\ \\ /\\ / | | \\___ \\\n");
    std::printf(" | |_| | |  _| |\\ V  V /| |_| |___) |\n");
    std::printf(" |____/|_|_| |_| \\_/\\_/  \\___/|____/\n");
    std::printf("\n Difiw OS %s %s\n", kVersion, kEdition);
    if (ok) std::printf(" Kernel %s %s | host %s\n", u.sysname, u.release, u.nodename);
    if (sysinfo(&si) == 0) {
        const std::uint64_t unit = si.mem_unit ? si.mem_unit : 1;
        std::printf(" RAM %s | uptime %s\n", human_size(si.totalram * unit).c_str(),
                    format_duration(si.uptime).c_str());
    }
    std::printf(" Storage: RAM only; files are lost on reboot%s\n",
                is_mountpoint_used("/data") ? " (except /data)" : "");
    std::printf(" Type 'help' for the command list.\n\n");
}

int run_command(const std::vector<std::string>& args) {
    if (args.empty()) return 0;
    for (const auto& b : kBuiltins)
        if (args[0] == b.name) return b.fn(args);
    return run_external(args);
}

}  // namespace difiw
