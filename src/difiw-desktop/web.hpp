// Netline page engine: a small HTML reader (no JavaScript, no CSS) that lays a page out as text, links and images.
// Header-only; included inside the anonymous namespace of apps.cpp (and of the host test tool).
// SPDX-License-Identifier: MIT
#pragma once

// ------------------------------------------------------------------- URLs
struct Url { std::string scheme, host, path; };  // path includes the query string

inline Url url_split(const std::string& u) {
    Url r;
    const std::size_t sc = u.find("://");
    if (sc == std::string::npos) { r.path = u; return r; }
    r.scheme = u.substr(0, sc);
    const std::size_t hs = sc + 3, pe = u.find_first_of("/?#", hs);
    r.host = u.substr(hs, pe == std::string::npos ? std::string::npos : pe - hs);
    std::string p = pe == std::string::npos ? "/" : u.substr(pe);
    const std::size_t h = p.find('#');
    if (h != std::string::npos) p.erase(h);
    if (p.empty() || p[0] != '/') p = "/" + p;
    r.path = p;
    return r;
}

inline std::string url_resolve(const std::string& base, std::string ref) {
    while (!ref.empty() && (ref.front() == ' ' || ref.front() == '\n' || ref.front() == '\t')) ref.erase(0, 1);
    while (!ref.empty() && (ref.back() == ' ' || ref.back() == '\n' || ref.back() == '\t')) ref.pop_back();
    if (ref.empty()) return base;
    if (ref.find("://") != std::string::npos && ref.find("://") < 12) return ref;
    if (ref.compare(0, 5, "data:") == 0 || ref.compare(0, 11, "javascript:") == 0 || ref.compare(0, 7, "mailto:") == 0) return ref;
    const Url b = url_split(base);
    if (ref.compare(0, 2, "//") == 0) return b.scheme + ":" + ref;
    if (ref[0] == '#') { std::string nb = base; const std::size_t h = nb.find('#'); if (h != std::string::npos) nb.erase(h); return nb + ref; }
    const std::string origin = b.scheme + "://" + b.host;
    if (ref[0] == '/') return origin + ref;
    if (ref[0] == '?') { const std::size_t q = b.path.find('?'); return origin + (q == std::string::npos ? b.path : b.path.substr(0, q)) + ref; }
    std::string dir = b.path;
    const std::size_t q = dir.find('?');
    if (q != std::string::npos) dir.erase(q);
    dir.erase(dir.rfind('/') + 1);
    std::string path = dir + ref;
    // collapse ./ and ../
    std::vector<std::string> parts;
    std::string seg;
    std::istringstream is(path);
    const std::size_t qq = path.find('?');
    std::string tail;
    if (qq != std::string::npos) { tail = path.substr(qq); path.erase(qq); is.str(path); is.clear(); }
    while (std::getline(is, seg, '/')) {
        if (seg == "..") { if (!parts.empty()) parts.pop_back(); }
        else if (seg != "." && !seg.empty()) parts.push_back(seg);
    }
    std::string out;
    for (const auto& p : parts) out += "/" + p;
    if (out.empty() || (!path.empty() && path.back() == '/')) out += "/";
    return origin + out + tail;
}

// Bing wraps result links in a tracking redirect (bing.com/ck/a?...&u=a1<base64url of the real address>) that only
// forwards with JavaScript; unwrap it so the link goes straight to the site.
inline std::string unwrap_tracking(const std::string& url) {
    if (url.find("bing.com/ck/a") == std::string::npos) return url;
    std::size_t p = url.find("&u=a1");
    if (p == std::string::npos) p = url.find("?u=a1");
    if (p == std::string::npos) return url;
    p += 5;
    std::string out;
    unsigned acc = 0;
    int bits = 0;
    for (; p < url.size() && url[p] != '&' && url[p] != '#'; ++p) {
        const char c = url[p];
        int v;
        if (c >= 'A' && c <= 'Z') v = c - 'A';
        else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
        else if (c >= '0' && c <= '9') v = c - '0' + 52;
        else if (c == '-' || c == '+') v = 62;
        else if (c == '_' || c == '/') v = 63;
        else continue;
        acc = (acc << 6) | static_cast<unsigned>(v);
        bits += 6;
        if (bits >= 8) { bits -= 8; out += static_cast<char>((acc >> bits) & 0xFF); }
    }
    if (out.compare(0, 4, "http") == 0) return out;
    if (!out.empty() && out[0] == '/') return "https://www.bing.com" + out;
    return url;
}

inline std::string url_decode(const std::string& s) {
    std::string o;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size() && std::isxdigit(static_cast<unsigned char>(s[i + 1])) && std::isxdigit(static_cast<unsigned char>(s[i + 2]))) {
            o += static_cast<char>(std::stoi(s.substr(i + 1, 2), nullptr, 16));
            i += 2;
        } else if (s[i] == '+') o += ' ';
        else o += s[i];
    }
    return o;
}

inline std::string url_encode(const std::string& s) {
    std::string o;
    char b[4];
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') o += static_cast<char>(c);
        else if (c == ' ') o += '+';
        else { std::snprintf(b, sizeof b, "%%%02X", c); o += b; }
    }
    return o;
}

// Search engines wrap result links in redirects; the real address is inside.
inline std::string url_unwrap(const std::string& u) {
    auto param = [&](const char* key) -> std::string {
        const std::size_t q = u.find('?');
        if (q == std::string::npos) return "";
        std::size_t i = q + 1;
        const std::string k = std::string(key) + "=";
        while (i < u.size()) {
            std::size_t e = u.find('&', i);
            if (e == std::string::npos) e = u.size();
            if (u.compare(i, k.size(), k) == 0) return url_decode(u.substr(i + k.size(), e - i - k.size()));
            i = e + 1;
        }
        return "";
    };
    const Url p = url_split(u);
    if (p.host.find("google.") != std::string::npos && p.path.compare(0, 5, "/url?") == 0) {
        const std::string t = param("q");
        if (t.compare(0, 4, "http") == 0) return t;
    }
    if (p.host.find("duckduckgo.com") != std::string::npos && p.path.compare(0, 3, "/l/") == 0) {
        const std::string t = param("uddg");
        if (t.compare(0, 4, "http") == 0) return t;
    }
    return u;
}

// ------------------------------------------------------------------- text helpers
inline void utf8_put(std::string& o, std::uint32_t cp) {
    if (cp < 0x80) o += static_cast<char>(cp);
    else if (cp < 0x800) { o += static_cast<char>(0xC0 | (cp >> 6)); o += static_cast<char>(0x80 | (cp & 0x3F)); }
    else if (cp < 0x10000) { o += static_cast<char>(0xE0 | (cp >> 12)); o += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); o += static_cast<char>(0x80 | (cp & 0x3F)); }
    else { o += static_cast<char>(0xF0 | (cp >> 18)); o += static_cast<char>(0x80 | ((cp >> 12) & 0x3F)); o += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); o += static_cast<char>(0x80 | (cp & 0x3F)); }
}

// Characters the console font does not have are replaced by plain look-alikes.
inline std::uint32_t simplify_cp(std::uint32_t cp) {
    switch (cp) {
        case 0xA0: case 0x2002: case 0x2003: case 0x2009: case 0x200A: case 0x202F: return ' ';
        case 0x200B: case 0x200C: case 0x200D: case 0xFEFF: case 0xAD: return 0;
        case 0x2018: case 0x2019: case 0x201A: case 0x2032: return '\'';
        case 0x201C: case 0x201D: case 0x201E: case 0x2033: return '"';
        case 0x2010: case 0x2011: case 0x2012: case 0x2013: case 0x2014: case 0x2212: return '-';
        case 0x2022: case 0x25CF: case 0x25AA: case 0x00B7: return '*';
        case 0x2192: return '>';
        case 0x2190: return '<';
        case 0x2026: return '.';
        case 0x2122: return 'T';
        case 0x142: return 'l'; case 0x141: return 'L'; case 0x15B: case 0x161: case 0x15F: return 's'; case 0x15A: case 0x160: return 'S';
        case 0x144: return 'n'; case 0x107: case 0x10D: return 'c'; case 0x17A: case 0x17C: case 0x17E: return 'z'; case 0x119: return 'e'; case 0x105: return 'a';
        case 0x151: return 'o'; case 0x171: return 'u'; case 0x11F: return 'g'; case 0x131: return 'i';
        case 0x00A9: return 'c';
        default:
            if (cp >= 0xC0 && cp <= 0xFF) {  // Latin-1 letters with accents -> plain letters
                static const char* const a = "AAAAAAACEEEEIIIIDNOOOOO*OUUUUYTsaaaaaaaceeeeiiiidnooooo/ouuuuyty";
                return static_cast<std::uint32_t>(static_cast<unsigned char>(a[cp - 0xC0]));
            }
            return cp;
    }
}

inline std::string decode_entities(const std::string& in) {
    std::string o;
    for (std::size_t i = 0; i < in.size(); ++i) {
        if (in[i] != '&') { o += in[i]; continue; }
        const std::size_t e = in.find(';', i);
        if (e == std::string::npos || e - i > 10) { o += '&'; continue; }
        const std::string n = in.substr(i + 1, e - i - 1);
        std::uint32_t cp = 0;
        if (!n.empty() && n[0] == '#') {
            cp = n.size() > 1 && (n[1] == 'x' || n[1] == 'X') ? static_cast<std::uint32_t>(std::strtoul(n.c_str() + 2, nullptr, 16)) : static_cast<std::uint32_t>(std::strtoul(n.c_str() + 1, nullptr, 10));
        } else if (n == "amp") cp = '&';
        else if (n == "lt") cp = '<';
        else if (n == "gt") cp = '>';
        else if (n == "quot") cp = '"';
        else if (n == "apos") cp = '\'';
        else if (n == "nbsp") cp = ' ';
        else if (n == "copy") cp = 0xA9;
        else if (n == "mdash" || n == "ndash") cp = '-';
        else if (n == "laquo") cp = 0xAB;
        else if (n == "raquo") cp = 0xBB;
        else if (n == "hellip") cp = '.';
        else if (n == "bull" || n == "middot") cp = '*';
        else if (n == "rarr") cp = '>';
        else if (n == "larr") cp = '<';
        else if (n == "times") cp = 'x';
        else if (n == "trade") cp = 'T';
        else { o += '&'; continue; }
        if (cp == 0) { i = e; continue; }
        utf8_put(o, cp);
        i = e;
    }
    return o;
}

inline std::string simplify_str(const std::string& t) {
    std::string m;
    for (std::size_t i = 0; i < t.size();) {
        const std::uint32_t cp = simplify_cp(u8_codepoint(t, i));
        if (cp) utf8_put(m, cp);
    }
    return m;
}

// Converts a page body to UTF-8 when it is declared (or detected) as windows-1251 / latin-1.
inline std::string to_utf8(const std::string& body, std::string charset) {
    for (auto& c : charset) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    std::string o;
    if (charset == "windows-1251" || charset == "cp1251" || charset == "win-1251") {
        for (unsigned char c : body) {
            if (c < 0x80) o += static_cast<char>(c);
            else if (c >= 0xC0) utf8_put(o, 0x410 + (c - 0xC0));
            else if (c == 0xA8) utf8_put(o, 0x401);
            else if (c == 0xB8) utf8_put(o, 0x451);
            else if (c == 0x96 || c == 0x97) o += '-';
            else if (c == 0xAB) utf8_put(o, 0xAB);
            else if (c == 0xBB) utf8_put(o, 0xBB);
            else o += '?';
        }
        return o;
    }
    if (charset == "iso-8859-1" || charset == "latin1" || charset == "windows-1252") {
        for (unsigned char c : body) utf8_put(o, c);
        return o;
    }
    return body;
}

// ------------------------------------------------------------------- document model
struct Pic { int w = 0, h = 0; std::vector<u32> px; };

struct WItem {
    enum Kind { Text, Box, Img } kind = Text;
    int x = 0, y = 0, w = 0, h = 0;
    std::string s;
    u32 color = 0xFF000000u;
    int scale = 1;
    bool bold = false, under = false;
    int link = -1, img = -1, field = -1;
};

struct ImgRef { std::string src, alt; int aw = 0, ah = 0; };

// Form controls: the page keeps what the user typed; submitting builds the request from these.
struct Field { int form = -1; std::string name, type, value, placeholder; bool checked = false; int size = 20; };
struct Form { std::string action, method; };

struct Doc {
    std::string title, base;
    std::vector<WItem> items;
    std::vector<std::string> links;
    std::vector<ImgRef> imgs;
    std::vector<Field> fields;
    std::vector<Form> forms;
    std::vector<std::string> css_urls;
    int autofocus = -1;
    int height = 0;
};

// ------------------------------------------------------------------- minimal CSS
// Only what changes whether a page is readable: display:none / visibility:hidden, color, bold, centered text.
// Selectors: tag, .class, #id and combinations of those (no descendants, pseudo classes or media queries).
struct Props { bool hide = false, unhide = false, bold = false, center = false, has_color = false; u32 color = 0; };

struct CssRule { std::string tag, id; std::vector<std::string> cls; Props p; };

struct Sheet {
    std::vector<CssRule> rules;
    std::map<std::string, std::vector<int>> by_tag, by_id, by_cls;
};

inline std::string css_trim(std::string s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.erase(0, 1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
    return s;
}

inline std::string css_lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

inline bool css_color(const std::string& v, u32& out) {
    std::string s = css_lower(css_trim(v));
    if (s.compare(0, 1, "#") == 0 && (s.size() == 7 || s.size() == 4)) {
        std::string h = s.substr(1);
        if (h.size() == 3) h = std::string(2, h[0]) + std::string(2, h[1]) + std::string(2, h[2]);
        char* e = nullptr;
        const unsigned long n = std::strtoul(h.c_str(), &e, 16);
        if (e && *e == 0) { out = 0xFF000000u | static_cast<u32>(n); return true; }
        return false;
    }
    if (s.compare(0, 4, "rgb(") == 0 || s.compare(0, 5, "rgba(") == 0) {
        int v[3] = {0, 0, 0}, k = 0;
        std::size_t i = s.find('(') + 1;
        while (i < s.size() && k < 3) {
            v[k++] = std::atoi(s.c_str() + i);
            while (i < s.size() && s[i] != ',' && s[i] != ' ') ++i;
            while (i < s.size() && (s[i] == ',' || s[i] == ' ')) ++i;
        }
        out = rgba(clampi(v[0], 0, 255), clampi(v[1], 0, 255), clampi(v[2], 0, 255));
        return k == 3;
    }
    static const struct { const char* n; u32 c; } names[] = {{"black", 0xFF000000u}, {"white", 0xFFFFFFFFu}, {"red", 0xFFCC0000u}, {"green", 0xFF008000u}, {"blue", 0xFF0000CCu},
                                                             {"gray", 0xFF808080u}, {"grey", 0xFF808080u}, {"navy", 0xFF000080u}, {"maroon", 0xFF800000u}, {"orange", 0xFFCC7000u}};
    for (const auto& n : names) if (s == n.n) { out = n.c; return true; }
    return false;
}

inline void css_decls(const std::string& body, Props& p) {
    std::size_t i = 0;
    while (i < body.size()) {
        std::size_t e = body.find(';', i);
        if (e == std::string::npos) e = body.size();
        const std::string d = body.substr(i, e - i);
        i = e + 1;
        const std::size_t c = d.find(':');
        if (c == std::string::npos) continue;
        const std::string k = css_lower(css_trim(d.substr(0, c)));
        std::string v = css_lower(css_trim(d.substr(c + 1)));
        const std::size_t imp = v.find("!important");
        if (imp != std::string::npos) v = css_trim(v.substr(0, imp));
        if (k == "display") { if (v == "none") { p.hide = true; p.unhide = false; } else { p.unhide = true; p.hide = false; } }
        else if (k == "visibility" && v == "hidden") p.hide = true;
        else if (k == "font-weight") p.bold = v == "bold" || v == "bolder" || std::atoi(v.c_str()) >= 600;
        else if (k == "text-align") p.center = v == "center";
        else if (k == "color") {
            u32 col = 0;
            if (css_color(v, col)) {
                // very light text was designed for a dark background that is not drawn: keep the default colour
                const int lum = (cr(col) * 3 + cg(col) * 6 + cb(col)) / 10;
                if (lum < 170) { p.has_color = true; p.color = col; }
            }
        }
    }
}

inline void css_parse(const std::string& in, Sheet& sh) {
    std::string css;
    for (std::size_t i = 0; i < in.size(); ++i) {  // strip comments
        if (in.compare(i, 2, "/*") == 0) { const std::size_t e = in.find("*/", i + 2); if (e == std::string::npos) break; i = e + 1; }
        else css += in[i];
    }
    std::size_t i = 0;
    while (i < css.size() && sh.rules.size() < 20000) {
        while (i < css.size() && std::isspace(static_cast<unsigned char>(css[i]))) ++i;
        if (i >= css.size()) break;
        if (css[i] == '@') {  // @media, @font-face, @import ... : skipped completely
            std::size_t j = i;
            while (j < css.size() && css[j] != '{' && css[j] != ';') ++j;
            if (j < css.size() && css[j] == '{') { int depth = 0; while (j < css.size()) { if (css[j] == '{') ++depth; else if (css[j] == '}' && --depth == 0) break; ++j; } }
            i = j + 1;
            continue;
        }
        const std::size_t ob = css.find('{', i);
        if (ob == std::string::npos) break;
        const std::size_t cb2 = css.find('}', ob);
        if (cb2 == std::string::npos) break;
        const std::string sel = css.substr(i, ob - i);
        Props p;
        css_decls(css.substr(ob + 1, cb2 - ob - 1), p);
        i = cb2 + 1;
        if (!p.hide && !p.unhide && !p.bold && !p.center && !p.has_color) continue;
        std::size_t s = 0;
        while (s <= sel.size()) {
            std::size_t e = sel.find(',', s);
            if (e == std::string::npos) e = sel.size();
            const std::string one = css_trim(sel.substr(s, e - s));
            s = e + 1;
            if (one.empty() || one.find_first_of(" >+~:[*()") != std::string::npos) continue;
            CssRule r;
            r.p = p;
            std::size_t k = 0;
            auto ident = [&]() { std::size_t b = k; while (k < one.size() && one[k] != '.' && one[k] != '#') ++k; return css_lower(one.substr(b, k - b)); };
            if (one[0] != '.' && one[0] != '#') r.tag = ident();
            bool ok = true;
            while (k < one.size() && ok) {
                const char t = one[k++];
                const std::string name = ident();
                if (name.empty()) ok = false;
                else if (t == '.') r.cls.push_back(name);
                else if (t == '#') r.id = name;
                else ok = false;
            }
            if (!ok) continue;
            const int idx = static_cast<int>(sh.rules.size());
            sh.rules.push_back(r);
            if (!r.id.empty()) sh.by_id[r.id].push_back(idx);
            else if (!r.cls.empty()) sh.by_cls[r.cls[0]].push_back(idx);
            else sh.by_tag[r.tag].push_back(idx);
        }
    }
}

inline Props css_match(const Sheet& sh, const std::string& tag, const std::string& id, const std::vector<std::string>& cls) {
    Props out;
    if (sh.rules.empty()) return out;
    std::vector<int> cand;
    auto add = [&](const std::map<std::string, std::vector<int>>& m, const std::string& k) { auto it = m.find(k); if (it != m.end()) cand.insert(cand.end(), it->second.begin(), it->second.end()); };
    add(sh.by_tag, tag);
    if (!id.empty()) add(sh.by_id, id);
    for (const auto& c : cls) add(sh.by_cls, c);
    std::sort(cand.begin(), cand.end());
    for (int ri : cand) {
        const CssRule& r = sh.rules[static_cast<std::size_t>(ri)];
        if (!r.tag.empty() && r.tag != tag) continue;
        if (!r.id.empty() && r.id != id) continue;
        bool all = true;
        for (const auto& c : r.cls) if (std::find(cls.begin(), cls.end(), c) == cls.end()) all = false;
        if (!all) continue;
        if (r.p.hide) { out.hide = true; out.unhide = false; }
        if (r.p.unhide) { out.unhide = true; out.hide = false; }
        if (r.p.bold) out.bold = true;
        if (r.p.center) out.center = true;
        if (r.p.has_color) { out.has_color = true; out.color = r.p.color; }
    }
    return out;
}

// ------------------------------------------------------------------- layout
class Layouter {
public:
    Layouter(Doc& d, int width, const std::function<bool(int, int&, int&)>& isz, const Sheet* sheet = nullptr) : d_(d), W_(width), isz_(isz) { indent_ = 0; if (sheet) sheet_ = *sheet; }

    void run(const std::string& html);

private:
    struct Style { u32 color = 0xFF111111u; int scale = 1; bool bold = false, under = false; int link = -1; };
    struct Atom { WItem it; int w = 0, h = 0; bool space_before = false; };
    struct Frame { std::string tag; Style prev; int indent_add = 0; bool pre_prev = false; bool center_prev = false; };

    Doc& d_;
    int W_;
    std::function<bool(int, int&, int&)> isz_;
    Style st_;
    std::vector<Frame> stack_;
    std::vector<Atom> line_;
    int y_ = 8, indent_ = 0, pending_margin_ = 0, line_w_ = 0;
    bool pre_ = false, center_ = false, space_ = false, at_top_ = true;
    int list_n_ = 0;
    std::vector<int> list_counters_;
    Sheet sheet_;
    Props cur_;
    std::string style_buf_;
    int cur_form_ = -1;

    int left() const { return 12 + indent_; }
    int right() const { return W_ - 20; }
    int fw() const { return font_w(); }
    int fh() const { return font_h(); }

    void flush();
    void margin(int m) { flush(); if (m > pending_margin_) pending_margin_ = m; }
    void begin_line_y() { if (line_.empty()) { if (!at_top_) y_ += pending_margin_; pending_margin_ = 0; } }
    void put_atom(Atom a);
    void add_text(const std::string& raw);
    void add_word(const std::string& w);
    void add_box(int w, int h, u32 fill_color, const std::string& label, u32 label_color);
    void push(const std::string& tag, int indent_add = 0) {
        Frame f;
        f.tag = tag; f.prev = st_; f.indent_add = indent_add; f.pre_prev = pre_; f.center_prev = center_;
        stack_.push_back(f);
        indent_ += indent_add;
        if (cur_.bold) st_.bold = true;
        if (cur_.center) center_ = true;
        if (cur_.has_color && st_.link < 0) st_.color = cur_.color;
    }
    void pop(const std::string& tag) {
        for (std::size_t i = stack_.size(); i-- > 0;) {
            if (stack_[i].tag != tag) continue;
            while (stack_.size() > i) {
                const Frame f = stack_.back();
                stack_.pop_back();
                st_ = f.prev; indent_ -= f.indent_add; pre_ = f.pre_prev; center_ = f.center_prev;
            }
            return;
        }
    }
    static std::string attr(const std::map<std::string, std::string>& a, const char* k) { auto it = a.find(k); return it == a.end() ? "" : it->second; }
    static bool is_block(const std::string& t) {
        static const char* const b[] = {"p", "div", "section", "article", "header", "footer", "nav", "main", "aside", "form", "fieldset", "address", "figure", "figcaption",
                                        "table", "tr", "thead", "tbody", "tfoot", "dl", "dt", "dd", "details", "summary", "center", "h1", "h2", "h3", "h4", "h5", "h6", "ul", "ol", "li", "blockquote", "pre", "hr", "caption", "body", "html", "legend"};
        for (const char* x : b) if (t == x) return true;
        return false;
    }
};

inline void Layouter::flush() {
    if (line_.empty()) return;
    int lw = 0, lh = 0;
    for (const auto& a : line_) { lw += a.w + (a.space_before ? fw() * st_.scale : 0); lh = std::max(lh, a.h); }
    // spaces are accounted per atom (see put_atom): recompute exactly
    lw = 0;
    for (const auto& a : line_) lw += a.w + (a.space_before ? a.it.scale * fw() : 0);
    int x = left();
    if (center_) x = left() + std::max(0, (right() - left() - lw) / 2);
    const int y0 = y_;
    for (auto& a : line_) {
        if (a.space_before) x += a.it.scale * fw();
        a.it.x = x;
        a.it.y = y0 + (lh - a.h);
        d_.items.push_back(a.it);
        x += a.w;
    }
    y_ += lh + 2;
    line_.clear();
    line_w_ = 0;
    space_ = false;
    at_top_ = false;
}

inline void Layouter::put_atom(Atom a) {
    begin_line_y();
    const int sp = (a.space_before ? a.it.scale * fw() : 0);
    if (!line_.empty() && left() + line_w_ + sp + a.w > right()) { flush(); begin_line_y(); a.space_before = false; }
    if (line_.empty()) a.space_before = false;
    line_w_ += a.w + (a.space_before ? a.it.scale * fw() : 0);
    line_.push_back(a);
}

inline void Layouter::add_word(const std::string& w) {
    if (w.empty()) return;
    const int sc = st_.scale;
    const int maxcols = std::max(4, (right() - left()) / (fw() * sc));
    std::string rest = w;
    while (!rest.empty()) {
        std::size_t cut = 0;
        int n = 0;
        while (cut < rest.size() && n < maxcols) { cut = u8_next(rest, cut); ++n; }
        Atom a;
        a.it.kind = WItem::Text;
        a.it.s = rest.substr(0, cut);
        a.it.color = st_.color; a.it.scale = sc; a.it.bold = st_.bold; a.it.under = st_.under; a.it.link = st_.link;
        a.w = n * fw() * sc;
        a.h = fh() * sc;
        a.space_before = space_;
        put_atom(a);
        space_ = false;
        rest.erase(0, cut);
        if (!rest.empty()) flush();
    }
}

inline void Layouter::add_text(const std::string& raw) {
    if (raw.empty()) return;
    std::string t = decode_entities(raw);
    // map characters the font lacks, drop zero-width ones
    std::string m;
    for (std::size_t i = 0; i < t.size();) {
        std::uint32_t cp = u8_codepoint(t, i);
        cp = simplify_cp(cp);
        if (cp) utf8_put(m, cp);
    }
    if (pre_) {
        std::string cur;
        for (char c : m) {
            if (c == '\n') { add_word(cur); cur.clear(); flush(); }
            else if (c == '\t') cur += "    ";
            else if (c != '\r') cur += c;
        }
        add_word(cur);
        return;
    }
    std::string word;
    for (char c : m) {
        if (c == ' ' || c == '\n' || c == '\t' || c == '\r') {
            if (!word.empty()) { add_word(word); word.clear(); }
            if (!line_.empty()) space_ = true;
        } else word += c;
    }
    if (!word.empty()) add_word(word);
}

inline void Layouter::add_box(int w, int h, u32 fill_color, const std::string& label, u32 label_color) {
    Atom a;
    a.it.kind = WItem::Box;
    a.it.color = fill_color;
    a.it.s = label;
    a.it.bold = false;
    a.it.scale = 1;
    a.it.w = w; a.it.h = h;
    a.it.img = static_cast<int>(label_color);  // label colour travels in the otherwise unused img field
    a.it.link = st_.link;
    a.w = w; a.h = h;
    a.space_before = space_;
    put_atom(a);
    space_ = true;
}

inline void Layouter::run(const std::string& html) {
    std::size_t i = 0;
    const std::size_t n = html.size();
    bool in_head = false, in_title = false;
    std::string skip_tag;  // skipping until this closing tag
    int skip_depth = 0;
    std::string title_buf;
    while (i < n) {
        if (html[i] != '<') {
            const std::size_t e = html.find('<', i);
            const std::string text = html.substr(i, e == std::string::npos ? std::string::npos : e - i);
            i = e == std::string::npos ? n : e;
            if (!skip_tag.empty()) { if (skip_tag == "style") style_buf_ += text; continue; }
            if (in_title) { title_buf += text; continue; }
            if (in_head) continue;
            add_text(text);
            continue;
        }
        if (html.compare(i, 4, "<!--") == 0) { const std::size_t e = html.find("-->", i + 4); i = e == std::string::npos ? n : e + 3; continue; }
        if (i + 1 < n && (html[i + 1] == '!' || html[i + 1] == '?')) { const std::size_t e = html.find('>', i); i = e == std::string::npos ? n : e + 1; continue; }
        // a tag
        std::size_t e = i + 1;
        char q = 0;
        while (e < n && (q || html[e] != '>')) { if (q) { if (html[e] == q) q = 0; } else if (html[e] == '"' || html[e] == '\'') q = html[e]; ++e; }
        std::string tag = html.substr(i + 1, e - i - 1);
        i = e < n ? e + 1 : n;
        if (tag.empty()) continue;
        const bool closing = tag[0] == '/';
        if (closing) tag.erase(0, 1);
        std::size_t ne = 0;
        while (ne < tag.size() && !std::isspace(static_cast<unsigned char>(tag[ne])) && tag[ne] != '/') ++ne;
        std::string name = tag.substr(0, ne);
        for (auto& c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        std::map<std::string, std::string> at;
        if (!closing) {
            std::size_t p = ne;
            while (p < tag.size()) {
                while (p < tag.size() && (std::isspace(static_cast<unsigned char>(tag[p])) || tag[p] == '/')) ++p;
                std::size_t ks = p;
                while (p < tag.size() && !std::isspace(static_cast<unsigned char>(tag[p])) && tag[p] != '=' && tag[p] != '/') ++p;
                std::string k = tag.substr(ks, p - ks);
                for (auto& c : k) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                while (p < tag.size() && std::isspace(static_cast<unsigned char>(tag[p]))) ++p;
                std::string v;
                if (p < tag.size() && tag[p] == '=') {
                    ++p;
                    while (p < tag.size() && std::isspace(static_cast<unsigned char>(tag[p]))) ++p;
                    if (p < tag.size() && (tag[p] == '"' || tag[p] == '\'')) {
                        const char qc = tag[p++];
                        const std::size_t ve = tag.find(qc, p);
                        v = tag.substr(p, ve == std::string::npos ? std::string::npos : ve - p);
                        p = ve == std::string::npos ? tag.size() : ve + 1;
                    } else {
                        const std::size_t vs = p;
                        while (p < tag.size() && !std::isspace(static_cast<unsigned char>(tag[p]))) ++p;
                        v = tag.substr(vs, p - vs);
                    }
                }
                if (!k.empty()) at[k] = v;
            }
        }
        // ---- skipping subtrees (script, style, hidden elements)
        if (!skip_tag.empty()) {
            if (name == skip_tag) { if (closing) { if (--skip_depth <= 0) { if (skip_tag == "style") { css_parse(style_buf_, sheet_); style_buf_.clear(); } skip_tag.clear(); } } else if (tag.empty() || tag.back() != '/') ++skip_depth; }
            continue;
        }
        if (!closing) {
            std::string style = attr(at, "style");
            for (auto& c : style) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            style.erase(std::remove(style.begin(), style.end(), ' '), style.end());
            std::vector<std::string> classes;
            { std::istringstream cs(css_lower(attr(at, "class"))); std::string c1; while (cs >> c1) classes.push_back(c1); }
            cur_ = css_match(sheet_, name, css_lower(attr(at, "id")), classes);
            { Props ip; css_decls(attr(at, "style"), ip); if (ip.hide) cur_.hide = true; if (ip.bold) cur_.bold = true; if (ip.center) cur_.center = true; if (ip.has_color) { cur_.has_color = true; cur_.color = ip.color; } if (ip.unhide) cur_.hide = false; }
            const bool hidden = at.count("hidden") || (cur_.hide && !cur_.unhide) || style.find("visibility:hidden") != std::string::npos;
            static const char* const voids[] = {"br", "hr", "img", "input", "meta", "link", "base", "area", "col", "wbr", "source", "param"};
            bool is_void = false;
            for (const char* v : voids) if (name == v) is_void = true;
            if (name == "script" || name == "style" || name == "svg" || name == "template" || name == "noframes" || name == "iframe" || name == "object" || name == "select" || (hidden && !is_void)) {
                skip_tag = name; skip_depth = 1;
                if (name == "select") add_box(fw() * 9, fh() + 6, 0xFFF0F0F0u, "v", 0xFF555555u);
                continue;
            }
            if (hidden) continue;
            // page furniture that would push the real content far down without CSS (menus, jump links, cookie banners)
            if (name != "html" && name != "body" && !is_void) {
                std::string cls = attr(at, "class") + " " + attr(at, "id") + " ";
                for (auto& c : cls) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                static const char* const junk[] = {"mw-jump-link", "vector-main-menu", "vector-main-menu-container", "vector-user-links", "vector-sticky-header", "vector-page-toolbar",
                                                   "vector-column-start", "mw-editsection", "navbox", "noprint", "mw-navigation", "mw-panel", "cookie-banner", "cookie-consent", "sidebar",
                                                   "vector-header-container", "vector-dropdown", "mw-indicators", "b_skip_to_content", "b_a11y_feedback", "badbrowserheader"};
                bool skip = name == "nav";
                for (const char* j : junk) {
                    const std::string w = std::string(" ") + j + " ";
                    if ((" " + cls).find(w) != std::string::npos) skip = true;
                }
                if (skip) { skip_tag = name; skip_depth = 1; continue; }
            }
        } else if (name == "script" || name == "style") continue;

        if (name == "head") { in_head = !closing; continue; }
        if (name == "body") { in_head = false; if (!closing) continue; }
        if (name == "title") { in_title = !closing; if (closing && d_.title.empty()) { d_.title = simplify_str(decode_entities(title_buf)); while (!d_.title.empty() && std::isspace(static_cast<unsigned char>(d_.title.back()))) d_.title.pop_back(); while (!d_.title.empty() && std::isspace(static_cast<unsigned char>(d_.title.front()))) d_.title.erase(0, 1); } continue; }
        if (name == "link" && !closing) {
            const std::string rel = css_lower(attr(at, "rel")), href = attr(at, "href");
            if (rel.find("stylesheet") != std::string::npos && !href.empty() && d_.css_urls.size() < 8) {
                const std::string media = css_lower(attr(at, "media"));
                if (media.empty() || media.find("screen") != std::string::npos || media == "all") d_.css_urls.push_back(url_resolve(d_.base, decode_entities(href)));
            }
            continue;
        }
        if (in_head) { if (name == "base" && !closing && !attr(at, "href").empty()) d_.base = url_resolve(d_.base, attr(at, "href")); continue; }

        if (closing) {
            if (name == "form") cur_form_ = -1;
            if (name == "a" || name == "b" || name == "strong" || name == "i" || name == "em" || name == "u" || name == "span" || name == "font" || name == "code" || name == "tt" || name == "small" || name == "big" || name == "cite" || name == "label" || name == "button" || name == "mark" || name == "sup" || name == "sub") pop(name);
            else if (name == "td" || name == "th") { space_ = !line_.empty(); if (name == "th") pop("th"); }
            else if (is_block(name)) {
                flush();
                if (name == "ul" || name == "ol") { if (!list_counters_.empty()) list_counters_.pop_back(); margin(name == "ul" || name == "ol" ? (list_counters_.empty() ? 6 : 0) : 0); }
                else if (name == "p" || name == "blockquote" || name == "pre" || name[0] == 'h' || name == "table" || name == "form") margin(name[0] == 'h' ? 8 : 8);
                pop(name);
            }
            continue;
        }
        // ---- opening tags
        if (name == "br") { if (line_.empty()) { y_ += fh(); at_top_ = false; } else flush(); continue; }
        if (name == "hr") {
            margin(6);
            begin_line_y();
            WItem it; it.kind = WItem::Box; it.x = left(); it.y = y_ + 4; it.w = right() - left(); it.h = 1; it.color = 0xFFB0B0B0u;
            d_.items.push_back(it);
            y_ += 10; at_top_ = false;
            continue;
        }
        if (name == "img") {
            const std::string src = attr(at, "src");
            if (src.empty() || src.compare(0, 5, "data:") == 0) { const std::string alt = attr(at, "alt"); if (!alt.empty() && at.count("src")) { Style keep = st_; st_.color = 0xFF777777u; add_text("[" + alt + "]"); st_ = keep; } continue; }
            ImgRef r;
            r.src = url_resolve(d_.base, src);
            r.alt = decode_entities(attr(at, "alt"));
            r.aw = std::atoi(attr(at, "width").c_str());
            r.ah = std::atoi(attr(at, "height").c_str());
            if ((r.aw > 0 && r.aw <= 3) || (r.ah > 0 && r.ah <= 3)) continue;  // tracking pixels
            if (d_.imgs.size() >= 40) continue;
            d_.imgs.push_back(r);
            const int idx = static_cast<int>(d_.imgs.size()) - 1;
            int nw = 0, nh = 0;
            const int maxw = right() - left();
            Atom a;
            a.it.kind = WItem::Img;
            a.it.img = idx; a.it.link = st_.link;
            if (isz_ && isz_(idx, nw, nh) && nw > 0 && nh > 0) {
                int w = r.aw > 0 ? std::min(r.aw, nw) : nw;
                w = std::min(w, maxw);
                int h = nh * w / nw;
                if (h > 700) { h = 700; w = nw * h / nh; }
                a.w = w; a.h = h;
            } else if (r.aw > 0 && r.ah > 0) {
                a.w = std::min(r.aw, maxw); a.h = std::min(r.ah, 300);
                a.it.s = r.alt;
            } else {
                if (r.alt.empty()) continue;
                a.w = std::min(maxw, fw() * (u8_len(r.alt) + 2)); a.h = fh() + 4;
                a.it.s = r.alt;
            }
            a.it.w = a.w; a.it.h = a.h;
            a.space_before = space_;
            put_atom(a);
            space_ = true;
            continue;
        }
        if (name == "input" || name == "textarea") {
            std::string type = name == "textarea" ? "textarea" : css_lower(attr(at, "type"));
            if (type.empty()) type = "text";
            Field fl;
            fl.form = cur_form_;
            fl.name = attr(at, "name");
            fl.type = type;
            fl.value = decode_entities(attr(at, "value"));
            fl.placeholder = decode_entities(attr(at, "placeholder"));
            fl.checked = at.count("checked") > 0;
            const int szattr = std::atoi(attr(at, "size").c_str());
            fl.size = std::max(8, std::min(48, szattr > 0 ? szattr : 24));
            if (name == "textarea") { skip_tag = "textarea"; skip_depth = 1; fl.size = 40; }
            d_.fields.push_back(fl);
            const int fi = static_cast<int>(d_.fields.size()) - 1;
            if (type == "hidden") continue;
            if (type == "image") continue;
            int bw = fl.size * fw() + 12, bh = fh() + 10;
            if (type == "submit" || type == "button" || type == "reset") {
                std::string v = fl.value.empty() ? (type == "reset" ? "Reset" : "OK") : fl.value;
                bw = static_cast<int>(u8_len(v)) * fw() + 20;
            } else if (type == "checkbox" || type == "radio") { bw = fh(); bh = fh(); }
            else if (type == "textarea") bh = fh() * 4 + 8;
            add_box(bw, bh, 0xFFFFFFFFu, "", 0xFF222222u);
            line_.back().it.field = fi;
            if (at.count("autofocus") && (type == "text" || type == "search")) d_.autofocus = fi;
            continue;
        }
        if (name == "a") {
            const std::string href = attr(at, "href");
            push("a");
            if (!href.empty() && href[0] != '#' && href.compare(0, 11, "javascript:") != 0) {
                d_.links.push_back(unwrap_tracking(url_resolve(d_.base, decode_entities(href))));
                st_.link = static_cast<int>(d_.links.size()) - 1;
                st_.color = 0xFF1A5FB4u;
                st_.under = true;
            }
            continue;
        }
        if (name == "b" || name == "strong") { push(name); st_.bold = true; continue; }
        if (name == "i" || name == "em" || name == "cite" || name == "var") { push(name); if (st_.link < 0) st_.color = 0xFF3A3A5Cu; continue; }
        if (name == "u") { push(name); st_.under = true; continue; }
        if (name == "code" || name == "tt" || name == "kbd" || name == "samp") { push(name); if (st_.link < 0) st_.color = 0xFF7A2E2Eu; continue; }
        if (name == "mark") { push(name); st_.color = 0xFF8A5A00u; continue; }
        if (name == "small" || name == "big" || name == "span" || name == "label" || name == "button" || name == "sup" || name == "sub") { push(name); if (name == "button") space_ = true; continue; }
        if (name == "font") {
            push("font");
            std::string c = attr(at, "color");
            if (c.size() == 7 && c[0] == '#' && st_.link < 0) st_.color = 0xFF000000u | static_cast<u32>(std::strtoul(c.c_str() + 1, nullptr, 16));
            continue;
        }
        if (name == "td" || name == "th") { if (name == "th") { push("th"); st_.bold = true; } if (!line_.empty()) space_ = true; if (attr(at, "align") == "center") center_ = true; continue; }
        if (name == "ul" || name == "ol") { margin(list_counters_.empty() ? 6 : 0); push(name, 24); list_counters_.push_back(name == "ol" ? 1 : 0); continue; }
        if (name == "li") {
            margin(2);
            flush();
            begin_line_y();
            const int ind = indent_;
            indent_ -= 14;
            if (!list_counters_.empty() && list_counters_.back() > 0) {
                const std::string num = std::to_string(list_counters_.back()++) + ".";
                Atom a;
                a.it.kind = WItem::Text; a.it.s = num; a.it.color = st_.color; a.it.scale = 1;
                a.w = static_cast<int>(num.size()) * fw(); a.h = fh();
                indent_ -= a.w;
                put_atom(a);
                indent_ = ind;
                space_ = true;
            } else {
                Atom a;
                a.it.kind = WItem::Box; a.it.color = 0xFF333333u; a.it.w = 5; a.it.h = 5; a.it.img = 0;
                a.w = 5; a.h = fh();
                a.it.y = 0;
                put_atom(a);
                // the bullet is a small square lowered to the middle of the line
                line_.back().h = fh();
                line_.back().it.h = 5;
                indent_ = ind;
                space_ = true;
            }
            push("li");
            continue;
        }
        if (name == "blockquote") { margin(8); push(name, 24); st_.color = 0xFF444444u; continue; }
        if (name == "dd") { margin(2); push(name, 24); continue; }
        if (name == "pre") { margin(8); push(name); pre_ = true; continue; }
        if (name[0] == 'h' && name.size() == 2 && name[1] >= '1' && name[1] <= '6') {
            margin(name == "h1" ? 12 : 10);
            push(name);
            st_.bold = true;
            st_.scale = name == "h1" ? 2 : 1;
            if (attr(at, "align") == "center") center_ = true;
            continue;
        }
        if (name == "center") { margin(0); push(name); center_ = true; continue; }
        if (is_block(name)) {
            const std::string a = attr(at, "align");
            if (name == "form") { Form fm; fm.action = url_resolve(d_.base, decode_entities(attr(at, "action"))); fm.method = css_lower(attr(at, "method")); d_.forms.push_back(fm); cur_form_ = static_cast<int>(d_.forms.size()) - 1; }
            margin(name == "p" || name == "table" || name == "form" ? 6 : 0);
            push(name);
            if (a == "center") center_ = true;
            if (name == "div" || name == "td") { std::string cls = attr(at, "class"); (void)cls; }
            continue;
        }
        // unknown inline tag: ignore
    }
    flush();
    d_.height = y_ + 16;
}

// ------------------------------------------------------------------- drawing
inline void draw_doc(Surface& s, const Rect& view, const Doc& d, int scroll, const std::map<std::string, Pic>& pics, int focus = -1) {
    const Rect save = s.clip;
    s.clip = save.intersect(view);
    fill(s, view, 0xFFFFFFFFu);
    for (const auto& it : d.items) {
        const int y = view.y + it.y - scroll;
        if (y > view.bottom() || y + std::max(it.h, font_h() * it.scale) < view.y) continue;
        const int x = view.x + it.x;
        if (it.kind == WItem::Text) {
            draw_text(s, x, y, it.s, it.color, it.scale);
            if (it.bold) draw_text(s, x + 1, y, it.s, it.color, it.scale);
            if (it.under) fill(s, {x, y + font_h() * it.scale - 1, u8_len(it.s) * font_w() * it.scale, 1}, it.color);
        } else if (it.kind == WItem::Box) {
            if (it.h <= 1) { fill(s, {x, y, it.w, it.h}, it.color); continue; }
            if (it.s.empty() && it.img == 0 && it.w == 5) { fill(s, {x, y + font_h() / 2 - 2, 5, 5}, it.color); continue; }
            if (it.field >= 0 && it.field < static_cast<int>(d.fields.size())) {  // a form control
                const Field& f = d.fields[static_cast<std::size_t>(it.field)];
                const bool btn = f.type == "submit" || f.type == "button" || f.type == "reset";
                const bool foc = it.field == focus;
                fill(s, {x, y, it.w, it.h}, btn ? 0xFFE4E7EDu : 0xFFFFFFFFu);
                rrect_outline(s, {x, y, it.w, it.h}, 2, foc ? 2 : 1, foc ? 0xFF2F6FE0u : 0xFF8A919Eu);
                if (btn) {
                    const std::string v = f.value.empty() ? (f.type == "reset" ? "Reset" : "OK") : f.value;
                    draw_text(s, x + (it.w - u8_len(v) * font_w()) / 2, y + (it.h - font_h()) / 2, v, 0xFF222222u, 1);
                } else if (f.type == "checkbox" || f.type == "radio") {
                    if (f.checked) { if (f.type == "radio") circle(s, x + it.w / 2, y + it.h / 2, it.w / 4, 0xFF2F6FE0u); else { line(s, x + 3, y + it.h / 2, x + it.w / 2 - 1, y + it.h - 4, 0xFF2F6FE0u, 2); line(s, x + it.w / 2 - 1, y + it.h - 4, x + it.w - 3, y + 3, 0xFF2F6FE0u, 2); } }
                } else {
                    std::string v = f.value;
                    if (f.type == "password") v = std::string(static_cast<std::size_t>(u8_len(v)), '*');
                    const bool ph = v.empty();
                    if (ph) v = f.placeholder;
                    const int cols = std::max(1, (it.w - 12) / font_w());
                    std::string shown = v;
                    if (u8_len(shown) > cols - (foc ? 1 : 0)) {  // long text: show its end
                        std::size_t p = 0;
                        const int skip = u8_len(shown) - (cols - (foc ? 1 : 0));
                        for (int k = 0; k < skip; ++k) p = u8_next(shown, p);
                        shown = shown.substr(p);
                    }
                    draw_text(s, x + 6, y + (it.h - font_h()) / 2, shown + (foc && !ph ? "_" : ""), ph ? 0xFF8A8F99u : 0xFF111111u, 1);
                    if (foc && ph) fill(s, {x + 6, y + (it.h - font_h()) / 2, 2, font_h()}, 0xFF2F6FE0u);
                }
                continue;
            }
            fill(s, {x, y, it.w, it.h}, it.color);
            rrect_outline(s, {x, y, it.w, it.h}, 2, 1, 0xFF9AA0AAu);
            if (!it.s.empty()) draw_text(s, x + (it.s.size() < 3 && it.w < 40 ? (it.w - u8_len(it.s) * font_w()) / 2 : 6), y + (it.h - font_h()) / 2, it.s, static_cast<u32>(it.img), 1);
        } else {
            const ImgRef& r = d.imgs[static_cast<std::size_t>(it.img)];
            auto p = pics.find(r.src);
            if (p == pics.end() || p->second.px.empty()) {
                fill(s, {x, y, it.w, it.h}, 0xFFF1F3F6u);
                rrect_outline(s, {x, y, it.w, it.h}, 2, 1, 0xFFC8CCD4u);
                if (!it.s.empty()) draw_text(s, x + 4, y + 2, fit_text(it.s, it.w - 8, 1), 0xFF777777u, 1);
                continue;
            }
            const Pic& pc = p->second;
            for (int yy = 0; yy < it.h; ++yy) {
                const int dy = y + yy;
                if (dy < s.clip.y || dy >= s.clip.bottom()) continue;
                const u32* src = &pc.px[static_cast<std::size_t>(std::min(pc.h - 1, yy * pc.h / std::max(1, it.h))) * static_cast<std::size_t>(pc.w)];
                for (int xx = 0; xx < it.w; ++xx) {
                    const int dx = x + xx;
                    if (dx < s.clip.x || dx >= s.clip.right()) continue;
                    const u32 c = src[std::min(pc.w - 1, xx * pc.w / std::max(1, it.w))];
                    const int a = ca(c);
                    u32& dst = s.px[static_cast<std::size_t>(dy) * static_cast<std::size_t>(s.stride) + static_cast<std::size_t>(dx)];
                    dst = a >= 255 ? (c | 0xFF000000u) : mix(dst, c | 0xFF000000u, static_cast<float>(a) / 255.0f);
                }
            }
        }
    }
    s.clip = save;
}

inline int doc_link_at(const Doc& d, int x, int y_in_doc) {
    for (std::size_t i = d.items.size(); i-- > 0;) {
        const WItem& it = d.items[i];
        if (it.link < 0) continue;
        const int h = it.kind == WItem::Text ? font_h() * it.scale : it.h, w = it.kind == WItem::Text ? u8_len(it.s) * font_w() * it.scale : it.w;
        if (x >= it.x && x < it.x + w && y_in_doc >= it.y && y_in_doc < it.y + h) return it.link;
    }
    return -1;
}

inline int doc_field_at(const Doc& d, int x, int y_in_doc) {
    for (const auto& it : d.items)
        if (it.field >= 0 && it.kind == WItem::Box && x >= it.x && x < it.x + it.w && y_in_doc >= it.y && y_in_doc < it.y + it.h) return it.field;
    return -1;
}

// Address for submitting a form: GET puts the fields into the query, POST returns the body separately.
inline std::string form_submit(const Doc& d, int field, std::string& post_body) {
    const Field& clicked = d.fields[static_cast<std::size_t>(field)];
    const int fi = clicked.form;
    std::string base = d.base, action = d.base, method = "get";
    if (fi >= 0 && fi < static_cast<int>(d.forms.size())) { action = d.forms[static_cast<std::size_t>(fi)].action; method = d.forms[static_cast<std::size_t>(fi)].method; }
    std::string q;
    for (std::size_t i = 0; i < d.fields.size(); ++i) {
        const Field& f = d.fields[i];
        if (f.form != fi || f.name.empty()) continue;
        if (f.type == "submit" || f.type == "button" || f.type == "image" || f.type == "reset") { if (static_cast<int>(i) != field) continue; }
        else if ((f.type == "checkbox" || f.type == "radio") && !f.checked) continue;
        if (!q.empty()) q += "&";
        q += url_encode(f.name) + "=" + url_encode(f.value);
    }
    if (method == "post") { post_body = q; return action; }
    post_body.clear();
    const std::size_t qm = action.find('?');
    const std::size_t h = action.find('#');
    if (h != std::string::npos) action.erase(h);
    if (qm != std::string::npos) action = action.substr(0, qm);
    return q.empty() ? action : action + "?" + q;
}
