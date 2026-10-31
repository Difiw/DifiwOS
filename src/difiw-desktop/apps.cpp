// difiw-apps: the test applications of Difiw OS (one multi-call binary).
//   difiw-apps notes|calc|terminal|monitor|probe|files|recorder|camera|worker
// Each app is started by the desktop as its own unprivileged user with its own home.
// SPDX-License-Identifier: MIT
#include <dirent.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/videodev2.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <poll.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <sstream>
#include <cctype>
#include <functional>
#include <map>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_ONLY_GIF
#define STBI_ONLY_BMP
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wall"
#pragma GCC diagnostic ignored "-Wextra"
#pragma GCC diagnostic ignored "-Wpedantic"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wcast-qual"
#include "third_party/stb_image.h"
#pragma GCC diagnostic pop

#include "client.hpp"
#include "sysinfo.hpp"
#include "tokens.hpp"

using namespace dg;
using namespace dg::tok;

namespace {

std::string font_path() {
    const char* d = std::getenv("DIFIW_DATADIR");
    return std::string(d ? d : "/usr/share/difiw") + "/font.psf";
}

long mono_ms() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

bool lang_ru() {
    std::ifstream f("/run/difiw-lang");
    std::string s;
    std::getline(f, s);
    return s.compare(0, 2, "en") != 0;
}

// ------------------------------------------------------------- app framework
struct App {
    Client cl;
    int win = -1, w = 0, h = 0, mx = -1, my = -1;
    bool dirty = true, quit = false, ru = true;

    virtual ~App() {}
    virtual void draw(Surface& s) = 0;
    virtual void key(const Event&) {}
    virtual void mouse(const Event&) {}
    virtual void tick() {}
    virtual int tick_ms() { return 1000; }
    virtual void on_close() { quit = true; }
    virtual void on_denied(int) {}
    virtual void on_lang() {}

    int sc() const { return std::max(1, cl.scale); }
    std::string t(const char* en, const char* r) const { return ru ? r : en; }

    void button(Surface& s, Rect r, const std::string& label, bool primary = false, bool danger = false, int ls = 0) const {
        if (ls < 1) ls = sc();
        const bool over = r.has(mx, my);
        u32 base = primary ? 0xFF2F5FC0u : danger ? 0xFF8A3A30u : kGraphite600;
        u32 hot = primary ? kAccentBlue : danger ? kDanger : kGraphite500;
        rrect(s, r, kRadiusControl * sc(), over ? hot : base);
        rrect_outline(s, r, kRadiusControl * sc(), 1, over ? 0x60FFFFFFu : 0x28FFFFFFu);
        draw_text(s, r.x + (r.w - text_width(label, ls)) / 2, r.y + (r.h - font_h() * ls) / 2, label, kText, ls);
    }

    Surface off;  // apps draw here and the finished picture is copied to the shared buffer at once: the compositor never shows a half-drawn frame (flicker)
    void redraw() {
        Surface& s = cl.surface(win);
        w = s.w; h = s.h;
        if (off.w != w || off.h != h) off.alloc(w, h);
        off.reset_clip();
        fill(off, {0, 0, w, h}, kGraphite800);
        draw(off);
        for (int y = 0; y < h; ++y)
            std::memcpy(s.px + static_cast<std::size_t>(y) * static_cast<std::size_t>(s.stride), off.px + static_cast<std::size_t>(y) * static_cast<std::size_t>(off.stride), static_cast<std::size_t>(w) * 4);
        cl.commit(win);
        dirty = false;
    }

    std::string ten, tru;
    long last_lang_ms = 0;
    int run(int ww, int wh, const std::string& en, const std::string& ru_title) {
        if (!load_font(font_path().c_str())) { std::fprintf(stderr, "cannot load font\n"); return 2; }
        if (!cl.connect()) { std::fprintf(stderr, "cannot connect to the desktop\n"); return 3; }
        ru = lang_ru();
        ten = en; tru = ru_title;
        win = cl.create_window(ww * sc(), wh * sc(), ru ? tru : ten);
        if (win < 0) { std::fprintf(stderr, "cannot create window\n"); return 4; }
        long last_tick = mono_ms();
        redraw();
        while (!quit) {
            Event ev;
            const int wait = std::max(1, tick_ms() - static_cast<int>(mono_ms() - last_tick));
            bool got = cl.next_event(ev, wait);
            // handle everything that is already waiting, then redraw once (a burst of mouse moves costs one frame)
            for (int guard = 0; got && guard < 256 && !quit; ++guard) {
                switch (ev.kind) {
                    case Event::Disconnected: quit = true; break;
                    case Event::Close: on_close(); break;
                    case Event::Denied: on_denied(ev.code); break;
                    case Event::Configure: dirty = true; break;
                    case Event::Mouse: mx = ev.x; my = ev.y; mouse(ev); dirty = true; break;
                    case Event::Key: if (ev.state != 0) key(ev); dirty = true; break;
                    default: break;
                }
                got = cl.next_event(ev, 0);
            }
            if (mono_ms() - last_tick >= tick_ms()) { last_tick = mono_ms(); tick(); dirty = true; }
            if (mono_ms() - last_lang_ms > 500) {  // the language is changed in Settings: titles and texts follow at once
                last_lang_ms = mono_ms();
                const bool nr = lang_ru();
                if (nr != ru) { ru = nr; cl.set_title(win, ru ? tru : ten); on_lang(); dirty = true; }
            }
            if (dirty && !quit) redraw();
        }
        return 0;
    }
};

// ------------------------------------------------------------------- Notes
// Notepad: a plain text editor. Files live in the shared Documents folder (or any path the user types).
struct Notepad : App {
    std::vector<std::string> lines{""};
    int cy = 0, cx = 0, top = 0;
    bool changed = false, prompting = false, prompt_save = false;
    std::string path, status, pin;

    void load() { lines = {""}; path.clear(); }
    void ask_name(bool for_save) { prompting = true; prompt_save = for_save; pin.clear(); }
    void open_file(std::string name) {
        if (name.empty()) return;
        if (name[0] != '/') name = "/homes/shared/Documents/" + name;
        if (name.find('.', name.rfind('/')) == std::string::npos) name += ".txt";
        if (prompt_save) { path = name; save(); return; }
        std::ifstream f(name);
        std::string l;
        std::vector<std::string> v;
        while (std::getline(f, l)) v.push_back(l);
        if (v.empty()) v.push_back("");
        lines = v; path = name; cy = cx = top = 0; changed = false;
        status = f.is_open() ? t("Opened ", "Открыто: ") + name : t("New file: ", "Новый файл: ") + name;
    }
    bool save() {
        if (path.empty()) { ask_name(true); return false; }
        std::ofstream f(path, std::ios::trunc);
        for (const auto& l : lines) f << l << "\n";
        f.flush();
        if (!f) { status = t("Save failed: ", "Не удалось сохранить: ") + std::strerror(errno); return false; }
        changed = false;
        status = t("Saved ", "Сохранено: ") + path;
        return true;
    }
    void on_close() override { if (changed && !path.empty()) save(); quit = true; }
    Rect box() const { return {S_(12), S_(52), w - 2 * S_(12), h - S_(52) - S_(34)}; }
    int S_(int v) const { return v * sc(); }
    void draw(Surface& s) override {
        draw_text(s, S_(14), S_(14), (changed ? "* " : "") + fit_text(path.empty() ? t("Untitled", "Без имени") : path, w - S_(340), sc()), changed ? kWarn : kAccentBlue, sc());
        button(s, {w - S_(12) - S_(104) - S_(8) - S_(104), S_(8), S_(104), S_(32)}, t("Open", "Открыть"));
        const Rect sv = {w - S_(12) - S_(104), S_(8), S_(104), S_(32)};
        button(s, sv, t("Save", "Сохранить"), true);
        const Rect b = box();
        rrect(s, b, S_(10), kGraphite900);
        rrect_outline(s, b, S_(10), 1, 0x30FFFFFFu);
        const int fh = font_h() * sc(), rows = (b.h - S_(10)) / fh, cols = (b.w - S_(16)) / (font_w() * sc());
        top = clampi(top, 0, std::max(0, static_cast<int>(lines.size()) - 1));
        if (cy < top) top = cy;
        if (cy >= top + rows) top = cy - rows + 1;
        const Rect save = s.clip;
        s.clip = save.intersect(b.inset(2));
        for (int i = 0; i < rows && top + i < static_cast<int>(lines.size()); ++i)
            draw_text(s, b.x + S_(8), b.y + S_(5) + i * fh, fit_text(lines[static_cast<std::size_t>(top + i)], cols * font_w() * sc(), sc()), kText, sc());
        const std::string& cl_ = lines[static_cast<std::size_t>(cy)];
        const int ccol = u8_len(cl_.substr(0, static_cast<std::size_t>(cx)));
        fill(s, {b.x + S_(8) + ccol * font_w() * sc(), b.y + S_(5) + (cy - top) * fh, std::max(2, sc() * 2), fh}, kAccentBlue);
        s.clip = save;
        draw_text(s, S_(14), h - S_(26), fit_text(status.empty() ? t("Ctrl+S save, Ctrl+O open. Files: Shared/Documents.", "Ctrl+S сохранить, Ctrl+O открыть. Файлы: Общая папка/Документы.") : status, w - S_(28), sc()), kTextDim, sc());
        if (prompting) {
            const Rect pb = {S_(40), h / 2 - S_(40), w - S_(80), S_(80)};
            rrect(s, pb, S_(12), 0xF0141925u);
            rrect_outline(s, pb, S_(12), 2, kAccentBlue);
            draw_text(s, pb.x + S_(14), pb.y + S_(10), t(prompt_save ? "Save as (file name):" : "Open (file name; a new name creates a file):", prompt_save ? "Сохранить как (имя файла):" : "Открыть (имя файла; новое имя создаст файл):"), kTextDim, sc());
            draw_text(s, pb.x + S_(14), pb.y + S_(40), fit_text(pin, pb.w - S_(40), sc()) + "_", kText, sc());
        }
        (void)cols;
    }
    void mouse(const Event& e) override {
        if (e.mouse_kind != 1) return;
        const Rect sv = {w - S_(12) - S_(104), S_(8), S_(104), S_(32)};
        if (prompting) return;
        if (Rect{w - S_(12) - S_(104) - S_(8) - S_(104), S_(8), S_(104), S_(32)}.has(e.x, e.y)) { ask_name(false); return; }
        if (sv.has(e.x, e.y)) { save(); return; }
        const Rect b = box();
        if (!b.has(e.x, e.y)) return;
        const int fh = font_h() * sc();
        cy = clampi(top + (e.y - b.y - S_(5)) / fh, 0, static_cast<int>(lines.size()) - 1);
        cx = static_cast<int>(u8_at(lines[static_cast<std::size_t>(cy)], std::max(0, (e.x - b.x - S_(8)) / (font_w() * sc()))));
    }
    static std::size_t u8_at(const std::string& s, int col) {
        std::size_t i = 0;
        while (col > 0 && i < s.size()) { i = u8_next(s, i); --col; }
        return i;
    }
    void key(const Event& e) override {
        std::string& L = lines[static_cast<std::size_t>(cy)];
        if (prompting) {
            if (e.code == KEY_ESC) { prompting = false; return; }
            if (e.code == KEY_ENTER) { prompting = false; open_file(pin); return; }
            if (e.code == KEY_BACKSPACE) { if (!pin.empty()) pin.erase(u8_prev(pin, pin.size())); return; }
            if (!e.text.empty() && pin.size() < 200) pin += e.text;
            return;
        }
        if ((e.mods & 2) && e.code == KEY_O) { ask_name(false); return; }
        if ((e.mods & 2) && e.code == KEY_S) { save(); return; }
        switch (e.code) {
            case KEY_ENTER: { const std::string tail = L.substr(static_cast<std::size_t>(cx)); L.erase(static_cast<std::size_t>(cx)); lines.insert(lines.begin() + cy + 1, tail); ++cy; cx = 0; changed = true; return; }
            case KEY_BACKSPACE:
                if (cx > 0) { const std::size_t p = u8_prev(L, static_cast<std::size_t>(cx)); L.erase(p, static_cast<std::size_t>(cx) - p); cx = static_cast<int>(p); changed = true; }
                else if (cy > 0) { const int len = static_cast<int>(lines[static_cast<std::size_t>(cy) - 1].size()); lines[static_cast<std::size_t>(cy) - 1] += L; lines.erase(lines.begin() + cy); --cy; cx = len; changed = true; }
                return;
            case KEY_DELETE:
                if (static_cast<std::size_t>(cx) < L.size()) { L.erase(static_cast<std::size_t>(cx), u8_next(L, static_cast<std::size_t>(cx)) - static_cast<std::size_t>(cx)); changed = true; }
                else if (static_cast<std::size_t>(cy) + 1 < lines.size()) { L += lines[static_cast<std::size_t>(cy) + 1]; lines.erase(lines.begin() + cy + 1); changed = true; }
                return;
            case KEY_LEFT: if (cx > 0) cx = static_cast<int>(u8_prev(L, static_cast<std::size_t>(cx))); else if (cy > 0) { --cy; cx = static_cast<int>(lines[static_cast<std::size_t>(cy)].size()); } return;
            case KEY_RIGHT: if (static_cast<std::size_t>(cx) < L.size()) cx = static_cast<int>(u8_next(L, static_cast<std::size_t>(cx))); else if (static_cast<std::size_t>(cy) + 1 < lines.size()) { ++cy; cx = 0; } return;
            case KEY_UP: case KEY_DOWN: case KEY_PAGEUP: case KEY_PAGEDOWN: {
                const int col = u8_len(L.substr(0, static_cast<std::size_t>(cx)));
                const int step = (e.code == KEY_PAGEUP || e.code == KEY_PAGEDOWN) ? 12 : 1;
                cy = clampi(cy + ((e.code == KEY_UP || e.code == KEY_PAGEUP) ? -step : step), 0, static_cast<int>(lines.size()) - 1);
                cx = static_cast<int>(u8_at(lines[static_cast<std::size_t>(cy)], col));
                return;
            }
            case KEY_HOME: cx = 0; return;
            case KEY_END: cx = static_cast<int>(L.size()); return;
            case KEY_TAB: L.insert(static_cast<std::size_t>(cx), "    "); cx += 4; changed = true; return;
            default: break;
        }
        if (!e.text.empty()) { L.insert(static_cast<std::size_t>(cx), e.text); cx += static_cast<int>(e.text.size()); changed = true; status.clear(); }
    }
};

// -------------------------------------------------------------- Calculator
struct Calc : App {
    std::string expr, shown;
    // recursive descent evaluator: + - * / ( ) and unary minus
    const char* p = nullptr;
    bool err = false;
    double num() {
        while (*p == ' ') ++p;
        if (*p == '(') { ++p; const double v = add(); if (*p == ')') ++p; else err = true; return v; }
        if (*p == '-') { ++p; return -num(); }
        char* end = nullptr;
        const double v = std::strtod(p, &end);
        if (end == p) { err = true; return 0; }
        p = end;
        return v;
    }
    double mul() {
        double v = num();
        for (;;) {
            while (*p == ' ') ++p;
            if (*p == '*') { ++p; v *= num(); }
            else if (*p == '/') { ++p; const double d = num(); if (d == 0) { err = true; return 0; } v /= d; }
            else return v;
        }
    }
    double add() {
        double v = mul();
        for (;;) {
            while (*p == ' ') ++p;
            if (*p == '+') { ++p; v += mul(); }
            else if (*p == '-') { ++p; v -= mul(); }
            else return v;
        }
    }
    void eval() {
        p = expr.c_str();
        err = false;
        const double v = add();
        while (*p == ' ') ++p;
        if (err || *p) { shown = t("Error", "Ошибка"); return; }
        char buf[64];
        std::snprintf(buf, sizeof buf, "%.10g", v);
        shown = buf;
        expr = buf;
    }
    static const char* label(int i) {
        static const char* const L[20] = {"C", "(", ")", "/", "7", "8", "9", "*", "4", "5", "6", "-", "1", "2", "3", "+", "0", ".", "<", "="};
        return L[i];
    }
    Rect cell(int i) const {
        const int pad = 12 * sc(), top = 96 * sc(), gap = 8 * sc();
        const int cw = (w - 2 * pad - 3 * gap) / 4, ch = (h - top - pad - 4 * gap) / 5;
        return {pad + (i % 4) * (cw + gap), top + (i / 4) * (ch + gap), cw, ch};
    }
    void press(const std::string& k) {
        if (k == "C") { expr.clear(); shown.clear(); }
        else if (k == "<") { if (!expr.empty()) expr.pop_back(); shown.clear(); }
        else if (k == "=") eval();
        else { if (!shown.empty() && shown == expr && (k[0] >= '0' && k[0] <= '9')) expr.clear(); shown.clear(); expr += k; }
    }
    void draw(Surface& s) override {
        const Rect disp = {12 * sc(), 12 * sc(), w - 24 * sc(), 72 * sc()};
        rrect(s, disp, 12 * sc(), kGraphite900);
        rrect_outline(s, disp, 12 * sc(), 1, 0x30FFFFFFu);
        const std::string e = fit_text(expr.empty() ? "0" : expr, disp.w - 24 * sc(), sc() * 2);
        draw_text(s, disp.right() - 12 * sc() - text_width(e, sc() * 2), disp.y + 14 * sc(), e, kText, sc() * 2);
        if (!shown.empty() && shown != expr) draw_text(s, disp.x + 12 * sc(), disp.y + 4 * sc(), shown, shown == t("Error", "Ошибка") ? kDanger : kTextDim, sc());
        // operators are single thin glyphs: draw the keys at double size when the key is tall enough
        for (int i = 0; i < 20; ++i) {
            const Rect c = cell(i);
            const int ls = c.h >= font_h() * sc() * 2 + 6 * sc() ? sc() * 2 : sc();
            button(s, c, label(i), i == 19, i == 0, ls);
        }
    }
    void mouse(const Event& e) override {
        if (e.mouse_kind != 1) return;
        for (int i = 0; i < 20; ++i) if (cell(i).has(e.x, e.y)) press(label(i));
    }
    void key(const Event& e) override {
        if (e.code == KEY_ENTER || e.code == KEY_KPENTER) { press("="); return; }
        if (e.code == KEY_BACKSPACE) { press("<"); return; }
        if (e.code == KEY_ESC) { press("C"); return; }
        if (e.text.size() == 1 && std::strchr("0123456789+-*/.()", e.text[0])) press(e.text);
        else if (e.code == KEY_EQUAL && !(e.mods & 1)) press("=");
    }
};

// ---------------------------------------------------------------- Terminal
struct Terminal : App {
    std::vector<std::string> out;
    std::string in;
    std::vector<std::string> hist;
    int hidx = -1, scroll = 0;

    void banner() {
        add("Difiw OS 0.5.5 Core");
        add(t("Type help for the list of commands.", "Введите help, чтобы увидеть список команд."));
        add("");
        if (chdir("/homes/shared") != 0) {}
    }
    void add(const std::string& line) {
        const int cols = std::max(10, (w - 28 * sc()) / (font_w() * sc()));
        std::string rest = line;
        do {
            std::size_t cut = 0;
            for (int i = 0; i < cols && cut < rest.size(); ++i) cut = u8_next(rest, cut);
            out.push_back(rest.substr(0, cut));
            rest.erase(0, cut);
        } while (!rest.empty());
        if (out.size() > 800) out.erase(out.begin(), out.begin() + 200);
    }
    void add_block(const std::string& text) {
        std::string cur;
        for (std::size_t i = 0; i < text.size(); ++i) {
            const char c = text[i];
            if (c == 0x1b) { ++i; if (i < text.size() && text[i] == '[') while (i + 1 < text.size() && !(text[i + 1] >= 0x40 && text[i + 1] <= 0x7e)) ++i; ++i; continue; }
            if (c == '\n') { add(cur); cur.clear(); }
            else if (c == '\t') cur += "    ";
            else if (c != '\r') cur += c;
        }
        if (!cur.empty()) add(cur);
    }
    std::string run_cmd(const std::string& line) {
        int pp[2];
        if (pipe2(pp, O_CLOEXEC) != 0) return std::string("pipe: ") + std::strerror(errno) + "\n";
        const pid_t pid = fork();
        if (pid == 0) {
            const int nul = open("/dev/null", O_RDONLY);
            if (nul >= 0) dup2(nul, 0);
            dup2(pp[1], 1);
            dup2(pp[1], 2);
            execl("/usr/bin/difiw-shell", "difiw-shell", "-c", line.c_str(), static_cast<char*>(nullptr));
            _exit(127);
        }
        close(pp[1]);
        std::string res;
        char buf[2048];
        const long deadline = mono_ms() + 15000;
        for (;;) {
            struct pollfd pf = {pp[0], POLLIN, 0};
            if (mono_ms() > deadline) { kill(pid, SIGKILL); res += "\n[timeout]\n"; break; }
            if (poll(&pf, 1, 500) <= 0) continue;
            const ssize_t n = read(pp[0], buf, sizeof buf);
            if (n <= 0) break;
            res.append(buf, static_cast<std::size_t>(n));
            if (res.size() > 100000) { kill(pid, SIGKILL); break; }
        }
        close(pp[0]);
        int st = 0;
        waitpid(pid, &st, 0);
        return res;
    }
    void exec(const std::string& line) {
        char cwd[4096];
        const std::string wd = getcwd(cwd, sizeof cwd) ? cwd : "/";
        add("difiw:" + wd + "> " + line);
        std::istringstream is(line);
        std::string cmd;
        is >> cmd;
        if (cmd.empty()) return;
        if (cmd == "exit") { quit = true; return; }  // closes this Terminal window only
        if (cmd == "terminal") { cl.request(proto::RQ_QUIT_DESKTOP); return; }  // back to the text console
        if (cmd == "poweroff") { cl.request(proto::RQ_POWEROFF); return; }
        if (cmd == "reboot") { cl.request(proto::RQ_REBOOT); return; }
        if (cmd == "screen") { add(t("The graphical shell is already running.", "Графическая оболочка уже запущена.")); return; }
        if (cmd == "clear" || cmd == "cls") { out.clear(); return; }
        if (cmd == "cd") {
            std::string arg;
            is >> arg;
            const char* home = std::getenv("HOME");
            if (arg.empty()) arg = home ? home : "/";
            if (chdir(arg.c_str()) != 0) add("difiw: cd: " + arg + ": " + std::strerror(errno));
            return;
        }
        add_block(run_cmd(line));
    }
    void on_denied(int rq) override {
        add(t("Permission denied for this request (", "Отказано: у приложения нет разрешения (") + std::to_string(rq) + ")");
    }
    void draw(Surface& s) override {
        const Rect b = {10 * sc(), 10 * sc(), w - 20 * sc(), h - 20 * sc()};
        fill(s, {0, 0, w, h}, 0xFF000000u);  // like the text console: plain black
        const int fh = font_h() * sc();
        const int rows = (b.h - 12 * sc()) / fh - 1;
        const int total = static_cast<int>(out.size());
        scroll = clampi(scroll, 0, std::max(0, total - rows));
        const int first = std::max(0, total - rows - scroll);
        const Rect save = s.clip;
        s.clip = save.intersect(b.inset(2));
        int y = b.y + 6 * sc();
        for (int i = first; i < total && i < first + rows; ++i) { draw_text(s, b.x + 10 * sc(), y, out[static_cast<std::size_t>(i)], kText, sc()); y += fh; }
        char cwd[4096];
        const std::string prompt = std::string("difiw:") + (getcwd(cwd, sizeof cwd) ? cwd : "/") + "> ";
        const int py = b.bottom() - fh - 6 * sc();
        fill(s, {b.x + 1, py - 4 * sc(), b.w - 2, 1}, 0x30FFFFFFu);
        const int x = draw_text(s, b.x + 10 * sc(), py, prompt, kOk, sc());
        const int ex = draw_text(s, x, py, in, kText, sc());
        fill(s, {ex, py, font_w() * sc(), fh}, kAccentBlue);
        s.clip = save;
    }
    void key(const Event& e) override {
        switch (e.code) {
            case KEY_ENTER: { const std::string l = in; in.clear(); scroll = 0; if (!l.empty()) { hist.push_back(l); hidx = -1; } exec(l); return; }
            case KEY_BACKSPACE: if (!in.empty()) in.erase(u8_prev(in, in.size())); return;
            case KEY_UP: if (!hist.empty()) { hidx = hidx < 0 ? static_cast<int>(hist.size()) - 1 : std::max(0, hidx - 1); in = hist[static_cast<std::size_t>(hidx)]; } return;
            case KEY_DOWN: if (hidx >= 0) { ++hidx; if (hidx >= static_cast<int>(hist.size())) { hidx = -1; in.clear(); } else in = hist[static_cast<std::size_t>(hidx)]; } return;
            case KEY_PAGEUP: scroll += 8; return;
            case KEY_PAGEDOWN: scroll = std::max(0, scroll - 8); return;
            default: break;
        }
        if (!e.text.empty() && in.size() < 1000) in += e.text;
    }
    void mouse(const Event& e) override { if (e.mouse_kind == 3) scroll = std::max(0, scroll + e.wheel * 3); }
};

// ------------------------------------------------------------------ Monitor
struct Monitor : App {
    long total = 0, avail = 0, procs = 0;
    double up = 0, l1 = 0;
    unsigned long long prev_busy = 0, prev_all = 0;
    double cpu = 0;
    void tick() override {
        std::ifstream mi("/proc/meminfo");
        std::string k, unit;
        long v;
        while (mi >> k >> v >> unit) { if (k == "MemTotal:") total = v; else if (k == "MemAvailable:") avail = v; }
        std::ifstream st("/proc/stat");
        std::string cpuw;
        unsigned long long u = 0, n = 0, sy = 0, id = 0, io = 0, irq = 0, sirq = 0;
        st >> cpuw >> u >> n >> sy >> id >> io >> irq >> sirq;
        const unsigned long long all = u + n + sy + id + io + irq + sirq, busy = all - id - io;
        if (prev_all && all > prev_all) cpu = 100.0 * static_cast<double>(busy - prev_busy) / static_cast<double>(all - prev_all);
        prev_busy = busy; prev_all = all;
        std::ifstream ut("/proc/uptime");
        ut >> up;
        std::ifstream la("/proc/loadavg");
        la >> l1;
    }
    void draw(Surface& s) override {
        if (total == 0) tick();
        int y = 16 * sc();
        draw_text(s, 16 * sc(), y, t("System monitor", "Монитор системы"), kText, sc()); y += 34 * sc();
        auto bar = [&](const std::string& label, double frac, const std::string& value) {
            draw_text(s, 16 * sc(), y, label, kTextDim, sc());
            const Rect b = {16 * sc(), y + 22 * sc(), w - 32 * sc(), 16 * sc()};
            rrect(s, b, 8 * sc(), kGraphite900);
            rrect(s, {b.x, b.y, std::max(8 * sc(), static_cast<int>(static_cast<double>(b.w) * clampf(static_cast<float>(frac), 0.0f, 1.0f))), b.h}, 8 * sc(), kAccentTeal);
            draw_text(s, w - 16 * sc() - text_width(value, sc()), y, value, kText, sc());
            y += 54 * sc();
        };
        const long used = total - avail;
        char b1[64], b2[64];
        std::snprintf(b1, sizeof b1, "%ld / %ld MiB", used / 1024, total / 1024);
        bar(t("Memory", "Память"), total ? static_cast<double>(used) / static_cast<double>(total) : 0, b1);
        std::snprintf(b2, sizeof b2, "%.0f%%", cpu);
        bar(t("Processor", "Процессор"), cpu / 100.0, b2);
        char b3[96];
        std::snprintf(b3, sizeof b3, "%s %dh %02dm   load %.2f", t("Uptime", "Работает").c_str(), static_cast<int>(up) / 3600, (static_cast<int>(up) / 60) % 60, l1);
        draw_text(s, 16 * sc(), y, b3, kTextDim, sc());
    }
};

// -------------------------------------------------------------------- Probe
struct Probe : App {
    struct Row { bool pass; std::string name, detail; };
    std::vector<Row> rows;
    void check(bool pass, const std::string& name, const std::string& detail) { rows.push_back({pass, name, detail}); }
    static const char* err(int e) { return std::strerror(e); }
    void run_checks() {
        rows.clear();
        const char* home = std::getenv("HOME");
        const std::string me = std::getenv("DIFIW_APP") ? std::getenv("DIFIW_APP") : "?";
        const std::string mine = std::string(home ? home : "/tmp") + "/probe-write.txt";
        // 1. my own home is writable
        { const int fd = open(mine.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600); check(fd >= 0, t("write to own home", "запись в собственный дом"), fd >= 0 ? "ok" : err(errno)); if (fd >= 0) close(fd); }
        // 2. another app's home is not readable
        const char* others[] = {"notes", "calc", "terminal", "monitor"};
        for (const char* o : others) {
            if (me == o) continue;
            const std::string p = std::string("/homes/user/apps/") + o + "/home-marker.txt";
            const int fd = open(p.c_str(), O_RDONLY);
            const int e = errno;
            check(fd < 0, t("read home of '", "чтение дома «") + o + (ru ? "»" : "'"), fd < 0 ? err(e) : "READ SUCCEEDED");
            if (fd >= 0) close(fd);
            break;
        }
        // 3. system files cannot be changed
        { const int fd = open("/usr/bin/difiw-probe-test", O_WRONLY | O_CREAT, 0600); const int e = errno;
          check(fd < 0, t("write to /usr (system)", "запись в /usr (система)"), fd < 0 ? err(e) : "WRITE SUCCEEDED"); if (fd >= 0) { close(fd); unlink("/usr/bin/difiw-probe-test"); } }
        { const int fd = open("/etc/difiw/probe-test", O_WRONLY | O_CREAT, 0600); const int e = errno;
          check(fd < 0, t("write to /etc (system)", "запись в /etc (система)"), fd < 0 ? err(e) : "WRITE SUCCEEDED"); if (fd >= 0) { close(fd); unlink("/etc/difiw/probe-test"); } }
        // 4. runtime dir of another app
        { const int fd = open("/run/difiw/notes/app.log", O_RDONLY); const int e = errno;
          if (me != "notes") { check(fd < 0, t("read runtime data of another app", "чтение данных запуска другого приложения"), fd < 0 ? err(e) : "READ SUCCEEDED"); } if (fd >= 0) close(fd); }
        // 5. root-only process data
        { const int fd = open("/proc/1/environ", O_RDONLY); const int e = errno;
          check(fd < 0, t("read environment of PID 1", "чтение окружения процесса 1"), fd < 0 ? err(e) : "READ SUCCEEDED"); if (fd >= 0) close(fd); }
        // 6b. processes of other users are not visible (/proc is mounted with hidepid=2)
        { int foreign = 0, seen = 0;
          if (DIR* d = opendir("/proc")) {
              while (struct dirent* e = readdir(d)) {
                  if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
                  struct stat sb;
                  const std::string p = std::string("/proc/") + e->d_name;
                  if (stat(p.c_str(), &sb) != 0) continue;
                  ++seen;
                  if (sb.st_uid != getuid()) ++foreign;
              }
              closedir(d);
          }
          check(foreign == 0 && seen > 0, t("other users' processes are hidden", "чужие процессы не видны"), std::to_string(seen) + " visible, " + std::to_string(foreign) + " foreign"); }
        // 6. no root privileges
        { const bool nr = getuid() != 0 && geteuid() != 0 && setuid(0) != 0;
          check(nr, t("no root privileges", "нет прав root"), "uid=" + std::to_string(getuid())); }
        std::ofstream f(std::string(home ? home : "/tmp") + "/probe-result.txt", std::ios::trunc);
        for (const auto& r : rows) f << (r.pass ? "PASS " : "FAIL ") << r.name << " (" << r.detail << ")\n";
    }
    void draw(Surface& s) override {
        if (rows.empty()) run_checks();
        draw_text(s, 16 * sc(), 14 * sc(), t("Home isolation check", "Проверка изоляции дома"), kText, sc());
        char who[64];
        std::snprintf(who, sizeof who, "uid=%d  app=%s", static_cast<int>(getuid()), std::getenv("DIFIW_APP") ? std::getenv("DIFIW_APP") : "?");
        draw_text(s, 16 * sc(), 38 * sc(), who, kTextDim, sc());
        int y = 68 * sc();
        for (const auto& r : rows) {
            draw_text(s, 16 * sc(), y, r.pass ? "[PASS]" : "[FAIL]", r.pass ? kOk : kDanger, sc());
            draw_text(s, 16 * sc() + 8 * font_w() * sc(), y, fit_text(r.name, w - 150 * sc(), sc()), kText, sc());
            draw_text(s, 16 * sc() + 8 * font_w() * sc(), y + font_h() * sc(), fit_text(r.detail, w - 150 * sc(), 1), kTextFaint, 1);
            y += font_h() * sc() + 22 * sc();
        }
        button(s, {w - 130 * sc(), h - 46 * sc(), 114 * sc(), 32 * sc()}, t("Repeat", "Повторить"), true);
    }
    void mouse(const Event& e) override {
        if (e.mouse_kind == 1 && Rect{w - 130 * sc(), h - 46 * sc(), 114 * sc(), 32 * sc()}.has(e.x, e.y)) run_checks();
    }
};

#include "apps_extra.hpp"
#include "apps_extra2.hpp"
#include "web.hpp"
#include "browser.hpp"
#include "settings_app.hpp"

// ------------------------------------------------------------------- Worker
// A background app without a window: it is shown only in the background-tasks panel.
int worker_main() {
    Client cl;
    if (!cl.connect()) return 3;
    Event ev;
    for (;;) {
        if (cl.next_event(ev, 1000) && ev.kind == Event::Disconnected) return 0;
    }
}

// Test app without any permission: it asks to quit the desktop and must be refused.
int reqtest_main() {
    Client cl;
    if (!cl.connect()) return 3;
    cl.request(proto::RQ_QUIT_DESKTOP);
    Event ev;
    const long until = mono_ms() + 3000;
    while (mono_ms() < until)
        if (cl.next_event(ev, 200) && ev.kind == Event::Denied) return 0;  // refused, as it should be
    return 1;
}

}  // namespace

int main(int argc, char** argv) {
    const std::string name = argc > 1 ? argv[1] : "";
    signal(SIGPIPE, SIG_IGN);
    if (name == "notes") { Todo a; a.load(); return a.run(460, 480, "Notes", "Заметки"); }
    if (name == "notepad") { Notepad a; a.load(); return a.run(620, 440, "Notepad", "Блокнот"); }
    if (name == "calc") { Calc a; return a.run(300, 420, "Calculator", "Калькулятор"); }
    if (name == "terminal") { Terminal a; a.ru = lang_ru(); a.banner(); return a.run(660, 420, "Terminal", "Терминал"); }
    if (name == "monitor") { Monitor a; return a.run(420, 210, "System Monitor", "Системный монитор"); }
    if (name == "probe") { Probe a; return a.run(560, 440, "Home check", "Проверка дома"); }
    if (name == "files") { Files a; a.init(); return a.run(720, 430, "Files", "Файлы"); }
    if (name == "recorder") { Recorder a; a.load(); return a.run(520, 400, "Voice Recorder", "Диктофон"); }
    if (name == "camera") { Camera a; return a.run(640, 480, "Camera", "Камера"); }
    if (name == "netline") { Netline a; a.init(); return a.run(1000, 640, "Netline", "Netline"); }
    if (name == "settings") { Settings a; return a.run(780, 560, "Settings", "Параметры"); }
    if (name == "worker") return worker_main();
    if (name == "reqtest") return reqtest_main();
    std::fprintf(stderr, "usage: difiw-apps notes|notepad|settings|calc|terminal|monitor|probe|files|recorder|camera|worker\n");
    return 2;
}
