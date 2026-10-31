// Netline: the Difiw OS web browser (tabs, address bar, start page, downloads, bookmarks, search).
// Pages are fetched by curl (HTTPS) in a child process and shown by the page engine in web.hpp (no JavaScript).
// Included by apps.cpp after web.hpp, inside its anonymous namespace.
// SPDX-License-Identifier: MIT
#pragma once

struct Netline : App {
    // A modern browser identity (sites refuse old ones); search engines get the text-browser identity because it makes them
    // send plain HTML result pages that work without JavaScript.
    static constexpr const char* kUA = "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/124.0.0.0 Safari/537.36";
    static constexpr const char* kUALynx = "Lynx/2.9.0rel.0 libwww-FM/2.14 SSL-MM/1.4.1 OpenSSL/3.0.0";
    static const char* ua_for(const std::string& url) {
        const std::string h = url_split(url).host;
        if (h.find("duckduckgo.com") != std::string::npos) return kUALynx;
        return kUA;
    }
    static constexpr const char* kDownloads = "/homes/shared/Downloads";

    struct Tab {
        int id = 0;
        std::string url, title, html, status;
        bool newtab = true, loading = false, is_image = false, pushed = false;
        int focus_field = -1, retries = 0, redirs = 0;
        std::string post;
        std::vector<std::string> css_queue;
        std::vector<std::string> hist;
        int hidx = -1, scroll = 0, laid_w = 0;
        Doc doc;
        Pic image;
        pid_t pid = 0;
        std::string hdr, body, err, req_url;
        long t0 = 0;
        std::vector<std::string> img_queue;
    };
    struct Dl { std::string name, path, url; long long total = 0, done = 0; pid_t pid = 0; int state = 0; /* 0 running 1 done 2 failed */ };
    struct ImgJob { pid_t pid = 0; std::string url, path; int tab = 0; bool css = false; };
    struct Bm { std::string title, url; };

    std::vector<Tab> tabs;
    int cur = 0, next_id = 1;
    std::vector<Dl> dls;
    std::vector<ImgJob> jobs;
    std::map<std::string, Pic> pics;
    std::map<std::string, std::string> css_cache;
    std::vector<Bm> bms;
    std::string addr;
    bool addr_focus = false, addr_all = false, adding = false;
    int panel = 0;  // 0 none, 1 menu, 2 downloads, 3 bookmarks
    int engine = 0;  // 0 Yandex, 1 Bing, 2 DuckDuckGo (Google needs JavaScript for search now)
    mutable std::string last_query;  // text of the last search, offered to another engine when Yandex asks for a captcha
    std::string hover_link;
    Image logo;
    long last_relayout = 0;
    bool relayout_needed = false;
    std::string conf_path;

    // ------------------------------------------------------------- setup / persistence
    int tick_ms() override {
        for (const auto& t : tabs) if (t.loading) return 40;
        if (!jobs.empty()) return 80;
        for (const auto& t : tabs) if (!t.css_queue.empty() || !t.img_queue.empty()) return 80;
        for (const auto& d : dls) if (d.state == 0) return 200;
        return 500;
    }
    void init() {
        const char* h = std::getenv("HOME");
        conf_path = std::string(h ? h : "/tmp") + "/netline.conf";
        std::ifstream f(conf_path);
        std::string l;
        while (std::getline(f, l)) {
            if (l.compare(0, 4, "eng=") == 0) engine = clampi(std::atoi(l.c_str() + 4), 0, 2);
            else if (l.compare(0, 3, "bm|") == 0) {
                const std::size_t p = l.find('|', 3);
                if (p != std::string::npos) bms.push_back({l.substr(3, p - 3), l.substr(p + 1)});
            }
        }
        const char* dd = std::getenv("DIFIW_DATADIR");
        std::ifstream ic(std::string(dd ? dd : "/usr/share/difiw") + "/icons/netline.dicon", std::ios::binary);
        if (ic) {
            unsigned char hd[8];
            ic.read(reinterpret_cast<char*>(hd), 8);
            const int iw = hd[0] | (hd[1] << 8), ih = hd[4] | (hd[5] << 8);
            if (iw > 0 && iw <= 512 && ih > 0 && ih <= 512) {
                std::vector<unsigned char> raw(static_cast<std::size_t>(iw) * static_cast<std::size_t>(ih) * 4);
                ic.read(reinterpret_cast<char*>(raw.data()), static_cast<std::streamsize>(raw.size()));
                logo.w = iw; logo.h = ih; logo.px.resize(static_cast<std::size_t>(iw) * static_cast<std::size_t>(ih));
                for (std::size_t i = 0; i < logo.px.size(); ++i) logo.px[i] = rgba(raw[i * 4], raw[i * 4 + 1], raw[i * 4 + 2], raw[i * 4 + 3]);
            }
        }
        new_tab();
    }
    void save_conf() {
        std::ofstream f(conf_path, std::ios::trunc);
        f << "engine=" << engine << "\n";
        for (const auto& b : bms) f << "bm|" << b.title << "|" << b.url << "\n";
    }
    void on_close() override {
        for (auto& t : tabs) kill_fetch(t);
        for (auto& j : jobs) if (j.pid > 0) { kill(j.pid, SIGKILL); int st; waitpid(j.pid, &st, 0); }
        for (auto& d : dls) if (d.state == 0 && d.pid > 0) { kill(d.pid, SIGKILL); int st; waitpid(d.pid, &st, 0); unlink(d.path.c_str()); }
        quit = true;
    }

    // ------------------------------------------------------------- tabs
    Tab& T() { return tabs[static_cast<std::size_t>(cur)]; }
    void new_tab() {
        if (tabs.size() >= 8) return;
        Tab t;
        t.id = next_id++;
        t.title = this->t("New tab", "Новая вкладка");
        tabs.push_back(t);
        cur = static_cast<int>(tabs.size()) - 1;
        addr.clear();
        addr_focus = true; addr_all = false; panel = 0;
    }
    void close_tab(int i) {
        if (i < 0 || i >= static_cast<int>(tabs.size())) return;
        kill_fetch(tabs[static_cast<std::size_t>(i)]);
        tabs.erase(tabs.begin() + i);
        if (tabs.empty()) { on_close(); return; }
        cur = clampi(cur >= i ? cur - 1 : cur, 0, static_cast<int>(tabs.size()) - 1);
        if (cur < 0) cur = 0;
        sync_addr();
    }
    void sync_addr() { addr = T().newtab ? "" : T().url; addr_focus = false; addr_all = false; }

    // ------------------------------------------------------------- fetching
    static std::string cookie_jar() { const char* h = std::getenv("HOME"); return std::string(h ? h : "/tmp") + "/netline.cookies"; }
    static std::string tmpdir() { const char* t = std::getenv("TMPDIR"); return t ? t : "/tmp"; }
    pid_t spawn(const std::vector<std::string>& args, const std::string& errfile) {
        const pid_t pid = fork();
        if (pid == 0) {
            const int nul = open("/dev/null", O_RDWR);
            if (nul >= 0) { dup2(nul, 0); dup2(nul, 1); }
            const int er = open(errfile.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
            if (er >= 0) dup2(er, 2);
            std::vector<char*> av;
            for (const auto& a : args) av.push_back(const_cast<char*>(a.c_str()));
            av.push_back(nullptr);
            execv(av[0], av.data());
            _exit(127);
        }
        return pid;
    }
    void kill_fetch(Tab& t) {
        if (t.pid > 0) { kill(t.pid, SIGKILL); int st; waitpid(t.pid, &st, 0); }
        t.pid = 0; t.loading = false;
        if (!t.body.empty()) unlink(t.body.c_str());
        if (!t.hdr.empty()) unlink(t.hdr.c_str());
    }
    std::string search_url(const std::string& q) const {
        last_query = q;
        const std::string e = url_encode(q);
        if (engine == 0) return "https://yandex.ru/search/?text=" + e + "&lr=213";
        if (engine == 1) return "https://www.bing.com/search?q=" + e + "&setlang=ru";
        return "https://html.duckduckgo.com/html/?q=" + e;
    }
    // Typed text -> address: a URL as is, something that looks like a host gets https://, anything else is a search.
    std::string resolve_input(std::string s) const {
        while (!s.empty() && s.back() == ' ') s.pop_back();
        while (!s.empty() && s.front() == ' ') s.erase(0, 1);
        if (s.empty()) return "";
        if (s.find("://") != std::string::npos) return s;
        if (s.find(' ') == std::string::npos && (s.find('.') != std::string::npos || s.compare(0, 9, "localhost") == 0) && s.back() != '.') return "https://" + s;
        return search_url(s);
    }
    void navigate(const std::string& input, bool push = true, const std::string& post = "") {
        const std::string u = resolve_input(input);
        if (u.empty()) return;
        Tab& t = T();
        kill_fetch(t);
        t.newtab = false; t.is_image = false; t.pushed = push; t.post = post; t.focus_field = -1;
        if (push) { t.retries = 0; t.redirs = 0; }
        t.req_url = u;
        t.url = u;
        t.title = url_split(u).host;
        t.status.clear();
        t.img_queue.clear();
        if (push) {
            if (t.hidx + 1 < static_cast<int>(t.hist.size())) t.hist.erase(t.hist.begin() + t.hidx + 1, t.hist.end());
            t.hist.push_back(u);
            t.hidx = static_cast<int>(t.hist.size()) - 1;
        }
        t.hdr = tmpdir() + "/t" + std::to_string(t.id) + ".hdr";
        t.body = tmpdir() + "/t" + std::to_string(t.id) + ".body";
        t.err = tmpdir() + "/t" + std::to_string(t.id) + ".err";
        unlink(t.hdr.c_str());
        std::vector<std::string> args = {"/usr/bin/curl", "-4", "-s", "-S", "-L", "--max-redirs", "8", "--compressed", "--connect-timeout", "15", "-m", "90", "-b", cookie_jar(), "-c", cookie_jar(), "-A", ua_for(u),
                                         "-D", t.hdr, "-o", t.body};
        if (!post.empty()) { args.push_back("--data"); args.push_back(post); }
        args.push_back(u);
        t.pid = spawn(args, t.err);
        t.loading = true;
        t.t0 = mono_ms();
        t.scroll = 0;
        sync_addr();
        panel = 0;
    }
    void go_hist(int d) {
        Tab& t = T();
        const int n = t.hidx + d;
        if (n < 0 || n >= static_cast<int>(t.hist.size())) return;
        t.hidx = n;
        const std::string u = t.hist[static_cast<std::size_t>(n)];
        navigate(u, false);
    }
    // Headers of the last response in the file (curl -D writes one block per redirect hop).
    struct Hdr { bool complete = false; int code = 0; std::string type, charset, disp, loc; long long len = -1; std::string eff; };
    Hdr read_headers(const Tab& t) const {
        Hdr h;
        std::ifstream f(t.hdr, std::ios::binary);
        if (!f) return h;
        std::stringstream ss;
        ss << f.rdbuf();
        const std::string all = ss.str();
        std::string cur_url = t.req_url;
        std::size_t pos = 0;
        while (pos < all.size()) {
            const std::size_t end = all.find("\r\n\r\n", pos);
            if (end == std::string::npos) break;
            std::string block = all.substr(pos, end - pos);
            pos = end + 4;
            Hdr b;
            b.complete = true;
            std::istringstream is(block);
            std::string line;
            bool first = true;
            while (std::getline(is, line)) {
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (first) { const std::size_t sp = line.find(' '); b.code = sp == std::string::npos ? 0 : std::atoi(line.c_str() + sp + 1); first = false; continue; }
                const std::size_t c = line.find(':');
                if (c == std::string::npos) continue;
                std::string k = line.substr(0, c), v = line.substr(c + 1);
                for (auto& ch : k) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                while (!v.empty() && v.front() == ' ') v.erase(0, 1);
                if (k == "content-type") {
                    b.type = v.substr(0, v.find(';'));
                    for (auto& ch : b.type) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                    const std::size_t cs = v.find("charset=");
                    if (cs != std::string::npos) { b.charset = v.substr(cs + 8); while (!b.charset.empty() && (b.charset.back() == '"' || b.charset.back() == ' ')) b.charset.pop_back(); if (!b.charset.empty() && b.charset.front() == '"') b.charset.erase(0, 1); }
                } else if (k == "content-length") b.len = std::atoll(v.c_str());
                else if (k == "content-disposition") b.disp = v;
                else if (k == "location") b.loc = v;
            }
            if (b.code >= 300 && b.code < 400 && !b.loc.empty()) cur_url = url_resolve(cur_url, b.loc);
            b.eff = cur_url;
            h = b;
        }
        return h;
    }
    static bool page_type(const std::string& type) {
        if (type.empty()) return true;
        return type == "text/html" || type == "application/xhtml+xml" || type.compare(0, 5, "text/") == 0 || type == "application/json" || type == "application/xml" ||
               type == "image/png" || type == "image/jpeg" || type == "image/gif" || type == "image/bmp";
    }
    std::string file_name_for(const Hdr& h) const {
        std::string n;
        const std::size_t fp = h.disp.find("filename=");
        if (fp != std::string::npos) { n = h.disp.substr(fp + 9); n = n.substr(0, n.find(';')); while (!n.empty() && (n.front() == '"' || n.front() == '\'')) n.erase(0, 1); while (!n.empty() && (n.back() == '"' || n.back() == '\'' || n.back() == ' ')) n.pop_back(); }
        if (n.empty()) {
            std::string p = url_split(h.eff).path;
            p = p.substr(0, p.find('?'));
            n = url_decode(p.substr(p.rfind('/') + 1));
        }
        for (auto& c : n) if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|') c = '_';
        if (n.empty() || n == "." || n == "..") n = "download";
        return n;
    }
    void start_download(const std::string& url, const std::string& name, long long total) {
        std::string path = std::string(kDownloads) + "/" + name;
        for (int i = 1; path_exists(path) && i < 200; ++i) {
            const std::size_t dot = name.rfind('.');
            path = std::string(kDownloads) + "/" + (dot == std::string::npos ? name + "_" + std::to_string(i) : name.substr(0, dot) + "_" + std::to_string(i) + name.substr(dot));
        }
        Dl d;
        d.name = path.substr(path.rfind('/') + 1); d.path = path; d.url = url; d.total = total;
        d.pid = spawn({"/usr/bin/curl", "-4", "-s", "-S", "-L", "--max-redirs", "8", "--connect-timeout", "15", "-A", kUA, "-o", path, url}, tmpdir() + "/dl.err");
        dls.push_back(d);
        panel = 2;
    }
    void finish_page(Tab& t, const Hdr& h, int rc) {
        auto tr = [this](const char* en, const char* ru) { return this->t(en, ru); };
        t.pid = 0; t.loading = false;
        if (rc != 0 && (rc == 6 || rc == 7 || rc == 28) && t.retries < 2 && !t.req_url.empty()) {  // the virtual network sometimes drops the first request
            ++t.retries;
            const std::string again = t.req_url, post = t.post;
            const bool keep = t.pushed;
            const int me = static_cast<int>(&t - &tabs[0]), saved = cur;
            cur = me;
            navigate(again, false, post);
            cur = saved;
            t.pushed = keep;
            return;
        }
        if (rc != 0) {
            t.title = tr("Page not loaded", "Страница не загружена");
            std::ifstream ef(t.err);
            std::string e;
            std::getline(ef, e);
            std::string why = tr("Cannot open the page.", "Не удалось открыть страницу.");
            if (rc == 6) why = tr("The site name was not found (no network or DNS?).", "Адрес сайта не найден (нет сети или DNS?).");
            else if (rc == 7) why = tr("Cannot connect to the site.", "Не удаётся подключиться к сайту.");
            else if (rc == 28) why = tr("The site took too long to answer.", "Сайт слишком долго отвечает.");
            else if (rc == 60 || rc == 35 || rc == 51) why = tr("The security certificate could not be verified (check the date and time).", "Не удалось проверить сертификат безопасности (проверьте дату и время).");
            t.html = "<html><head><title></title></head><body><h1>" + std::string(tr("Page not loaded", "Страница не загружена")) + "</h1><p>" + why + "</p><p>" + t.req_url + "</p><p><small>" + e + "</small></p></body></html>";
            t.is_image = false;
            t.laid_w = 0;
            return;
        }
        std::ifstream f(t.body, std::ios::binary);
        std::stringstream ss;
        ss << f.rdbuf();
        std::string body = ss.str();
        unlink(t.body.c_str());
        if (!h.eff.empty()) { t.url = h.eff; t.req_url = h.eff; if (t.hidx >= 0) t.hist[static_cast<std::size_t>(t.hidx)] = h.eff; }
        if (h.type.compare(0, 6, "image/") == 0) {
            t.is_image = decode_pic(body, t.image);
            t.html.clear();
            t.title = url_split(t.url).host;
            t.laid_w = 0;
            return;
        }
        std::string cs = h.charset;
        if (cs.empty()) {  // look for <meta charset> in the first bytes
            std::string head = body.substr(0, 2048);
            for (auto& c : head) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            const std::size_t p = head.find("charset=");
            if (p != std::string::npos) { std::size_t e = p + 8; if (e < head.size() && (head[e] == '"' || head[e] == '\'')) ++e; std::size_t q = e; while (q < head.size() && (std::isalnum(static_cast<unsigned char>(head[q])) || head[q] == '-' || head[q] == '_')) ++q; cs = head.substr(e, q - e); }
        }
        body = to_utf8(body, cs);
        if (h.type == "text/plain" || h.type == "application/json" || h.type == "application/xml" || (h.type.compare(0, 5, "text/") == 0 && h.type != "text/html")) {
            std::string esc;
            for (char c : body) { if (c == '<') esc += "&lt;"; else if (c == '&') esc += "&amp;"; else esc += c; }
            body = "<pre>" + esc + "</pre>";
        }
        {   // VK and similar single-page sites: the server sends a "your browser is outdated" stub and builds the page with JavaScript.
            // Drop the misleading stub so the "needs JavaScript" notice is shown instead.
            const std::size_t bb = body.find("<div class=\"BadBrowserHeader\"");
            const std::size_t te = bb == std::string::npos ? std::string::npos : body.find("<template", bb);
            if (te != std::string::npos) body.erase(bb, te - bb);
        }
        if (!last_query.empty() && h.eff.find("/showcaptcha") != std::string::npos) {  // Yandex asks for a captcha (it needs JavaScript and cannot be solved here)
            const std::string q = url_encode(last_query);
            t.title = tr("Yandex asks for a check", "Яндекс просит подтвердить, что вы не робот");
            t.html = std::string("<html><head><title>") + t.title + "</title></head><body><h1>" + t.title + "</h1><p>" +
                     tr("Yandex shows a captcha to this connection. It needs JavaScript, which Netline does not run, so the search cannot continue here. Cookies are saved between visits, so Yandex may let you through later. Search the same text with another engine:",
                        "Яндекс показал капчу для этого подключения. Ей нужен JavaScript, а Netline его не выполняет, поэтому поиск здесь продолжить нельзя. Cookie сохраняются между запусками, и позже Яндекс может пропустить без проверки. Найдите то же самое в другом поисковике:") +
                     "</p><p><a href=\"https://www.bing.com/search?q=" + q + "&setlang=ru\">Bing</a></p><p><a href=\"https://html.duckduckgo.com/html/?q=" + q +
                     "\">DuckDuckGo</a></p><p><a href=\"https://yandex.ru/search/?text=" + q + "\">" + tr("Try Yandex again", "Повторить в Яндексе") + "</a></p></body></html>";
            t.is_image = false; t.laid_w = 0; t.doc = Doc();
            return;
        }
        // Click-through / interstitial pages forward with a <meta refresh> or a script; follow them since no script runs here.
        const std::string fwd = t.redirs < 3 ? find_forward(body, t.req_url) : std::string();
        if (!fwd.empty() && fwd != t.req_url) {
            ++t.redirs;
            const int me = static_cast<int>(&t - &tabs[0]), saved = cur;
            const bool keep = t.pushed;
            const int r = t.redirs;
            cur = me;
            navigate(fwd, false);
            cur = saved;
            t.pushed = keep;
            t.redirs = r;
            return;
        }
        t.html = body;
        t.is_image = false;
        t.laid_w = 0;
        t.doc = Doc();
    }
    // Address a page forwards to by <meta http-equiv=refresh> or by location.replace/href in a script, only when the page has almost no content.
    static std::string find_forward(const std::string& body, const std::string& base) {
        std::string low = body;
        for (auto& c : low) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        std::string target;
        const std::size_t mr = low.find("http-equiv=\"refresh\"");
        if (mr != std::string::npos) {
            const std::size_t u = low.find("url=", mr);
            const std::size_t end = u == std::string::npos ? u : low.find_first_of("\"'>", u);
            if (u != std::string::npos && end != std::string::npos) target = body.substr(u + 4, end - u - 4);
        }
        if (target.empty() && body.size() < 6000 && low.find("<a ") == std::string::npos) {
            static const char* const keys[] = {"location.replace(", "location.href=", "location.href =", "window.location=", "window.location =", "location=", "location.assign("};
            for (const char* k : keys) {
                std::size_t p = low.find(k);
                if (p == std::string::npos) continue;
                p += std::strlen(k);
                while (p < body.size() && body[p] == ' ') ++p;
                if (p >= body.size() || (body[p] != '"' && body[p] != '\'')) continue;
                const char q = body[p];
                const std::size_t e = body.find(q, p + 1);
                if (e == std::string::npos) continue;
                target = body.substr(p + 1, e - p - 1);
                break;
            }
        }
        if (target.empty()) return "";
        std::string out;  // JavaScript string escapes: \/ \x26 &
        for (std::size_t i = 0; i < target.size(); ++i) {
            if (target[i] == '\\' && i + 1 < target.size()) {
                if (target[i + 1] == '/') { out += '/'; ++i; continue; }
                if (target[i + 1] == 'x' && i + 3 < target.size()) { out += static_cast<char>(std::strtol(target.substr(i + 2, 2).c_str(), nullptr, 16)); i += 3; continue; }
                if (target[i + 1] == 'u' && i + 5 < target.size()) { const long cp = std::strtol(target.substr(i + 2, 4).c_str(), nullptr, 16); if (cp < 128) { out += static_cast<char>(cp); i += 5; continue; } }
            }
            out += target[i];
        }
        out = decode_entities(out);
        if (out.empty() || out[0] == '#' || out.compare(0, 11, "javascript:") == 0) return "";
        return unwrap_tracking(url_resolve(base, out));
    }
    static bool decode_pic(const std::string& data, Pic& out) {
        int w = 0, h = 0, comp = 0;
        if (!stbi_info_from_memory(reinterpret_cast<const unsigned char*>(data.data()), static_cast<int>(data.size()), &w, &h, &comp)) return false;
        if (w <= 0 || h <= 0 || static_cast<long long>(w) * h > 12000000LL) return false;
        unsigned char* px = stbi_load_from_memory(reinterpret_cast<const unsigned char*>(data.data()), static_cast<int>(data.size()), &w, &h, &comp, 4);
        if (!px) return false;
        out.w = w; out.h = h;
        out.px.resize(static_cast<std::size_t>(w) * static_cast<std::size_t>(h));
        for (std::size_t i = 0; i < out.px.size(); ++i) out.px[i] = rgba(px[i * 4], px[i * 4 + 1], px[i * 4 + 2], px[i * 4 + 3]);
        stbi_image_free(px);
        return true;
    }
    void layout_tab(Tab& t) {
        const int vw = view().w;
        Doc nd;
        nd.base = t.url;
        Sheet sheet;
        for (const auto& cu : t.doc.css_urls) { auto it = css_cache.find(cu); if (it != css_cache.end()) css_parse(it->second, sheet); }
        Layouter(nd, vw, [&](int idx, int& w, int& h) {
            if (idx < 0 || idx >= static_cast<int>(nd.imgs.size())) return false;
            auto it = pics.find(nd.imgs[static_cast<std::size_t>(idx)].src);
            if (it == pics.end() || it->second.px.empty()) return false;
            w = it->second.w; h = it->second.h;
            return true;
        }, &sheet).run(t.html);
        if (!nd.title.empty()) t.title = nd.title;
        const bool first = t.doc.items.empty() && t.doc.fields.empty();
        if (nd.fields.size() == t.doc.fields.size())  // what the user typed survives a re-layout
            for (std::size_t i = 0; i < nd.fields.size(); ++i) { nd.fields[i].value = t.doc.fields[i].value; nd.fields[i].checked = t.doc.fields[i].checked; }
        if (first && nd.autofocus >= 0) t.focus_field = nd.autofocus;
        for (const auto& cu : nd.css_urls) if (!css_cache.count(cu) && std::find(t.css_queue.begin(), t.css_queue.end(), cu) == t.css_queue.end() && !job_running(cu)) t.css_queue.push_back(cu);
        t.doc = nd;
        t.laid_w = vw;
        t.scroll = clampi(t.scroll, 0, std::max(0, t.doc.height - view().h));
        // queue pictures that are not loaded yet
        for (const auto& r : t.doc.imgs)
            if (!pics.count(r.src) && std::find(t.img_queue.begin(), t.img_queue.end(), r.src) == t.img_queue.end()) {
                bool running = false;
                for (const auto& j : jobs) if (j.url == r.src) running = true;
                if (!running) t.img_queue.push_back(r.src);
            }
    }
    bool job_running(const std::string& url) const { for (const auto& j : jobs) if (j.url == url) return true; return false; }
    void pump_images() {
        for (std::size_t i = 0; i < jobs.size();) {
            int st = 0;
            if (jobs[i].pid > 0 && waitpid(jobs[i].pid, &st, WNOHANG) == jobs[i].pid) {
                std::ifstream f(jobs[i].path, std::ios::binary);
                std::stringstream ss;
                ss << f.rdbuf();
                Pic p;
                if (jobs[i].css) {
                    css_cache[jobs[i].url] = WIFEXITED(st) && WEXITSTATUS(st) == 0 ? ss.str() : std::string();
                    relayout_needed = true;
                    unlink(jobs[i].path.c_str());
                    jobs.erase(jobs.begin() + static_cast<std::ptrdiff_t>(i));
                    continue;
                }
                if (WIFEXITED(st) && WEXITSTATUS(st) == 0 && decode_pic(ss.str(), p)) {
                    if (pics.size() > 60) pics.clear();
                    pics[jobs[i].url] = std::move(p);
                    relayout_needed = true;
                } else pics[jobs[i].url] = Pic();  // remember the failure: do not ask again
                unlink(jobs[i].path.c_str());
                jobs.erase(jobs.begin() + static_cast<std::ptrdiff_t>(i));
            } else ++i;
        }
        Tab& t = T();
        while (jobs.size() < 4 && !t.css_queue.empty()) {
            ImgJob j;
            j.url = t.css_queue.front();
            t.css_queue.erase(t.css_queue.begin());
            if (j.url.compare(0, 4, "http") != 0) continue;
            j.css = true; j.tab = t.id;
            j.path = tmpdir() + "/css" + std::to_string(mono_ms() % 100000000) + std::to_string(jobs.size());
            j.pid = spawn({"/usr/bin/curl", "-4", "-s", "-L", "--max-redirs", "5", "--max-filesize", "900000", "--compressed", "--connect-timeout", "10", "-m", "30", "-A", kUA, "-o", j.path, j.url}, tmpdir() + "/css.err");
            jobs.push_back(j);
        }
        while (jobs.size() < 4 && !t.img_queue.empty()) {
            ImgJob j;
            j.url = t.img_queue.front();
            t.img_queue.erase(t.img_queue.begin());
            if (j.url.compare(0, 4, "http") != 0) continue;
            j.tab = t.id;
            j.path = tmpdir() + "/img" + std::to_string(mono_ms() % 100000000) + std::to_string(jobs.size());
            j.pid = spawn({"/usr/bin/curl", "-4", "-s", "-L", "--max-redirs", "5", "--max-filesize", "6000000", "--compressed", "--connect-timeout", "10", "-m", "30", "-A", kUA, "-o", j.path, j.url}, tmpdir() + "/img.err");
            jobs.push_back(j);
        }
    }
    void tick() override {
        for (auto& t : tabs) {
            if (!t.loading || t.pid <= 0) continue;
            int st = 0;
            const Hdr h = read_headers(t);
            if (h.complete && !(h.code >= 300 && h.code < 400) && h.code > 0 && !page_type(h.type)) {
                // not a web page: download the file instead
                kill_fetch(t);
                start_download(h.eff, file_name_for(h), h.len);
                t.loading = false;
                t.status = this->t("Downloading: ", "Скачивание: ") + file_name_for(h);
                if (t.pushed && !t.hist.empty()) { t.hist.pop_back(); t.hidx = static_cast<int>(t.hist.size()) - 1; }
                t.url = t.hidx >= 0 ? t.hist[static_cast<std::size_t>(t.hidx)] : ""; t.newtab = t.url.empty();
                sync_addr();
                continue;
            }
            if (waitpid(t.pid, &st, WNOHANG) == t.pid) finish_page(t, h, WIFEXITED(st) ? WEXITSTATUS(st) : 1);
        }
        for (auto& d : dls) {
            if (d.state != 0) continue;
            struct stat sb;
            if (stat(d.path.c_str(), &sb) == 0) d.done = sb.st_size;
            int st = 0;
            if (d.pid > 0 && waitpid(d.pid, &st, WNOHANG) == d.pid) {
                d.pid = 0;
                d.state = WIFEXITED(st) && WEXITSTATUS(st) == 0 ? 1 : 2;
                if (d.state == 2) unlink(d.path.c_str());
                else { d.total = d.done; notify_done(d); }
            }
        }
        pump_images();
        Tab& t = T();
        const long now = mono_ms();
        if (!t.html.empty() && !t.is_image && (t.laid_w != view().w || (relayout_needed && now - last_relayout > 400))) {
            if (t.laid_w == 0 || relayout_needed || t.laid_w != view().w) { relayout_needed = false; last_relayout = now; layout_tab(t); }
        }
        if (t.laid_w == 0 && !t.html.empty()) layout_tab(t);
    }
    void notify_done(const Dl& d) { (void)d; }
    void on_lang() override {}

    // ------------------------------------------------------------- geometry
    int S_(int v) const { return v * sc(); }
    int tab_h() const { return S_(36); }
    int nav_h() const { return S_(44); }
    Rect view() const { return {0, tab_h() + nav_h(), w, h - tab_h() - nav_h()}; }
    Rect tab_rect(int i) const {
        const int n = static_cast<int>(tabs.size());
        const int tw = std::min(S_(210), (w - S_(120)) / std::max(1, n));
        return {S_(8) + i * (tw + S_(2)), S_(6), tw, S_(30)};
    }
    Rect plus_rect() const { const Rect last = tab_rect(static_cast<int>(tabs.size()) - 1); return {last.right() + S_(6), S_(8), S_(26), S_(26)}; }
    Rect nav_btn(int i) const { return {S_(8) + i * S_(38), tab_h() + S_(6), S_(32), S_(32)}; }  // 0 back 1 forward 2 reload
    Rect addr_rect() const { return {S_(8) + 3 * S_(38) + S_(4), tab_h() + S_(6), w - (S_(8) + 3 * S_(38) + S_(4)) - 3 * S_(38) - S_(8), S_(32)}; }
    Rect tool_btn(int i) const { return {w - S_(8) - (3 - i) * S_(38) + S_(4), tab_h() + S_(6), S_(32), S_(32)}; }  // 0 star 1 downloads 2 menu
    Rect panel_rect() const { return {w - S_(340), tab_h() + nav_h() + S_(4), S_(332), std::min(h - tab_h() - nav_h() - S_(12), S_(420))}; }
    static constexpr int kSpeed = 10;
    struct Tile { const char* name; const char* url; u32 color; const char* mark; };
    static const Tile* tiles() {
        static const Tile t[] = {{"Yandex", "https://ya.ru/", 0xFFD83A3Au, "Y"}, {"YouTube", "https://www.youtube.com/", 0xFFE02E2Eu, ">"},
                                 {"VK", "https://m.vk.com/", 0xFF2A6FE0u, "VK"}, {"GitHub", "https://github.com/", 0xFF2B2F3Au, "GH"},
                                 {"Wikipedia", "https://ru.wikipedia.org/", 0xFF6B6B7Bu, "W"}, {"DuckDuckGo", "https://duckduckgo.com/html/", 0xFFE8742Cu, "D"},
                                 {"Habr", "https://habr.com/ru/feed/", 0xFF3E8A9Cu, "H"}, {"Bing", "https://www.bing.com/", 0xFF1F8A70u, "B"},
                                 {"Telegram", "https://telegram.org/", 0xFF2AA3DBu, "T"}};
        return t;
    }
    Rect tile_rect(int i) const {
        const Rect v = view();
        const int tw = S_(92), gap = S_(14), cols = 5;
        const int total = cols * tw + (cols - 1) * gap;
        const int x0 = v.x + (v.w - total) / 2, y0 = v.y + std::min(v.h - S_(220), S_(256));
        return {x0 + (i % cols) * (tw + gap), y0 + (i / cols) * (tw + S_(14)), tw, tw};
    }
    Rect start_search() const { const Rect v = view(); const int sw = std::min(S_(560), v.w - S_(60)); return {v.x + (v.w - sw) / 2, v.y + S_(176), sw, S_(44)}; }

    // ------------------------------------------------------------- drawing
    void icon_btn(Surface& s, Rect r, int kind, bool on = false) {
        const bool over = r.has(mx, my);
        if (over || on) rrect(s, r, S_(8), on ? 0xFF2A4A8Au : 0xFF1B2745u);
        const int cx = r.x + r.w / 2, cy = r.y + r.h / 2, u = r.w / 4;
        const u32 col = 0xFFD8E2FFu;
        const int th = std::max(1, S_(2));
        if (kind == 0) { line(s, cx + u, cy, cx - u, cy, col, th); line(s, cx - u, cy, cx - u / 2 + 1, cy - u / 2 - 1, col, th); line(s, cx - u, cy, cx - u / 2 + 1, cy + u / 2 + 1, col, th); }
        else if (kind == 1) { line(s, cx - u, cy, cx + u, cy, col, th); line(s, cx + u, cy, cx + u / 2 - 1, cy - u / 2 - 1, col, th); line(s, cx + u, cy, cx + u / 2 - 1, cy + u / 2 + 1, col, th); }
        else if (kind == 2) { ring(s, cx, cy, u, th, col); line(s, cx + u, cy - u / 2, cx + u + 3, cy - u, col, th); line(s, cx + u, cy - u / 2, cx + u - 3, cy - u / 2 - 3, col, th); }
        else if (kind == 3) {  // stop
            line(s, cx - u, cy - u, cx + u, cy + u, col, th); line(s, cx - u, cy + u, cx + u, cy - u, col, th);
        } else if (kind == 4) {  // star
            const int R = u + u / 2, r2 = R / 2;
            int px = 0, py = 0;
            for (int k = 0; k <= 10; ++k) {
                const float a = -1.5707963f + static_cast<float>(k) * 0.62831853f;
                const int rr = (k % 2 == 0) ? R : r2;
                const int x = cx + static_cast<int>(std::cos(a) * static_cast<float>(rr)), y = cy + static_cast<int>(std::sin(a) * static_cast<float>(rr));
                if (k > 0) line(s, px, py, x, y, on ? 0xFFFFD35Au : col, th);
                px = x; py = y;
            }
        } else if (kind == 5) {  // download
            line(s, cx, cy - u, cx, cy + u / 2, col, th); line(s, cx, cy + u / 2, cx - u / 2, cy, col, th); line(s, cx, cy + u / 2, cx + u / 2, cy, col, th);
            line(s, cx - u, cy + u, cx + u, cy + u, col, th);
        } else if (kind == 6) {  // menu
            for (int k = -1; k <= 1; ++k) fill(s, {cx - u, cy + k * u / 2 * 2 - th / 2, 2 * u, th}, col);
        }
    }
    void draw_tabs(Surface& s) {
        fill(s, {0, 0, w, tab_h()}, 0xFF080D1Cu);
        for (int i = 0; i < static_cast<int>(tabs.size()); ++i) {
            const Rect r = tab_rect(i);
            const bool act = i == cur;
            rrect(s, r, S_(9), act ? 0xFF1A2B52u : (r.has(mx, my) ? 0xFF121C38u : 0xFF0E1630u));
            if (act) rrect_outline(s, r, S_(9), 1, 0xFF3C6FE0u);
            const Tab& t = tabs[static_cast<std::size_t>(i)];
            const std::string title = t.loading ? this->t("Loading...", "Загрузка...") : (t.newtab ? this->t("New tab", "Новая вкладка") : t.title);
            draw_text(s, r.x + S_(10), r.y + (r.h - font_h() * sc()) / 2, fit_text(title, r.w - S_(40), sc()), act ? 0xFFFFFFFFu : 0xFFA9B6D8u, sc());
            const Rect cx = {r.right() - S_(24), r.y + S_(5), S_(20), S_(20)};
            if (cx.has(mx, my)) rrect(s, cx, S_(6), 0xFF33406Au);
            const int ux = cx.x + cx.w / 2, uy = cx.y + cx.h / 2, q = S_(4);
            line(s, ux - q, uy - q, ux + q, uy + q, 0xFFC8D4F5u, std::max(1, S_(1))); line(s, ux - q, uy + q, ux + q, uy - q, 0xFFC8D4F5u, std::max(1, S_(1)));
        }
        const Rect p = plus_rect();
        if (static_cast<int>(tabs.size()) < 8) {
            if (p.has(mx, my)) rrect(s, p, S_(8), 0xFF1B2745u);
            line(s, p.x + p.w / 2 - S_(6), p.y + p.h / 2, p.x + p.w / 2 + S_(6), p.y + p.h / 2, 0xFFD8E2FFu, std::max(1, S_(2)));
            line(s, p.x + p.w / 2, p.y + p.h / 2 - S_(6), p.x + p.w / 2, p.y + p.h / 2 + S_(6), 0xFFD8E2FFu, std::max(1, S_(2)));
        }
    }
    bool is_bookmarked() const { for (const auto& b : bms) if (b.url == tabs[static_cast<std::size_t>(cur)].url) return true; return false; }
    void draw_nav(Surface& s) {
        fill(s, {0, tab_h(), w, nav_h()}, 0xFF0F1936u);
        const Tab& t = tabs[static_cast<std::size_t>(cur)];
        icon_btn(s, nav_btn(0), 0);
        icon_btn(s, nav_btn(1), 1);
        icon_btn(s, nav_btn(2), t.loading ? 3 : 2);
        const Rect a = addr_rect();
        rrect(s, a, S_(16), 0xFF16224Au);
        rrect_outline(s, a, S_(16), 1, addr_focus ? 0xFF4C8DFFu : 0xFF2C3C70u);
        const int ty = a.y + (a.h - font_h() * sc()) / 2;
        if (addr_focus || !t.newtab) {
            const std::string shown = adding ? this->t("Add a site: ", "Добавить сайт: ") + addr : addr;
            const std::string txt = fit_text(shown + (addr_focus ? "_" : ""), a.w - S_(24), sc());
            if (addr_focus && addr_all) rrect(s, {a.x + S_(10), ty - 2, text_width(txt, sc()) + 4, font_h() * sc() + 4}, 3, 0xFF2F5FC0u);
            draw_text(s, a.x + S_(12), ty, txt, 0xFFFFFFFFu, sc());
        } else draw_text(s, a.x + S_(12), ty, this->t("Enter a search or address", "Введите запрос или адрес"), 0xFF7C8BB5u, sc());
        icon_btn(s, tool_btn(0), 4, is_bookmarked());
        icon_btn(s, tool_btn(1), 5, panel == 2);
        icon_btn(s, tool_btn(2), 6, panel == 1);
        if (t.loading) {  // moving bar under the address field
            const int bw = a.w / 4, pos = static_cast<int>((mono_ms() / 4) % (a.w + bw)) - bw;
            const Rect bar = {a.x + std::max(0, pos), nav_h() + tab_h() - S_(3), std::min(bw, a.w - std::max(0, pos)), S_(2)};
            if (bar.w > 0) fill(s, bar, 0xFF4C8DFFu);
        }
    }
    void draw_newtab(Surface& s) {
        const Rect v = view();
        for (int y = 0; y < v.h; ++y) fill(s, {v.x, v.y + y, v.w, 1}, mix(0xFF0B1B45u, 0xFF070B1Au, static_cast<float>(y) / static_cast<float>(v.h)));
        // soft glow behind the logo
        for (int i = 8; i >= 1; --i) circle(s, v.x + v.w / 2, v.y + S_(90), S_(20) + i * S_(9), with_alpha(0xFF3C6FE0u, 8));
        if (logo.ok()) {
            const int ls = S_(84), lx = v.x + v.w / 2 - S_(150) - ls / 2, ly = v.y + S_(50);
            for (int yy = 0; yy < ls; ++yy)
                for (int xx = 0; xx < ls; ++xx) {
                    const u32 c = logo.px[static_cast<std::size_t>(yy * logo.h / ls) * static_cast<std::size_t>(logo.w) + static_cast<std::size_t>(xx * logo.w / ls)];
                    const int dx = lx + xx, dy = ly + yy;
                    if (dx < 0 || dy < 0 || dx >= s.w || dy >= s.h || ca(c) == 0) continue;
                    u32& d = s.px[static_cast<std::size_t>(dy) * static_cast<std::size_t>(s.stride) + static_cast<std::size_t>(dx)];
                    d = ca(c) >= 255 ? (c | 0xFF000000u) : (mix(d, c | 0xFF000000u, static_cast<float>(ca(c)) / 255.0f) | 0xFF000000u);
                }
        }
        draw_text(s, v.x + v.w / 2 - S_(100), v.y + S_(52), "Netline", 0xFFFFFFFFu, 4 * sc());
        draw_text(s, v.x + v.w / 2 - S_(100), v.y + S_(120), this->t("More than just a browser", "Больше, чем просто браузер"), 0xFFB7C6EEu, sc());
        const Rect sr = start_search();
        rrect(s, sr, S_(22), 0xFF16224Au);
        rrect_outline(s, sr, S_(22), 1, addr_focus ? 0xFF4C8DFFu : 0xFF35508Fu);
        ring(s, sr.x + S_(24), sr.y + sr.h / 2 - 1, S_(7), 2, 0xFF8FB0FFu);
        line(s, sr.x + S_(29), sr.y + sr.h / 2 + S_(4), sr.x + S_(35), sr.y + sr.h / 2 + S_(10), 0xFF8FB0FFu, 2);
        draw_text(s, sr.x + S_(50), sr.y + (sr.h - font_h() * sc()) / 2, fit_text(addr.empty() ? this->t("Enter a search or address", "Введите запрос или адрес") : addr + (addr_focus ? "_" : ""), sr.w - S_(70), sc()), addr.empty() ? 0xFF8A99C4u : 0xFFFFFFFFu, sc());
        const Tile* tl = tiles();
        int n = 0;
        for (int i = 0; i < 9; ++i, ++n) tile_draw(s, i, tl[i].name, tl[i].color, tl[i].mark);
        for (int j = 0; j < static_cast<int>(bms.size()) && n < kSpeed - 1; ++j, ++n) tile_draw(s, n, url_split(bms[static_cast<std::size_t>(j)].url).host.c_str(), 0xFF3D5AA8u, "*");
        tile_draw(s, std::min(n, kSpeed - 1) == n ? kSpeed - 1 : n, this->t("Add", "Добавить").c_str(), 0xFF1D2B55u, "+");
    }
    void tile_draw(Surface& s, int i, const char* name, u32 color, const char* mark) {
        if (i >= kSpeed) return;
        const Rect r = tile_rect(i);
        rrect(s, r, S_(16), r.has(mx, my) ? 0xFF22346Eu : 0xFF182447u);
        rrect_outline(s, r, S_(16), 1, 0xFF2C4082u);
        const int isz = S_(44);
        const Rect ic = {r.x + (r.w - isz) / 2, r.y + S_(12), isz, isz};
        rrect(s, ic, S_(12), color);
        const std::string m = mark;
        if (m == ">") { for (int k = 0; k < S_(18); ++k) fill(s, {ic.x + S_(16) + k / 2, ic.y + S_(12) + k, std::max(1, S_(18) - k), 1}, 0xFFFFFFFFu); }
        else { const int sz2 = m.size() > 1 ? sc() : 2 * sc(); draw_text(s, ic.x + (ic.w - text_width(m, sz2)) / 2, ic.y + (ic.h - font_h() * sz2) / 2, m, 0xFFFFFFFFu, sz2); }
        const std::string nm = fit_text(name, r.w - S_(8), sc());
        draw_text(s, r.x + (r.w - text_width(nm, sc())) / 2, r.y + S_(64), nm, 0xFFE4EBFFu, sc());
    }
    void draw_page(Surface& s) {
        const Rect v = view();
        Tab& t = T();
        if (t.newtab) { draw_newtab(s); return; }
        if (t.is_image && !t.image.px.empty()) {
            fill(s, v, 0xFF14161Cu);
            const float k = std::min(1.0f, std::min(static_cast<float>(v.w - 20) / static_cast<float>(t.image.w), static_cast<float>(v.h - 20) / static_cast<float>(t.image.h)));
            const int dw = static_cast<int>(static_cast<float>(t.image.w) * k), dh = static_cast<int>(static_cast<float>(t.image.h) * k);
            const int ox = v.x + (v.w - dw) / 2, oy = v.y + (v.h - dh) / 2;
            for (int yy = 0; yy < dh; ++yy)
                for (int xx = 0; xx < dw; ++xx) {
                    const u32 c = t.image.px[static_cast<std::size_t>(yy * t.image.h / dh) * static_cast<std::size_t>(t.image.w) + static_cast<std::size_t>(xx * t.image.w / dw)];
                    s.px[static_cast<std::size_t>(oy + yy) * static_cast<std::size_t>(s.stride) + static_cast<std::size_t>(ox + xx)] = c | 0xFF000000u;
                }
            return;
        }
        if (t.loading && t.html.empty()) {
            fill(s, v, 0xFFFFFFFFu);
            draw_text(s, v.x + S_(20), v.y + S_(20), this->t("Loading ", "Загрузка ") + fit_text(t.req_url, v.w - S_(200), sc()), 0xFF555555u, sc());
            return;
        }
        draw_doc(s, v, t.doc, t.scroll, pics, t.focus_field);
        if (!t.loading && t.doc.items.size() < 4 && t.html.size() > 1500) {  // a page that is built by JavaScript only
            const std::string m1 = this->t("This page needs JavaScript, which Netline does not run.", "Этой странице нужен JavaScript, а Netline его не выполняет."),
                              m2 = this->t("Try the mobile version of the site (for example m.site.com) or another site.", "Попробуйте мобильную версию сайта (например m.сайт.ru) или другой сайт.");
            const Rect nb = {v.x + S_(24), v.y + S_(60), v.w - S_(48), S_(64)};
            rrect(s, nb, S_(10), 0xFFFFF4D6u);
            rrect_outline(s, nb, S_(10), 1, 0xFFE0B040u);
            draw_text(s, nb.x + S_(14), nb.y + S_(12), fit_text(m1, nb.w - S_(28), sc()), 0xFF5A4300u, sc());
            draw_text(s, nb.x + S_(14), nb.y + S_(36), fit_text(m2, nb.w - S_(28), sc()), 0xFF5A4300u, sc());
        }
        // scrollbar
        if (t.doc.height > v.h) {
            const int th2 = std::max(S_(24), v.h * v.h / t.doc.height), ty = v.y + t.scroll * (v.h - th2) / std::max(1, t.doc.height - v.h);
            rrect(s, {v.right() - S_(8), ty, S_(6), th2}, S_(3), 0x80606A80u);
        }
        if (!hover_link.empty()) {  // address of the link under the pointer
            const std::string txt = fit_text(hover_link, v.w - S_(30), 1);
            const Rect r = {v.x, v.bottom() - S_(22), text_width(txt, 1) + S_(16), S_(22)};
            fill(s, r, 0xFFF0F2F7u);
            draw_text(s, r.x + S_(8), r.y + (r.h - font_h()) / 2, txt, 0xFF222222u, 1);
        }
    }
    void draw_panel(Surface& s) {
        if (panel == 0) return;
        const Rect p = panel_rect();
        rrect(s, p.inset(-S_(2)), S_(14), 0x40000000u);
        rrect(s, p, S_(12), 0xFF121C3Cu);
        rrect_outline(s, p, S_(12), 1, 0xFF3C5099u);
        int y = p.y + S_(12);
        auto row = [&](const std::string& label, u32 col = 0xFFFFFFFFu) -> Rect {
            const Rect r = {p.x + S_(8), y, p.w - S_(16), S_(30)};
            if (r.has(mx, my)) rrect(s, r, S_(8), 0xFF1F2F66u);
            draw_text(s, r.x + S_(10), r.y + (r.h - font_h() * sc()) / 2, fit_text(label, r.w - S_(20), sc()), col, sc());
            y += S_(32);
            return r;
        };
        if (panel == 1) {
            draw_text(s, p.x + S_(14), y, "Netline", 0xFFB7C6EEu, sc()); y += S_(26);
            row(this->t("New tab", "Новая вкладка"));
            row(this->t("Bookmarks", "Закладки"));
            row(this->t("Downloads", "Загрузки"));
            static const char* const names[] = {"Яндекс (Yandex)", "Bing", "DuckDuckGo"};
            row(this->t("Search: ", "Поиск: ") + names[engine] + this->t("  (click to change)", "  (нажмите, чтобы сменить)"));

        } else if (panel == 2) {
            draw_text(s, p.x + S_(14), y, this->t("Downloads", "Загрузки"), 0xFFFFFFFFu, 2 * sc() > 2 ? 2 : sc()); y += S_(36);
            if (dls.empty()) draw_text(s, p.x + S_(14), y, this->t("Nothing downloaded yet.", "Пока ничего не скачано."), 0xFF8A99C4u, sc());
            for (std::size_t i = dls.size(); i-- > 0 && y < p.bottom() - S_(50);) {
                const Dl& d = dls[i];
                draw_text(s, p.x + S_(14), y, fit_text(d.name, p.w - S_(28), sc()), 0xFFFFFFFFu, sc());
                const Rect bar = {p.x + S_(14), y + S_(22), p.w - S_(28), S_(8)};
                rrect(s, bar, S_(4), 0xFF1F2B55u);
                const float f = d.state == 1 ? 1.0f : (d.total > 0 ? static_cast<float>(d.done) / static_cast<float>(d.total) : 0.0f);
                if (f > 0) rrect(s, {bar.x, bar.y, std::max(S_(8), static_cast<int>(static_cast<float>(bar.w) * f)), bar.h}, S_(4), d.state == 2 ? 0xFFE5604Fu : 0xFF4C8DFFu);
                const std::string st = d.state == 1 ? this->t("Done  ", "Готово  ") + fmt_size(d.done) : d.state == 2 ? this->t("Failed", "Ошибка") : fmt_size(d.done) + (d.total > 0 ? " / " + fmt_size(d.total) : "");
                draw_text(s, p.x + S_(14), y + S_(36), st + (d.state == 1 ? "   " + std::string("Shared/Downloads") : ""), d.state == 2 ? 0xFFFF8A7Au : 0xFF9FB0DDu, 1);
                y += S_(62);
            }
        } else if (panel == 3) {
            draw_text(s, p.x + S_(14), y, this->t("Bookmarks", "Закладки"), 0xFFFFFFFFu, 2 * sc() > 2 ? 2 : sc()); y += S_(36);
            if (bms.empty()) draw_text(s, p.x + S_(14), y, fit_text(this->t("Press the star to add a page.", "Нажмите звёздочку, чтобы добавить страницу."), p.w - S_(28), sc()), 0xFF8A99C4u, sc());
            for (std::size_t i = 0; i < bms.size() && y < p.bottom() - S_(40); ++i) {
                const Rect r = row(bms[i].title.empty() ? bms[i].url : bms[i].title);
                draw_text(s, r.right() - S_(22), r.y + (r.h - font_h() * sc()) / 2, "x", r.has(mx, my) && mx > r.right() - S_(30) ? 0xFFFF8A7Au : 0xFF7C8BB5u, sc());
            }
        }
    }
    void draw(Surface& s) override {
        if (tabs.empty()) return;
        draw_page(s);
        draw_tabs(s);
        draw_nav(s);
        draw_panel(s);
    }

    // ------------------------------------------------------------- input
    void submit_addr() {
        const std::string txt = addr;
        if (adding) {
            adding = false;
            std::string u = txt;
            if (!u.empty() && u.find("://") == std::string::npos) u = "https://" + u;
            if (!u.empty()) { bms.push_back({url_split(u).host, u}); save_conf(); }
            addr_focus = false; sync_addr();
            return;
        }
        addr_focus = false;
        if (!txt.empty()) navigate(txt);
    }
    void toggle_bookmark() {
        Tab& t = T();
        if (t.newtab) return;
        for (std::size_t i = 0; i < bms.size(); ++i) if (bms[i].url == t.url) { bms.erase(bms.begin() + static_cast<std::ptrdiff_t>(i)); save_conf(); return; }
        bms.push_back({t.title, t.url});
        save_conf();
    }
    void submit_form(int fi) {
        Tab& t = T();
        std::string body;
        const std::string url = form_submit(t.doc, fi, body);
        navigate(url, true, body);
    }
    void click_field(int fi) {
        Tab& t = T();
        Field& f = t.doc.fields[static_cast<std::size_t>(fi)];
        if (f.type == "submit" || f.type == "button" || f.type == "image") { if (f.type != "button") submit_form(fi); return; }
        if (f.type == "reset") { return; }
        if (f.type == "checkbox") { f.checked = !f.checked; return; }
        if (f.type == "radio") {
            for (auto& o : t.doc.fields) if (o.type == "radio" && o.form == f.form && o.name == f.name) o.checked = false;
            f.checked = true;
            return;
        }
        t.focus_field = fi;
    }
    void field_key(const Event& e) {
        Tab& t = T();
        Field& f = t.doc.fields[static_cast<std::size_t>(t.focus_field)];
        if (e.code == KEY_ENTER && f.type != "textarea") { submit_form(t.focus_field); return; }
        if (e.code == KEY_ESC) { t.focus_field = -1; return; }
        if (e.code == KEY_TAB) {  // next text field
            const int n = static_cast<int>(t.doc.fields.size());
            for (int k = 1; k <= n; ++k) {
                const int j = (t.focus_field + ((e.mods & 1) ? -k : k) + 2 * n) % n;
                const std::string& ty = t.doc.fields[static_cast<std::size_t>(j)].type;
                if (ty == "text" || ty == "search" || ty == "password" || ty == "email" || ty == "url" || ty == "tel" || ty == "number" || ty == "textarea") { t.focus_field = j; return; }
            }
            return;
        }
        if (e.code == KEY_BACKSPACE) { if (!f.value.empty()) f.value.erase(u8_prev(f.value, f.value.size())); return; }
        if (!e.text.empty() && f.value.size() < 1000) f.value += e.text;
    }
    void open_link(int idx) {
        Tab& t = T();
        if (idx < 0 || idx >= static_cast<int>(t.doc.links.size())) return;
        const std::string u = url_unwrap(t.doc.links[static_cast<std::size_t>(idx)]);
        if (u.compare(0, 4, "http") == 0) navigate(u);
    }
    void mouse(const Event& e) override {
        if (tabs.empty()) return;
        Tab& t = T();
        if (e.mouse_kind == 0) {
            hover_link.clear();
            const Rect v = view();
            if (!t.newtab && v.has(e.x, e.y) && panel == 0) {
                const int li = doc_link_at(t.doc, e.x - v.x, e.y - v.y + t.scroll);
                if (li >= 0) hover_link = url_unwrap(t.doc.links[static_cast<std::size_t>(li)]);
            }
            return;
        }
        if (e.mouse_kind == 3) {
            if (panel == 0 || !panel_rect().has(e.x, e.y)) t.scroll = clampi(t.scroll - e.wheel * S_(60), 0, std::max(0, t.doc.height - view().h));
            return;
        }
        if (e.mouse_kind != 1) return;
        // panels first
        if (panel != 0 && panel_rect().has(e.x, e.y)) {
            const Rect p = panel_rect();
            int y = p.y + S_(12);
            if (panel == 1) {
                y += S_(26);
                const int sel = (e.y - y) / S_(32);
                if (e.y >= y && sel == 0) { new_tab(); }
                else if (sel == 1) panel = 3;
                else if (sel == 2) panel = 2;
                else if (sel == 3) { engine = (engine + 1) % 3; save_conf(); }
            } else if (panel == 3) {
                y += S_(36);
                const int idx = (e.y - y) / S_(32);
                if (e.y >= y && idx >= 0 && idx < static_cast<int>(bms.size())) {
                    if (e.x > p.right() - S_(34)) { bms.erase(bms.begin() + idx); save_conf(); }
                    else { const std::string u = bms[static_cast<std::size_t>(idx)].url; panel = 0; navigate(u); }
                }
            }
            return;
        }
        const int was = panel;
        if (panel != 0) { panel = 0; if (!tool_btn(1).has(e.x, e.y) && !tool_btn(2).has(e.x, e.y)) return; }
        // tabs
        for (int i = 0; i < static_cast<int>(tabs.size()); ++i) {
            const Rect r = tab_rect(i);
            if (!r.has(e.x, e.y)) continue;
            if (e.x > r.right() - S_(28)) close_tab(i);
            else { cur = i; sync_addr(); }
            return;
        }
        if (plus_rect().has(e.x, e.y)) { new_tab(); return; }
        if (nav_btn(0).has(e.x, e.y)) { go_hist(-1); return; }
        if (nav_btn(1).has(e.x, e.y)) { go_hist(1); return; }
        if (nav_btn(2).has(e.x, e.y)) { if (t.loading) { kill_fetch(t); } else if (!t.newtab) navigate(t.url, false); return; }
        if (tool_btn(0).has(e.x, e.y)) { toggle_bookmark(); return; }
        if (tool_btn(1).has(e.x, e.y)) { panel = was == 2 ? 0 : 2; return; }
        if (tool_btn(2).has(e.x, e.y)) { panel = was == 1 ? 0 : 1; return; }
        if (addr_rect().has(e.x, e.y)) { addr_focus = true; addr_all = true; return; }
        // start page
        if (t.newtab) {
            if (start_search().has(e.x, e.y)) { addr_focus = true; addr_all = false; return; }
            const Tile* tl = tiles();
            int n = 0;
            for (int i = 0; i < 9; ++i, ++n) if (tile_rect(i).has(e.x, e.y)) { navigate(tl[i].url); return; }
            for (int j = 0; j < static_cast<int>(bms.size()) && n < kSpeed - 1; ++j, ++n) if (tile_rect(n).has(e.x, e.y)) { navigate(bms[static_cast<std::size_t>(j)].url); return; }
            const int add_i = std::min(n, kSpeed - 1) == n ? kSpeed - 1 : n;
            if (tile_rect(add_i).has(e.x, e.y)) { adding = true; addr.clear(); addr_focus = true; addr_all = false; return; }
            addr_focus = false;
            return;
        }
        addr_focus = false;
        const Rect v = view();
        if (v.has(e.x, e.y)) {
            const int fi = doc_field_at(t.doc, e.x - v.x, e.y - v.y + t.scroll);
            if (fi >= 0) { click_field(fi); return; }
            t.focus_field = -1;
            const int li = doc_link_at(t.doc, e.x - v.x, e.y - v.y + t.scroll);
            if (li >= 0) open_link(li);
        }
    }
    void key(const Event& e) override {
        if (tabs.empty()) return;
        Tab& t = T();
        const bool ctrl = (e.mods & 2) != 0, alt = (e.mods & 4) != 0;
        if (ctrl && e.code == KEY_T) { new_tab(); return; }
        if (ctrl && e.code == KEY_W) { close_tab(cur); return; }
        if (ctrl && e.code == KEY_L) { addr_focus = true; addr_all = true; return; }
        if (ctrl && e.code == KEY_R) { if (!t.newtab) navigate(t.url, false); return; }
        if (ctrl && e.code == KEY_D) { toggle_bookmark(); return; }
        if (e.code == KEY_F5) { if (!t.newtab) navigate(t.url, false); return; }
        if (alt && e.code == KEY_LEFT) { go_hist(-1); return; }
        if (alt && e.code == KEY_RIGHT) { go_hist(1); return; }
        if (ctrl && e.code == KEY_TAB) { cur = (cur + 1) % static_cast<int>(tabs.size()); sync_addr(); return; }
        if (!addr_focus && !t.newtab && t.focus_field >= 0 && t.focus_field < static_cast<int>(t.doc.fields.size()) && !ctrl && !alt && e.code != KEY_F5) { field_key(e); return; }
        if (e.code == KEY_ESC) { if (panel) panel = 0; else if (t.loading) kill_fetch(t); else { addr_focus = false; adding = false; sync_addr(); } return; }
        if (addr_focus || (t.newtab && !ctrl && !alt && !e.text.empty() && e.code != KEY_SPACE)) {
            if (!addr_focus) { addr_focus = true; addr_all = false; }
            if (e.code == KEY_ENTER) { submit_addr(); return; }
            if (e.code == KEY_BACKSPACE) { if (addr_all) addr.clear(); else if (!addr.empty()) addr.erase(u8_prev(addr, addr.size())); addr_all = false; return; }
            if (ctrl && e.code == KEY_A) { addr_all = true; return; }
            if (!e.text.empty() && !ctrl && addr.size() < 500) { if (addr_all) { addr.clear(); addr_all = false; } addr += e.text; }
            return;
        }
        const int page = view().h - S_(40);
        const int maxs = std::max(0, t.doc.height - view().h);
        switch (e.code) {
            case KEY_DOWN: t.scroll = std::min(maxs, t.scroll + S_(48)); break;
            case KEY_UP: t.scroll = std::max(0, t.scroll - S_(48)); break;
            case KEY_PAGEDOWN: case KEY_SPACE: t.scroll = std::min(maxs, t.scroll + page); break;
            case KEY_PAGEUP: t.scroll = std::max(0, t.scroll - page); break;
            case KEY_HOME: t.scroll = 0; break;
            case KEY_END: t.scroll = maxs; break;
            default: break;
        }
    }
};
