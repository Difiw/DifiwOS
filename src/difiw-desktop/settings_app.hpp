// Difiw 0.5.6 Settings: a normal window with pages (Network, Sound, Personalization, Language and input, About).
// Included by apps.cpp after apps_extra2.hpp, inside its anonymous namespace.
// SPDX-License-Identifier: MIT
#pragma once

struct Settings : App {
    struct Wall { std::string id, en, ru; };
    struct Hit { Rect r; std::function<void()> fn; };
    struct Slider { Rect r; std::string key; };

    std::map<std::string, std::string> conf;
    std::vector<Wall> walls;
    std::vector<Hit> hits;
    std::vector<Slider> sliders;
    std::vector<sys::NetIf> nets;
    sys::Vol out_v, in_v;
    std::string internet, active_slider;
    int page = 0;
    long last_audio_ms = 0;

    static constexpr int kSide = 200;
    int tick_ms() override { return 400; }

    // ------------------------------------------------------------------ data
    void load_walls() {
        walls = {{"aurora", "Aurora", "Северное сияние"}, {"graphite", "Graphite", "Графит"}, {"ocean", "Ocean", "Океан"},
                 {"sunset", "Sunset", "Закат"}, {"forest", "Forest", "Лес"}};
        for (const char* dir : {"/usr/share/difiw/wallpapers", "/homes/shared/Wallpapers"})
            if (DIR* d = opendir(dir)) {
                while (struct dirent* e = readdir(d)) {
                    const std::string n = e->d_name;
                    if (n.size() > 4 && n.compare(n.size() - 4, 4, ".ppm") == 0)
                        walls.push_back({std::string("file:") + dir + "/" + n, n.substr(0, n.size() - 4), n.substr(0, n.size() - 4)});
                }
                closedir(d);
            }
    }
    void read_conf() {
        conf.clear();
        std::ifstream f("/etc/difiw/desktop.conf");
        std::string l;
        while (std::getline(f, l)) {
            const std::size_t eq = l.find('=');
            if (eq != std::string::npos) conf[l.substr(0, eq)] = l.substr(eq + 1);
        }
    }
    std::string get(const std::string& k, const std::string& d) const {
        auto it = conf.find(k);
        return it == conf.end() ? d : it->second;
    }
    void set(const std::string& k, const std::string& v) {
        cl.request(proto::RQ_SET, k + "=" + v);
        conf[k] = v;
    }
    void refresh_audio() { out_v = sys::vol_get(false); in_v = sys::vol_get(true); last_audio_ms = mono_ms(); }
    void tick() override {
        read_conf();
        nets = sys::net_ifs();
        internet = sys::read_line("/run/difiw/net.state");
        if (page == 1 && active_slider.empty() && mono_ms() - last_audio_ms > 1500) refresh_audio();
        // the shell panels ("Network settings", "Sound settings") ask for a page through this file
        std::ifstream f("/run/difiw/settings-page");
        std::string p;
        std::getline(f, p);
        f.close();
        if (!p.empty()) {
            static const char* const keys[] = {"network", "sound", "personal", "language", "about"};
            for (int i = 0; i < 5; ++i) if (p == keys[i]) { page = i; if (i == 1) refresh_audio(); }
            std::ofstream clr("/run/difiw/settings-page", std::ios::trunc);
        }
    }
    void on_lang() override { read_conf(); }

    // ------------------------------------------------------------------ widgets
    int S_(int v) const { return v * sc(); }
    Rect content() const { return {S_(kSide) + S_(20), S_(60), w - S_(kSide) - S_(40), h - S_(80)}; }
    void add_hit(Rect r, std::function<void()> fn) { hits.push_back({r, std::move(fn)}); }
    void btn(Surface& s, Rect r, const std::string& label, std::function<void()> fn, bool primary = false, bool danger = false) {
        button(s, r, label, primary, danger);
        add_hit(r, std::move(fn));
    }
    void card(Surface& s, Rect r) {
        rrect(s, r, S_(12), 0xFF1B2230u);
        rrect_outline(s, r, S_(12), 1, 0x28FFFFFFu);
    }
    void label(Surface& s, int x, int y, const std::string& text, u32 col = kText, int scale = 0) {
        draw_text(s, x, y, text, col, scale > 0 ? scale : sc());
    }
    void slider(Surface& s, Rect r, const std::string& key, int pct, bool on) {
        const int tr = S_(8);
        const Rect track = {r.x, r.y + (r.h - tr) / 2, r.w, tr};
        rrect(s, track, tr / 2, 0xFF2A3142u);
        rrect(s, {track.x, track.y, std::max(tr, track.w * pct / 100), tr}, tr / 2, on ? kAccentBlue : kTextFaint);
        circle(s, track.x + track.w * pct / 100, r.y + r.h / 2, S_(9), 0xFFF4F7FFu);
        sliders.push_back({r.inset(-S_(4)), key});
    }
    int pct_at(const Rect& r, int x) const { return clampi((x - r.x - S_(4)) * 100 / std::max(1, r.w - 2 * S_(4) + 2 * S_(4)), 0, 100); }

    // ------------------------------------------------------------------ pages
    void page_network(Surface& s) {
        const Rect c = content();
        int y = c.y;
        if (nets.empty()) {
            label(s, c.x, y, t("No network adapter found.", "Сетевой адаптер не найден."), kTextDim);
            label(s, c.x, y + S_(24), t("VirtualBox: Settings > Network.", "VirtualBox: Настройки > Сеть."), kTextFaint);
            return;
        }
        for (const auto& n : nets) {
            const Rect cd = {c.x, y, c.w, S_(200)};
            card(s, cd);
            const bool connected = n.up && !n.ip.empty();
            const int gx = cd.x + S_(16), gy = cd.y + S_(16), gs = S_(34);
            rrect(s, {gx, gy, gs, gs * 3 / 4}, S_(6), connected ? kAccentBlue : kTextFaint);
            fill(s, {gx + gs / 2 - S_(2), gy + gs * 3 / 4, S_(4), S_(6)}, connected ? kAccentBlue : kTextFaint);
            fill(s, {gx + gs / 4, gy + gs * 3 / 4 + S_(6), gs / 2, S_(3)}, connected ? kAccentBlue : kTextFaint);
            label(s, cd.x + S_(64), cd.y + S_(14), t("Wired connection", "Проводное подключение") + "  (" + n.name + ")", kText);
            std::string st;
            u32 col = kTextDim;
            if (!n.up) st = t("Disconnected", "Отключено");
            else if (n.ip.empty()) { st = t("Connecting... (waiting for an address)", "Подключение... (ожидание адреса)"); col = kWarn; }
            else if (internet == "200" || internet == "301" || internet == "302" || internet == "204") { st = t("Connected, internet available", "Подключено, доступ в интернет есть"); col = kOk; }
            else if (!internet.empty()) { st = t("Connected, no internet access", "Подключено, без доступа в интернет"); col = kWarn; }
            else { st = t("Connected (internet not checked yet)", "Подключено (интернет ещё не проверен)"); col = kAccentTeal; }
            label(s, cd.x + S_(64), cd.y + S_(36), fit_text(st, cd.w - S_(80), sc()), col);
            int ty = cd.y + S_(66);
            auto kv = [&](const std::string& k, const std::string& v) {
                label(s, cd.x + S_(16), ty, k, kTextFaint);
                label(s, cd.x + S_(16) + S_(110), ty, fit_text(v.empty() ? "-" : v, cd.w - S_(140), sc()), kTextDim);
                ty += S_(20);
            };
            kv(t("IP address", "IP-адрес"), n.ip);
            kv(t("Gateway", "Шлюз"), n.gw);
            kv(t("DNS servers", "DNS-серверы"), sys::dns_servers());
            kv(t("MAC address", "MAC-адрес"), n.mac);
            const int bw = (cd.w - S_(32) - S_(16)) / 3, bh = S_(30), by = cd.bottom() - bh - S_(12);
            const std::string ifn = n.name;
            const bool up = n.up;
            btn(s, {cd.x + S_(16), by, bw, bh}, up ? t("Disconnect", "Отключить") : t("Connect", "Подключить"), [this, up, ifn]() { cl.request(proto::RQ_NET, std::string(up ? "down " : "up ") + ifn); }, !up);
            btn(s, {cd.x + S_(16) + bw + S_(8), by, bw, bh}, t("Reconnect", "Переподключить"), [this, ifn]() { cl.request(proto::RQ_NET, "renew " + ifn); });
            btn(s, {cd.x + S_(16) + 2 * (bw + S_(8)), by, bw, bh}, t("Check internet", "Проверить интернет"), [this]() { cl.request(proto::RQ_NET, "check -"); });
            y += cd.h + S_(14);
        }
        label(s, c.x, y, fit_text(t("This is a virtual adapter: the internet of the host computer, Wi-Fi included,", "Это виртуальный адаптер: интернет компьютера-хоста, в том числе его Wi-Fi,"), c.w, sc()), kTextFaint);
        label(s, c.x, y + S_(20), fit_text(t("reaches this system as a wired connection.", "приходит в систему как проводное подключение."), c.w, sc()), kTextFaint);
    }
    void page_sound(Surface& s) {
        const Rect c = content();
        const std::string card_n = sys::card_name();
        if (card_n.empty()) {
            label(s, c.x, c.y, t("No sound device found.", "Звуковое устройство не найдено."), kTextDim);
            label(s, c.x, c.y + S_(24), t("VirtualBox: Settings > Audio: enable output and input.", "VirtualBox: Настройки > Звук: включите вывод и ввод."), kTextFaint);
            return;
        }
        if (!out_v.ok && !in_v.ok && last_audio_ms == 0) refresh_audio();
        label(s, c.x, c.y - S_(2), t("Sound card: ", "Звуковая карта: ") + card_n, kTextDim);
        int y = c.y + S_(26);
        for (int k = 0; k < 2; ++k) {
            const bool in = k == 1;
            sys::Vol& v = in ? in_v : out_v;
            const std::string pcm = sys::pcm_name(in);
            const Rect cd = {c.x, y, c.w, S_(130)};
            card(s, cd);
            label(s, cd.x + S_(16), cd.y + S_(12), in ? t("Input (microphone)", "Ввод (микрофон)") : t("Output (speakers)", "Вывод (динамики)"), kText);
            label(s, cd.x + S_(16), cd.y + S_(34), fit_text((in ? t("Microphone", "Микрофон") : t("Speakers", "Динамики")) + " - " + (pcm.empty() ? card_n : pcm), cd.w - S_(32), sc()), kTextDim);
            if (!v.ok) {
                label(s, cd.x + S_(16), cd.y + S_(66), t("No volume control for this device.", "Для этого устройства нет регулятора громкости."), kTextFaint);
            } else {
                slider(s, {cd.x + S_(16), cd.y + S_(58), cd.w - S_(32) - S_(64), S_(28)}, in ? "in" : "out", v.pct, v.on);
                const std::string pct = std::to_string(v.on ? v.pct : 0) + "%";
                label(s, cd.right() - S_(16) - text_width(pct, sc()), cd.y + S_(58) + (S_(28) - font_h() * sc()) / 2, pct, v.on ? kText : kTextFaint);
                const std::string ctl = v.ctl;
                btn(s, {cd.x + S_(16), cd.bottom() - S_(36), S_(200), S_(28)}, v.on ? (in ? t("Mute microphone", "Выключить микрофон") : t("Mute sound", "Выключить звук")) : (in ? t("Unmute microphone", "Включить микрофон") : t("Unmute sound", "Включить звук")),
                    [this, ctl, in]() { sys::vol_mute(ctl); (in ? in_v : out_v).on = !(in ? in_v : out_v).on; });
            }
            y += cd.h + S_(12);
        }
        btn(s, {c.x, y, S_(200), S_(32)}, t("Test sound", "Проверить звук"), []() { sys::play_test(); }, true);
        label(s, c.x + S_(212), y + S_(8), fit_text(t("A short melody plays through the output device.", "Через устройство вывода играет короткая мелодия."), c.w - S_(212), sc()), kTextFaint);
    }
    void toggle_row(Surface& s, int& y, const std::string& text, const std::string& val, const std::string& key, const std::string& next) {
        label(s, content().x, y + S_(8), fit_text(text, content().w - S_(190), sc()), kText);
        btn(s, {content().right() - S_(170), y, S_(170), S_(32)}, val, [this, key, next]() { set(key, next); });
        y += S_(42);
    }
    void page_personal(Surface& s) {
        const Rect c = content();
        int y = c.y;
        auto on = [&](const char* k, const char* def) { return get(k, def) == "1"; };
        const std::string yes = t("On", "Вкл"), no = t("Off", "Выкл");
        const bool grid = get("menu", "grid") != "list";
        toggle_row(s, y, t("Interface scale", "Масштаб интерфейса"), get("scale", "1") == "2" ? "200%" : "100%", "scale", get("scale", "1") == "2" ? "1" : "2");
        toggle_row(s, y, t("Transparency (glass)", "Прозрачность (стекло)"), on("transparency", "1") ? yes : no, "transparency", on("transparency", "1") ? "off" : "on");
        toggle_row(s, y, t("Reduce motion", "Меньше анимаций"), on("reduce_motion", "0") ? yes : no, "reduce", on("reduce_motion", "0") ? "off" : "on");
        toggle_row(s, y, t("Hover tips", "Подсказки при наведении"), on("tips", "1") ? yes : no, "tips", on("tips", "1") ? "off" : "on");
        toggle_row(s, y, t("Applications menu", "Меню приложений"), grid ? t("Grid", "Сетка") : t("List", "Список"), "menu", grid ? "list" : "grid");
        label(s, c.x, y + S_(8), t("Desktop wallpaper", "Фон рабочего стола"), kText);
        y += S_(40);
        const std::string cur = get("wallpaper", "aurora");
        const int per = std::max(1, c.w / S_(150));
        for (int i = 0; i < static_cast<int>(walls.size()); ++i) {
            const Wall& wl = walls[static_cast<std::size_t>(i)];
            const Rect r = {c.x + (i % per) * S_(150), y + (i / per) * S_(40), S_(142), S_(32)};
            const std::string id = wl.id;
            btn(s, r, fit_text(t(wl.en.c_str(), wl.ru.c_str()), S_(126), sc()), [this, id]() { set("wallpaper", id); }, id == cur);
        }
    }
    void page_language(Surface& s) {
        const Rect c = content();
        int y = c.y;
        const bool lru = get("lang", "ru") != "en";
        const std::string yes = t("On", "Вкл"), no = t("Off", "Выкл");
        toggle_row(s, y, t("Language", "Язык"), lru ? "Русский" : "English", "lang", lru ? "en" : "ru");
        toggle_row(s, y, t("Win key opens the menu", "Клавиша Win открывает меню"), get("winkey", "0") == "1" ? yes : no, "winkey", get("winkey", "0") == "1" ? "off" : "on");
        label(s, c.x, y + S_(8), fit_text(t("Switch the keyboard layout with Alt+Shift or in the panel.", "Раскладка клавиатуры переключается Alt+Shift или в панели."), c.w, sc()), kTextFaint);
    }
    void page_about(Surface& s) {
        const Rect c = content();
        std::string os = sys::read_line("/usr/lib/os-release");
        std::ifstream f("/usr/lib/os-release");
        std::string l;
        while (std::getline(f, l)) if (l.compare(0, 12, "PRETTY_NAME=") == 0) { os = l.substr(12); os.erase(std::remove(os.begin(), os.end(), '"'), os.end()); }
        const Rect cd = {c.x, c.y, c.w, S_(260)};
        card(s, cd);
        label(s, cd.x + S_(20), cd.y + S_(16), os.empty() ? "Difiw OS" : os, kText, 2 * sc());
        int y = cd.y + S_(60);
        struct stat a, b;
        const bool disk = stat("/data", &a) == 0 && stat("/", &b) == 0 && a.st_dev != b.st_dev;
        auto kv = [&](const std::string& k, const std::string& v) {
            label(s, cd.x + S_(20), y, k, kTextFaint);
            label(s, cd.x + S_(20) + S_(150), y, fit_text(v, cd.w - S_(190), sc()), kText);
            y += S_(26);
        };
        kv(t("Kernel", "Ядро Linux"), sys::read_line("/proc/sys/kernel/osrelease"));
        kv(t("Processor", "Процессор"), sys::cpu_model());
        kv(t("Memory", "Оперативная память"), std::to_string(sys::mem_total_mb()) + " MB");
        kv(t("Uptime", "Время работы"), sys::uptime_text());
        kv(t("Computer name", "Имя компьютера"), sys::read_line("/proc/sys/kernel/hostname"));
        kv(t("Data disk", "Диск данных"), disk ? t("connected: settings and files are saved", "подключён: настройки и файлы сохраняются") : t("none: settings last until reboot", "нет: настройки до перезагрузки"));
    }

    // ------------------------------------------------------------------ window
    void draw(Surface& s) override {
        if (conf.empty()) read_conf();
        if (walls.empty()) load_walls();
        if (nets.empty()) nets = sys::net_ifs();
        hits.clear();
        sliders.clear();
        fill(s, {0, 0, S_(kSide), h}, kGraphite900);
        label(s, S_(18), S_(16), t("Settings", "Параметры"), kText, 2 * sc());
        static const char* const en[] = {"Network", "Sound", "Personalization", "Language and input", "About"};
        static const char* const ru_[] = {"Сеть и интернет", "Звук", "Персонализация", "Язык и ввод", "О системе"};
        for (int i = 0; i < 5; ++i) {
            const Rect r = {S_(8), S_(64) + i * S_(40), S_(kSide) - S_(16), S_(34)};
            if (i == page) rrect(s, r, S_(8), 0xFF2F5FC0u);
            else if (r.has(mx, my)) rrect(s, r, S_(8), kGraphite600);
            label(s, r.x + S_(12), r.y + (r.h - font_h() * sc()) / 2, fit_text(t(en[i], ru_[i]), r.w - S_(20), sc()), kText);
            add_hit(r, [this, i]() { page = i; if (i == 1) refresh_audio(); });
        }
        label(s, S_(kSide) + S_(20), S_(18), t(en[page], ru_[page]), kText, 2 * sc());
        switch (page) {
            case 0: page_network(s); break;
            case 1: page_sound(s); break;
            case 2: page_personal(s); break;
            case 3: page_language(s); break;
            default: page_about(s); break;
        }
    }
    void mouse(const Event& e) override {
        if (e.mouse_kind == 1) {
            for (const auto& sl : sliders)
                if (sl.r.has(e.x, e.y)) {
                    active_slider = sl.key;
                    sys::Vol& v = sl.key == "in" ? in_v : out_v;
                    v.pct = pct_at(sl.r, e.x);
                    sys::vol_set(v.ctl, v.pct);
                    return;
                }
            for (const auto& h2 : hits) if (h2.r.has(e.x, e.y)) { h2.fn(); return; }
        } else if (e.mouse_kind == 0 && !active_slider.empty() && (e.buttons & 1)) {
            for (const auto& sl : sliders)
                if (sl.key == active_slider) {
                    sys::Vol& v = active_slider == "in" ? in_v : out_v;
                    const int np = pct_at(sl.r, e.x);
                    if (np != v.pct) { v.pct = np; sys::vol_set(v.ctl, np); }
                }
        } else if (e.mouse_kind == 2) {
            active_slider.clear();
            last_audio_ms = mono_ms();
        }
    }
};
