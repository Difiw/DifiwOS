// Difiw 0.5.0 applications: Files, Voice Recorder, Camera.
// Included by apps.cpp inside its anonymous namespace (they use the App framework defined there).
// SPDX-License-Identifier: MIT
#pragma once

// ------------------------------------------------------------------- shared helpers
std::string fmt_size(long long n) {
    char b[32];
    if (n < 1024) std::snprintf(b, sizeof b, "%lld B", n);
    else if (n < 1024 * 1024) std::snprintf(b, sizeof b, "%.1f KB", static_cast<double>(n) / 1024.0);
    else if (n < 1024LL * 1024 * 1024) std::snprintf(b, sizeof b, "%.1f MB", static_cast<double>(n) / (1024.0 * 1024.0));
    else std::snprintf(b, sizeof b, "%.1f GB", static_cast<double>(n) / (1024.0 * 1024.0 * 1024.0));
    return b;
}

std::string fmt_time(time_t t) {
    struct tm tm;
    localtime_r(&t, &tm);
    char b[32];
    std::strftime(b, sizeof b, "%d.%m.%Y %H:%M", &tm);
    return b;
}

std::string stamp() {
    time_t tt = time(nullptr);
    struct tm tm;
    localtime_r(&tt, &tm);
    char b[32];
    std::strftime(b, sizeof b, "%Y%m%d-%H%M%S", &tm);
    return b;
}

bool path_exists(const std::string& p) {
    struct stat sb;
    return stat(p.c_str(), &sb) == 0;
}

// ------------------------------------------------------------------- Files
struct Files : App {
    struct Entry { std::string name; bool dir = false; long long size = 0; time_t mtime = 0; };
    struct Place { std::string en, ru, path; };
    std::string cwd, status, clip, newname;
    std::vector<Entry> list;
    std::vector<Place> places;
    int sel = -1, top = 0, last_click = -1;
    long last_click_ms = 0;
    bool naming = false, confirm_del = false, show_hidden = false;
    std::vector<std::string> preview;

    void init() {
        const char* h = std::getenv("HOME");
        places.push_back({"Home", "Домой", h ? h : "/"});
        places.push_back({"Shared", "Общая папка", "/homes/shared"});
        places.push_back({"Documents", "Документы", "/homes/shared/Documents"});
        places.push_back({"Pictures", "Фото", "/homes/shared/Pictures"});
        places.push_back({"Recordings", "Записи", "/homes/shared/Recordings"});
        places.push_back({"Downloads", "Загрузки", "/homes/shared/Downloads"});
        struct stat sa, sb;
        if (stat("/data", &sa) == 0 && stat("/", &sb) == 0 && sa.st_dev != sb.st_dev) places.push_back({"Data disk", "Диск данных", "/data"});
        places.push_back({"System", "Система", "/"});
        go(places[1].path);
    }
    static std::string parent_of(const std::string& p) {
        std::string q = p;
        while (q.size() > 1 && q.back() == '/') q.pop_back();
        const std::size_t s = q.rfind('/');
        return s == 0 || s == std::string::npos ? "/" : q.substr(0, s);
    }
    static std::string join(const std::string& a, const std::string& b) { return a == "/" ? "/" + b : a + "/" + b; }
    bool load(const std::string& path) {
        DIR* d = opendir(path.c_str());
        if (!d) { status = path + ": " + std::strerror(errno); return false; }
        std::vector<Entry> v;
        while (struct dirent* e = readdir(d)) {
            const std::string n = e->d_name;
            if (n == "." || n == ".." || (!show_hidden && n[0] == '.')) continue;
            struct stat sb;
            Entry en;
            en.name = n;
            if (stat(join(path, n).c_str(), &sb) == 0) { en.dir = S_ISDIR(sb.st_mode); en.size = sb.st_size; en.mtime = sb.st_mtime; }
            else if (e->d_type == DT_DIR) en.dir = true;
            v.push_back(en);
        }
        closedir(d);
        std::sort(v.begin(), v.end(), [](const Entry& a, const Entry& b) {
            if (a.dir != b.dir) return a.dir;
            return strcasecmp(a.name.c_str(), b.name.c_str()) < 0;
        });
        list = v;
        return true;
    }
    void go(const std::string& path) {
        if (!load(path)) return;
        cwd = path; sel = -1; top = 0; status.clear(); confirm_del = false; naming = false;
        make_preview();
    }
    void make_preview() {
        preview.clear();
        if (sel < 0 || sel >= static_cast<int>(list.size()) || list[static_cast<std::size_t>(sel)].dir) return;
        std::ifstream f(join(cwd, list[static_cast<std::size_t>(sel)].name), std::ios::binary);
        if (!f) return;
        char buf[1024];
        f.read(buf, sizeof buf);
        const std::streamsize n = f.gcount();
        for (std::streamsize i = 0; i < n; ++i) if (buf[i] == 0) return;  // binary: no text preview
        std::istringstream is(std::string(buf, static_cast<std::size_t>(n)));
        std::string l;
        while (static_cast<int>(preview.size()) < 4 && std::getline(is, l)) preview.push_back(l);
    }
    void open_sel() {
        if (sel < 0 || sel >= static_cast<int>(list.size())) return;
        const Entry& e = list[static_cast<std::size_t>(sel)];
        if (e.dir) go(join(cwd, e.name));
    }
    static bool remove_tree(const std::string& p, int depth = 0) {
        struct stat sb;
        if (lstat(p.c_str(), &sb) != 0) return false;
        if (S_ISDIR(sb.st_mode) && depth < 16) {
            if (DIR* d = opendir(p.c_str())) {
                while (struct dirent* e = readdir(d)) {
                    const std::string n = e->d_name;
                    if (n != "." && n != "..") remove_tree(p + "/" + n, depth + 1);
                }
                closedir(d);
            }
            return rmdir(p.c_str()) == 0;
        }
        return unlink(p.c_str()) == 0;
    }
    void del_sel() {
        if (sel < 0 || sel >= static_cast<int>(list.size())) return;
        if (!confirm_del) {
            confirm_del = true;
            status = t("Press Delete again to remove: ", "Нажмите Delete ещё раз, чтобы удалить: ") + list[static_cast<std::size_t>(sel)].name;
            return;
        }
        const bool ok = remove_tree(join(cwd, list[static_cast<std::size_t>(sel)].name));
        const int e = errno;
        confirm_del = false;
        go(cwd);
        status = ok ? t("Deleted.", "Удалено.") : t("Cannot delete: ", "Не удалось удалить: ") + std::strerror(e);
    }
    void copy_here() {
        if (clip.empty()) { status = t("Nothing to paste (Ctrl+C first).", "Нечего вставлять (сначала Ctrl+C)."); return; }
        const std::string base = clip.substr(clip.rfind('/') + 1);
        std::string dst = join(cwd, base);
        for (int i = 1; path_exists(dst) && i < 100; ++i) dst = join(cwd, std::to_string(i) + "_" + base);
        const int in = open(clip.c_str(), O_RDONLY);
        if (in < 0) { status = clip + ": " + std::strerror(errno); return; }
        struct stat sb;
        if (fstat(in, &sb) != 0 || !S_ISREG(sb.st_mode)) { close(in); status = t("Only files can be copied.", "Копировать можно только файлы."); return; }
        const int out = open(dst.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0644);
        if (out < 0) { const int e = errno; close(in); status = t("Cannot copy here: ", "Сюда копировать нельзя: ") + std::strerror(e); return; }
        char buf[16384];
        ssize_t n;
        bool ok = true;
        while ((n = read(in, buf, sizeof buf)) > 0)
            if (write(out, buf, static_cast<std::size_t>(n)) != n) { ok = false; break; }
        close(in); close(out);
        go(cwd);
        status = ok ? t("Copied.", "Скопировано.") : t("Copy failed (disk full?).", "Ошибка копирования (нет места?).");
    }
    // ---- layout
    int side_w() const { return 150 * sc(); }
    int bar_h() const { return 64 * sc(); }
    int row_h() const { return (font_h() + 8) * sc(); }
    int prev_h() const { return 84 * sc(); }
    Rect list_rect() const { return {side_w(), bar_h() + row_h(), w - side_w(), h - bar_h() - row_h() - prev_h() - 22 * sc()}; }
    int rows() const { return std::max(1, list_rect().h / row_h()); }
    Rect btn(int i) const { return {side_w() + 8 * sc() + i * 112 * sc(), 8 * sc(), 104 * sc(), 28 * sc()}; }
    Rect place_rect(std::size_t i) const { return {6 * sc(), (40 + static_cast<int>(i) * 30) * sc(), side_w() - 12 * sc(), 26 * sc()}; }
    void draw(Surface& s) override {
        fill(s, {0, 0, side_w(), h}, kGraphite900);
        draw_text(s, 12 * sc(), 12 * sc(), t("Places", "Места"), kTextFaint, sc());
        for (std::size_t i = 0; i < places.size(); ++i) {
            const Rect r = place_rect(i);
            const bool cur = cwd == places[i].path;
            if (cur || r.has(mx, my)) rrect(s, r, 8 * sc(), cur ? 0xFF2F5FC0u : kGraphite600);
            draw_text(s, r.x + 10 * sc(), r.y + (r.h - font_h() * sc()) / 2, fit_text(t(places[i].en.c_str(), places[i].ru.c_str()), r.w - 16 * sc(), sc()), kText, sc());
        }
        button(s, btn(0), t("Up", "Вверх"));
        button(s, btn(1), t("New folder", "Новая папка"));
        button(s, btn(2), t("Delete", "Удалить"), false, true);
        button(s, btn(3), show_hidden ? t("Hide hidden", "Скрыть скрытые") : t("Show hidden", "Скрытые"));
        const Rect lr = list_rect();
        const int pbx = side_w() + 8 * sc();
        draw_text(s, pbx, 44 * sc(), fit_text(naming ? t("New folder name: ", "Имя новой папки: ") + newname + "_" : cwd, w - pbx - 8 * sc(), sc()), naming ? kWarn : kAccentBlue, sc());
        const int cs = side_w() + 10 * sc();
        const int c_size = w - 270 * sc(), c_date = w - 150 * sc();
        fill(s, {side_w(), lr.y - 1, w - side_w(), 1}, 0x30FFFFFFu);
        draw_text(s, cs, lr.y - row_h() + 3 * sc(), t("Name", "Имя"), kTextFaint, sc());
        draw_text(s, c_size, lr.y - row_h() + 3 * sc(), t("Size", "Размер"), kTextFaint, sc());
        draw_text(s, c_date, lr.y - row_h() + 3 * sc(), t("Changed", "Изменён"), kTextFaint, sc());
        const Rect save = s.clip;
        s.clip = save.intersect(lr);
        if (list.empty()) draw_text(s, cs, lr.y + 8 * sc(), t("This folder is empty.", "Папка пуста."), kTextDim, sc());
        for (int i = 0; i < rows() + 1; ++i) {
            const int idx = top + i;
            if (idx >= static_cast<int>(list.size())) break;
            const Entry& e = list[static_cast<std::size_t>(idx)];
            const Rect rr = {lr.x, lr.y + i * row_h(), lr.w, row_h()};
            if (idx == sel) fill(s, rr, 0xFF2F5FC0u);
            else if (rr.has(mx, my)) fill(s, rr, kGraphite700);
            const std::string nm = (e.dir ? "[+] " : "    ") + e.name;
            draw_text(s, cs, rr.y + 4 * sc(), fit_text(nm, c_size - cs - 8 * sc(), sc()), e.dir ? kWarn : kText, sc());
            if (!e.dir) draw_text(s, c_size, rr.y + 4 * sc(), fmt_size(e.size), kTextDim, sc());
            draw_text(s, c_date, rr.y + 4 * sc(), fit_text(fmt_time(e.mtime), w - c_date - 4 * sc(), sc()), kTextFaint, sc());
        }
        s.clip = save;
        const int py = lr.bottom() + 2 * sc();
        fill(s, {side_w(), py, w - side_w(), 1}, 0x30FFFFFFu);
        for (std::size_t i = 0; i < preview.size(); ++i)
            draw_text(s, cs, py + 6 * sc() + static_cast<int>(i) * font_h() * sc(), fit_text(preview[i], w - cs - 8 * sc(), sc()), kTextDim, sc());
        if (preview.empty() && sel >= 0 && sel < static_cast<int>(list.size()) && !list[static_cast<std::size_t>(sel)].dir)
            draw_text(s, cs, py + 6 * sc(), t("No text preview for this file.", "Предпросмотр недоступен."), kTextFaint, sc());
        const std::string info = status.empty() ? std::to_string(list.size()) + t(" items   Ctrl+C / Ctrl+V copy, Delete removes", " элементов   Ctrl+C / Ctrl+V копировать, Delete удалить") : status;
        draw_text(s, cs, h - 20 * sc(), fit_text(info, w - cs - 8 * sc(), sc()), status.empty() ? kTextFaint : kWarn, sc());
    }
    void mouse(const Event& e) override {
        if (e.mouse_kind == 3) { top = clampi(top - e.wheel * 3, 0, std::max(0, static_cast<int>(list.size()) - rows())); return; }
        if (e.mouse_kind != 1) return;
        for (std::size_t i = 0; i < places.size(); ++i)
            if (place_rect(i).has(e.x, e.y)) { go(places[i].path); return; }
        if (btn(0).has(e.x, e.y)) { go(parent_of(cwd)); return; }
        if (btn(1).has(e.x, e.y)) { naming = true; newname.clear(); confirm_del = false; return; }
        if (btn(2).has(e.x, e.y)) { del_sel(); return; }
        if (btn(3).has(e.x, e.y)) { show_hidden = !show_hidden; go(cwd); return; }
        const Rect lr = list_rect();
        if (lr.has(e.x, e.y)) {
            const int idx = top + (e.y - lr.y) / row_h();
            if (idx < static_cast<int>(list.size())) {
                const long now = mono_ms();
                const bool dbl = idx == last_click && now - last_click_ms < 450;
                sel = idx; last_click = idx; last_click_ms = now; confirm_del = false; status.clear();
                make_preview();
                if (dbl) open_sel();
            }
        }
    }
    void key(const Event& e) override {
        if (naming) {
            if (e.code == KEY_ESC) { naming = false; return; }
            if (e.code == KEY_ENTER) {
                naming = false;
                if (!newname.empty()) {
                    const std::string n = newname;
                    if (mkdir(join(cwd, n).c_str(), 0755) != 0) status = std::string(t("Cannot create folder: ", "Не удалось создать папку: ")) + std::strerror(errno);
                    else {
                        go(cwd);
                        status = t("Folder created.", "Папка создана.");
                        for (std::size_t i = 0; i < list.size(); ++i) if (list[i].name == n) sel = static_cast<int>(i);
                    }
                }
                return;
            }
            if (e.code == KEY_BACKSPACE) { if (!newname.empty()) newname.erase(u8_prev(newname, newname.size())); return; }
            if (!e.text.empty() && e.text != "/" && newname.size() < 100) newname += e.text;
            return;
        }
        const int n = static_cast<int>(list.size());
        switch (e.code) {
            case KEY_UP: sel = std::max(0, (sel < 0 ? n : sel) - 1); break;
            case KEY_DOWN: sel = std::min(n - 1, sel + 1); break;
            case KEY_HOME: sel = n ? 0 : -1; break;
            case KEY_END: sel = n - 1; break;
            case KEY_ENTER: open_sel(); return;
            case KEY_BACKSPACE: go(parent_of(cwd)); return;
            case KEY_DELETE: del_sel(); return;
            case KEY_F5: go(cwd); return;
            case KEY_C:
                if ((e.mods & 2) && sel >= 0 && sel < n && !list[static_cast<std::size_t>(sel)].dir) {
                    clip = join(cwd, list[static_cast<std::size_t>(sel)].name);
                    status = t("Copied to clipboard.", "Скопировано в буфер.");
                }
                return;
            case KEY_V: if (e.mods & 2) copy_here(); return;
            default: return;
        }
        confirm_del = false;
        if (sel >= 0) { if (sel < top) top = sel; if (sel >= top + rows()) top = sel - rows() + 1; }
        make_preview();
    }
};

// ------------------------------------------------------------------- Voice Recorder
struct Recorder : App {
    struct Rec { std::string name; long long size = 0; };
    const std::string dir = "/homes/shared/Recordings";
    std::vector<Rec> recs;
    int sel = -1;
    pid_t rec_pid = 0, play_pid = 0;
    long rec_start = 0;
    std::string rec_file, status;

    int tick_ms() override { return 250; }
    void load() {
        recs.clear();
        if (DIR* d = opendir(dir.c_str())) {
            while (struct dirent* e = readdir(d)) {
                const std::string n = e->d_name;
                if (n.size() < 5 || n.compare(n.size() - 4, 4, ".wav") != 0) continue;
                struct stat sb;
                if (stat((dir + "/" + n).c_str(), &sb) != 0) continue;
                recs.push_back({n, sb.st_size});
            }
            closedir(d);
        }
        std::sort(recs.begin(), recs.end(), [](const Rec& a, const Rec& b) { return a.name > b.name; });
        if (sel >= static_cast<int>(recs.size())) sel = static_cast<int>(recs.size()) - 1;
    }
    std::string errfile() const {
        const char* tmp = std::getenv("TMPDIR");
        return std::string(tmp ? tmp : "/tmp") + "/audio.err";
    }
    std::string first_error() const {
        std::ifstream f(errfile());
        std::string l;
        std::getline(f, l);
        return l;
    }
    pid_t spawn(const std::vector<std::string>& args) {
        const pid_t pid = fork();
        if (pid == 0) {
            const int nul = open("/dev/null", O_RDWR);
            if (nul >= 0) { dup2(nul, 0); dup2(nul, 1); }
            const int er = open(errfile().c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
            if (er >= 0) dup2(er, 2);
            std::vector<char*> av;
            for (const auto& a : args) av.push_back(const_cast<char*>(a.c_str()));
            av.push_back(nullptr);
            execv(av[0], av.data());
            _exit(127);
        }
        return pid;
    }
    void start_rec() {
        stop_play();
        if (access("/dev/snd", F_OK) != 0) {
            status = t("No sound device. VirtualBox: Settings > Audio, enable input.", "Нет звукового устройства. VirtualBox: Настройки > Звук, включите ввод.");
            return;
        }
        rec_file = dir + "/rec-" + stamp() + ".wav";
        rec_pid = spawn({"/usr/bin/arecord", "-q", "-D", "plughw:0,0", "-f", "S16_LE", "-r", "16000", "-c", "1", "-t", "wav", rec_file});
        rec_start = mono_ms();
        status.clear();
    }
    void stop_rec() {
        if (rec_pid > 0) { kill(rec_pid, SIGINT); int st; waitpid(rec_pid, &st, 0); }
        rec_pid = 0;
        load();
        for (std::size_t i = 0; i < recs.size(); ++i) if (dir + "/" + recs[i].name == rec_file) sel = static_cast<int>(i);
        if (status.empty()) status = t("Saved in Shared/Recordings.", "Сохранено: Общая папка/Записи.");
    }
    void stop_play() {
        if (play_pid > 0) { kill(play_pid, SIGTERM); int st; waitpid(play_pid, &st, 0); }
        play_pid = 0;
    }
    void play_sel() {
        if (sel < 0 || sel >= static_cast<int>(recs.size())) return;
        stop_play();
        play_pid = spawn({"/usr/bin/aplay", "-q", "-D", "plughw:0,0", dir + "/" + recs[static_cast<std::size_t>(sel)].name});
        status.clear();
    }
    void tick() override {
        int st = 0;
        if (rec_pid > 0 && waitpid(rec_pid, &st, WNOHANG) == rec_pid) {  // arecord ended by itself: it failed
            rec_pid = 0;
            status = t("Recording failed: ", "Запись не удалась: ") + first_error();
            load();
        }
        if (play_pid > 0 && waitpid(play_pid, &st, WNOHANG) == play_pid) {
            play_pid = 0;
            if (WIFEXITED(st) && WEXITSTATUS(st) != 0) status = t("Playback failed: ", "Воспроизведение не удалось: ") + first_error();
        }
    }
    void on_close() override { stop_rec(); stop_play(); quit = true; }
    Rect rec_btn() const { return {w / 2 - 36 * sc(), 18 * sc(), 72 * sc(), 72 * sc()}; }
    Rect row(int i) const { return {16 * sc(), (166 + i * 28) * sc(), w - 32 * sc(), 26 * sc()}; }
    Rect act(int i) const { return {16 * sc() + i * 112 * sc(), h - 46 * sc(), 104 * sc(), 30 * sc()}; }
    void draw(Surface& s) override {
        if (recs.empty() && sel < 0) load();
        const bool rec = rec_pid > 0;
        const Rect rb = rec_btn();
        const long el = rec ? (mono_ms() - rec_start) / 1000 : 0;
        circle(s, rb.x + rb.w / 2, rb.y + rb.h / 2, rb.w / 2, rec ? kDanger : 0xFF8A3A30u);
        if (rec) rrect(s, {rb.x + rb.w / 2 - 14 * sc(), rb.y + rb.h / 2 - 14 * sc(), 28 * sc(), 28 * sc()}, 4 * sc(), kText);
        else circle(s, rb.x + rb.w / 2, rb.y + rb.h / 2, 16 * sc(), kText);
        char tm[32];
        std::snprintf(tm, sizeof tm, "%02ld:%02ld", el / 60, el % 60);
        draw_text(s, w / 2 - text_width(tm, 2 * sc()) / 2, rb.bottom() + 10 * sc(), tm, rec ? kDanger : kTextDim, 2 * sc());
        if (rec) {  // moving bars: "recording is running" indicator
            for (int i = 0; i < 9; ++i) {
                const int bh = (6 + static_cast<int>((mono_ms() / 90 + i * 5) % 11)) * sc();
                fill(s, {(i < 4 ? w / 2 - 150 * sc() + i * 20 * sc() : w / 2 + 70 * sc() + (i - 5) * 20 * sc()), rb.y + rb.h / 2 - bh / 2, 8 * sc(), bh}, 0xFFFF9A9Au);
            }
        }
        draw_text(s, 16 * sc(), 142 * sc(), t("Recordings (newest first)", "Записи (новые сверху)"), kTextFaint, sc());
        const int maxrows = std::max(1, (h - 166 * sc() - 60 * sc()) / (28 * sc()));
        for (int i = 0; i < maxrows && i < static_cast<int>(recs.size()); ++i) {
            const Rect r = row(i);
            if (i == sel) rrect(s, r, 6 * sc(), 0xFF2F5FC0u);
            else if (r.has(mx, my)) rrect(s, r, 6 * sc(), kGraphite700);
            const long long sz = recs[static_cast<std::size_t>(i)].size;
            const long secs = static_cast<long>(sz / 32000);
            char info[64];
            std::snprintf(info, sizeof info, "%ld:%02ld  %s", secs / 60, secs % 60, fmt_size(sz).c_str());
            draw_text(s, r.x + 8 * sc(), r.y + (r.h - font_h() * sc()) / 2, fit_text(recs[static_cast<std::size_t>(i)].name, r.w - 170 * sc(), sc()), kText, sc());
            draw_text(s, r.right() - 150 * sc(), r.y + (r.h - font_h() * sc()) / 2, info, kTextDim, sc());
        }
        if (recs.empty()) draw_text(s, 16 * sc(), 170 * sc(), t("No recordings yet. Press the red button.", "Записей пока нет. Нажмите красную кнопку."), kTextDim, sc());
        button(s, act(0), play_pid > 0 ? t("Stop", "Стоп") : t("Play", "Играть"), true);
        button(s, act(1), t("Delete", "Удалить"), false, true);
        if (!status.empty()) draw_text(s, 16 * sc() + 232 * sc(), h - 38 * sc(), fit_text(status, w - 270 * sc(), 1), kWarn, 1);
    }
    void mouse(const Event& e) override {
        if (e.mouse_kind != 1) return;
        const Rect rb = rec_btn();
        const int dx = e.x - (rb.x + rb.w / 2), dy = e.y - (rb.y + rb.h / 2);
        if (dx * dx + dy * dy <= (rb.w / 2) * (rb.w / 2)) { if (rec_pid > 0) stop_rec(); else start_rec(); return; }
        for (int i = 0; i < static_cast<int>(recs.size()); ++i) if (row(i).has(e.x, e.y)) { sel = i; return; }
        if (act(0).has(e.x, e.y)) { if (play_pid > 0) stop_play(); else play_sel(); return; }
        if (act(1).has(e.x, e.y) && sel >= 0 && sel < static_cast<int>(recs.size())) {
            stop_play();
            if (unlink((dir + "/" + recs[static_cast<std::size_t>(sel)].name).c_str()) != 0) status = std::strerror(errno);
            load();
        }
    }
    void key(const Event& e) override {
        if (e.code == KEY_SPACE) { if (rec_pid > 0) stop_rec(); else start_rec(); }
        else if (e.code == KEY_UP && sel > 0) --sel;
        else if (e.code == KEY_DOWN && sel + 1 < static_cast<int>(recs.size())) ++sel;
        else if (e.code == KEY_ENTER) play_sel();
    }
};

// ------------------------------------------------------------------- Camera (V4L2, YUYV)
struct Camera : App {
    struct Buf { void* p = nullptr; std::size_t len = 0; };
    int fd = -1, cw = 0, ch = 0, shots = 0;
    std::vector<Buf> bufs;
    std::vector<u32> frame;
    bool streaming = false, have_frame = false;
    std::string status, devname;
    long last_scan = 0;

    int tick_ms() override { return streaming ? 20 : 1000; }
    ~Camera() override { close_dev(); }
    void close_dev() {
        if (fd >= 0) {
            if (streaming) { v4l2_buf_type tp = V4L2_BUF_TYPE_VIDEO_CAPTURE; ioctl(fd, VIDIOC_STREAMOFF, &tp); }
            for (auto& b : bufs) if (b.p && b.p != MAP_FAILED) munmap(b.p, b.len);
            close(fd);
        }
        bufs.clear(); fd = -1; streaming = false;
    }
    bool try_open(const std::string& path) {
        const int f = open(path.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
        if (f < 0) return false;
        struct v4l2_capability cap;
        if (ioctl(f, VIDIOC_QUERYCAP, &cap) != 0 || !(cap.device_caps & V4L2_CAP_VIDEO_CAPTURE) || !(cap.device_caps & V4L2_CAP_STREAMING)) { close(f); return false; }
        struct v4l2_format fmt;
        std::memset(&fmt, 0, sizeof fmt);
        fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        fmt.fmt.pix.width = 640;
        fmt.fmt.pix.height = 480;
        fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
        fmt.fmt.pix.field = V4L2_FIELD_ANY;
        if (ioctl(f, VIDIOC_S_FMT, &fmt) != 0) { close(f); return false; }
        if (fmt.fmt.pix.pixelformat != V4L2_PIX_FMT_YUYV) {
            status = t("The camera gives only compressed video (MJPEG): not supported yet.", "Камера отдаёт только сжатое видео (MJPEG): пока не поддерживается.");
            close(f);
            return false;
        }
        struct v4l2_requestbuffers rq;
        std::memset(&rq, 0, sizeof rq);
        rq.count = 4;
        rq.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        rq.memory = V4L2_MEMORY_MMAP;
        if (ioctl(f, VIDIOC_REQBUFS, &rq) != 0 || rq.count < 1) { close(f); return false; }
        std::vector<Buf> bs;
        for (unsigned i = 0; i < rq.count; ++i) {
            struct v4l2_buffer b;
            std::memset(&b, 0, sizeof b);
            b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            b.memory = V4L2_MEMORY_MMAP;
            b.index = i;
            if (ioctl(f, VIDIOC_QUERYBUF, &b) != 0) { close(f); return false; }
            void* p = mmap(nullptr, b.length, PROT_READ | PROT_WRITE, MAP_SHARED, f, b.m.offset);
            if (p == MAP_FAILED) { close(f); return false; }
            bs.push_back({p, b.length});
            if (ioctl(f, VIDIOC_QBUF, &b) != 0) { close(f); return false; }
        }
        v4l2_buf_type tp = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (ioctl(f, VIDIOC_STREAMON, &tp) != 0) { close(f); return false; }
        fd = f; bufs = bs; streaming = true;
        cw = static_cast<int>(fmt.fmt.pix.width);
        ch = static_cast<int>(fmt.fmt.pix.height);
        frame.assign(static_cast<std::size_t>(cw) * static_cast<std::size_t>(ch), 0xFF000000u);
        devname = std::string(reinterpret_cast<char*>(cap.card)) + " (" + path + ")";
        status.clear();
        return true;
    }
    void scan() {
        for (int i = 0; i < 8 && fd < 0; ++i) try_open("/dev/video" + std::to_string(i));
        if (fd < 0 && status.empty())
            status = t("No camera found. VirtualBox: Devices > Webcam, pick your camera.", "Камера не найдена. VirtualBox: Устройства > Веб-камеры, выберите камеру.");
    }
    static int clamp8(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }
    void grab() {
        for (int n = 0; n < 4 && fd >= 0; ++n) {  // drain the queue: the newest frame wins
            struct v4l2_buffer b;
            std::memset(&b, 0, sizeof b);
            b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            b.memory = V4L2_MEMORY_MMAP;
            if (ioctl(fd, VIDIOC_DQBUF, &b) != 0) {
                if (errno != EAGAIN) { status = t("The camera was disconnected.", "Камера отключена."); close_dev(); have_frame = false; }
                return;
            }
            const unsigned char* y = static_cast<const unsigned char*>(bufs[b.index].p);
            const std::size_t px = std::min<std::size_t>(frame.size(), b.bytesused / 2);
            for (std::size_t i = 0; i + 1 < px; i += 2) {
                const int y0 = y[i * 2], u = y[i * 2 + 1] - 128, y1 = y[i * 2 + 2], v = y[i * 2 + 3] - 128;
                const int rr = (359 * v) >> 8, gg = (88 * u + 183 * v) >> 8, bb = (454 * u) >> 8;
                frame[i] = rgba(clamp8(y0 + rr), clamp8(y0 - gg), clamp8(y0 + bb));
                frame[i + 1] = rgba(clamp8(y1 + rr), clamp8(y1 - gg), clamp8(y1 + bb));
            }
            have_frame = true;
            ioctl(fd, VIDIOC_QBUF, &b);
        }
    }
    void tick() override {
        if (fd < 0) {
            const long now = mono_ms();
            if (now - last_scan > 2000) { last_scan = now; scan(); }
            return;
        }
        grab();
    }
    Rect view() const { return {12 * sc(), 12 * sc(), w - 24 * sc(), h - 70 * sc()}; }
    Rect shot_btn() const { return {w / 2 - 70 * sc(), h - 50 * sc(), 140 * sc(), 36 * sc()}; }
    bool save_bmp() {
        if (!have_frame) return false;
        const std::string path = "/homes/shared/Pictures/photo-" + stamp() + "-" + std::to_string(++shots) + ".bmp";
        FILE* f = std::fopen(path.c_str(), "wb");
        if (!f) { status = std::string(t("Cannot save: ", "Не удалось сохранить: ")) + std::strerror(errno); return false; }
        const int rowb = (cw * 3 + 3) & ~3, size = 54 + rowb * ch;
        unsigned char hd[54] = {'B', 'M'};
        auto put32 = [&](int o, unsigned v) { for (int i = 0; i < 4; ++i) hd[o + i] = static_cast<unsigned char>(v >> (8 * i)); };
        put32(2, static_cast<unsigned>(size));
        put32(10, 54);
        put32(14, 40);
        put32(18, static_cast<unsigned>(cw));
        put32(22, static_cast<unsigned>(ch));
        hd[26] = 1;
        hd[28] = 24;
        put32(34, static_cast<unsigned>(rowb * ch));
        std::fwrite(hd, 1, 54, f);
        std::vector<unsigned char> row(static_cast<std::size_t>(rowb), 0);
        for (int y = ch - 1; y >= 0; --y) {
            for (int x = 0; x < cw; ++x) {
                const u32 c = frame[static_cast<std::size_t>(y) * static_cast<std::size_t>(cw) + static_cast<std::size_t>(x)];
                row[static_cast<std::size_t>(x) * 3] = static_cast<unsigned char>(c);
                row[static_cast<std::size_t>(x) * 3 + 1] = static_cast<unsigned char>(c >> 8);
                row[static_cast<std::size_t>(x) * 3 + 2] = static_cast<unsigned char>(c >> 16);
            }
            std::fwrite(row.data(), 1, row.size(), f);
        }
        std::fclose(f);
        status = t("Photo saved in Shared/Pictures.", "Фото сохранено: Общая папка/Фото.");
        return true;
    }
    void draw(Surface& s) override {
        const Rect v = view();
        rrect(s, v, 10 * sc(), kGraphite900);
        if (have_frame && fd >= 0) {
            // nearest-neighbour scaling that keeps the aspect ratio
            const float k = std::min(static_cast<float>(v.w) / static_cast<float>(cw), static_cast<float>(v.h) / static_cast<float>(ch));
            const int dw = static_cast<int>(static_cast<float>(cw) * k), dh = static_cast<int>(static_cast<float>(ch) * k);
            const int ox = v.x + (v.w - dw) / 2, oy = v.y + (v.h - dh) / 2;
            for (int y = 0; y < dh; ++y) {
                const int sy = std::min(ch - 1, static_cast<int>(static_cast<float>(y) / k));
                const u32* src = &frame[static_cast<std::size_t>(sy) * static_cast<std::size_t>(cw)];
                u32* dst = s.px + static_cast<std::size_t>(oy + y) * static_cast<std::size_t>(s.stride) + static_cast<std::size_t>(ox);
                for (int x = 0; x < dw; ++x) dst[x] = src[std::min(cw - 1, static_cast<int>(static_cast<float>(x) / k))];
            }
        } else {
            const std::string msg = status.empty() ? t("Looking for a camera...", "Ищу камеру...") : status;
            draw_text(s, v.x + 16 * sc(), v.y + 16 * sc(), fit_text(msg, v.w - 32 * sc(), sc()), kTextDim, sc());
        }
        button(s, shot_btn(), t("Take photo", "Снять фото"), true);
        if (fd >= 0) draw_text(s, 16 * sc(), h - 18 * sc(), fit_text(status.empty() ? devname : status, w / 2 - 90 * sc(), 1), status.empty() ? kTextFaint : kOk, 1);
    }
    void mouse(const Event& e) override { if (e.mouse_kind == 1 && shot_btn().has(e.x, e.y)) save_bmp(); }
    void key(const Event& e) override { if (e.code == KEY_SPACE || e.code == KEY_ENTER) save_bmp(); }
};
