// Netline 0.5.6 - web browser for Difiw OS. Web content is rendered by the Chromium Embedded Framework (CEF,
// off-screen mode); the browser chrome (tabs, address bar, menu) is drawn here with SDL2.
// SPDX-License-Identifier: MIT
#include <unistd.h>
#include <sys/stat.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "include/cef_app.h"
#include "include/cef_browser.h"
#include "include/cef_client.h"
#include "include/cef_command_line.h"
#include "include/cef_version.h"
#include "include/cef_parser.h"
#include "include/wrapper/cef_helpers.h"

#include "platform.hpp"

#define NETLINE_VERSION "0.5.6"

namespace {

// ---------------------------------------------------------------- paths & settings
std::string g_exeDir, g_home, g_dataDir, g_downloadDir;
bool g_private = false;

std::string homeDir() { const char* h = getenv("HOME"); return h ? h : "/tmp"; }
void mkdirs(const std::string& p) {
    for (size_t i = 1; i <= p.size(); i++)
        if (i == p.size() || p[i] == '/') mkdir(p.substr(0, i).c_str(), 0700);
}

struct Engine { const char* id; const char* name; const char* url; };
const Engine kEngines[] = {
    {"yandex", "Яндекс", "https://yandex.ru/search/?text="},
    {"google", "Google", "https://www.google.com/search?q="},
    {"ddg", "DuckDuckGo", "https://duckduckgo.com/?q="},
    {"bing", "Bing", "https://www.bing.com/search?q="},
};
int g_engine = 0;  // Yandex is the default search engine

void loadSettings() {
    std::ifstream f(g_dataDir + "/settings");
    std::string k, v;
    while (f >> k >> v)
        if (k == "search")
            for (int i = 0; i < 4; i++) if (v == kEngines[i].id) g_engine = i;
}
void saveSettings() {
    if (g_private) return;
    std::ofstream f(g_dataDir + "/settings");
    f << "search " << kEngines[g_engine].id << "\n";
}

// ---------------------------------------------------------------- URL / text helpers
std::string urlEncode(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string o;
    for (unsigned char c : s) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') o += (char)c;
        else if (c == ' ') o += '+';
        else { o += '%'; o += hex[c >> 4]; o += hex[c & 15]; }
    }
    return o;
}
std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}
bool hasScheme(const std::string& s) {
    size_t i = 0;
    if (s.empty() || !isalpha((unsigned char)s[0])) return false;
    while (i < s.size() && (isalnum((unsigned char)s[i]) || s[i] == '+' || s[i] == '-' || s[i] == '.')) i++;
    return i < s.size() && s[i] == ':' && (s.compare(i, 3, "://") == 0 || s == "about:blank");
}
bool looksLikeHost(const std::string& s) {  // host[:port][/path] without spaces; has a dot or is localhost
    if (s.find(' ') != std::string::npos) return false;
    std::string host = s.substr(0, s.find_first_of("/?#"));
    size_t colon = host.rfind(':');
    if (colon != std::string::npos) host = host.substr(0, colon);
    if (host == "localhost") return true;
    size_t dot = host.rfind('.');
    if (dot == std::string::npos || dot == 0 || dot + 1 >= host.size()) return false;
    std::string tld = host.substr(dot + 1);
    bool allDigits = true;
    for (unsigned char c : tld) if (!isdigit(c)) allDigits = false;
    if (allDigits) return std::count(host.begin(), host.end(), '.') == 3;  // IPv4
    return tld.size() >= 2;  // letters or non-ASCII (IDN)
}
// What the address bar does with typed text: an address opens, anything else is searched.
std::string resolveInput(const std::string& raw) {
    std::string s = trim(raw);
    if (s.empty()) return "";
    if (s.compare(0, 11, "javascript:") == 0) return "";
    if (hasScheme(s) || s.compare(0, 5, "file:") == 0) return s;
    if (looksLikeHost(s)) {
        bool local = s.compare(0, 9, "localhost") == 0 || s.compare(0, 4, "127.") == 0 || s.compare(0, 3, "10.") == 0 ||
                     s.compare(0, 8, "192.168.") == 0;
        return std::string(local ? "http://" : "https://") + s;
    }
    return std::string(kEngines[g_engine].url) + urlEncode(s);
}
std::string htmlEsc(const std::string& s) {
    std::string o;
    for (char c : s) {
        if (c == '&') o += "&amp;"; else if (c == '<') o += "&lt;"; else if (c == '>') o += "&gt;";
        else if (c == '"') o += "&quot;"; else o += c;
    }
    return o;
}
std::string hostOf(const std::string& url) {
    size_t a = url.find("://");
    if (a == std::string::npos) return "";
    a += 3;
    size_t b = url.find_first_of("/?#:", a);
    std::string h = url.substr(a, b == std::string::npos ? std::string::npos : b - a);
    if (h.compare(0, 4, "www.") == 0) h = h.substr(4);
    return h;
}
bool isInternal(const std::string& url) {
    return url.compare(0, 7, "file://") == 0 || url.compare(0, 5, "data:") == 0 || url == "about:blank";
}
std::string utf8Pop(std::string s) {  // remove last UTF-8 character
    if (s.empty()) return s;
    size_t i = s.size() - 1;
    while (i > 0 && ((unsigned char)s[i] & 0xC0) == 0x80) i--;
    return s.substr(0, i);
}
std::string utf8Shift(const std::string& s) {  // drop first UTF-8 character
    size_t i = 1;
    while (i < s.size() && ((unsigned char)s[i] & 0xC0) == 0x80) i++;
    return s.substr(std::min(i, s.size()));
}
std::string sizeStr(int64_t b) {
    char buf[32];
    if (b >= (1 << 20)) snprintf(buf, sizeof buf, "%.1f МБ", b / 1048576.0);
    else snprintf(buf, sizeof buf, "%.0f КБ", b / 1024.0);
    return buf;
}

// ---------------------------------------------------------------- tabs / state
struct Tab {
    int id = 0;
    CefRefPtr<CefBrowser> br;
    std::string title, url;
    bool loading = false, back = false, fwd = false, internal = true;
    std::vector<uint32_t> px;  // 0xAARRGGBB of the page (CEF BGRA)
    int pw = 0, ph = 0;
    double zoom = 0;
};
struct Download { int id; std::string name, path; int64_t got = 0, total = 0; bool done = false, canceled = false; };

std::vector<std::unique_ptr<Tab>> g_tabs;
int g_active = 0;
std::vector<std::string> g_closed;          // closed tab URLs (Ctrl+Shift+T)
std::vector<Download> g_downloads;
std::vector<std::string> g_pending;         // URLs waiting to be opened as tabs
int g_viewW = 1280, g_viewH = 700;
bool g_quit = false, g_redraw = true;
nl::Platform* g_plat = nullptr;
bool g_wantActivate = true;

Tab* findTab(CefRefPtr<CefBrowser> b) {
    for (auto& t : g_tabs) if (t->br && t->br->IsSame(b)) return t.get();
    return nullptr;
}
Tab* activeTab() { return g_tabs.empty() ? nullptr : g_tabs[std::min<int>(g_active, g_tabs.size() - 1)].get(); }

// ---------------------------------------------------------------- bookmarks / history / internal pages
std::vector<std::pair<std::string, std::string>> g_bookmarks;  // url, title
void loadBookmarks() {
    g_bookmarks.clear();
    std::ifstream f(g_dataDir + "/bookmarks.tsv");
    std::string line;
    while (std::getline(f, line)) {
        size_t t = line.find('\t');
        if (t != std::string::npos) g_bookmarks.push_back({line.substr(0, t), line.substr(t + 1)});
    }
}
void saveBookmarks() {
    if (g_private) return;
    std::ofstream f(g_dataDir + "/bookmarks.tsv");
    for (auto& b : g_bookmarks) f << b.first << '\t' << b.second << '\n';
}
int bookmarkIndex(const std::string& url) {
    for (size_t i = 0; i < g_bookmarks.size(); i++) if (g_bookmarks[i].first == url) return (int)i;
    return -1;
}
void addHistory(const std::string& url, const std::string& title) {
    if (g_private || isInternal(url) || url.empty()) return;
    std::ofstream f(g_dataDir + "/history.tsv", std::ios::app);
    f << (long)time(nullptr) << '\t' << url << '\t' << title << '\n';
}
const char* kPageCss =
    "body{margin:0;padding:32px 8vw;background:#0a1030;color:#e8eeff;font:15px 'DejaVu Sans',sans-serif}"
    "h1{font-weight:600;margin:0 0 20px}a{color:#7fa8ff;text-decoration:none}a:hover{text-decoration:underline}"
    ".r{padding:9px 14px;border-bottom:1px solid #1b2a66;display:flex;gap:16px;justify-content:space-between}"
    ".s{color:#8c9bd6;font-size:13px;overflow:hidden;text-overflow:ellipsis;white-space:nowrap;max-width:50%}"
    ".b{height:6px;background:#1b2a66;border-radius:3px;margin-top:6px}.b i{display:block;height:6px;background:#3d7bff;border-radius:3px}";
std::string writePage(const std::string& name, const std::string& title, const std::string& body) {
    std::string dir = (g_private ? std::string("/tmp/netline-private-") + std::to_string(getpid()) : g_dataDir) + "/pages";
    mkdirs(dir);
    std::string path = dir + "/" + name + ".html";
    std::ofstream f(path);
    f << "<!doctype html><meta charset=utf-8><title>" << htmlEsc(title) << "</title><style>" << kPageCss
      << "</style><h1>" << htmlEsc(title) << "</h1>" << body;
    return "file://" + path;
}
std::string historyPage() {
    std::vector<std::string> lines;
    std::ifstream f(g_dataDir + "/history.tsv");
    std::string l;
    while (std::getline(f, l)) lines.push_back(l);
    std::ostringstream o;
    int n = 0;
    for (size_t i = lines.size(); i-- > 0 && n < 500; n++) {
        std::istringstream ls(lines[i]);
        std::string ts, url, title;
        std::getline(ls, ts, '\t'); std::getline(ls, url, '\t'); std::getline(ls, title);
        char tb[32]; time_t t = atol(ts.c_str()); strftime(tb, sizeof tb, "%d.%m %H:%M", localtime(&t));
        o << "<div class=r><a href=\"" << htmlEsc(url) << "\">" << htmlEsc(title.empty() ? url : title)
          << "</a><span class=s>" << tb << " · " << htmlEsc(hostOf(url)) << "</span></div>";
    }
    if (!n) o << "<p class=s>История пуста" << (g_private ? " (приватный режим не сохраняет историю)" : "") << ".</p>";
    return writePage("history", "История", o.str());
}
std::string bookmarksPage() {
    std::ostringstream o;
    for (auto& b : g_bookmarks)
        o << "<div class=r><a href=\"" << htmlEsc(b.first) << "\">" << htmlEsc(b.second.empty() ? b.first : b.second)
          << "</a><span class=s>" << htmlEsc(hostOf(b.first)) << "</span></div>";
    if (g_bookmarks.empty()) o << "<p class=s>Закладок пока нет — нажмите ★ в адресной строке.</p>";
    return writePage("bookmarks", "Закладки", o.str());
}
std::string downloadsPage() {
    std::ostringstream o;
    for (size_t i = g_downloads.size(); i-- > 0;) {
        auto& d = g_downloads[i];
        int pct = d.total > 0 ? (int)(d.got * 100 / d.total) : (d.done ? 100 : 0);
        o << "<div class=r><div><b>" << htmlEsc(d.name) << "</b><div class=s style='max-width:none'>" << htmlEsc(d.path)
          << "</div><div class=b><i style='width:" << pct << "%'></i></div></div><span class=s>"
          << (d.canceled ? "Отменено" : d.done ? "Завершено" : sizeStr(d.got) + " / " + (d.total > 0 ? sizeStr(d.total) : "?"))
          << "</span></div>";
    }
    if (g_downloads.empty()) o << "<p class=s>Загрузок пока нет. Файлы сохраняются в " << htmlEsc(g_downloadDir) << ".</p>";
    return writePage("downloads", "Загрузки", o.str());
}

// ---------------------------------------------------------------- CEF handlers
std::string errorPage(const std::string& url, const std::string& text, int code) {
    std::ostringstream o;
    o << "<!doctype html><meta charset=utf-8><title>Не удалось открыть страницу</title><style>" << kPageCss
      << "</style><h1>Не удалось открыть страницу</h1><p>" << htmlEsc(url) << "</p><p class=s style='max-width:none'>" << htmlEsc(text)
      << " (код " << code << ")</p><p>Проверьте адрес и подключение к сети. Проверка сертификатов не отключена: сайт с "
      << "недействительным сертификатом не будет открыт.</p>";
    return "data:text/html;charset=utf-8," + CefURIEncode(o.str(), false).ToString();
}

class NetClient : public CefClient, public CefLifeSpanHandler, public CefLoadHandler, public CefDisplayHandler,
                  public CefRenderHandler, public CefDownloadHandler, public CefContextMenuHandler,
                  public CefRequestHandler {
public:
    CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override { return this; }
    CefRefPtr<CefLoadHandler> GetLoadHandler() override { return this; }
    CefRefPtr<CefDisplayHandler> GetDisplayHandler() override { return this; }
    CefRefPtr<CefRenderHandler> GetRenderHandler() override { return this; }
    CefRefPtr<CefDownloadHandler> GetDownloadHandler() override { return this; }
    CefRefPtr<CefContextMenuHandler> GetContextMenuHandler() override { return this; }
    CefRefPtr<CefRequestHandler> GetRequestHandler() override { return this; }

    // --- a crashed or killed renderer must not take the browser down: show a reload page
    void OnRenderProcessTerminated(CefRefPtr<CefBrowser> b, TerminationStatus status, int code, const CefString& err) override {
        if (Tab* t = findTab(b)) t->loading = false;
        b->GetMainFrame()->LoadURL(errorPage(b->GetMainFrame()->GetURL().ToString(), "Процесс вкладки завершился (" + err.ToString() + ")", code));
    }

    // --- life span
    bool OnBeforePopup(CefRefPtr<CefBrowser> b, CefRefPtr<CefFrame>, int, const CefString& target_url, const CefString&,
                       cef_window_open_disposition_t disp, bool, const CefPopupFeatures&, CefWindowInfo&, CefRefPtr<CefClient>&,
                       CefBrowserSettings&, CefRefPtr<CefDictionaryValue>&, bool*) override {
        // target=_blank and window.open become tabs of the same profile
        if (!target_url.empty()) {
            g_pending.push_back(target_url.ToString());
            g_wantActivate = disp != CEF_WOD_NEW_BACKGROUND_TAB;
        }
        return true;
    }
    void OnAfterCreated(CefRefPtr<CefBrowser> b) override {
        auto t = std::make_unique<Tab>();
        t->id = b->GetIdentifier();
        t->br = b;
        g_tabs.push_back(std::move(t));
        if (g_wantActivate) g_active = (int)g_tabs.size() - 1;
        for (size_t i = 0; i < g_tabs.size(); i++) g_tabs[i]->br->GetHost()->WasHidden((int)i != g_active);
        g_wantActivate = true;
        if ((int)g_tabs.size() - 1 == g_active) b->GetHost()->SetFocus(true);
        g_redraw = true;
    }
    bool DoClose(CefRefPtr<CefBrowser>) override { return false; }
    void OnBeforeClose(CefRefPtr<CefBrowser> b) override {
        for (size_t i = 0; i < g_tabs.size(); i++)
            if (g_tabs[i]->br && g_tabs[i]->br->IsSame(b)) {
                g_tabs.erase(g_tabs.begin() + i);
                if (g_active >= (int)i && g_active > 0) g_active--;
                break;
            }
        if (g_tabs.empty()) g_quit = true;
        g_redraw = true;
    }

    // --- load
    void OnLoadingStateChange(CefRefPtr<CefBrowser> b, bool loading, bool back, bool fwd) override {
        if (Tab* t = findTab(b)) { t->loading = loading; t->back = back; t->fwd = fwd; }
        g_redraw = true;
    }
    void OnLoadEnd(CefRefPtr<CefBrowser> b, CefRefPtr<CefFrame> f, int) override {
        if (!f->IsMain()) return;
        if (Tab* t = findTab(b)) addHistory(t->url, t->title);
    }
    void OnLoadError(CefRefPtr<CefBrowser> b, CefRefPtr<CefFrame> f, ErrorCode code, const CefString& text,
                     const CefString& url) override {
        if (!f->IsMain() || code == ERR_ABORTED) return;
        f->LoadURL(errorPage(url.ToString(), text.ToString(), code));
    }

    // --- display
    void OnAddressChange(CefRefPtr<CefBrowser> b, CefRefPtr<CefFrame> f, const CefString& url) override {
        if (!f->IsMain()) return;
        if (Tab* t = findTab(b)) { t->url = url.ToString(); t->internal = isInternal(t->url); }
        g_redraw = true;
    }
    void OnTitleChange(CefRefPtr<CefBrowser> b, const CefString& title) override {
        if (Tab* t = findTab(b)) t->title = title.ToString();
        g_redraw = true;
    }
    bool OnCursorChange(CefRefPtr<CefBrowser> b, CefCursorHandle, cef_cursor_type_t type, const CefCursorInfo&) override {
        Tab* t = findTab(b);
        if (t != activeTab()) return true;
        g_plat->set_cursor((int)type);
        return true;
    }

    // --- off-screen rendering
    void GetViewRect(CefRefPtr<CefBrowser>, CefRect& r) override { r = CefRect(0, 0, g_viewW, g_viewH); }
    void OnPaint(CefRefPtr<CefBrowser> b, PaintElementType type, const RectList& dirty, const void* buf, int w, int h) override {
        if (type != PET_VIEW) return;
        Tab* t = findTab(b);
        if (!t) return;
        const uint32_t* src = static_cast<const uint32_t*>(buf);
        if (t->pw != w || t->ph != h) { t->pw = w; t->ph = h; t->px.assign((size_t)w * h, 0xFFFFFFFFu); t->px.assign(src, src + (size_t)w * h); }
        else
            for (const CefRect& r : dirty) {
                int x0 = std::max(0, r.x), x1 = std::min(w, r.x + r.width);
                for (int y = std::max(0, r.y); y < std::min(h, r.y + r.height); y++)
                    if (x1 > x0) memcpy(&t->px[(size_t)y * w + x0], src + (size_t)y * w + x0, (size_t)(x1 - x0) * 4);
            }
        if (t == activeTab()) g_redraw = true;
    }

    // --- downloads: saved to ~/Downloads, never silently overwriting
    bool OnBeforeDownload(CefRefPtr<CefBrowser>, CefRefPtr<CefDownloadItem> item, const CefString& suggested,
                          CefRefPtr<CefBeforeDownloadCallback> cb) override {
        std::string name = suggested.ToString();
        for (auto& c : name) if (c == '/' || c == '\\' || c == ':' || (unsigned char)c < 32) c = '_';
        if (name.empty() || name == "." || name == "..") name = "download";
        mkdirs(g_downloadDir);
        std::string path = g_downloadDir + "/" + name;
        for (int n = 1; access(path.c_str(), F_OK) == 0 && n < 1000; n++) {
            size_t dot = name.rfind('.');
            std::string stem = dot == std::string::npos || dot == 0 ? name : name.substr(0, dot);
            std::string ext = dot == std::string::npos || dot == 0 ? "" : name.substr(dot);
            path = g_downloadDir + "/" + stem + " (" + std::to_string(n) + ")" + ext;
        }
        g_downloads.push_back({(int)item->GetId(), name, path});
        cb->Continue(path, false);
        return true;
    }
    void OnDownloadUpdated(CefRefPtr<CefBrowser>, CefRefPtr<CefDownloadItem> item, CefRefPtr<CefDownloadItemCallback>) override {
        for (auto& d : g_downloads)
            if (d.id == (int)item->GetId()) {
                d.got = item->GetReceivedBytes(); d.total = item->GetTotalBytes();
                d.done = item->IsComplete(); d.canceled = item->IsCanceled();
            }
        g_redraw = true;
    }

    // --- context menu: the page gets its own menu from SDL; the native one is suppressed
    bool RunContextMenu(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>, CefRefPtr<CefContextMenuParams>, CefRefPtr<CefMenuModel>,
                        CefRefPtr<CefRunContextMenuCallback> cb) override {
        cb->Cancel();
        return true;
    }

private:
    IMPLEMENT_REFCOUNTING(NetClient);
};
CefRefPtr<NetClient> g_client;
CefRefPtr<CefRequestContext> g_ctx;

class NetApp : public CefApp, public CefBrowserProcessHandler {
public:
    CefRefPtr<CefBrowserProcessHandler> GetBrowserProcessHandler() override { return this; }
    void OnBeforeCommandLineProcessing(const CefString&, CefRefPtr<CefCommandLine> cl) override {
        // Difiw OS has no X11/Wayland and (in VMs) no GPU: headless Ozone + software compositing unless overridden.
        const char* oz = getenv("NETLINE_OZONE");
        cl->AppendSwitchWithValue("ozone-platform", oz ? oz : "headless");
        if (!getenv("NETLINE_GPU")) { cl->AppendSwitch("disable-gpu"); cl->AppendSwitch("disable-gpu-compositing"); }
        cl->AppendSwitchWithValue("password-store", "basic");
        cl->AppendSwitchWithValue("lang", "ru");
        cl->AppendSwitchWithValue("disable-features", "Translate");
    }
private:
    IMPLEMENT_REFCOUNTING(NetApp);
};

void openTab(const std::string& url, bool activate = true) {
    CefWindowInfo wi;
    wi.SetAsWindowless(0);
    wi.runtime_style = CEF_RUNTIME_STYLE_ALLOY;
    CefBrowserSettings bs;
    bs.windowless_frame_rate = 60;
    g_wantActivate = activate;
    CefBrowserHost::CreateBrowser(wi, g_client, url, bs, nullptr, g_ctx);
}
void newTab() { openTab("file://" + g_exeDir + "/res/newtab.html"); }

void closeTab(int i) {
    if (i < 0 || i >= (int)g_tabs.size()) return;
    Tab* t = g_tabs[i].get();
    if (!t->internal && !t->url.empty() && !g_private) g_closed.push_back(t->url);
    if (g_tabs.size() == 1) {  // the last tab is never closed: it returns to the start page
        t->br->GetMainFrame()->LoadURL("file://" + g_exeDir + "/res/newtab.html");
        return;
    }
    t->br->GetHost()->CloseBrowser(true);
}
void setActive(int i) {
    if (i < 0 || i >= (int)g_tabs.size()) return;
    g_active = i;
    for (size_t k = 0; k < g_tabs.size(); k++) {
        g_tabs[k]->br->GetHost()->WasHidden((int)k != i);
        g_tabs[k]->br->GetHost()->SetFocus((int)k == i);
    }
    if (g_tabs[i]->br) g_tabs[i]->br->GetHost()->Invalidate(PET_VIEW);
    g_redraw = true;
}
void navigate(const std::string& input) {
    std::string u = resolveInput(input);
    if (u.empty()) return;
    if (Tab* t = activeTab()) t->br->GetMainFrame()->LoadURL(u);
}

// ---------------------------------------------------------------- drawing (Difiw gfx, shared by every backend)
using dg::rgba;
using dg::Rect;
const int kTabH = 40, kBarH = 52, kTop = kTabH + kBarH;
const dg::u32 cBg = rgba(9, 14, 40), cTab = rgba(20, 32, 82), cTabAct = rgba(34, 56, 130), cBar = rgba(14, 22, 62),
              cField = rgba(8, 14, 44), cAccent = rgba(61, 123, 255), cText = rgba(232, 238, 255), cDim = rgba(140, 155, 214),
              cLine = rgba(40, 62, 150), cWarn = rgba(255, 190, 60), cOff = rgba(70, 85, 140);
dg::Surface g_logoSurf;
dg::Image g_logoImg;

std::string g_addr;
bool g_addrFocus = false, g_findMode = false, g_menuOpen = false;
int g_mx = 0, g_my = 0, g_mods = 0;
uint32_t g_toastUntil = 0;
std::string g_toast;

const int FW = 8, FH = 16;
int tw(const std::string& s) { return dg::text_width(s, 1); }
void text(dg::Surface& s, int x, int y, const std::string& t, dg::u32 c) { dg::draw_text(s, x, y, t, c, 1); }

Rect tabRect(int i, int n) {
    int avail = g_viewW - 90 - 8, w = std::min(220, std::max(60, avail / std::max(1, n)));
    return {40 + i * (w + 2), 6, w, kTabH - 6};
}
Rect plusRect() { int n = (int)g_tabs.size(); Rect r = tabRect(n > 0 ? n - 1 : 0, std::max(1, n)); return {r.x + r.w + 6, 8, 28, 28}; }
Rect btnRect(int i) { return {10 + i * 40, kTabH + 8, 36, 36}; }  // 0 back, 1 forward, 2 reload, 3 home
Rect addrRect() { int x = 10 + 4 * 40 + 4; return {x, kTabH + 8, g_viewW - x - 10 - 2 * 40 - 4, 36}; }
Rect starRect() { Rect a = addrRect(); return {a.x + a.w - 34, a.y + 2, 32, 32}; }
Rect menuBtnRect() { return {g_viewW - 46, kTabH + 8, 36, 36}; }
Rect dlBtnRect() { return {g_viewW - 86, kTabH + 8, 36, 36}; }
const char* kMenu[] = {"Новая вкладка", "Новое приватное окно", "История", "Загрузки", "Закладки", "Поисковая система", "Увеличить масштаб",
                       "Уменьшить масштаб", "Найти на странице", "О Netline", "Выход"};
const char* kMenuKeys[] = {"Ctrl+T", "Ctrl+Shift+N", "Ctrl+H", "Ctrl+J", "Ctrl+B", "", "Ctrl +", "Ctrl -", "Ctrl+F", "", ""};
const int kMenuN = 11;
Rect menuRect() { return {g_viewW - 300, kTop - 4, 290, kMenuN * 34 + 12}; }

void toast(const std::string& s) { g_toast = s; g_toastUntil = g_plat->ticks() + 3000; g_redraw = true; }

void drawIcon(dg::Surface& s, int kind, Rect r, dg::u32 c) {  // vector glyphs for the toolbar buttons
    int cx = r.x + r.w / 2, cy = r.y + r.h / 2;
    switch (kind) {
        case 0: dg::line(s, cx + 7, cy, cx - 7, cy, c, 2); dg::line(s, cx - 7, cy, cx - 1, cy - 6, c, 2); dg::line(s, cx - 7, cy, cx - 1, cy + 6, c, 2); break;
        case 1: dg::line(s, cx - 7, cy, cx + 7, cy, c, 2); dg::line(s, cx + 7, cy, cx + 1, cy - 6, c, 2); dg::line(s, cx + 7, cy, cx + 1, cy + 6, c, 2); break;
        case 2: dg::ring(s, cx, cy, 8, 2, c); dg::line(s, cx + 8, cy - 9, cx + 8, cy - 3, c, 2); dg::line(s, cx + 8, cy - 3, cx + 2, cy - 3, c, 2); break;
        case 3: dg::line(s, cx - 9, cy, cx, cy - 8, c, 2); dg::line(s, cx, cy - 8, cx + 9, cy, c, 2);
                dg::line(s, cx - 6, cy, cx - 6, cy + 8, c, 2); dg::line(s, cx + 6, cy, cx + 6, cy + 8, c, 2); dg::line(s, cx - 6, cy + 8, cx + 6, cy + 8, c, 2); break;
        case 4: for (int i = -6; i <= 6; i += 6) dg::fill(s, {cx - 8, cy + i - 1, 16, 2}, c); break;
        case 5: dg::line(s, cx, cy - 8, cx, cy + 4, c, 2); dg::line(s, cx - 5, cy - 1, cx, cy + 5, c, 2); dg::line(s, cx + 5, cy - 1, cx, cy + 5, c, 2); dg::line(s, cx - 8, cy + 9, cx + 8, cy + 9, c, 2); break;
        case 6: dg::line(s, cx - 4, cy - 4, cx + 4, cy + 4, c, 2); dg::line(s, cx - 4, cy + 4, cx + 4, cy - 4, c, 2); break;
    }
}

void render() {
    dg::Surface& s = g_plat->surface();
    s.reset_clip();
    g_viewW = s.w;
    g_viewH = std::max(100, s.h - kTop);
    dg::fill(s, {0, 0, s.w, s.h}, cBg);
    Tab* at = activeTab();
    if (at && !at->px.empty()) {  // the page itself (opaque)
        int w = std::min(at->pw, s.w), h = std::min(at->ph, s.h - kTop);
        for (int y = 0; y < h; y++) {
            dg::u32* d = s.px + (size_t)(kTop + y) * s.stride;
            const uint32_t* p = &at->px[(size_t)y * at->pw];
            for (int x = 0; x < w; x++) d[x] = p[x] | 0xFF000000u;
        }
    }
    // tab strip: logo, tabs, "+"
    if (g_logoSurf.w) dg::blit_blend(s, 8, 8, g_logoSurf);
    int n = (int)g_tabs.size();
    for (int i = 0; i < n; i++) {
        Rect r = tabRect(i, n);
        bool act = i == g_active, hover = r.has(g_mx, g_my);
        dg::rrect(s, {r.x, r.y, r.w, r.h + 8}, 10, act ? cBar : (hover ? cTabAct : cTab));
        Tab* t = g_tabs[i].get();
        std::string ttl = t->title.empty() ? (t->loading ? "Загрузка…" : "Новая вкладка") : t->title;
        if (t->internal && t->title.empty()) ttl = "Новая вкладка";
        std::string host = hostOf(t->url);
        dg::circle(s, r.x + 18, r.y + 17, 8, t->loading ? cWarn : cAccent);
        std::string letter = host.empty() ? "N" : std::string(1, (char)toupper(host[0]));
        text(s, r.x + 14, r.y + 9, letter, rgba(255, 255, 255));
        text(s, r.x + 32, r.y + 9, dg::fit_text(ttl, r.w - 32 - 30), act ? cText : cDim);
        if (hover || act) drawIcon(s, 6, {r.x + r.w - 26, r.y + 7, 20, 20}, cDim);
    }
    { Rect p = plusRect(); dg::line(s, p.x + 8, p.y + 14, p.x + 20, p.y + 14, cText, 2); dg::line(s, p.x + 14, p.y + 8, p.x + 14, p.y + 20, cText, 2); }
    // toolbar
    dg::fill(s, {0, kTabH, s.w, kBarH}, cBar);
    dg::fill(s, {0, kTop - 1, s.w, 1}, cLine);
    for (int i = 0; i < 4; i++) {
        Rect r = btnRect(i);
        bool en = i == 0 ? (at && at->back) : i == 1 ? (at && at->fwd) : true;
        if (r.has(g_mx, g_my) && en) dg::rrect(s, r, 8, cTabAct);
        drawIcon(s, i == 2 && at && at->loading ? 6 : i, r, en ? cText : cOff);
    }
    Rect a = addrRect();
    dg::rrect(s, a, 18, g_addrFocus ? cAccent : cLine);
    dg::rrect(s, a.inset(1), 17, cField);
    std::string shown = g_addrFocus ? g_addr : (at && !at->internal ? at->url : "");
    int tx = a.x + (g_findMode ? 72 : 16), maxw = a.w - 70 - (tx - a.x);
    if (g_findMode) text(s, a.x + 14, a.y + 10, "Найти:", cDim);
    if (shown.empty() && !g_addrFocus) text(s, tx, a.y + 10, "Введите запрос или адрес", cDim);
    else {
        std::string vis = shown;
        while (!vis.empty() && tw(vis) > maxw) vis = g_addrFocus ? utf8Shift(vis) : utf8Pop(vis);
        text(s, tx, a.y + 10, vis, cText);
        if (g_addrFocus && (g_plat->ticks() / 500) % 2 == 0) dg::fill(s, {tx + tw(vis) + 1, a.y + 8, 2, 20}, cText);
    }
    if (at && !g_findMode) {
        Rect sr = starRect();
        text(s, sr.x + 8, sr.y + 8, bookmarkIndex(at->url) >= 0 ? "*" : "+", bookmarkIndex(at->url) >= 0 ? cWarn : cDim);
    }
    { Rect r = dlBtnRect(); bool act = false; for (auto& d : g_downloads) if (!d.done && !d.canceled) act = true;
      if (r.has(g_mx, g_my)) dg::rrect(s, r, 8, cTabAct); drawIcon(s, 5, r, act ? cWarn : cText); }
    { Rect r = menuBtnRect(); if (r.has(g_mx, g_my) || g_menuOpen) dg::rrect(s, r, 8, cTabAct); drawIcon(s, 4, r, cText); }
    if (at && at->loading) {  // progress stripe under the toolbar
        int w = (int)((g_plat->ticks() / 4) % std::max(1, s.w));
        dg::fill(s, {0, kTop - 3, w, 2}, cAccent);
        g_redraw = true;
    }
    // menu
    if (g_menuOpen) {
        Rect m = menuRect();
        dg::shadow(s, m, 12, 12, 4, rgba(0, 0, 0, 120));
        dg::rrect(s, m, 12, cLine); dg::rrect(s, m.inset(1), 11, cBar);
        for (int i = 0; i < kMenuN; i++) {
            Rect it{m.x + 6, m.y + 6 + i * 34, m.w - 12, 32};
            if (it.has(g_mx, g_my)) dg::rrect(s, it, 8, cTabAct);
            std::string label = kMenu[i];
            if (i == 5) label += std::string(": ") + kEngines[g_engine].name;
            text(s, it.x + 12, it.y + 8, label, cText);
            text(s, it.x + it.w - 12 - tw(kMenuKeys[i]), it.y + 8, kMenuKeys[i], cDim);
            if (i == 4 || i == 1 || i == 8) dg::fill(s, {it.x + 8, it.y + 33, it.w - 16, 1}, cLine);
        }
    }
    // download / toast strip
    std::string msg;
    for (auto& d : g_downloads) if (!d.done && !d.canceled) {
        int pct = d.total > 0 ? (int)(d.got * 100 / d.total) : 0;
        msg = "Загрузка: " + d.name + "  " + (d.total > 0 ? std::to_string(pct) + "%" : sizeStr(d.got));
    }
    if (msg.empty() && g_plat->ticks() < g_toastUntil) msg = g_toast;
    if (!msg.empty()) {
        std::string fit = dg::fit_text(msg, s.w - 80);
        Rect r{20, s.h - 52, tw(fit) + 40, 34};
        dg::rrect(s, r, 17, cTabAct);
        text(s, r.x + 20, r.y + 9, fit, cText);
    }
    g_plat->present();
    g_redraw = false;
}

// ---------------------------------------------------------------- input forwarding
CefMouseEvent mouseEvent(const nl::PEvent& e, bool leftDown) {
    CefMouseEvent me;
    me.x = e.x; me.y = e.y - kTop;
    me.modifiers = (uint32_t)e.mods;
    if (leftDown) me.modifiers |= EVENTFLAG_LEFT_MOUSE_BUTTON;
    return me;
}
void sendKey(Tab* t, const nl::PEvent& e) {
    CefKeyEvent ke;
    ke.windows_key_code = e.vk;
    ke.native_key_code = e.vk;
    ke.modifiers = (uint32_t)e.mods;
    ke.is_system_key = false;
    ke.focus_on_editable_field = false;
    ke.character = ke.unmodified_character = 0;
    if (!e.down) { ke.type = KEYEVENT_KEYUP; t->br->GetHost()->SendKeyEvent(ke); return; }
    ke.type = KEYEVENT_RAWKEYDOWN;
    t->br->GetHost()->SendKeyEvent(ke);
    if (e.vk == 13 || e.vk == 9) {  // Enter / Tab also produce a character
        ke.type = KEYEVENT_CHAR; ke.character = ke.unmodified_character = e.vk == 13 ? '\r' : '\t';
        t->br->GetHost()->SendKeyEvent(ke);
    }
}
void sendText(Tab* t, const std::string& utf8) {
    std::u16string s = CefString(utf8).ToString16();
    for (char16_t ch : s) {
        CefKeyEvent ke;
        ke.type = KEYEVENT_CHAR; ke.windows_key_code = ch; ke.native_key_code = 0; ke.modifiers = 0;
        ke.character = ke.unmodified_character = ch; ke.is_system_key = false; ke.focus_on_editable_field = true;
        t->br->GetHost()->SendKeyEvent(ke);
    }
}

void focusAddr(bool find = false) {
    g_addrFocus = true; g_findMode = find;
    Tab* t = activeTab();
    g_addr = find ? "" : (t && !t->internal ? t->url : "");
    g_redraw = true;
}
void toggleBookmark() {
    Tab* t = activeTab();
    if (!t || t->internal) return;
    int i = bookmarkIndex(t->url);
    if (i >= 0) { g_bookmarks.erase(g_bookmarks.begin() + i); toast("Закладка удалена"); }
    else { g_bookmarks.push_back({t->url, t->title}); toast("Добавлено в закладки"); }
    saveBookmarks();
}
void zoomBy(double d) {
    Tab* t = activeTab();
    if (!t) return;
    t->zoom = std::max(-4.0, std::min(6.0, t->zoom + d));
    t->br->GetHost()->SetZoomLevel(t->zoom);
}
void runMenu(int i) {
    g_menuOpen = false;
    switch (i) {
        case 0: newTab(); break;
        case 1: if (fork() == 0) { execl("/proc/self/exe", "netline", "--private", (char*)nullptr); _exit(1); } break;
        case 2: openTab(historyPage()); break;
        case 3: openTab(downloadsPage()); break;
        case 4: openTab(bookmarksPage()); break;
        case 5: g_engine = (g_engine + 1) % 4; saveSettings(); toast(std::string("Поиск: ") + kEngines[g_engine].name); break;
        case 6: zoomBy(0.5); break;
        case 7: zoomBy(-0.5); break;
        case 8: focusAddr(true); break;
        case 9: openTab("data:text/html;charset=utf-8," + CefURIEncode(
                    std::string("<!doctype html><meta charset=utf-8><title>О Netline</title><style>") + kPageCss +
                    "</style><h1>Netline " NETLINE_VERSION "</h1><p>Браузер Difiw OS на движке Chromium (CEF).</p><p class=s style='max-width:none'>Chromium " +
                    std::to_string(CHROME_VERSION_MAJOR) + " · CEF " CEF_VERSION "</p>", false).ToString()); break;
        case 10: g_quit = true; break;
    }
    g_redraw = true;
}

// returns true if the key was a browser shortcut
bool shortcut(const nl::PEvent& e) {
    bool ctrl = e.mods & nl::MOD_CTRL, shift = e.mods & nl::MOD_SHIFT, alt = e.mods & nl::MOD_ALT;
    int k = e.vk;
    Tab* t = activeTab();
    if (ctrl && k == 'T' && !shift) { newTab(); return true; }
    if (ctrl && k == 'T' && shift) { if (!g_closed.empty()) { openTab(g_closed.back()); g_closed.pop_back(); } return true; }
    if (ctrl && k == 'W') { closeTab(g_active); return true; }
    if (ctrl && k == 'N' && shift) { runMenu(1); return true; }
    if (ctrl && k == 'L') { focusAddr(); return true; }
    if (alt && k == 'D') { focusAddr(); return true; }
    if (ctrl && k == 'F') { focusAddr(true); return true; }
    if (ctrl && k == 'H') { openTab(historyPage()); return true; }
    if (ctrl && k == 'J') { openTab(downloadsPage()); return true; }
    if (ctrl && k == 'B') { openTab(bookmarksPage()); return true; }
    if (ctrl && k == 'D') { toggleBookmark(); return true; }
    if (ctrl && k == 9) { setActive((g_active + (shift ? (int)g_tabs.size() - 1 : 1)) % (int)g_tabs.size()); return true; }
    if (ctrl && k == 0xBB) { zoomBy(0.5); return true; }
    if (ctrl && k == 0xBD) { zoomBy(-0.5); return true; }
    if (ctrl && k == '0') { if (t) { t->zoom = 0; t->br->GetHost()->SetZoomLevel(0); } return true; }
    if (ctrl && k >= '1' && k <= '8') { setActive(std::min<int>(k - '1', g_tabs.size() - 1)); return true; }
    if (t && (k == 0x74 || (ctrl && k == 'R'))) { if (shift) t->br->ReloadIgnoreCache(); else t->br->Reload(); return true; }
    if (t && alt && k == 37) { t->br->GoBack(); return true; }
    if (t && alt && k == 39) { t->br->GoForward(); return true; }
    if (t && alt && k == 36) { navigate("https://yandex.ru/"); return true; }
    if (t && k == 27 && t->loading && !g_addrFocus) { t->br->StopLoad(); return true; }
    if (!g_addrFocus && t && ctrl && k == 'C') { t->br->GetFocusedFrame()->Copy(); return true; }
    if (!g_addrFocus && t && ctrl && k == 'X') { t->br->GetFocusedFrame()->Cut(); return true; }
    if (!g_addrFocus && t && ctrl && k == 'A') { t->br->GetFocusedFrame()->SelectAll(); return true; }
    if (!g_addrFocus && t && ctrl && k == 'V') {
        std::string c = g_plat->clipboard();
        if (!c.empty()) sendText(t, c);  // paste from the platform clipboard (CEF has no clipboard of its own in headless mode)
        return true;
    }
    return false;
}

void handleAddrKey(const nl::PEvent& e) {
    int k = e.vk;
    bool ctrl = e.mods & nl::MOD_CTRL, shift = e.mods & nl::MOD_SHIFT;
    if (k == 13) {
        if (g_findMode) { if (Tab* t = activeTab()) t->br->GetHost()->Find(g_addr, !shift, false, true); }
        else { navigate(g_addr); g_addrFocus = false; }
    } else if (k == 27) {
        if (g_findMode) if (Tab* t = activeTab()) t->br->GetHost()->StopFinding(true);
        g_addrFocus = false; g_findMode = false;
    } else if (k == 8) g_addr = utf8Pop(g_addr);
    else if (ctrl && k == 'V') g_addr += g_plat->clipboard();
    else if (ctrl && k == 'A') g_addr.clear();
    else if (ctrl && k == 'C') g_plat->set_clipboard(g_addr);
    g_redraw = true;
}

bool g_leftDown = false;
void handleMouseDown(const nl::PEvent& e) {
    int x = e.x, y = e.y;
    if (g_menuOpen) {
        Rect m = menuRect();
        if (m.has(x, y)) { int i = (y - m.y - 6) / 34; if (i >= 0 && i < kMenuN) runMenu(i); }
        else g_menuOpen = false;
        g_redraw = true;
        return;
    }
    if (y < kTop) {
        g_addrFocus = false; g_findMode = false;
        Tab* t = activeTab();
        if (y < kTabH) {
            int n = (int)g_tabs.size();
            for (int i = 0; i < n; i++) {
                Rect r = tabRect(i, n);
                if (!r.has(x, y)) continue;
                if (e.button == 2 || x > r.x + r.w - 28) closeTab(i); else setActive(i);
                return;
            }
            if (plusRect().has(x, y)) newTab();
        } else if (btnRect(0).has(x, y) && t) t->br->GoBack();
        else if (btnRect(1).has(x, y) && t) t->br->GoForward();
        else if (btnRect(2).has(x, y) && t) { if (t->loading) t->br->StopLoad(); else t->br->Reload(); }
        else if (btnRect(3).has(x, y)) navigate("https://yandex.ru/");
        else if (menuBtnRect().has(x, y)) g_menuOpen = true;
        else if (dlBtnRect().has(x, y)) openTab(downloadsPage());
        else if (starRect().has(x, y)) toggleBookmark();
        else if (addrRect().has(x, y)) focusAddr();
        g_redraw = true;
        return;
    }
    g_addrFocus = false; g_findMode = false;
    if (Tab* t = activeTab()) {
        t->br->GetHost()->SetFocus(true);
        if (e.button == 1) g_leftDown = true;
        auto bt = e.button == 1 ? MBT_LEFT : e.button == 2 ? MBT_MIDDLE : MBT_RIGHT;
        t->br->GetHost()->SendMouseClickEvent(mouseEvent(e, g_leftDown), bt, false, e.clicks);
    }
}

void drainPending() {
    while (!g_pending.empty()) {
        std::string u = g_pending.front();
        g_pending.erase(g_pending.begin());
        openTab(u, g_wantActivate);
    }
}

bool writeBmp(const dg::Surface& s, const std::string& path) {  // 32-bit BMP, top-down (test hook)
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return false;
    uint32_t sz = 54 + s.w * s.h * 4, off = 54, hdr = 40;
    int32_t w = s.w, h = -s.h;
    uint16_t planes = 1, bpp = 32;
    fwrite("BM", 1, 2, f); fwrite(&sz, 4, 1, f); uint32_t z = 0; fwrite(&z, 4, 1, f); fwrite(&off, 4, 1, f);
    fwrite(&hdr, 4, 1, f); fwrite(&w, 4, 1, f); fwrite(&h, 4, 1, f); fwrite(&planes, 2, 1, f); fwrite(&bpp, 2, 1, f);
    for (int i = 0; i < 6; i++) fwrite(&z, 4, 1, f);
    for (int y = 0; y < s.h; y++) fwrite(s.px + (size_t)y * s.stride, 4, s.w, f);
    fclose(f);
    return true;
}

std::string findFont() {
    const char* d = getenv("DIFIW_DATADIR");
    std::string c[] = {std::string(d ? d : "/usr/share/difiw") + "/font.psf", g_exeDir + "/res/font.psf"};
    for (auto& p : c) if (access(p.c_str(), R_OK) == 0) return p;
    return c[0];
}

}  // namespace

// ---------------------------------------------------------------- main
int main(int argc, char** argv) {
    CefMainArgs args(argc, argv);
    CefRefPtr<NetApp> app = new NetApp;
    int ec = CefExecuteProcess(args, app, nullptr);
    if (ec >= 0) return ec;

    char exe[4096] = {0};
    ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    g_exeDir = n > 0 ? std::string(exe, n) : ".";
    g_exeDir = g_exeDir.substr(0, g_exeDir.rfind('/'));
    g_home = homeDir();
    g_dataDir = g_home + "/.netline";
    g_downloadDir = g_home + "/Downloads";
    std::vector<std::string> startUrls;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--private")) g_private = true;
        else if (argv[i][0] != '-') startUrls.push_back(resolveInput(argv[i]));
    }
    mkdirs(g_dataDir);
    loadSettings();
    loadBookmarks();

    if (!dg::load_font(findFont().c_str())) { fprintf(stderr, "netline: cannot load font %s\n", findFont().c_str()); return 1; }
    g_plat = nl::make_platform();
    if (!g_plat->init(g_viewW, g_viewH + kTop, g_private ? "Netline (приватный режим)" : "Netline")) {
        fprintf(stderr, "netline: cannot open a window\n");
        return 1;
    }
    {  // the approved Netline icon (res/netline.dicon: u32 w, u32 h, RGBA bytes)
        std::ifstream f(g_exeDir + "/res/netline.dicon", std::ios::binary);
        std::vector<char> d((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        if (d.size() >= 8 + 128 * 128 * 4) {
            dg::Image im; im.w = im.h = 128; im.px.resize(128 * 128);
            const unsigned char* p = (const unsigned char*)d.data() + 8;
            for (int i = 0; i < 128 * 128; i++) im.px[i] = rgba(p[i * 4], p[i * 4 + 1], p[i * 4 + 2], p[i * 4 + 3]);
            g_logoImg = dg::scale_cover(im, 26, 26);
            g_logoSurf.wrap(g_logoImg.px.data(), g_logoImg.w, g_logoImg.h);
        }
    }
    {
        dg::Surface& s = g_plat->surface();
        g_viewW = s.w; g_viewH = std::max(100, s.h - kTop);
    }

    CefSettings st;
    st.windowless_rendering_enabled = true;
    st.no_sandbox = getenv("NETLINE_NO_SANDBOX") != nullptr;  // sandbox stays on unless explicitly disabled for diagnostics
    st.multi_threaded_message_loop = false;
    CefString(&st.locale) = "ru";
    CefString(&st.accept_language_list) = "ru,en;q=0.8";
    std::string root = g_private ? "" : g_dataDir + "/profile";
    if (!g_private) { mkdirs(root); CefString(&st.root_cache_path) = root; CefString(&st.cache_path) = root; }
    else { CefString(&st.root_cache_path) = "/tmp/netline-private-" + std::to_string(getpid()); }
    CefString(&st.log_file) = g_dataDir + "/cef.log";
    st.log_severity = LOGSEVERITY_WARNING;
    if (!CefInitialize(args, st, app, nullptr)) { fprintf(stderr, "netline: CefInitialize failed\n"); return 1; }

    CefRequestContextSettings rs;
    if (!g_private) CefString(&rs.cache_path) = root;
    g_ctx = CefRequestContext::CreateContext(rs, nullptr);
    g_client = new NetClient;

    if (startUrls.empty()) newTab(); else for (auto& u : startUrls) if (!u.empty()) openTab(u);

    // test hook: NETLINE_SHOT="<file.bmp>:<ms>" saves the window as BMP after <ms> and prints the tab state (tests/netline/smoke.sh)
    std::string shotFile; uint32_t shotAt = 0;
    if (const char* sh = getenv("NETLINE_SHOT")) {
        std::string v = sh; size_t c = v.rfind(':');
        if (c != std::string::npos) { shotFile = v.substr(0, c); shotAt = (uint32_t)atoi(v.c_str() + c + 1); }
    }
    uint32_t lastRender = 0;
    while (!g_quit) {
        if (shotAt && g_plat->ticks() > shotAt) {
            render();
            writeBmp(g_plat->surface(), shotFile);
            for (auto& t : g_tabs) printf("TAB title=[%s] url=[%s] loading=%d\n", t->title.c_str(), t->url.c_str(), t->loading);
            fflush(stdout);
            shotAt = 0;
            if (getenv("NETLINE_EXIT_AFTER_SHOT")) g_quit = true;
        }
        nl::PEvent e;
        for (int guard = 0; guard < 256 && g_plat->wait(e, guard ? 0 : 4); guard++) {
            Tab* t = activeTab();
            switch (e.kind) {
                case nl::PEvent::Quit: g_quit = true; break;
                case nl::PEvent::Resize:
                    g_viewW = g_plat->surface().w; g_viewH = std::max(100, g_plat->surface().h - kTop);
                    for (auto& tb : g_tabs) tb->br->GetHost()->WasResized();
                    g_redraw = true;
                    break;
                case nl::PEvent::Expose: g_redraw = true; break;
                case nl::PEvent::Key:
                    g_mods = e.mods;
                    if (e.down && shortcut(e)) break;
                    if (g_addrFocus) { if (e.down) handleAddrKey(e); break; }
                    if (g_menuOpen && e.down && e.vk == 27) { g_menuOpen = false; g_redraw = true; break; }
                    if (t) sendKey(t, e);
                    break;
                case nl::PEvent::Text:
                    if (g_addrFocus) { g_addr += e.text; g_redraw = true; }
                    else if (t && !(e.mods & nl::MOD_CTRL)) sendText(t, e.text);
                    break;
                case nl::PEvent::MouseMove:
                    g_mx = e.x; g_my = e.y; g_mods = e.mods;
                    if (t && e.y >= kTop && !g_menuOpen) t->br->GetHost()->SendMouseMoveEvent(mouseEvent(e, g_leftDown), false);
                    g_redraw = true;
                    break;
                case nl::PEvent::MouseDown: g_mx = e.x; g_my = e.y; handleMouseDown(e); break;
                case nl::PEvent::MouseUp:
                    if (e.button == 1) g_leftDown = false;
                    if (t && e.y >= kTop && !g_menuOpen) {
                        auto bt = e.button == 1 ? MBT_LEFT : e.button == 2 ? MBT_MIDDLE : MBT_RIGHT;
                        t->br->GetHost()->SendMouseClickEvent(mouseEvent(e, false), bt, true, 1);
                    }
                    break;
                case nl::PEvent::Wheel:
                    if (t && e.y >= kTop && !g_menuOpen) {
                        if (e.mods & nl::MOD_CTRL) zoomBy(e.wheel > 0 ? 0.5 : -0.5);
                        else t->br->GetHost()->SendMouseWheelEvent(mouseEvent(e, g_leftDown), 0, e.wheel * 60);
                    }
                    break;
                default: break;
            }
        }
        CefDoMessageLoopWork();
        drainPending();
        uint32_t now = g_plat->ticks();
        if (g_redraw && now - lastRender >= 12) { render(); lastRender = now; }
        else if (now < g_toastUntil + 20 && now - lastRender > 250) g_redraw = true;
    }
    for (auto& t : g_tabs) if (t->br) t->br->GetHost()->CloseBrowser(true);
    for (int i = 0; i < 200 && !g_tabs.empty(); i++) { CefDoMessageLoopWork(); usleep(5000); }
    g_tabs.clear();
    g_client = nullptr; g_ctx = nullptr;
    CefShutdown();
    return 0;
}
