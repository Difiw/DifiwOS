// difiw-gui: the minimal graphical shell of Difiw OS (Linux framebuffer + evdev).
// Tabs: Files, Editor, Terminal; buttons for reboot / power off. No X11, no extra libraries.
// SPDX-License-Identifier: MIT
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <linux/kd.h>
#include <poll.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include "format.hpp"
#include "parser.hpp"

using namespace difiw;

namespace {

constexpr const char* kVersion = "0.3.0";
constexpr const char* kFontPath = "/usr/share/difiw/font.psf";
constexpr const char* kLangFile = "/run/difiw-lang";
constexpr std::size_t kMaxFile = 256 * 1024;

// ------------------------------------------------------------------ colors
constexpr std::uint32_t C_BG = 0x1b1f2b, C_BAR = 0x2a3042, C_PANEL = 0x232838, C_EDIT = 0x12151d,
                        C_TEXT = 0xe8ebf5, C_DIM = 0x98a1bd, C_ACCENT = 0x5b9bff, C_BTN = 0x38405c,
                        C_BTN_HOT = 0x4b5683, C_DANGER = 0xb8402f, C_DANGER_HOT = 0xd5523f,
                        C_SEL = 0x2f4f8f, C_OK = 0x59c27a, C_ERR = 0xf0746a, C_LINE = 0x3b4360;

struct Rect {
    int x = 0, y = 0, w = 0, h = 0;
    bool has(int px, int py) const { return px >= x && py >= y && px < x + w && py < y + h; }
};

// ---------------------------------------------------------------- graphics
int W = 1024, H = 768;
std::vector<std::uint32_t> ui;  // back buffer, 0x00RRGGBB
Rect clip;

struct Fb {
    int fd = -1;
    std::uint8_t* mem = nullptr;
    std::size_t len = 0;
    int stride = 0, bpp = 32;
    int ro = 16, go = 8, bo = 0, rl = 8, gl = 8, bl = 8;
    bool fast = false;
} fb;

inline void px(int x, int y, std::uint32_t c) {
    if (x >= clip.x && y >= clip.y && x < clip.x + clip.w && y < clip.y + clip.h) ui[static_cast<std::size_t>(y) * W + x] = c;
}

void fill(Rect r, std::uint32_t c) {
    const int x0 = std::max(r.x, clip.x), y0 = std::max(r.y, clip.y);
    const int x1 = std::min(r.x + r.w, clip.x + clip.w), y1 = std::min(r.y + r.h, clip.y + clip.h);
    for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x) ui[static_cast<std::size_t>(y) * W + x] = c;
}

void frame(Rect r, std::uint32_t c) {
    fill({r.x, r.y, r.w, 1}, c);
    fill({r.x, r.y + r.h - 1, r.w, 1}, c);
    fill({r.x, r.y, 1, r.h}, c);
    fill({r.x + r.w - 1, r.y, 1, r.h}, c);
}

// ------------------------------------------------------------------- UTF-8
std::uint32_t next_cp(const std::string& s, std::size_t& i) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    int n = 0;
    std::uint32_t cp = 0;
    if (c < 0x80) { ++i; return c; }
    if ((c & 0xE0) == 0xC0) { n = 1; cp = c & 0x1F; }
    else if ((c & 0xF0) == 0xE0) { n = 2; cp = c & 0x0F; }
    else if ((c & 0xF8) == 0xF0) { n = 3; cp = c & 0x07; }
    else { ++i; return 0xFFFD; }
    ++i;
    for (int k = 0; k < n; ++k) {
        if (i >= s.size() || (static_cast<unsigned char>(s[i]) & 0xC0) != 0x80) return 0xFFFD;
        cp = (cp << 6) | (static_cast<unsigned char>(s[i]) & 0x3F);
        ++i;
    }
    return cp;
}

std::size_t u8_next(const std::string& s, std::size_t i) {
    ++i;
    while (i < s.size() && (static_cast<unsigned char>(s[i]) & 0xC0) == 0x80) ++i;
    return std::min(i, s.size());
}

std::size_t u8_prev(const std::string& s, std::size_t i) {
    if (i == 0) return 0;
    --i;
    while (i > 0 && (static_cast<unsigned char>(s[i]) & 0xC0) == 0x80) --i;
    return i;
}

int u8_cols(const std::string& s, std::size_t upto) {
    int n = 0;
    for (std::size_t i = 0; i < upto && i < s.size(); i = u8_next(s, i)) ++n;
    return n;
}

std::size_t u8_at_col(const std::string& s, int col) {
    std::size_t i = 0;
    while (col > 0 && i < s.size()) { i = u8_next(s, i); --col; }
    return i;
}

// -------------------------------------------------------------------- font
struct Font {
    int w = 8, h = 16, bpr = 1, n = 0;
    std::vector<std::uint8_t> glyphs;
    std::unordered_map<std::uint32_t, int> map;
    bool ok = false;
} font;

std::uint32_t le32(const std::vector<std::uint8_t>& b, std::size_t o) {
    return b[o] | (b[o + 1] << 8) | (b[o + 2] << 16) | (static_cast<std::uint32_t>(b[o + 3]) << 24);
}

bool load_font(const char* path) {
    FILE* f = std::fopen(path, "rb");
    if (!f) return false;
    std::vector<std::uint8_t> b;
    std::uint8_t tmp[4096];
    std::size_t n;
    while ((n = std::fread(tmp, 1, sizeof tmp, f)) > 0) b.insert(b.end(), tmp, tmp + n);
    std::fclose(f);
    if (b.size() > 4 && b[0] == 0x36 && b[1] == 0x04) {  // PSF1
        const int mode = b[2];
        font.h = b[3];
        font.w = 8;
        font.bpr = 1;
        font.n = (mode & 1) ? 512 : 256;
        const std::size_t gl = static_cast<std::size_t>(font.n) * font.h;
        if (b.size() < 4 + gl) return false;
        font.glyphs.assign(b.begin() + 4, b.begin() + 4 + static_cast<std::ptrdiff_t>(gl));
        if (mode & 2) {
            std::size_t p = 4 + gl;
            for (int g = 0; g < font.n && p + 1 < b.size(); ++g) {
                bool seq = false;
                while (p + 1 < b.size()) {
                    const unsigned v = b[p] | (b[p + 1] << 8);
                    p += 2;
                    if (v == 0xFFFF) break;
                    if (v == 0xFFFE) { seq = true; continue; }
                    if (!seq && !font.map.count(v)) font.map[v] = g;
                }
            }
        }
        font.ok = true;
        return true;
    }
    if (b.size() > 32 && b[0] == 0x72 && b[1] == 0xb5 && b[2] == 0x4a && b[3] == 0x86) {  // PSF2
        const std::uint32_t hs = le32(b, 8), flags = le32(b, 12), len = le32(b, 16), cs = le32(b, 20);
        font.h = static_cast<int>(le32(b, 24));
        font.w = static_cast<int>(le32(b, 28));
        font.bpr = (font.w + 7) / 8;
        font.n = static_cast<int>(len);
        const std::size_t gl = static_cast<std::size_t>(len) * cs;
        if (b.size() < hs + gl || cs != static_cast<std::uint32_t>(font.bpr * font.h)) return false;
        font.glyphs.assign(b.begin() + hs, b.begin() + static_cast<std::ptrdiff_t>(hs + gl));
        if (flags & 1) {
            std::size_t p = hs + gl;
            for (int g = 0; g < font.n && p < b.size(); ++g) {
                bool seq = false;
                while (p < b.size() && b[p] != 0xFF) {
                    if (b[p] == 0xFE) { seq = true; ++p; continue; }
                    std::string s;
                    while (p < b.size() && b[p] != 0xFF && b[p] != 0xFE) s += static_cast<char>(b[p++]);
                    std::size_t i = 0;
                    const std::uint32_t cp = next_cp(s, i);
                    if (!seq && !font.map.count(cp)) font.map[cp] = g;
                }
                ++p;
            }
        }
        font.ok = true;
        return true;
    }
    return false;
}

int glyph_for(std::uint32_t cp) {
    auto it = font.map.find(cp);
    if (it != font.map.end()) return it->second;
    if (font.map.empty() && cp < static_cast<std::uint32_t>(font.n)) return static_cast<int>(cp);
    it = font.map.find('?');
    return it != font.map.end() ? it->second : 0;
}

int text_w(const std::string& s, int scale = 1) { return u8_cols(s, s.size()) * font.w * scale; }

int draw_text(int x, int y, const std::string& s, std::uint32_t col, int scale = 1) {
    std::size_t i = 0;
    while (i < s.size()) {
        const std::uint32_t cp = next_cp(s, i);
        if (cp != ' ' && font.ok && x + font.w * scale > clip.x && x < clip.x + clip.w) {
            const std::uint8_t* g = &font.glyphs[static_cast<std::size_t>(glyph_for(cp)) * font.bpr * font.h];
            for (int row = 0; row < font.h; ++row)
                for (int cx = 0; cx < font.w; ++cx)
                    if (g[row * font.bpr + cx / 8] & (0x80 >> (cx % 8)))
                        for (int sy = 0; sy < scale; ++sy)
                            for (int sx = 0; sx < scale; ++sx) px(x + cx * scale + sx, y + row * scale + sy, col);
        }
        x += font.w * scale;
    }
    return x;
}

std::string fit(const std::string& s, int maxcols) {
    if (u8_cols(s, s.size()) <= maxcols) return s;
    if (maxcols < 2) return "";
    return s.substr(0, u8_at_col(s, maxcols - 1)) + "~";
}

// ------------------------------------------------------------- state / i18n
struct Entry {
    std::string name;
    bool dir = false;
    std::uint64_t size = 0;
};

struct Hit {
    Rect r;
    std::function<void()> fn;
};

struct App {
    int tab = 0;  // 0 files, 1 editor, 2 terminal
    bool ru = true;       // interface language
    bool layout_ru = false;  // keyboard layout
    bool shift = false, ctrl = false, alt = false, caps = false;
    bool quit = false;
    std::string status;
    std::uint32_t status_col = C_DIM;

    std::string dir = "/root";
    std::vector<Entry> ents;
    int sel = -1, scroll = 0;
    long last_click_ms = 0;
    int last_click_row = -1;

    std::string epath;
    std::vector<std::string> lines{""};
    int cy = 0, cx = 0, etop = 0, eleft = 0;
    bool dirty = false;

    std::vector<std::string> tout;
    std::string tin;
    std::vector<std::string> hist;
    int hidx = -1, tscroll = 0;

    int modal = 0;  // 0 none, 1 prompt, 2 confirm
    std::string mtitle, mtext, minput;
    std::function<void(const std::string&)> mok;

    int mx = 0, my = 0;
    std::vector<Hit> hits;
    int hover = -1;
} A;

const char* TR(const char* en, const char* ru) { return A.ru ? ru : en; }

void set_status(const std::string& s, std::uint32_t col = C_DIM) {
    A.status = s;
    A.status_col = col;
}

bool data_mounted() {
    FILE* f = std::fopen("/proc/mounts", "r");
    if (!f) return false;
    char line[512];
    bool found = false;
    while (std::fgets(line, sizeof line, f))
        if (std::strstr(line, " /data ")) { found = true; break; }
    std::fclose(f);
    return found;
}

void read_lang() {
    FILE* f = std::fopen(kLangFile, "r");
    if (!f) return;
    char buf[8] = {0};
    if (std::fgets(buf, sizeof buf, f)) A.ru = std::strncmp(buf, "ru", 2) == 0;
    std::fclose(f);
}

void write_lang() {
    FILE* f = std::fopen(kLangFile, "w");
    if (!f) return;
    std::fputs(A.ru ? "ru\n" : "en\n", f);
    std::fclose(f);
}

long now_ms() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

// -------------------------------------------------------------------- files
void refresh_dir() {
    A.ents.clear();
    A.sel = -1;
    A.scroll = 0;
    DIR* d = opendir(A.dir.c_str());
    if (!d) {
        set_status(std::string(TR("Cannot open folder: ", "Не удалось открыть папку: ")) + std::strerror(errno), C_ERR);
        return;
    }
    while (struct dirent* e = readdir(d)) {
        if (!std::strcmp(e->d_name, ".") || !std::strcmp(e->d_name, "..")) continue;
        Entry en;
        en.name = e->d_name;
        struct stat sb;
        const std::string full = join_path(A.dir, en.name);
        if (lstat(full.c_str(), &sb) == 0) {
            en.dir = S_ISDIR(sb.st_mode);
            en.size = static_cast<std::uint64_t>(sb.st_size);
        }
        A.ents.push_back(en);
    }
    closedir(d);
    std::sort(A.ents.begin(), A.ents.end(), [](const Entry& a, const Entry& b) {
        if (a.dir != b.dir) return a.dir;
        return a.name < b.name;
    });
}

void go_dir(const std::string& d) {
    struct stat sb;
    if (stat(d.c_str(), &sb) != 0 || !S_ISDIR(sb.st_mode)) {
        set_status(std::string(TR("No such folder: ", "Нет такой папки: ")) + d, C_ERR);
        return;
    }
    A.dir = d;
    refresh_dir();
}

std::string parent_of(const std::string& d) { return normalize_path(d, ".."); }

bool valid_name(const std::string& n) {
    return !n.empty() && n.size() <= 200 && n != "." && n != ".." && n.find('/') == std::string::npos &&
           n.find('\\') == std::string::npos;
}

// ------------------------------------------------------------------- editor
void editor_clear() {
    A.lines.assign(1, "");
    A.cy = A.cx = A.etop = A.eleft = 0;
    A.dirty = false;
}

bool open_file(const std::string& path) {
    struct stat sb;
    if (stat(path.c_str(), &sb) != 0 || !S_ISREG(sb.st_mode)) {
        set_status(TR("Not a regular file", "Это не обычный файл"), C_ERR);
        return false;
    }
    if (static_cast<std::size_t>(sb.st_size) > kMaxFile) {
        set_status(TR("File is too large (max 256 KiB)", "Файл слишком большой (макс. 256 КиБ)"), C_ERR);
        return false;
    }
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) {
        set_status(std::string(TR("Cannot open: ", "Не удалось открыть: ")) + std::strerror(errno), C_ERR);
        return false;
    }
    std::string data;
    char buf[4096];
    std::size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) data.append(buf, n);
    std::fclose(f);
    if (data.find('\0') != std::string::npos) {
        set_status(TR("Binary file, not opened", "Двоичный файл, не открыт"), C_ERR);
        return false;
    }
    editor_clear();
    A.lines.clear();
    std::string cur;
    for (char c : data) {
        if (c == '\n') { A.lines.push_back(cur); cur.clear(); }
        else if (c != '\r') cur += c;
    }
    if (!cur.empty() || A.lines.empty()) A.lines.push_back(cur);
    A.epath = path;
    A.tab = 1;
    set_status(std::string(TR("Opened ", "Открыт ")) + path, C_DIM);
    return true;
}

bool save_file() {
    if (A.epath.empty()) {
        set_status(TR("No file is open", "Файл не открыт"), C_ERR);
        return false;
    }
    std::string data;
    for (const auto& l : A.lines) { data += l; data += '\n'; }
    const int fd = open(A.epath.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) {
        set_status(std::string(TR("Save failed: ", "Не удалось сохранить: ")) + std::strerror(errno), C_ERR);
        return false;
    }
    std::size_t off = 0;
    while (off < data.size()) {
        const ssize_t w = write(fd, data.data() + off, data.size() - off);
        if (w < 0) {
            if (errno == EINTR) continue;
            const int e = errno;
            close(fd);
            set_status(std::string(TR("Save failed: ", "Не удалось сохранить: ")) + std::strerror(e), C_ERR);
            return false;
        }
        off += static_cast<std::size_t>(w);
    }
    close(fd);
    A.dirty = false;
    set_status(std::string(TR("Saved ", "Сохранено: ")) + A.epath, C_OK);
    return true;
}

// ----------------------------------------------------------------- dialogs
void ask_prompt(const std::string& title, const std::string& text, std::function<void(const std::string&)> ok) {
    A.modal = 1;
    A.mtitle = title;
    A.mtext = text;
    A.minput.clear();
    A.mok = std::move(ok);
}

void ask_confirm(const std::string& title, const std::string& text, std::function<void()> ok) {
    A.modal = 2;
    A.mtitle = title;
    A.mtext = text;
    A.mok = [ok](const std::string&) { ok(); };
}

void do_power(int what, const char* msg) {
    set_status(msg, C_DIM);
    sync();
    if (reboot(what) != 0) set_status(std::string("reboot: ") + std::strerror(errno), C_ERR);
}

// ----------------------------------------------------------------- terminal
void term_add(const std::string& line) {
    const int cols = std::max(10, (W - 40) / font.w);
    std::string rest = line;
    do {
        const std::size_t cut = u8_at_col(rest, cols);
        A.tout.push_back(rest.substr(0, cut));
        rest.erase(0, cut);
    } while (!rest.empty());
    if (A.tout.size() > 600) A.tout.erase(A.tout.begin(), A.tout.begin() + 100);
}

void term_add_block(const std::string& text) {
    std::string cur;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (c == 0x1b) {  // skip ANSI escape sequences
            ++i;
            if (i < text.size() && text[i] == '[')
                while (i + 1 < text.size() && !(text[i + 1] >= 0x40 && text[i + 1] <= 0x7e)) ++i, (void)0;
            ++i;
            continue;
        }
        if (c == '\n') { term_add(cur); cur.clear(); }
        else if (c == '\t') cur += "    ";
        else if (c != '\r') cur += c;
    }
    if (!cur.empty()) term_add(cur);
}

std::string run_capture(const std::string& line) {
    int p[2];
    if (pipe2(p, O_CLOEXEC) != 0) return std::string("pipe: ") + std::strerror(errno) + "\n";
    const pid_t pid = fork();
    if (pid < 0) { close(p[0]); close(p[1]); return std::string("fork: ") + std::strerror(errno) + "\n"; }
    if (pid == 0) {
        const int nul = open("/dev/null", O_RDONLY);
        if (nul >= 0) dup2(nul, STDIN_FILENO);
        dup2(p[1], STDOUT_FILENO);
        dup2(p[1], STDERR_FILENO);
        signal(SIGINT, SIG_DFL);
        execl("/usr/bin/difiw-shell", "difiw-shell", "-c", line.c_str(), static_cast<char*>(nullptr));
        std::fprintf(stderr, "difiw-shell not found\n");
        _exit(127);
    }
    close(p[1]);
    std::string out;
    char buf[2048];
    const long deadline = now_ms() + 20000;
    for (;;) {
        struct pollfd pf = {p[0], POLLIN, 0};
        const long left = deadline - now_ms();
        if (left <= 0) { kill(pid, SIGKILL); out += "\n[timeout]\n"; break; }
        const int r = poll(&pf, 1, static_cast<int>(left));
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) continue;
        const ssize_t n = read(p[0], buf, sizeof buf);
        if (n <= 0) break;
        out.append(buf, static_cast<std::size_t>(n));
        if (out.size() > 200000) { out += "\n[output truncated]\n"; kill(pid, SIGKILL); break; }
    }
    close(p[0]);
    int st = 0;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
    return out;
}

std::string cwd_now() {
    char buf[4096];
    return getcwd(buf, sizeof buf) ? buf : "/";
}

void term_exec(const std::string& line_in) {
    term_add("difiw:" + cwd_now() + "> " + line_in);
    const ParseResult pr = parse_line(line_in);
    if (!pr.ok) { term_add(std::string(TR("syntax error: ", "синтаксическая ошибка: ")) + pr.error); return; }
    if (pr.args.empty()) return;
    std::string cmd = pr.args[0];
    std::vector<std::string> args = pr.args;
    // "cd.." / "cd\" typed without a space
    if (cmd.size() > 2 && cmd.compare(0, 2, "cd") == 0 && (cmd[2] == '.' || cmd[2] == '\\' || cmd[2] == '/')) {
        args.insert(args.begin() + 1, cmd.substr(2));
        cmd = args[0] = "cd";
    }
    if (cmd == "terminal" || cmd == "exit") { A.quit = true; return; }
    if (cmd == "screen") { term_add(TR("The graphical shell is already running.", "Графическая оболочка уже запущена.")); return; }
    if (cmd == "clear" || cmd == "cls") { A.tout.clear(); return; }
    if (cmd == "cd" || cmd == "chdir") {
        if (args.size() >= 2 && (args[1] == "/d" || args[1] == "/D")) args.erase(args.begin() + 1);
        const char* h = std::getenv("HOME");
        const std::string home = h && *h == '/' ? h : "/root";
        const std::string target = args.size() >= 2 ? normalize_path(cwd_now(), args[1], home) : home;
        if (chdir(target.c_str()) != 0)
            term_add("difiw: cd: " + target + ": " + std::strerror(errno));
        return;
    }
    term_add_block(run_capture(line_in));
    read_lang();  // `ru` / `en` change the language file
}

// --------------------------------------------------------------- keyboard
std::string lat_n[128], lat_s[128], ru_n[128], ru_s[128];

void set_keys(std::string* t, int start, const char* utf8) {
    const std::string s = utf8;
    std::size_t i = 0;
    int k = start;
    while (i < s.size()) {
        const std::size_t j = u8_next(s, i);
        t[k++] = s.substr(i, j - i);
        i = j;
    }
}

void init_keys() {
    set_keys(lat_n, 2, "1234567890-=");  set_keys(lat_s, 2, "!@#$%^&*()_+");
    set_keys(lat_n, 16, "qwertyuiop[]");  set_keys(lat_s, 16, "QWERTYUIOP{}");
    set_keys(lat_n, 30, "asdfghjkl;'");   set_keys(lat_s, 30, "ASDFGHJKL:\"");
    set_keys(lat_n, 41, "`");             set_keys(lat_s, 41, "~");
    set_keys(lat_n, 43, "\\");            set_keys(lat_s, 43, "|");
    set_keys(lat_n, 44, "zxcvbnm,./");    set_keys(lat_s, 44, "ZXCVBNM<>?");
    set_keys(ru_n, 2, "1234567890-=");    set_keys(ru_s, 2, "!\"№;%:?*()_+");
    set_keys(ru_n, 16, "йцукенгшщзхъ");   set_keys(ru_s, 16, "ЙЦУКЕНГШЩЗХЪ");
    set_keys(ru_n, 30, "фывапролджэ");     set_keys(ru_s, 30, "ФЫВАПРОЛДЖЭ");
    set_keys(ru_n, 41, "ё");              set_keys(ru_s, 41, "Ё");
    set_keys(ru_n, 43, "\\");             set_keys(ru_s, 43, "/");
    set_keys(ru_n, 44, "ячсмитьбю.");     set_keys(ru_s, 44, "ЯЧСМИТЬБЮ,");
    lat_n[57] = lat_s[57] = ru_n[57] = ru_s[57] = " ";
}

bool is_letter_cp(const std::string& s) {
    if (s.empty()) return false;
    std::size_t i = 0;
    const std::uint32_t cp = next_cp(s, i);
    return (cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z') || (cp >= 0x410 && cp <= 0x44F) || cp == 0x451 || cp == 0x401;
}

std::string key_text(int code) {
    if (code < 0 || code >= 128 || A.ctrl) return "";
    bool shift = A.shift;
    const std::string& base = A.layout_ru ? ru_n[code] : lat_n[code];
    if (base.empty()) return "";
    if (A.caps && is_letter_cp(base)) shift = !shift;
    return shift ? (A.layout_ru ? ru_s[code] : lat_s[code]) : base;
}

// ------------------------------------------------------------------ editor keys
void ed_insert(const std::string& s) {
    A.lines[static_cast<std::size_t>(A.cy)].insert(static_cast<std::size_t>(A.cx), s);
    A.cx += static_cast<int>(s.size());
    A.dirty = true;
}

void ed_key(int code, const std::string& text) {
    auto& L = A.lines[static_cast<std::size_t>(A.cy)];
    switch (code) {
        case KEY_ENTER: {
            const std::string tail = L.substr(static_cast<std::size_t>(A.cx));
            L.erase(static_cast<std::size_t>(A.cx));
            A.lines.insert(A.lines.begin() + A.cy + 1, tail);
            ++A.cy; A.cx = 0; A.dirty = true;
            return;
        }
        case KEY_BACKSPACE:
            if (A.cx > 0) {
                const std::size_t p = u8_prev(L, static_cast<std::size_t>(A.cx));
                L.erase(p, static_cast<std::size_t>(A.cx) - p);
                A.cx = static_cast<int>(p);
                A.dirty = true;
            } else if (A.cy > 0) {
                const int len = static_cast<int>(A.lines[static_cast<std::size_t>(A.cy) - 1].size());
                A.lines[static_cast<std::size_t>(A.cy) - 1] += L;
                A.lines.erase(A.lines.begin() + A.cy);
                --A.cy; A.cx = len; A.dirty = true;
            }
            return;
        case KEY_DELETE:
            if (static_cast<std::size_t>(A.cx) < L.size()) {
                L.erase(static_cast<std::size_t>(A.cx), u8_next(L, static_cast<std::size_t>(A.cx)) - static_cast<std::size_t>(A.cx));
                A.dirty = true;
            } else if (static_cast<std::size_t>(A.cy) + 1 < A.lines.size()) {
                L += A.lines[static_cast<std::size_t>(A.cy) + 1];
                A.lines.erase(A.lines.begin() + A.cy + 1);
                A.dirty = true;
            }
            return;
        case KEY_LEFT:
            if (A.cx > 0) A.cx = static_cast<int>(u8_prev(L, static_cast<std::size_t>(A.cx)));
            else if (A.cy > 0) { --A.cy; A.cx = static_cast<int>(A.lines[static_cast<std::size_t>(A.cy)].size()); }
            return;
        case KEY_RIGHT:
            if (static_cast<std::size_t>(A.cx) < L.size()) A.cx = static_cast<int>(u8_next(L, static_cast<std::size_t>(A.cx)));
            else if (static_cast<std::size_t>(A.cy) + 1 < A.lines.size()) { ++A.cy; A.cx = 0; }
            return;
        case KEY_UP:
        case KEY_DOWN:
        case KEY_PAGEUP:
        case KEY_PAGEDOWN: {
            const int col = u8_cols(L, static_cast<std::size_t>(A.cx));
            const int step = (code == KEY_PAGEUP || code == KEY_PAGEDOWN) ? 15 : 1;
            const int dir = (code == KEY_UP || code == KEY_PAGEUP) ? -1 : 1;
            A.cy = std::clamp(A.cy + dir * step, 0, static_cast<int>(A.lines.size()) - 1);
            A.cx = static_cast<int>(u8_at_col(A.lines[static_cast<std::size_t>(A.cy)], col));
            return;
        }
        case KEY_HOME: A.cx = 0; return;
        case KEY_END: A.cx = static_cast<int>(L.size()); return;
        case KEY_TAB: ed_insert("    "); return;
        default: break;
    }
    if (!text.empty()) ed_insert(text);
}

void editor_close() {
    auto close_it = [] {
        A.epath.clear();
        editor_clear();
        A.tab = 0;
        refresh_dir();
    };
    if (A.dirty) ask_confirm(TR("Unsaved changes", "Есть несохранённые изменения"),
                             TR("Close the file without saving?", "Закрыть файл без сохранения?"), close_it);
    else close_it();
}

// --------------------------------------------------------------- UI drawing
constexpr int BAR_H = 40, STATUS_H = 26, BTN_H = 28;

void button(Rect r, const std::string& label, std::function<void()> fn, std::uint32_t base = C_BTN,
            std::uint32_t hot = C_BTN_HOT) {
    const bool over = r.has(A.mx, A.my);
    fill(r, over ? hot : base);
    const int tw = text_w(label);
    draw_text(r.x + (r.w - tw) / 2, r.y + (r.h - font.h) / 2, label, C_TEXT);
    A.hits.push_back({r, std::move(fn)});
}

int btn_w(const std::string& label) { return text_w(label) + 24; }

void draw_files(Rect area) {
    int y = area.y + 8;
    draw_text(area.x + 12, y, std::string(TR("Folder: ", "Папка: ")) + A.dir, C_ACCENT);
    y += 26;
    int x = area.x + 12;
    auto add = [&](const std::string& label, std::function<void()> fn, std::uint32_t base = C_BTN, std::uint32_t hot = C_BTN_HOT) {
        const int w = btn_w(label);
        button({x, y, w, BTN_H}, label, std::move(fn), base, hot);
        x += w + 6;
    };
    add(TR("Up", "Вверх"), [] { go_dir(parent_of(A.dir)); });
    add(TR("Refresh", "Обновить"), [] { refresh_dir(); });
    add("/root", [] { go_dir("/root"); });
    add("/tmp", [] { go_dir("/tmp"); });
    if (data_mounted()) add("/data", [] { go_dir("/data"); });
    x += 18;
    add(TR("New file", "Новый файл"), [] {
        ask_prompt(TR("New file", "Новый файл"), TR("File name:", "Имя файла:"), [](const std::string& name) {
            if (!valid_name(name)) { set_status(TR("Invalid name", "Недопустимое имя"), C_ERR); return; }
            const std::string path = join_path(A.dir, name);
            const int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
            if (fd < 0) { set_status(std::string(path) + ": " + std::strerror(errno), C_ERR); return; }
            close(fd);
            refresh_dir();
            open_file(path);
        });
    });
    add(TR("New folder", "Новая папка"), [] {
        ask_prompt(TR("New folder", "Новая папка"), TR("Folder name:", "Имя папки:"), [](const std::string& name) {
            if (!valid_name(name)) { set_status(TR("Invalid name", "Недопустимое имя"), C_ERR); return; }
            const std::string path = join_path(A.dir, name);
            if (mkdir(path.c_str(), 0755) != 0) { set_status(path + ": " + std::strerror(errno), C_ERR); return; }
            refresh_dir();
            set_status(std::string(TR("Folder created: ", "Папка создана: ")) + path, C_OK);
        });
    });
    auto open_sel = [] {
        if (A.sel < 0 || A.sel >= static_cast<int>(A.ents.size())) return;
        const Entry& e = A.ents[static_cast<std::size_t>(A.sel)];
        const std::string path = join_path(A.dir, e.name);
        if (e.dir) go_dir(path); else open_file(path);
    };
    add(TR("Open", "Открыть"), open_sel);
    add(TR("Delete", "Удалить"), [] {
        if (A.sel < 0 || A.sel >= static_cast<int>(A.ents.size())) { set_status(TR("Select an item first", "Сначала выберите элемент"), C_ERR); return; }
        const Entry e = A.ents[static_cast<std::size_t>(A.sel)];
        const std::string path = join_path(A.dir, e.name);
        if (is_protected_path(path)) { set_status(TR("Protected system path", "Защищённый системный путь"), C_ERR); return; }
        ask_confirm(TR("Delete", "Удаление"), std::string(TR("Delete ", "Удалить ")) + e.name + "?", [path, e] {
            const int rc = e.dir ? rmdir(path.c_str()) : unlink(path.c_str());
            if (rc != 0) set_status(e.dir && errno == ENOTEMPTY ? TR("Folder is not empty", "Папка не пуста") : std::strerror(errno), C_ERR);
            else set_status(std::string(TR("Deleted ", "Удалено: ")) + path, C_OK);
            refresh_dir();
        });
    }, C_DANGER, C_DANGER_HOT);

    y += BTN_H + 10;
    Rect list = {area.x + 12, y, area.w - 24, area.y + area.h - y - 28};
    fill(list, C_PANEL);
    frame(list, C_LINE);
    const int row_h = 22;
    const int rows = (list.h - 4) / row_h;
    const int total = static_cast<int>(A.ents.size());
    A.scroll = std::clamp(A.scroll, 0, std::max(0, total - rows));
    Rect saved = clip;
    clip = {list.x + 1, list.y + 1, list.w - 2, list.h - 2};
    for (int i = 0; i < rows && A.scroll + i < total; ++i) {
        const int idx = A.scroll + i;
        const Entry& e = A.ents[static_cast<std::size_t>(idx)];
        Rect row = {list.x + 2, list.y + 2 + i * row_h, list.w - 4, row_h};
        if (idx == A.sel) fill(row, C_SEL);
        else if (row.has(A.mx, A.my)) fill(row, C_BTN);
        draw_text(row.x + 8, row.y + 3, e.dir ? "[+]" : "   ", C_ACCENT);
        draw_text(row.x + 40, row.y + 3, fit(e.name, (row.w - 200) / font.w), e.dir ? C_ACCENT : C_TEXT);
        if (!e.dir) {
            const std::string sz = human_size(e.size);
            draw_text(row.x + row.w - 12 - text_w(sz), row.y + 3, sz, C_DIM);
        }
        A.hits.push_back({row, [idx, open_sel] {
            const long t = now_ms();
            if (A.last_click_row == idx && t - A.last_click_ms < 500) { A.sel = idx; open_sel(); A.last_click_row = -1; return; }
            A.sel = idx; A.last_click_row = idx; A.last_click_ms = t;
        }});
    }
    clip = saved;
    if (total == 0) draw_text(list.x + 16, list.y + 12, TR("(empty folder)", "(папка пуста)"), C_DIM);
    if (total > rows) {  // simple scroll bar
        const int th = std::max(20, list.h * rows / total);
        const int ty = list.y + (list.h - th) * A.scroll / std::max(1, total - rows);
        fill({list.x + list.w - 6, ty, 4, th}, C_LINE);
    }
    draw_text(area.x + 12, area.y + area.h - 22,
              data_mounted() ? TR("Files outside /data are lost on reboot.", "Файлы вне /data пропадут после перезагрузки.")
                             : TR("Files live in RAM and are lost on reboot (no /data disk attached).",
                                  "Файлы хранятся в RAM и пропадут после перезагрузки (диск /data не подключён)."),
              C_DIM);
}

void draw_editor(Rect area) {
    int y = area.y + 8;
    if (A.epath.empty()) {
        draw_text(area.x + 12, y + 6, TR("No file is open. Open one in the Files tab (double click) or create a new file.",
                                          "Файл не открыт. Откройте его на вкладке «Файлы» (двойной щелчок) или создайте новый."), C_DIM);
        return;
    }
    draw_text(area.x + 12, y + 6, A.epath + (A.dirty ? " *" : ""), A.dirty ? C_ERR : C_ACCENT);
    int x = area.x + area.w - 12;
    auto add_r = [&](const std::string& label, std::function<void()> fn, std::uint32_t base = C_BTN, std::uint32_t hot = C_BTN_HOT) {
        const int w = btn_w(label);
        x -= w;
        button({x, y, w, BTN_H}, label, std::move(fn), base, hot);
        x -= 6;
    };
    add_r(TR("Close", "Закрыть"), editor_close);
    add_r(TR("Save", "Сохранить"), [] { save_file(); }, C_OK, 0x6fd88f);
    y += BTN_H + 10;
    Rect box = {area.x + 12, y, area.w - 24, area.y + area.h - y - 12};
    fill(box, C_EDIT);
    frame(box, C_LINE);
    const int cols = (box.w - 12) / font.w, rows = (box.h - 8) / font.h;
    // keep the caret visible
    if (A.cy < A.etop) A.etop = A.cy;
    if (A.cy >= A.etop + rows) A.etop = A.cy - rows + 1;
    const int ccol = u8_cols(A.lines[static_cast<std::size_t>(A.cy)], static_cast<std::size_t>(A.cx));
    if (ccol < A.eleft) A.eleft = ccol;
    if (ccol >= A.eleft + cols) A.eleft = ccol - cols + 1;
    Rect saved = clip;
    clip = {box.x + 1, box.y + 1, box.w - 2, box.h - 2};
    for (int i = 0; i < rows && A.etop + i < static_cast<int>(A.lines.size()); ++i) {
        const std::string& l = A.lines[static_cast<std::size_t>(A.etop + i)];
        draw_text(box.x + 6, box.y + 4 + i * font.h, l.substr(u8_at_col(l, A.eleft)), C_TEXT);
    }
    const int cxp = box.x + 6 + (ccol - A.eleft) * font.w;
    const int cyp = box.y + 4 + (A.cy - A.etop) * font.h;
    fill({cxp, cyp, 2, font.h}, C_ACCENT);
    clip = saved;
    A.hits.push_back({box, [box, cols] {
        const int row = A.etop + (A.my - box.y - 4) / font.h;
        A.cy = std::clamp(row, 0, static_cast<int>(A.lines.size()) - 1);
        const int col = std::max(0, (A.mx - box.x - 6) / font.w) + A.eleft;
        A.cx = static_cast<int>(u8_at_col(A.lines[static_cast<std::size_t>(A.cy)], std::min(col, cols * 1000)));
    }});
}

void draw_terminal(Rect area) {
    Rect box = {area.x + 12, area.y + 8, area.w - 24, area.h - 20};
    fill(box, C_EDIT);
    frame(box, C_LINE);
    const int rows = (box.h - 8) / font.h - 1;
    const int total = static_cast<int>(A.tout.size());
    A.tscroll = std::clamp(A.tscroll, 0, std::max(0, total - rows));
    const int first = std::max(0, total - rows - A.tscroll);
    Rect saved = clip;
    clip = {box.x + 1, box.y + 1, box.w - 2, box.h - 2};
    int y = box.y + 4;
    for (int i = first; i < total && i < first + rows; ++i) {
        draw_text(box.x + 8, y, A.tout[static_cast<std::size_t>(i)], C_TEXT);
        y += font.h;
    }
    const std::string prompt = "difiw:" + cwd_now() + "> ";
    const int py = box.y + box.h - font.h - 4;
    fill({box.x + 1, py - 3, box.w - 2, 1}, C_LINE);
    const int x = draw_text(box.x + 8, py, prompt, C_OK);
    const int ex = draw_text(x, py, A.tin, C_TEXT);
    fill({ex, py, 8, font.h}, C_ACCENT);
    clip = saved;
}

void draw_modal() {
    A.hits.clear();
    Rect shade = {0, 0, W, H};
    for (int y = 0; y < H; ++y)
        for (int x = (y & 1); x < W; x += 2) ui[static_cast<std::size_t>(y) * W + x] = (ui[static_cast<std::size_t>(y) * W + x] >> 1) & 0x7f7f7f;
    (void)shade;
    const int bw = 460, bh = A.modal == 1 ? 170 : 140;
    Rect b = {(W - bw) / 2, (H - bh) / 2, bw, bh};
    fill(b, C_BAR);
    frame(b, C_ACCENT);
    draw_text(b.x + 16, b.y + 14, A.mtitle, C_ACCENT);
    draw_text(b.x + 16, b.y + 44, A.mtext, C_TEXT);
    int by = b.y + b.h - BTN_H - 14;
    auto accept = [] {
        auto fn = A.mok;
        const std::string in = A.minput;
        A.modal = 0;
        if (fn) fn(in);
    };
    if (A.modal == 1) {
        Rect in = {b.x + 16, b.y + 70, b.w - 32, 28};
        fill(in, C_EDIT);
        frame(in, C_LINE);
        const int x = draw_text(in.x + 6, in.y + 6, fit(A.minput, (in.w - 20) / font.w), C_TEXT);
        fill({x, in.y + 5, 2, font.h + 2}, C_ACCENT);
    }
    const std::string ok = A.modal == 1 ? "OK" : TR("Yes", "Да");
    const std::string no = A.modal == 1 ? TR("Cancel", "Отмена") : TR("No", "Нет");
    const int w2 = std::max(btn_w(ok), btn_w(no));
    button({b.x + b.w - 16 - w2, by, w2, BTN_H}, no, [] { A.modal = 0; });
    button({b.x + b.w - 16 - w2 * 2 - 8, by, w2, BTN_H}, ok, accept, C_OK, 0x6fd88f);
}

void render() {
    A.hits.clear();
    clip = {0, 0, W, H};
    fill({0, 0, W, H}, C_BG);
    // ---- top bar
    fill({0, 0, W, BAR_H}, C_BAR);
    draw_text(12, (BAR_H - font.h) / 2, std::string("Difiw OS ") + kVersion, C_ACCENT);
    int x = 190;
    const char* tabs_en[] = {"Files", "Editor", "Terminal"};
    const char* tabs_ru[] = {"Файлы", "Редактор", "Терминал"};
    for (int t = 0; t < 3; ++t) {
        const std::string label = A.ru ? tabs_ru[t] : tabs_en[t];
        const int w = btn_w(label) + 8;
        Rect r = {x, 6, w, BAR_H - 6};
        fill(r, t == A.tab ? C_BG : (r.has(A.mx, A.my) ? C_BTN_HOT : C_BTN));
        draw_text(r.x + (w - text_w(label)) / 2, r.y + (r.h - font.h) / 2 + 2, label, t == A.tab ? C_ACCENT : C_TEXT);
        A.hits.push_back({r, [t] { A.tab = t; if (t == 0) refresh_dir(); }});
        x += w + 4;
    }
    int rx = W - 10;
    auto add_r = [&](const std::string& label, std::function<void()> fn, std::uint32_t base = C_BTN, std::uint32_t hot = C_BTN_HOT) {
        const int w = btn_w(label);
        rx -= w;
        button({rx, 6, w, BAR_H - 12}, label, std::move(fn), base, hot);
        rx -= 6;
    };
    add_r(TR("Power off", "Выключение"), [] {
        ask_confirm(TR("Power off", "Выключение"), TR("Power off the computer?", "Выключить компьютер?"),
                    [] { do_power(RB_POWER_OFF, TR("Powering off...", "Выключение...")); });
    }, C_DANGER, C_DANGER_HOT);
    add_r(TR("Reboot", "Перезагрузка"), [] {
        ask_confirm(TR("Reboot", "Перезагрузка"), TR("Reboot the computer?", "Перезагрузить компьютер?"),
                    [] { do_power(RB_AUTOBOOT, TR("Rebooting...", "Перезагрузка...")); });
    });
    add_r(TR("Console", "Консоль"), [] {
        if (A.dirty) ask_confirm(TR("Unsaved changes", "Есть несохранённые изменения"),
                                 TR("Leave without saving?", "Выйти без сохранения?"), [] { A.quit = true; });
        else A.quit = true;
    });
    add_r(A.ru ? "RU" : "EN", [] { A.ru = !A.ru; write_lang(); });
    {
        time_t t = time(nullptr);
        struct tm tm;
        localtime_r(&t, &tm);
        char buf[8];
        std::strftime(buf, sizeof buf, "%H:%M", &tm);
        rx -= 8;
        draw_text(rx - 40, (BAR_H - font.h) / 2, buf, C_DIM);
    }
    // ---- content
    Rect area = {0, BAR_H, W, H - BAR_H - STATUS_H};
    if (A.tab == 0) draw_files(area);
    else if (A.tab == 1) draw_editor(area);
    else draw_terminal(area);
    // ---- status bar
    fill({0, H - STATUS_H, W, STATUS_H}, C_BAR);
    draw_text(12, H - STATUS_H + (STATUS_H - font.h) / 2, fit(A.status, (W / 2) / font.w), A.status_col);
    const std::string hint = std::string("F1 F2 F3  |  Alt+Shift: ") + (A.layout_ru ? "RU" : "EN") + "  |  Ctrl+S";
    const std::string h2 = fit(hint, (W / 2 - 24) / font.w);
    draw_text(W - 12 - text_w(h2), H - STATUS_H + (STATUS_H - font.h) / 2, h2, C_DIM);
    if (A.modal) draw_modal();
}

// ------------------------------------------------------ framebuffer output
inline std::uint32_t pack(std::uint32_t c) {
    const std::uint32_t r = (c >> 16) & 0xff, g = (c >> 8) & 0xff, b = c & 0xff;
    if (fb.bpp == 16) return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
    return ((r >> (8 - fb.rl)) << fb.ro) | ((g >> (8 - fb.gl)) << fb.go) | ((b >> (8 - fb.bl)) << fb.bo);
}

// 24 bits per pixel (e.g. VESA mode 0x118): three separate bytes placed by channel offsets.
inline void put24(std::uint8_t* d, std::uint32_t c) {
    d[fb.ro / 8] = static_cast<std::uint8_t>(c >> 16);
    d[fb.go / 8] = static_cast<std::uint8_t>(c >> 8);
    d[fb.bo / 8] = static_cast<std::uint8_t>(c);
}

void blit(int x0, int y0, int w, int h) {
    x0 = std::max(0, x0); y0 = std::max(0, y0);
    w = std::min(w, W - x0); h = std::min(h, H - y0);
    if (w <= 0 || h <= 0 || !fb.mem) return;
    for (int y = y0; y < y0 + h; ++y) {
        const std::uint32_t* src = &ui[static_cast<std::size_t>(y) * W + x0];
        std::uint8_t* dst = fb.mem + static_cast<std::size_t>(y) * fb.stride + static_cast<std::size_t>(x0) * (fb.bpp / 8);
        if (fb.fast) {
            std::memcpy(dst, src, static_cast<std::size_t>(w) * 4);
        } else if (fb.bpp == 16) {
            for (int x = 0; x < w; ++x) reinterpret_cast<std::uint16_t*>(dst)[x] = static_cast<std::uint16_t>(pack(src[x]));
        } else if (fb.bpp == 24) {
            for (int x = 0; x < w; ++x) put24(dst + x * 3, src[x]);
        } else {
            for (int x = 0; x < w; ++x) reinterpret_cast<std::uint32_t*>(dst)[x] = pack(src[x]);
        }
    }
}

const char* const kCursor[] = {
    "X           ", "XX          ", "X.X         ", "X..X        ", "X...X       ", "X....X      ", "X.....X     ",
    "X......X    ", "X.......X   ", "X........X  ", "X.........X ", "X......XXXXX", "X...X..X    ", "X..XX..X    ",
    "X.X  X..X   ", "XX   X..X   ", "X     X..X  ", "      X..X  ", "       XX   "};
constexpr int kCurW = 12, kCurH = 19;

void draw_cursor() {
    if (!fb.mem) return;
    for (int y = 0; y < kCurH; ++y)
        for (int x = 0; x < kCurW; ++x) {
            const char c = kCursor[y][x];
            if (c == ' ') continue;
            const int px_ = A.mx + x, py_ = A.my + y;
            if (px_ < 0 || py_ < 0 || px_ >= W || py_ >= H) continue;
            const std::uint32_t rgb = c == 'X' ? 0x000000 : 0xffffff;
            const std::uint32_t col = pack(rgb);
            std::uint8_t* dst = fb.mem + static_cast<std::size_t>(py_) * fb.stride + static_cast<std::size_t>(px_) * (fb.bpp / 8);
            if (fb.bpp == 16) *reinterpret_cast<std::uint16_t*>(dst) = static_cast<std::uint16_t>(col);
            else if (fb.bpp == 24) put24(dst, rgb);
            else *reinterpret_cast<std::uint32_t*>(dst) = col;
        }
}

void present() {
    blit(0, 0, W, H);
    draw_cursor();
}

void move_cursor(int nx, int ny) {
    const int ox = A.mx, oy = A.my;
    A.mx = std::clamp(nx, 0, W - 1);
    A.my = std::clamp(ny, 0, H - 1);
    blit(ox, oy, kCurW, kCurH);
    draw_cursor();
}

int hit_index() {
    for (int i = static_cast<int>(A.hits.size()) - 1; i >= 0; --i)
        if (A.hits[static_cast<std::size_t>(i)].r.has(A.mx, A.my)) return i;
    return -1;
}

// ------------------------------------------------------------------- input
struct Dev {
    int fd = -1;
    bool kbd = false, abs = false, rel = false;
    int minx = 0, maxx = 1, miny = 0, maxy = 1;
};
std::vector<Dev> devs;

inline bool bit(const unsigned long* a, int n) { return (a[n / (8 * sizeof(long))] >> (n % (8 * sizeof(long)))) & 1; }

void scan_devices() {
    for (int i = 0; i < 32; ++i) {
        char path[64];
        std::snprintf(path, sizeof path, "/dev/input/event%d", i);
        const int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) continue;
        // skip devices we already track (same inode)
        struct stat sb;
        fstat(fd, &sb);
        unsigned long ev[(EV_MAX + 8 * sizeof(long)) / (8 * sizeof(long))] = {0};
        unsigned long key[(KEY_MAX + 8 * sizeof(long)) / (8 * sizeof(long))] = {0};
        unsigned long absb[(ABS_MAX + 8 * sizeof(long)) / (8 * sizeof(long))] = {0};
        unsigned long relb[(REL_MAX + 8 * sizeof(long)) / (8 * sizeof(long))] = {0};
        ioctl(fd, EVIOCGBIT(0, sizeof ev), ev);
        ioctl(fd, EVIOCGBIT(EV_KEY, sizeof key), key);
        ioctl(fd, EVIOCGBIT(EV_ABS, sizeof absb), absb);
        ioctl(fd, EVIOCGBIT(EV_REL, sizeof relb), relb);
        Dev d;
        d.fd = fd;
        d.kbd = bit(ev, EV_KEY) && bit(key, KEY_A) && bit(key, KEY_ENTER);
        d.abs = bit(ev, EV_ABS) && bit(absb, ABS_X) && bit(absb, ABS_Y) && (bit(key, BTN_LEFT) || bit(key, BTN_TOUCH));
        d.rel = bit(ev, EV_REL) && bit(relb, REL_X) && bit(relb, REL_Y) && bit(key, BTN_LEFT);
        if (d.abs) {
            struct input_absinfo ai;
            if (ioctl(fd, EVIOCGABS(ABS_X), &ai) == 0) { d.minx = ai.minimum; d.maxx = std::max(ai.maximum, ai.minimum + 1); }
            if (ioctl(fd, EVIOCGABS(ABS_Y), &ai) == 0) { d.miny = ai.minimum; d.maxy = std::max(ai.maximum, ai.minimum + 1); }
        }
        if (!d.kbd && !d.abs && !d.rel) { close(fd); continue; }
        // already open? compare by device id
        bool dup = false;
        for (const auto& o : devs) {
            struct stat s2;
            if (o.fd >= 0 && fstat(o.fd, &s2) == 0 && s2.st_rdev == sb.st_rdev) { dup = true; break; }
        }
        if (dup) { close(fd); continue; }
        if (d.kbd) ioctl(fd, EVIOCGRAB, 1);
        devs.push_back(d);
    }
}

bool g_need_render = true;

void toggle_layout() { A.layout_ru = !A.layout_ru; }

void on_key(int code, int value) {
    const bool down = value != 0;
    switch (code) {
        case KEY_LEFTSHIFT: case KEY_RIGHTSHIFT: A.shift = down; if (down && A.alt) toggle_layout(); g_need_render = true; return;
        case KEY_LEFTCTRL: case KEY_RIGHTCTRL: A.ctrl = down; return;
        case KEY_LEFTALT: case KEY_RIGHTALT: A.alt = down; if (down && A.shift) toggle_layout(); g_need_render = true; return;
        case KEY_CAPSLOCK: if (value == 1) A.caps = !A.caps; return;
        default: break;
    }
    if (!down) return;
    g_need_render = true;
    if (A.modal) {
        if (code == KEY_ESC) { A.modal = 0; return; }
        if (code == KEY_ENTER) {
            auto fn = A.mok; const std::string in = A.minput; A.modal = 0;
            if (fn) fn(in);
            return;
        }
        if (A.modal == 1) {
            if (code == KEY_BACKSPACE) { if (!A.minput.empty()) A.minput.erase(u8_prev(A.minput, A.minput.size())); return; }
            const std::string t = key_text(code);
            if (!t.empty() && A.minput.size() < 200) A.minput += t;
        }
        return;
    }
    if (code == KEY_F1) { A.tab = 0; refresh_dir(); return; }
    if (code == KEY_F2) { A.tab = 1; return; }
    if (code == KEY_F3) { A.tab = 2; return; }
    if (A.ctrl && code == KEY_S && A.tab == 1) { save_file(); return; }
    const std::string text = key_text(code);
    if (A.tab == 1) {
        if (!A.epath.empty()) ed_key(code, text);
    } else if (A.tab == 2) {
        switch (code) {
            case KEY_ENTER: {
                const std::string line = A.tin;
                A.tin.clear();
                A.tscroll = 0;
                if (!line.empty()) { A.hist.push_back(line); A.hidx = -1; }
                term_exec(line);
                break;
            }
            case KEY_BACKSPACE: if (!A.tin.empty()) A.tin.erase(u8_prev(A.tin, A.tin.size())); break;
            case KEY_UP:
                if (!A.hist.empty()) { A.hidx = A.hidx < 0 ? static_cast<int>(A.hist.size()) - 1 : std::max(0, A.hidx - 1); A.tin = A.hist[static_cast<std::size_t>(A.hidx)]; }
                break;
            case KEY_DOWN:
                if (A.hidx >= 0) { ++A.hidx; if (A.hidx >= static_cast<int>(A.hist.size())) { A.hidx = -1; A.tin.clear(); } else A.tin = A.hist[static_cast<std::size_t>(A.hidx)]; }
                break;
            case KEY_PAGEUP: A.tscroll += 10; break;
            case KEY_PAGEDOWN: A.tscroll = std::max(0, A.tscroll - 10); break;
            default: if (!text.empty() && A.tin.size() < 1000) A.tin += text;
        }
    } else {  // files
        const int total = static_cast<int>(A.ents.size());
        if (code == KEY_DOWN && total) A.sel = std::min(total - 1, A.sel + 1);
        else if (code == KEY_UP && total) A.sel = std::max(0, A.sel - 1);
        else if (code == KEY_BACKSPACE) go_dir(parent_of(A.dir));
        else if (code == KEY_ENTER && A.sel >= 0 && A.sel < total) {
            const Entry& e = A.ents[static_cast<std::size_t>(A.sel)];
            const std::string path = join_path(A.dir, e.name);
            if (e.dir) go_dir(path); else open_file(path);
        }
        const int rows = std::max(1, (H - BAR_H - STATUS_H - 150) / 22);
        if (A.sel >= 0) { if (A.sel < A.scroll) A.scroll = A.sel; if (A.sel >= A.scroll + rows) A.scroll = A.sel - rows + 1; }
    }
}

void on_click() {
    const int i = hit_index();
    if (i >= 0) {
        auto fn = A.hits[static_cast<std::size_t>(i)].fn;
        fn();
    }
    g_need_render = true;
}

void handle_event(const Dev& d, const input_event& e) {
    if (e.type == EV_KEY) {
        if (e.code == BTN_LEFT || e.code == BTN_TOUCH) { if (e.value == 1) on_click(); return; }
        if (e.code < 256 && d.kbd) on_key(e.code, e.value);
    } else if (e.type == EV_REL) {
        if (e.code == REL_X) { move_cursor(A.mx + e.value, A.my); }
        else if (e.code == REL_Y) { move_cursor(A.mx, A.my + e.value); }
        else if (e.code == REL_WHEEL) {
            if (A.tab == 0) A.scroll -= e.value * 3;
            else if (A.tab == 2) A.tscroll += e.value * 3;
            else if (A.tab == 1) A.etop = std::max(0, A.etop - e.value * 3);
            g_need_render = true;
        }
    } else if (e.type == EV_ABS && d.abs) {
        if (e.code == ABS_X) move_cursor((e.value - d.minx) * (W - 1) / (d.maxx - d.minx), A.my);
        else if (e.code == ABS_Y) move_cursor(A.mx, (e.value - d.miny) * (H - 1) / (d.maxy - d.miny));
    }
}

// ----------------------------------------------------------------- console
volatile sig_atomic_t g_sig = 0;
void on_sig(int) { g_sig = 1; }

int g_tty = -1;
int g_old_kbmode = K_UNICODE;
bool g_gfx_mode = false;

void enter_graphics() {
    g_tty = open("/dev/tty", O_RDWR | O_CLOEXEC);
    if (g_tty < 0) g_tty = STDIN_FILENO;
    if (ioctl(g_tty, KDGKBMODE, &g_old_kbmode) != 0) g_old_kbmode = K_UNICODE;
    ioctl(g_tty, KDSKBMODE, K_OFF);
    g_gfx_mode = ioctl(g_tty, KDSETMODE, KD_GRAPHICS) == 0;
    const char* s = "\033[9;0]\033[?25l";  // no screen blanking, hide the text cursor
    (void)!write(g_tty, s, std::strlen(s));
}

void leave_graphics() {
    for (auto& d : devs) if (d.fd >= 0) { ioctl(d.fd, EVIOCGRAB, 0); close(d.fd); }
    devs.clear();
    if (g_tty >= 0) {
        if (g_gfx_mode) ioctl(g_tty, KDSETMODE, KD_TEXT);
        ioctl(g_tty, KDSKBMODE, g_old_kbmode);
        const char* clr = "\033[?25h\033[2J\033[H";
        (void)!write(g_tty, clr, std::strlen(clr));
    }
}

bool open_fb() {
    fb.fd = open("/dev/fb0", O_RDWR | O_CLOEXEC);
    if (fb.fd < 0) return false;
    struct fb_var_screeninfo v;
    struct fb_fix_screeninfo f;
    if (ioctl(fb.fd, FBIOGET_VSCREENINFO, &v) != 0 || ioctl(fb.fd, FBIOGET_FSCREENINFO, &f) != 0) return false;
    if (v.bits_per_pixel != 32 && v.bits_per_pixel != 24 && v.bits_per_pixel != 16) {
        std::fprintf(stderr, "difiw-gui: unsupported framebuffer depth %u\n", v.bits_per_pixel);
        return false;
    }
    W = static_cast<int>(v.xres);
    H = static_cast<int>(v.yres);
    fb.bpp = static_cast<int>(v.bits_per_pixel);
    fb.stride = static_cast<int>(f.line_length);
    fb.ro = static_cast<int>(v.red.offset); fb.rl = static_cast<int>(v.red.length);
    fb.go = static_cast<int>(v.green.offset); fb.gl = static_cast<int>(v.green.length);
    fb.bo = static_cast<int>(v.blue.offset); fb.bl = static_cast<int>(v.blue.length);
    fb.fast = fb.bpp == 32 && fb.ro == 16 && fb.go == 8 && fb.bo == 0;
    fb.len = f.smem_len ? f.smem_len : static_cast<std::size_t>(fb.stride) * H;
    void* m = mmap(nullptr, fb.len, PROT_READ | PROT_WRITE, MAP_SHARED, fb.fd, 0);
    if (m == MAP_FAILED) return false;
    fb.mem = static_cast<std::uint8_t*>(m);
    return true;
}

int snapshot(const char* path, int tab) {
    ui.assign(static_cast<std::size_t>(W) * H, 0);
    A.tab = tab;
    A.mx = 640; A.my = 300;
    if (const char* d = std::getenv("DIFIW_GUI_DIR")) A.dir = d;
    refresh_dir();
    if (tab == 1) { A.epath = "/root/notes.txt"; A.lines = {"Hello Difiw OS!", "Привет, мир!", "Edit me."}; A.cy = 1; A.cx = 6; }
    if (tab == 2) { term_add("difiw:/root> dir"); term_add("  Directory of /root"); term_add("difiw:/root> "); }
    set_status(TR("Saved /root/notes.txt", "Сохранено: /root/notes.txt"), C_OK);
    render();
    FILE* f = std::fopen(path, "wb");
    if (!f) return 1;
    std::fprintf(f, "P6\n%d %d\n255\n", W, H);
    for (std::size_t i = 0; i < ui.size(); ++i) {
        const unsigned char rgb[3] = {static_cast<unsigned char>(ui[i] >> 16), static_cast<unsigned char>(ui[i] >> 8), static_cast<unsigned char>(ui[i])};
        std::fwrite(rgb, 1, 3, f);
    }
    std::fclose(f);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    const char* fontpath = kFontPath;
    if (argc >= 3 && !std::strcmp(argv[1], "--font")) { fontpath = argv[2]; argc -= 2; argv += 2; }
    if (!load_font(fontpath)) {
        std::fprintf(stderr, "difiw-gui: cannot load font %s\n", fontpath);
        return 1;
    }
    init_keys();
    read_lang();
    A.tout.clear();
    term_add(std::string("Difiw OS ") + kVersion + " - " + TR("graphical shell terminal.", "терминал графической оболочки."));
    term_add(TR("Type 'help'. Type 'terminal' to close the graphical shell and return to the text console.",
                "Введите 'help'. Команда 'terminal' закроет графическую оболочку и вернёт текстовую консоль."));
    if (argc >= 3 && !std::strcmp(argv[1], "--snapshot")) return snapshot(argv[2], argc >= 4 ? std::atoi(argv[3]) : 0);

    if (!open_fb()) {
        std::fprintf(stderr, "difiw-gui: no usable /dev/fb0 (the bootloader did not set a graphics mode)\n");
        return 1;
    }
    ui.assign(static_cast<std::size_t>(W) * H, 0);
    struct sigaction sa;
    std::memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_sig;
    sigaction(SIGTERM, &sa, nullptr);
    sigaction(SIGHUP, &sa, nullptr);
    sigaction(SIGINT, &sa, nullptr);

    enter_graphics();
    A.mx = W / 2;
    A.my = H / 2;
    refresh_dir();
    set_status(TR("Welcome! Create a file: Files -> New file.", "Добро пожаловать! Создайте файл: Файлы -> Новый файл."), C_DIM);
    scan_devices();
    render();
    present();

    long last_scan = now_ms();
    int last_min = -1;
    while (!A.quit && !g_sig) {
        std::vector<struct pollfd> pf;
        for (const auto& d : devs) pf.push_back({d.fd, POLLIN, 0});
        const int r = poll(pf.data(), static_cast<nfds_t>(pf.size()), 500);
        if (r > 0) {
            for (std::size_t i = 0; i < pf.size(); ++i) {
                if (!(pf[i].revents & POLLIN)) continue;
                input_event evs[64];
                const ssize_t n = read(devs[i].fd, evs, sizeof evs);
                if (n <= 0) continue;
                for (ssize_t k = 0; k < n / static_cast<ssize_t>(sizeof(input_event)); ++k) handle_event(devs[i], evs[k]);
            }
        }
        const long t = now_ms();
        bool have_ptr = false, have_kbd = false;
        for (const auto& d : devs) { have_ptr |= d.abs || d.rel; have_kbd |= d.kbd; }
        if ((!have_ptr || !have_kbd) && t - last_scan > 1000) { last_scan = t; scan_devices(); }
        time_t tt = time(nullptr);
        struct tm tm;
        localtime_r(&tt, &tm);
        if (tm.tm_min != last_min) { last_min = tm.tm_min; g_need_render = true; }
        const int hv = hit_index();
        if (hv != A.hover) { A.hover = hv; g_need_render = true; }
        if (g_need_render) {
            g_need_render = false;
            render();
            present();
        }
    }
    leave_graphics();
    return 0;
}
