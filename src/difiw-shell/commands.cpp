// SPDX-License-Identifier: MIT
#include "commands.hpp"

#include <dirent.h>
#include <fcntl.h>
#include <linux/kd.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/sysinfo.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

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
constexpr const char* kLangFile = "/run/difiw-lang";

enum class Lang { En, Ru };

std::string g_cwd = "/";
Lang g_lang = Lang::En;

// Picks the string for the current interface language.
const char* L(const char* en, const char* ru) { return g_lang == Lang::Ru ? ru : en; }

std::string home_dir() {
    const char* h = std::getenv("HOME");
    return (h && *h == '/') ? h : "/root";
}

std::string resolve(const std::string& p) { return normalize_path(g_cwd, p, home_dir()); }

std::string errtext(int e) {
    if (g_lang == Lang::Ru) {
        switch (e) {
            case ENOENT: return "Нет такого файла или каталога";
            case EEXIST: return "Уже существует";
            case ENOTDIR: return "Не является каталогом";
            case EISDIR: return "Это каталог";
            case EACCES:
            case EPERM: return "Отказано в доступе";
            case ENOTEMPTY: return "Каталог не пуст";
            case ENOSPC: return "Нет места на устройстве";
            case EROFS: return "Файловая система только для чтения";
            case ENAMETOOLONG: return "Слишком длинное имя";
            default: break;
        }
    }
    return std::strerror(e);
}

int fail(const char* cmd, const std::string& what, int err) {
    std::fprintf(stderr, "difiw: %s: %s: %s\n", cmd, what.c_str(), errtext(err).c_str());
    return 1;
}

const char* usage_for(const char* name, const char* fallback);

int bad_usage(const char* cmd, const char* usage) {
    std::fprintf(stderr, L("difiw: %s: invalid arguments\nusage: %s\n",
                           "difiw: %s: неверные аргументы\nформат: %s\n"),
                 cmd, usage_for(cmd, usage));
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
                std::fprintf(stderr, L("difiw: %s: unknown option '-%c'\n",
                                       "difiw: %s: неизвестный параметр '-%c'\n"),
                             cmd, a[k]);
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

std::string size_text(std::uint64_t bytes) {
    std::string s = human_size(bytes);
    if (g_lang == Lang::Ru) {
        for (const auto& [from, to] : {std::pair<const char*, const char*>{"KiB", "КиБ"},
                                       {"MiB", "МиБ"}, {"GiB", "ГиБ"}, {"TiB", "ТиБ"},
                                       {"PiB", "ПиБ"}, {"EiB", "ЭиБ"}, {" B", " Б"}}) {
            const std::size_t p = s.rfind(from);
            if (p != std::string::npos && p + std::strlen(from) == s.size()) {
                s.replace(p, std::strlen(from), to);
                break;
            }
        }
    }
    return s;
}

std::string size_bytes_text(std::uint64_t bytes) {
    if (bytes < 1024) return size_text(bytes);
    return size_text(bytes) + " (" + std::to_string(bytes) + (g_lang == Lang::Ru ? " Б)" : " B)");
}

// ----------------------------------------------------- language / keyboard

bool is_vt(int fd) {
    char t = 0;
    return ioctl(fd, KDGKBTYPE, &t) == 0 && (t == KB_101 || t == KB_84);
}

// Runs a helper program (setfont, loadkeys, ...) silently. True on exit status 0.
bool run_quiet(const std::vector<const char*>& argv_in) {
    std::vector<char*> argv;
    for (const char* a : argv_in) argv.push_back(const_cast<char*>(a));
    argv.push_back(nullptr);
    const pid_t pid = fork();
    if (pid < 0) return false;
    if (pid == 0) {
        const int nul = open("/dev/null", O_WRONLY);
        if (nul >= 0) { dup2(nul, STDOUT_FILENO); dup2(nul, STDERR_FILENO); }
        execvp(argv[0], argv.data());
        _exit(127);
    }
    int status = 0;
    while (waitpid(pid, &status, 0) < 0)
        if (errno != EINTR) return false;
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

void save_lang() {
    FILE* f = std::fopen(kLangFile, "w");
    if (!f) return;
    std::fputs(g_lang == Lang::Ru ? "ru\n" : "en\n", f);
    std::fclose(f);
}

void load_lang() {
    const char* env = std::getenv("DIFIW_LANG");
    char buf[16] = {0};
    if (env && *env) {
        std::strncpy(buf, env, sizeof buf - 1);
    } else if (FILE* f = std::fopen(kLangFile, "r")) {
        if (!std::fgets(buf, sizeof buf, f)) buf[0] = 0;
        std::fclose(f);
    }
    g_lang = std::strncmp(buf, "ru", 2) == 0 ? Lang::Ru : Lang::En;
}

// (Re)loads the Cyrillic font and the Latin/Russian keymap (Alt+Shift toggles the layout).
// Only on a Linux VT (VGA console). `tried` is false when this terminal is not a VT.
bool apply_console(bool& tried) {
    tried = is_vt(STDIN_FILENO);
    return tried && run_quiet({"/usr/libexec/difiw-console"});
}

void set_utf8_input() {
    struct termios t;
    if (tcgetattr(STDIN_FILENO, &t) == 0 && !(t.c_iflag & IUTF8)) {
        t.c_iflag |= IUTF8;  // let the line discipline erase whole UTF-8 characters
        tcsetattr(STDIN_FILENO, TCSANOW, &t);
    }
}

int set_language(Lang lang) {
    g_lang = lang;
    save_lang();
    set_utf8_input();
    bool tried = false;
    const bool ok = apply_console(tried);
    if (lang == Lang::Ru) {
        std::printf("Язык интерфейса: русский.\n");
        if (ok) std::printf("Клавиатура: Alt+Shift переключает латиницу и русскую раскладку (ЙЦУКЕН; T = е).\n"
                            "Команды набирайте латиницей, текст - по-русски: echo <Alt+Shift> тест\n");
        else if (tried) std::printf("Не удалось загрузить шрифт или раскладку (см. README).\n");
        else std::printf("Раскладка переключается только на консоли VGA; в этом терминале действует раскладка вашего компьютера.\n");
    } else {
        std::printf("Interface language: English.\n");
        if (ok) std::printf("Keyboard: Alt+Shift toggles Latin and Russian layouts (JCUKEN; T = е).\n"
                            "Type commands in Latin, text in Russian: echo <Alt+Shift> text\n");
        else if (tried) std::printf("Could not load the keyboard layout (see README).\n");
        else std::printf("The layout can only be switched on the VGA console; this terminal uses your host layout.\n");
    }
    return tried && !ok ? 1 : 0;
}

int cmd_lang(const std::vector<std::string>& args) {
    if (args.size() == 1) {
        std::printf(L("Language: %s (usage: lang en|ru, or just: en / ru)\n",
                      "Язык: %s (формат: lang en|ru, либо просто: en / ru)\n"),
                    g_lang == Lang::Ru ? "ru" : "en");
        return 0;
    }
    if (args.size() == 2 && (args[1] == "en" || args[1] == "ru"))
        return set_language(args[1] == "ru" ? Lang::Ru : Lang::En);
    return bad_usage("lang", "lang [en|ru]");
}

int cmd_en(const std::vector<std::string>& args) {
    if (args.size() != 1) return bad_usage("en", "en");
    return set_language(Lang::En);
}

int cmd_ru(const std::vector<std::string>& args) {
    if (args.size() != 1) return bad_usage("ru", "ru");
    return set_language(Lang::Ru);
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
    static const char* const wd_en[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
    static const char* const wd_ru[] = {"воскресенье", "понедельник", "вторник", "среда", "четверг", "пятница", "суббота"};
    char buf[16];
    std::strftime(buf, sizeof buf, "%Y-%m-%d", &tm);
    const int w = tm.tm_wday >= 0 && tm.tm_wday < 7 ? tm.tm_wday : 0;
    std::printf("%s, %s\n", buf, g_lang == Lang::Ru ? wd_ru[w] : wd_en[w]);
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
    std::printf(L("  Kernel:       %s %s (%s)\n", "  Ядро:         %s %s (%s)\n"), u.sysname, u.release, u.machine);
    std::printf(L("  Hostname:     %s\n", "  Имя узла:     %s\n"), u.nodename);
    if (have_si) std::printf(L("  Uptime:       %s\n", "  Время работы: %s\n"), format_duration(si.uptime).c_str());
    std::printf(L("  Shell:        Difiw Shell %s\n", "  Оболочка:     Difiw Shell %s\n"), kVersion);
    std::printf(L("  Terminal:     %s\n", "  Терминал:     %s\n"), tty ? tty : L("(not a terminal)", "(не терминал)"));
    std::printf(L("  Language:     %s\n", "  Язык:         %s\n"), g_lang == Lang::Ru ? "ru" : "en");
    std::printf("%s", L("  Root FS:      RAM (initramfs); changes are lost on reboot\n",
                        "  Корень ФС:    RAM (initramfs); изменения пропадают после перезагрузки\n"));
    if (is_mountpoint_used("/data"))
        std::printf("%s", L("  Persistence:  /data is mounted (disk labelled DIFIWDATA)\n",
                            "  Хранение:     /data подключён (диск с меткой DIFIWDATA)\n"));
    else
        std::printf("%s", L("  Persistence:  none (no DIFIWDATA disk attached)\n",
                            "  Хранение:     нет (диск DIFIWDATA не подключён)\n"));
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
    std::printf("%s", L("Memory\n", "Память\n"));
    std::printf(L("  Total:      %s\n", "  Всего:      %s\n"), size_text(total).c_str());
    std::printf(L("  Used:       %s (%u%%)\n", "  Занято:     %s (%u%%)\n"), size_text(used).c_str(), percent(used, total));
    std::printf(L("  Available:  %s\n", "  Доступно:   %s\n"), size_text(avail).c_str());
    std::printf(L("  Free:       %s\n", "  Свободно:   %s\n"), size_text(si.freeram * unit).c_str());
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
        std::printf(L("Filesystem of %s\n  No fixed capacity is reported (RAM-backed filesystem).\n",
                      "Файловая система %s\n  Ёмкость не ограничена (файловая система в RAM).\n"),
                    path.c_str());
        return 0;
    }
    const std::uint64_t used = total - std::min<std::uint64_t>(total, static_cast<std::uint64_t>(sv.f_bfree) * sv.f_frsize);
    std::printf(L("Filesystem of %s\n", "Файловая система %s\n"), path.c_str());
    std::printf(L("  Total:  %s\n", "  Всего:    %s\n"), size_text(total).c_str());
    std::printf(L("  Used:   %s (%u%%)\n", "  Занято:   %s (%u%%)\n"), size_text(used).c_str(), percent(used, total));
    std::printf(L("  Free:   %s\n", "  Свободно: %s\n"), size_text(freeb).c_str());
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
        std::fprintf(stderr, "difiw: size: %s: %s\n", path.c_str(), errtext(errno).c_str());
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
        std::fprintf(stderr, L("difiw: size: %s: too deep, skipped\n", "difiw: size: %s: слишком глубоко, пропущено\n"), path.c_str());
        ++st.errors;
        return;
    }
    if (!seen.insert({sb.st_dev, sb.st_ino}).second) return;  // loop guard
    DIR* d = opendir(path.c_str());
    if (!d) {
        std::fprintf(stderr, "difiw: size: %s: %s\n", path.c_str(), errtext(errno).c_str());
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
        std::printf(L("%s: %s%s in %llu files, %llu directories", "%s: %s%s в файлах: %llu, каталогах: %llu"),
                    path.c_str(), st.overflow ? ">= " : "", size_bytes_text(st.bytes).c_str(),
                    static_cast<unsigned long long>(st.files),
                    static_cast<unsigned long long>(st.dirs > 0 ? st.dirs - 1 : 0));
        if (st.links)
            std::printf(L(", %llu symlinks (not followed)", ", символических ссылок: %llu (не обходятся)"),
                        static_cast<unsigned long long>(st.links));
        std::printf("\n");
    } else {
        std::printf("%s: %s\n", path.c_str(), size_bytes_text(st.bytes).c_str());
    }
    if (st.errors) {
        std::fprintf(stderr, L("difiw: size: %llu objects could not be read\n",
                               "difiw: size: не удалось прочитать объектов: %llu\n"),
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
        std::printf("%c  %10s  %s\n", t, size_text(static_cast<std::uint64_t>(sb.st_size)).c_str(), name.c_str());
    }
}

std::vector<std::string> list_names(DIR* d, bool all) {
    std::vector<std::string> names;
    while (struct dirent* e = readdir(d)) {
        if (!std::strcmp(e->d_name, ".") || !std::strcmp(e->d_name, "..")) continue;
        if (e->d_name[0] == '.' && !all) continue;
        names.push_back(e->d_name);
    }
    std::sort(names.begin(), names.end());
    return names;
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
    const auto names = list_names(d, flags.count('a') != 0);
    closedir(d);
    for (const auto& n : names) print_ls_entry(join_path(path, n), n);
    if (names.empty()) std::printf("%s\n", L("(empty)", "(пусто)"));
    return 0;
}

// Windows-style directory listing:  date  time  <DIR>|size  name
void print_dir_entry(const std::string& full, const std::string& name, std::uint64_t& files,
                     std::uint64_t& dirs, std::uint64_t& bytes) {
    struct stat sb;
    if (lstat(full.c_str(), &sb) != 0) return;
    char when[32];
    struct tm tm;
    localtime_r(&sb.st_mtime, &tm);
    std::strftime(when, sizeof when, "%Y-%m-%d  %H:%M", &tm);
    if (S_ISDIR(sb.st_mode)) {
        std::printf("%s    <DIR>          %s\n", when, name.c_str());
        ++dirs;
    } else {
        const auto sz = static_cast<std::uint64_t>(sb.st_size);
        std::printf("%s  %12llu  %s\n", when, static_cast<unsigned long long>(sz), name.c_str());
        ++files;
        bool ov = false;
        bytes = sat_add(bytes, sz, ov);
    }
}

int cmd_dir(const std::vector<std::string>& args) {
    std::set<char> flags;
    std::vector<std::string> rest;
    if (!take_flags("dir", args, "a", flags, rest)) return 2;
    if (rest.size() > 1) return bad_usage("dir", "dir [-a] [path]");
    const std::string path = rest.empty() ? g_cwd : resolve(rest[0]);
    struct stat sb;
    if (stat(path.c_str(), &sb) != 0) return fail("dir", path, errno);
    if (!S_ISDIR(sb.st_mode)) return fail("dir", path, ENOTDIR);
    DIR* d = opendir(path.c_str());
    if (!d) return fail("dir", path, errno);
    auto names = list_names(d, flags.count('a') != 0);
    closedir(d);

    std::vector<std::string> dirs_v, files_v;
    for (const auto& n : names) {
        struct stat s2;
        if (lstat(join_path(path, n).c_str(), &s2) == 0 && S_ISDIR(s2.st_mode)) dirs_v.push_back(n);
        else files_v.push_back(n);
    }
    std::printf(L("\n Directory of %s\n\n", "\n Каталог: %s\n\n"), path.c_str());
    std::uint64_t files = 0, dirs = 0, bytes = 0;
    if (path != "/") {
        print_dir_entry(path, ".", files, dirs, bytes);
        print_dir_entry(join_path(path, ".."), "..", files, dirs, bytes);
    }
    for (const auto& n : dirs_v) print_dir_entry(join_path(path, n), n, files, dirs, bytes);
    for (const auto& n : files_v) print_dir_entry(join_path(path, n), n, files, dirs, bytes);
    const std::uint64_t subdirs = path != "/" ? dirs - 2 : dirs;
    std::printf(L("%16llu File(s)  %s\n", "%16llu файл(ов)  %s\n"), static_cast<unsigned long long>(files),
                size_text(bytes).c_str());
    struct statvfs sv;
    if (statvfs(path.c_str(), &sv) == 0 && sv.f_blocks != 0)
        std::printf(L("%16llu Dir(s)   %s free\n", "%16llu каталог(ов)  свободно: %s\n"),
                    static_cast<unsigned long long>(subdirs),
                    size_text(static_cast<std::uint64_t>(sv.f_bavail) * sv.f_frsize).c_str());
    else
        std::printf(L("%16llu Dir(s)\n", "%16llu каталог(ов)\n"), static_cast<unsigned long long>(subdirs));
    return 0;
}

int cmd_cd(const std::vector<std::string>& args_in) {
    std::vector<std::string> args = args_in;
    if (args.size() >= 2 && (args[1] == "/d" || args[1] == "/D")) args.erase(args.begin() + 1);  // Windows habit
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
        std::fprintf(stderr, L("difiw: read: %s: not a regular file\n", "difiw: read: %s: не обычный файл\n"), path.c_str());
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
        std::fprintf(stderr, L("difiw: read: %s: looks like a binary file, not shown\n",
                               "difiw: read: %s: похоже на двоичный файл, не показан\n"), path.c_str());
        return 1;
    }
    std::fwrite(buf.data(), 1, got, stdout);
    if (got && buf[got - 1] != '\n') std::fputc('\n', stdout);
    if (truncated)
        std::printf(L("[output truncated at %zu bytes; file is %s]\n", "[вывод обрезан на %zu байтах; размер файла %s]\n"),
                    kReadLimit, size_bytes_text(static_cast<std::uint64_t>(sb.st_size)).c_str());
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
            std::fprintf(stderr, L("difiw: del: %s: too deep, skipped\n", "difiw: del: %s: слишком глубоко, пропущено\n"), path.c_str());
            ++st.errors;
            return;
        }
        DIR* d = opendir(path.c_str());
        if (!d) {
            std::fprintf(stderr, "difiw: del: %s: %s\n", path.c_str(), errtext(errno).c_str());
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
        else { std::fprintf(stderr, "difiw: del: %s: %s\n", path.c_str(), errtext(errno).c_str()); ++st.errors; }
    } else if (unlink(path.c_str()) == 0) {
        ++st.removed;
    } else {
        std::fprintf(stderr, "difiw: del: %s: %s\n", path.c_str(), errtext(errno).c_str());
        ++st.errors;
    }
}

int cmd_del(const std::vector<std::string>& args) {
    std::set<char> flags;
    std::vector<std::string> rest;
    if (!take_flags("del", args, "ry", flags, rest)) return 2;
    if (rest.size() != 1) return bad_usage("del", "del [-r [-y]] <path>");
    if (rest[0].empty()) {
        std::fprintf(stderr, "%s", L("difiw: del: empty path refused\n", "difiw: del: пустой путь отклонён\n"));
        return 2;
    }
    const std::string path = resolve(rest[0]);
    if (is_protected_path(path) || protected_by_realpath(path)) {
        std::fprintf(stderr, L("difiw: del: %s: protected system path, refusing to delete\n",
                               "difiw: del: %s: защищённый системный путь, удаление отклонено\n"), path.c_str());
        return 3;
    }
    if (g_cwd == path || g_cwd.compare(0, path.size() + 1, path + "/") == 0) {
        std::fprintf(stderr, L("difiw: del: %s: this is the current directory (or contains it); cd elsewhere first\n",
                               "difiw: del: %s: это текущий каталог (или он внутри); сначала перейдите (cd) в другой\n"), path.c_str());
        return 3;
    }
    struct stat sb;
    if (lstat(path.c_str(), &sb) != 0) return fail("del", path, errno);
    if (!S_ISDIR(sb.st_mode)) {
        if (unlink(path.c_str()) != 0) return fail("del", path, errno);
        std::printf(L("deleted %s\n", "удалено: %s\n"), path.c_str());
        return 0;
    }
    if (!flags.count('r')) {
        if (rmdir(path.c_str()) != 0) {
            if (errno == ENOTEMPTY)
                std::fprintf(stderr, L("difiw: del: %s: directory not empty (use 'del -r' to delete it with contents)\n",
                                       "difiw: del: %s: каталог не пуст (используйте 'del -r', чтобы удалить с содержимым)\n"), path.c_str());
            else
                return fail("del", path, errno);
            return 1;
        }
        std::printf(L("deleted %s\n", "удалено: %s\n"), path.c_str());
        return 0;
    }
    SizeStats count;
    std::set<std::pair<dev_t, ino_t>> seen;
    walk_size(path, count, 0, seen);
    if (!flags.count('y')) {
        if (!isatty(STDIN_FILENO)) {
            std::fprintf(stderr, "%s", L("difiw: del: -r needs confirmation; add -y in non-interactive use\n",
                                         "difiw: del: для -r нужно подтверждение; без терминала добавьте -y\n"));
            return 3;
        }
        std::printf(L("Delete %s with %llu files and %llu subdirectories? [y/N] ",
                      "Удалить %s вместе с файлами (%llu) и подкаталогами (%llu)? [y/N] "), path.c_str(),
                    static_cast<unsigned long long>(count.files),
                    static_cast<unsigned long long>(count.dirs > 0 ? count.dirs - 1 : 0));
        std::fflush(stdout);
        char ans[16];
        if (!std::fgets(ans, sizeof ans, stdin) || (ans[0] != 'y' && ans[0] != 'Y')) {
            std::printf("%s\n", L("cancelled", "отменено"));
            return 1;
        }
    }
    DelStats st;
    remove_tree(path, 0, st);
    std::printf(L("deleted %llu objects%s\n", "удалено объектов: %llu%s\n"), static_cast<unsigned long long>(st.removed),
                st.errors ? L(" (with errors)", " (с ошибками)") : "");
    return st.errors ? 1 : 0;
}

int power_action(const char* cmd, int what, const char* msg) {
    if (geteuid() != 0) {
        std::fprintf(stderr, L("difiw: %s: permission denied (root required)\n",
                               "difiw: %s: отказано в доступе (нужен root)\n"), cmd);
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
    return power_action("reboot", RB_AUTOBOOT, L("Syncing disks... rebooting.", "Запись данных на диск... перезагрузка."));
}

int cmd_poweroff(const std::vector<std::string>& args) {
    if (args.size() != 1) return bad_usage("poweroff", "poweroff");
    return power_action("poweroff", RB_POWER_OFF, L("Syncing disks... powering off.", "Запись данных на диск... выключение."));
}

int cmd_echo(const std::vector<std::string>& args) {
    for (std::size_t i = 1; i < args.size(); ++i) std::printf("%s%s", i > 1 ? " " : "", args[i].c_str());
    std::printf("\n");
    return 0;
}

int cmd_ver(const std::vector<std::string>& args) {
    if (args.size() != 1) return bad_usage("ver", "ver");
    std::printf("Difiw OS %s %s (Difiw Shell %s)\n", kVersion, kEdition, kVersion);
    return 0;
}

int cmd_exit(const std::vector<std::string>&) { return kExitShell; }

int run_external(const std::vector<std::string>& args);

// `screen`: start the graphical shell; the text console is restored when it exits.
int cmd_screen(const std::vector<std::string>& args) {
    const bool simple = args.size() == 2 && args[1] == "--simple";
    if (args.size() != 1 && !simple) return bad_usage("screen", "screen [--simple]");
    if (access("/dev/fb0", F_OK) != 0) {
        std::fprintf(stderr, "%s", L("difiw: screen: no graphics (/dev/fb0): the bootloader did not set a graphics mode\n",
                                     "difiw: screen: нет графики (/dev/fb0): загрузчик не включил графический режим\n"));
        return 1;
    }
    const char* exe = simple ? "/usr/bin/difiw-gui" : "/usr/bin/difiw-desktop";
    if (access(exe, X_OK) != 0) {
        std::fprintf(stderr, L("difiw: screen: %s is missing\n", "difiw: screen: нет %s\n"), exe);
        return 1;
    }
    std::fflush(stdout);
    const int rc = run_external({exe});
    if (!simple && rc == 11) return power_action("reboot", RB_AUTOBOOT, L("Syncing disks... rebooting.", "Запись данных на диск... перезагрузка."));
    if (!simple && rc == 12) return power_action("poweroff", RB_POWER_OFF, L("Syncing disks... powering off.", "Запись данных на диск... выключение."));
    return rc;
}

// `terminal`: in the text console there is nothing to open; inside the GUI terminal it leaves the GUI.
int cmd_terminal(const std::vector<std::string>& args) {
    if (args.size() != 1) return bad_usage("terminal", "terminal");
    std::printf("%s\n", L("You are already in the text terminal. Type 'screen' for the graphical shell.",
                          "Вы уже в текстовом терминале. Команда 'screen' запускает графическую оболочку."));
    return 0;
}

struct Builtin {
    const char* name;
    const char* usage;
    const char* summary_en;
    const char* summary_ru;
    int (*fn)(const std::vector<std::string>&);
};

const Builtin kBuiltins[] = {
    {"help", "help [command]", "show this list or details of one command", "список команд или справка по одной", cmd_help},
    {"dir", "dir [-a] [path]", "Windows-style listing with dates and sizes", "список как в Windows: даты и размеры", cmd_dir},
    {"ls", "ls [-a] [path]", "compact list (-a shows hidden files)", "краткий список (-a: скрытые файлы)", cmd_ls},
    {"cd", "cd [path]", "change directory: cd .. / cd \\tmp / cd.. (no argument: home)", "сменить каталог: cd .. / cd \\tmp / cd.. (без аргумента: домашний)", cmd_cd},
    {"pwd", "pwd", "print the current directory", "показать текущий каталог", cmd_pwd},
    {"read", "read <file>", "print a text file (max 64 KiB, no binaries); alias: type", "показать текстовый файл (до 64 КиБ); псевдоним: type", cmd_read},
    {"mkdir", "mkdir [-p] <path>...", "create directories; alias: md", "создать каталоги; псевдоним: md", cmd_mkdir},
    {"touch", "touch <path>...", "create files or update their time", "создать файлы или обновить время", cmd_touch},
    {"del", "del [-r [-y]] <path>", "delete a file or empty directory (-r: with contents); rd for dirs", "удалить файл или пустой каталог (-r: с содержимым); rd для каталогов", cmd_del},
    {"size", "size <path>", "size of a file or recursive size of a directory", "размер файла или каталога (рекурсивно)", cmd_size},
    {"clear", "clear", "clear the terminal; alias: cls", "очистить экран; псевдоним: cls", cmd_clear},
    {"time", "time", "current time, e.g. 14:03:22 (UTC+00:00)", "текущее время, напр. 14:03:22 (UTC+00:00)", cmd_time},
    {"date", "date", "current date, e.g. 2026-10-30, Friday", "текущая дата, напр. 2026-10-30, пятница", cmd_date},
    {"info", "info", "system, kernel, uptime and storage mode", "система, ядро, время работы, режим хранения", cmd_info},
    {"mem", "mem", "RAM total / used / available", "память: всего / занято / доступно", cmd_mem},
    {"disk", "disk [path]", "filesystem capacity of a path", "ёмкость файловой системы пути", cmd_disk},
    {"lang", "lang [en|ru]", "show or set the language (also: en, ru)", "показать или задать язык (также: en, ru)", cmd_lang},
    {"en", "en", "switch messages to English (keyboard: Alt+Shift)", "сообщения на английском (клавиатура: Alt+Shift)", cmd_en},
    {"ru", "ru", "switch messages to Russian (keyboard: Alt+Shift)", "сообщения на русском (клавиатура: Alt+Shift)", cmd_ru},
    {"echo", "echo [text...]", "print text", "вывести текст", cmd_echo},
    {"ver", "ver", "print the Difiw OS version", "версия Difiw OS", cmd_ver},
    {"reboot", "reboot", "sync and restart the machine", "записать данные и перезагрузить", cmd_reboot},
    {"poweroff", "poweroff", "sync and power off the machine", "записать данные и выключить", cmd_poweroff},
    {"screen", "screen [--simple]", "start the graphical desktop (--simple: the old one-window GUI)", "запустить графический рабочий стол (--simple: прежняя оболочка в одном окне)", cmd_screen},
    {"terminal", "terminal", "text terminal; inside the graphical terminal it returns to the text console", "текстовый терминал; в терминале графической оболочки - возврат в текстовую консоль", cmd_terminal},
    {"exit", "exit", "close this session (a new one starts)", "закрыть сессию (запустится новая)", cmd_exit},
};

// Usage strings with Russian placeholders, e.g. "read <файл>".
const std::pair<const char*, const char*> kUsageRu[] = {
    {"help", "help [команда]"}, {"dir", "dir [-a] [путь]"}, {"ls", "ls [-a] [путь]"}, {"cd", "cd [путь]"},
    {"pwd", "pwd"}, {"read", "read <файл>"}, {"mkdir", "mkdir [-p] <путь>..."}, {"touch", "touch <путь>..."},
    {"del", "del [-r [-y]] <путь>"}, {"size", "size <путь>"}, {"clear", "clear"}, {"time", "time"},
    {"date", "date"}, {"info", "info"}, {"mem", "mem"}, {"disk", "disk [путь]"}, {"lang", "lang [en|ru]"},
    {"en", "en"}, {"ru", "ru"}, {"echo", "echo [текст...]"}, {"ver", "ver"}, {"reboot", "reboot"},
    {"poweroff", "poweroff"}, {"screen", "screen [--simple]"}, {"terminal", "terminal"}, {"exit", "exit"},
};

// Usage text of a built-in in the current language.
const char* usage_for(const char* name, const char* fallback) {
    for (const auto& b : kBuiltins)
        if (!std::strcmp(b.name, name)) {
            if (g_lang == Lang::Ru)
                for (const auto& [n, u] : kUsageRu)
                    if (!std::strcmp(n, name)) return u;
            return b.usage;
        }
    return fallback;
}

// Display width of a UTF-8 string (code points), for aligning columns.
std::size_t utf8_len(const char* s) {
    std::size_t n = 0;
    for (; *s; ++s)
        if ((static_cast<unsigned char>(*s) & 0xC0) != 0x80) ++n;
    return n;
}

// Windows-style aliases -> built-in command names.
const std::pair<const char*, const char*> kAliases[] = {
    {"cls", "clear"}, {"type", "read"}, {"md", "mkdir"}, {"chdir", "cd"}, {"rd", "del"}, {"rmdir", "del"},
};

int cmd_help(const std::vector<std::string>& args) {
    if (args.size() > 2) return bad_usage("help", "help [command]");
    if (args.size() == 2) {
        for (const auto& b : kBuiltins) {
            if (args[1] == b.name) {
                std::printf(L("%s\n  usage: %s\n", "%s\n  формат: %s\n"), L(b.summary_en, b.summary_ru),
                            usage_for(b.name, b.usage));
                std::printf("%s", L("  <x> required argument, [x] optional\n", "  <x> обязательный аргумент, [x] необязательный\n"));
                return 0;
            }
        }
        std::fprintf(stderr, L("difiw: help: no such command: %s\n", "difiw: help: нет такой команды: %s\n"), args[1].c_str());
        return 1;
    }
    std::printf(L("Difiw Shell %s - built-in commands:\n", "Difiw Shell %s - встроенные команды:\n"), kVersion);
    for (const auto& b : kBuiltins) {
        const char* u = usage_for(b.name, b.usage);
        const std::size_t w = utf8_len(u);
        std::printf("  %s%*s %s\n", u, w < 22 ? static_cast<int>(22 - w) : 0, "", L(b.summary_en, b.summary_ru));
    }
    std::printf("%s", L("<x> required argument, [x] optional.  Russian help: help ru   (any command: add --ru / --en)\n",
                        "<x> обязательный аргумент, [x] необязательный.  Справка на английском: help en   (любая команда: --ru / --en)\n"));
    std::printf("%s", L("Other programs from PATH are started as external commands.\n"
                        "Paths: / or \\ both work (cd \\tmp, dir ..\\etc). Quotes: 'single' and \"double\".\n"
                        "No pipes or redirects in this version.\n",
                        "Остальные программы из PATH запускаются как внешние команды.\n"
                        "Пути: подходят и / и \\ (cd \\tmp, dir ..\\etc). Кавычки: 'одинарные' и \"двойные\".\n"
                        "Конвейеров и перенаправлений в этой версии нет.\n"));
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
            std::fprintf(stderr, L("difiw: %s: command not found (type 'help')\n",
                                   "difiw: %s: команда не найдена (введите 'help')\n"), args[0].c_str());
        else
            std::fprintf(stderr, "difiw: %s: %s\n", args[0].c_str(), errtext(e).c_str());
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
    load_lang();
    if (g_lang == Lang::Ru) set_utf8_input();
}

std::string current_dir() { return g_cwd; }

const char* msg(const char* en, const char* ru) { return L(en, ru); }

void print_banner() {
    struct utsname u;
    struct sysinfo si;
    const bool ok = uname(&u) == 0;
    std::printf("\n");
    std::fputs(R"( ____  _  __ _
|  _ \(_)/ _(_)_      __
| | | | | |_| \ \ /\ / /
| |_| | |  _| |\ V  V /
|____/|_|_| |_| \_/\_/
)", stdout);
    std::printf("\n Difiw OS %s %s\n", kVersion, kEdition);
    if (ok) std::printf(L(" Kernel %s %s | host %s\n", " Ядро %s %s | узел %s\n"), u.sysname, u.release, u.nodename);
    if (sysinfo(&si) == 0) {
        const std::uint64_t unit = si.mem_unit ? si.mem_unit : 1;
        std::printf(L(" RAM %s | uptime %s\n", " ОЗУ %s | работает %s\n"), size_text(si.totalram * unit).c_str(),
                    format_duration(si.uptime).c_str());
    }
    std::printf(L(" Storage: RAM only; files are lost on reboot%s\n",
                  " Хранение: только RAM; файлы пропадают после перезагрузки%s\n"),
                is_mountpoint_used("/data") ? L(" (except /data)", " (кроме /data)") : "");
    std::printf("%s", L(" Type 'help' for the command list. Language: ru / en\n\n",
                        " Введите 'help' для списка команд. Язык: ru / en\n\n"));
}

int run_command(const std::vector<std::string>& args_in) {
    if (args_in.empty()) return 0;
    std::vector<std::string> args = args_in;

    // "cd.." / "cd\" / "dir\x" without a space, as typed on Windows.
    for (const char* base : {"cd", "dir"}) {
        const std::size_t n = std::strlen(base);
        if (args[0].size() > n && args[0].compare(0, n, base) == 0 &&
            (args[0][n] == '.' || args[0][n] == '\\' || args[0][n] == '/')) {
            args.insert(args.begin() + 1, args[0].substr(n));
            args[0] = base;
            break;
        }
    }
    for (const auto& [alias, target] : kAliases)
        if (args[0] == alias) { args[0] = target; break; }

    // One-off language for a single command: "--ru" / "--en" at the end of any built-in,
    // or a bare trailing "ru" / "en" for commands that take no path (help, info, mem, ...).
    const Lang saved_lang = g_lang;
    bool overridden = false;
    const bool lang_cmd = args[0] == "lang" || args[0] == "en" || args[0] == "ru";
    if (args.size() >= 2 && !lang_cmd) {
        const std::string last = args.back();
        const bool word = (last == "ru" || last == "en") &&
                          (args[0] == "help" || args[0] == "info" || args[0] == "mem" ||
                           args[0] == "time" || args[0] == "date" || args[0] == "ver");
        if (last == "--ru" || last == "--en" || word) {
            g_lang = last.back() == 'u' ? Lang::Ru : Lang::En;
            args.pop_back();
            overridden = true;
        }
    }

    int rc = -1;
    for (const auto& b : kBuiltins)
        if (args[0] == b.name) { rc = b.fn(args); break; }
    if (rc == -1) rc = run_external(args);
    if (overridden) g_lang = saved_lang;
    return rc;
}

}  // namespace difiw
