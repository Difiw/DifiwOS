// SPDX-License-Identifier: MIT
// Desktop rendering: wallpaper, windows, floating dock, tray, popups (menu D, tasks, ...).
#include <arpa/inet.h>
#include <dirent.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>

#include "desktop.hpp"

namespace dg {

using namespace tok;

// Animated colour phase; frozen when the user asked for reduced motion.
#define PHASE (set_.reduce_motion ? 0.0f : static_cast<float>(frame_ms_) * kGlowSpeed)

namespace {

AppDef& settings_app() {
    static AppDef a;
    a.id = "@settings"; a.name_en = "Settings"; a.name_ru = "Параметры"; a.category = "system"; a.icon = "gear";
    return a;
}

std::string encode_cp(std::uint32_t cp) {
    std::string o;
    if (cp < 0x80) o += static_cast<char>(cp);
    else if (cp < 0x800) { o += static_cast<char>(0xC0 | (cp >> 6)); o += static_cast<char>(0x80 | (cp & 0x3F)); }
    else { o += static_cast<char>(0xE0 | (cp >> 12)); o += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); o += static_cast<char>(0x80 | (cp & 0x3F)); }
    return o;
}

// Case-folds ASCII and Cyrillic for the search box.
std::string fold(const std::string& s) {
    std::string o;
    std::size_t i = 0;
    while (i < s.size()) {
        std::uint32_t cp = u8_codepoint(s, i);
        if (cp >= 'A' && cp <= 'Z') cp += 32;
        else if (cp >= 0x410 && cp <= 0x42F) cp += 0x20;
        else if (cp == 0x401) cp = 0x451;
        o += encode_cp(cp);
    }
    return o;
}

std::vector<std::string> tray_ids(bool battery) {
    std::vector<std::string> v = {"tasks", "net", "sound"};
    if (battery) v.push_back("battery");
    v.push_back("layout");
    v.push_back("clock");
    v.push_back("notify");
    return v;
}

void icon_colors(const std::string& kind, u32& top, u32& bottom) {
    if (kind == "notes") { top = 0xFF6AA6FF; bottom = 0xFF3A5FD0; }
    else if (kind == "calc") { top = 0xFF35D8C3; bottom = 0xFF0F8F86; }
    else if (kind == "terminal") { top = 0xFF2F374B; bottom = 0xFF151A24; }
    else if (kind == "monitor") { top = 0xFF9B72F7; bottom = 0xFF5B3BC4; }
    else if (kind == "probe") { top = 0xFF5ED083; bottom = 0xFF2E8B57; }
    else if (kind == "folder") { top = 0xFFF5C25B; bottom = 0xFFC98A1F; }
    else if (kind == "shield") { top = 0xFF4C8DFF; bottom = 0xFF2A4FA8; }
    else { top = 0xFF6B7690; bottom = 0xFF3A4358; }
}

}  // namespace

// ------------------------------------------------------------------------ helpers
u32 Desktop::ca_(u32 c) const { return with_alpha(c, static_cast<int>(static_cast<float>(ca(c)) * content_alpha_)); }

float Desktop::hover_amount(const std::string& key) const {
    auto it = hov_.find(key);
    return it == hov_.end() ? 0.0f : it->second;
}

float Desktop::press_amount(const std::string& key) const {
    auto it = press_t_.find(key);
    if (it == press_t_.end()) return 0.0f;
    const float p = static_cast<float>(frame_ms_ - it->second) / static_cast<float>(2 * kAnimPress);
    if (p < 0.0f || p > 1.0f) return 0.0f;
    return std::sin(p * 3.14159265f);
}

bool Desktop::animating() const {
    if (set_.reduce_motion) return false;
    for (const auto& [k, v] : hov_) {
        const float target = k == hover_ ? 1.0f : 0.0f;
        if (std::fabs(v - target) > 0.01f || v > 0.05f) return true;  // a hovered icon keeps its light moving
    }
    for (const auto& p : popups_) if (frame_ms_ - (p.closing ? p.t_close : p.t0) < kAnimPopup + 40) return true;
    for (const auto& w : wins_) if (frame_ms_ - w->opened_ms < kAnimWindow + 40) return true;
    for (const auto& [k, t] : press_t_) if (frame_ms_ - t < 2 * kAnimPress + 30) return true;
    return std::fabs(dock_scroll_ - dock_scroll_target_) > 0.01f;
}

void Desktop::update_animations(long dt) {
    if (!hover_.empty()) hov_[hover_];
    const float rate = set_.reduce_motion ? 1.0f : 1.0f - std::exp(-static_cast<float>(dt) / (static_cast<float>(kAnimHover) * 0.45f));
    for (auto it = hov_.begin(); it != hov_.end();) {
        const float target = it->first == hover_ ? 1.0f : 0.0f;
        it->second += (target - it->second) * rate;
        if (std::fabs(it->second - target) < 0.004f) it->second = target;
        if (it->second == 0.0f && it->first != hover_) it = hov_.erase(it); else ++it;
    }
    dock_scroll_ += (dock_scroll_target_ - dock_scroll_) * (set_.reduce_motion ? 1.0f : rate);
    for (auto it = press_t_.begin(); it != press_t_.end();) {
        if (frame_ms_ - it->second > 2 * kAnimPress + 60) it = press_t_.erase(it); else ++it;
    }
}

bool Desktop::battery_present() const { return access("/sys/class/power_supply/BAT0", F_OK) == 0; }

std::vector<std::string> Desktop::categories() const {
    std::vector<std::string> c = {"all"};
    for (const auto& a : tasks_.apps()) {
        if (a.hidden) continue;
        if (std::find(c.begin(), c.end(), a.category) == c.end()) c.push_back(a.category);
    }
    return c;
}

std::string Desktop::category_label(int idx) const {
    const auto c = categories();
    if (idx < 0 || idx >= static_cast<int>(c.size())) return "";
    const std::string& k = c[static_cast<std::size_t>(idx)];
    if (k == "all") return t("All", "Все");
    if (k == "work") return t("Work", "Работа");
    if (k == "utility") return t("Utilities", "Утилиты");
    if (k == "system") return t("System", "Система");
    if (k == "test") return t("Test", "Тест");
    return k;
}

std::vector<const AppDef*> Desktop::menu_apps() const {
    std::vector<const AppDef*> out;
    const auto cats = categories();
    const std::string cat = cats.empty() ? "all" : cats[static_cast<std::size_t>(std::min(menu_cat_, static_cast<int>(cats.size()) - 1))];
    const std::string q = fold(menu_search_);
    auto consider = [&](const AppDef* a) {
        if (a->hidden) return;
        if (cat != "all" && a->category != cat) return;
        if (!q.empty() && fold(a->name_en).find(q) == std::string::npos && fold(a->name_ru).find(q) == std::string::npos &&
            fold(a->id).find(q) == std::string::npos) return;
        out.push_back(a);
    };
    for (const auto& a : tasks_.apps()) consider(&a);
    consider(&settings_app());
    return out;
}

// --------------------------------------------------------------------- primitives
void Desktop::draw_glass(Rect r, int radius, int shadow_px) {
    shadow(back_, r, radius, S(shadow_px), S(6), kShadow);
    glass(back_, wall_blur_, r, radius, set_.transparency);
}

void Desktop::add_hit(Rect r, std::function<void(int)> fn, const std::string& hover_key, const std::string& tip) {
    hits_.push_back({r, std::move(fn), tip, hover_key});
}

void Desktop::draw_button(Rect r, const std::string& label, std::function<void(int)> fn, bool primary, bool danger, bool enabled) {
    const std::string key = "btn:" + std::to_string(r.x) + "," + std::to_string(r.y);
    const float hv = enabled ? hover_amount(key) : 0.0f;
    u32 base = primary ? 0xFF2F5FC0u : danger ? 0xFF8A3A30u : kGraphite600;
    u32 hot = primary ? 0xFF4C8DFFu : danger ? kDanger : kGraphite500;
    if (!enabled) base = hot = 0xFF1D2330u;
    rrect(back_, r, S(kRadiusControl), ca_(mix(base, hot, hv)));
    if (enabled) rrect_outline(back_, r, S(kRadiusControl), 1, ca_(with_alpha(0xFFFFFFFFu, 28 + static_cast<int>(60.0f * hv))));
    const int tw = text_width(label, set_.scale);
    const int th = font_h() * set_.scale;
    draw_text(back_, r.x + (r.w - tw) / 2, r.y + (r.h - th) / 2, label, ca_(enabled ? kText : kTextFaint), set_.scale);
    if (enabled && fn) { Hit h; h.r = r; h.fn = std::move(fn); h.hover_key = key; h.label = label; hits_.push_back(std::move(h)); }
}

// --------------------------------------------------------------------------- icons
static void glyph_gear(Surface& s, int cx, int cy, int rad, u32 c) {
    for (int k = 0; k < 8; ++k) {
        const float a = static_cast<float>(k) * 3.14159265f / 4.0f;
        line(s, cx + static_cast<int>(std::cos(a) * static_cast<float>(rad) * 0.62f), cy + static_cast<int>(std::sin(a) * static_cast<float>(rad) * 0.62f),
             cx + static_cast<int>(std::cos(a) * static_cast<float>(rad)), cy + static_cast<int>(std::sin(a) * static_cast<float>(rad)), c, std::max(2, rad / 4));
    }
    ring(s, cx, cy, static_cast<int>(static_cast<float>(rad) * 0.72f), std::max(2, rad / 4), c);
    circle(s, cx, cy, std::max(1, rad / 6), c);
}

void Desktop::draw_icon(Rect r, const std::string& kind, float hover, bool glow, int radius) {
    if (radius < 0) radius = r.w * kRadiusIcon / kDockIcon;
    u32 top, bottom;
    icon_colors(kind, top, bottom);
    const int cx = r.x + r.w / 2, cy = r.y + r.h / 2, u = std::min(r.w, r.h);
    const u32 white = ca_(0xFFF4F7FFu);
    if (hover > 0.02f) {  // soft outer light
        const u32 g = glow_color(PHASE);
        for (int i = 3; i >= 1; --i)
            rrect(back_, {r.x - i * 2, r.y - i * 2, r.w + 4 * i, r.h + 4 * i}, radius + i * 2, with_alpha(g, static_cast<int>(10.0f * hover * static_cast<float>(4 - i))));
    }
    rrect_gradient(back_, r, radius, ca_(top), ca_(bottom));
    rrect_outline(back_, r, radius, 1, ca_(0x38FFFFFFu));
    if (kind == "notes") {
        const Rect pg = {cx - u / 5, cy - u / 3, u * 2 / 5 + 1, u * 2 / 3};
        rrect(back_, pg, std::max(2, u / 12), white);
        for (int i = 0; i < 3; ++i) fill(back_, {pg.x + u / 14, pg.y + u / 6 + i * u / 7, pg.w - u / 7, std::max(1, u / 22)}, ca_(0xFF3A5FD0u));
    } else if (kind == "calc") {
        const int cell = u / 6;
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j) rrect(back_, {cx - cell * 3 / 2 - cell / 3 + i * (cell + cell / 3) - 0, cy - cell * 3 / 2 - cell / 3 + j * (cell + cell / 3), cell, cell}, std::max(1, cell / 4), white);
    } else if (kind == "terminal") {
        const int sc = std::max(1, u / 22);
        const int tw = text_width(">_", sc), th = font_h() * sc;
        draw_text(back_, cx - tw / 2, cy - th / 2, ">_", ca_(kAccentTeal), sc);
    } else if (kind == "monitor") {
        const int bw = std::max(2, u / 9);
        const int hs[4] = {u / 4, u / 2, u * 3 / 8, u * 5 / 8};
        for (int i = 0; i < 4; ++i) rrect(back_, {cx - u / 3 + i * (bw + bw / 2), cy + u / 3 - hs[i], bw, hs[i]}, 1, white);
    } else if (kind == "probe") {
        ring(back_, cx, cy, u / 3, std::max(2, u / 12), white);
        const int a = u / 7;
        line(back_, cx - a, cy, cx - a / 3, cy + a, white, std::max(2, u / 12));
        line(back_, cx - a / 3, cy + a, cx + a, cy - a, white, std::max(2, u / 12));
    } else if (kind == "folder") {
        rrect(back_, {cx - u / 3, cy - u / 4, u / 3, u / 6}, 2, white);
        rrect(back_, {cx - u / 3, cy - u / 6, u * 2 / 3, u / 2}, std::max(2, u / 12), white);
    } else if (kind == "shield") {
        rrect(back_, {cx - u / 5, cy - u / 12, u * 2 / 5, u / 3}, std::max(2, u / 14), white);
        ring(back_, cx, cy - u / 12, u / 8, std::max(2, u / 16), white);
    } else {
        glyph_gear(back_, cx, cy, u / 3, white);
    }
    if (glow && hover > 0.02f) {
        for (int th = 1; th <= std::max(1, S(2)); ++th)
            rrect_outline_glow(back_, r.inset(-th + 1), radius + 1, 1, PHASE, static_cast<int>(210.0f * hover));
    }
}

void Desktop::draw_d_logo(Rect r, float hover, float press) {
    const int u = r.w;
    float sc = 1.0f + 0.06f * hover - 0.08f * press;
    const int w = static_cast<int>(static_cast<float>(u) * sc);
    Rect b = {r.x + (r.w - w) / 2, r.y + (r.h - w) / 2 - static_cast<int>(hover * static_cast<float>(S(3))), w, w};
    const int rad = b.w * 22 / 58;
    const u32 g = glow_color(PHASE * 0.6f);
    for (int i = 4; i >= 1; --i) rrect(back_, {b.x - i * 2, b.y - i * 2, b.w + 4 * i, b.h + 4 * i}, rad + i * 2, with_alpha(g, 5 + static_cast<int>(7.0f * hover) * (5 - i) / 2));
    rrect_gradient(back_, b, rad, 0xFF232B3Du, 0xFF11151Fu);
    rrect_outline_glow(back_, b, rad, std::max(2, S(2)), PHASE * (0.5f + hover), 150 + static_cast<int>(100.0f * hover));
    // the letter D: left bar + top/bottom bars + a round right side
    const int bh = b.h * 11 / 20, bw = bh * 10 / 13;
    const int bx = b.x + (b.w - bw) / 2, by = b.y + (b.h - bh) / 2;
    const int t = std::max(3, bh / 6);
    const int cap = bh / 2;
    Surface& s = back_;
    const Rect save = s.clip;
    s.clip = save.intersect({bx, by, bw - cap, bh});
    fill(s, {bx, by, t, bh}, 0xFFF4F7FFu);
    fill(s, {bx, by, bw, t}, 0xFFF4F7FFu);
    fill(s, {bx, by + bh - t, bw, t}, 0xFFF4F7FFu);
    s.clip = save.intersect({bx + bw - cap, by, cap + 1, bh});
    rrect_outline(s, {bx, by, bw, bh}, cap, t, 0xFFF4F7FFu);
    s.clip = save;
}

// ---------------------------------------------------------------------------- windows
void Desktop::draw_window(const Window& w, bool focused) {
    Rect f = frame_of(w);
    const long age = frame_ms_ - w.opened_ms;
    float e = set_.reduce_motion ? 1.0f : ease_out(static_cast<float>(age) / static_cast<float>(kAnimWindow));
    f.y += static_cast<int>((1.0f - e) * static_cast<float>(S(14)));
    const int rad = S(kRadiusWindow);
    shadow(back_, f, rad, S(focused ? 22 : 12), S(8), with_alpha(kShadow, focused ? 0x78 : 0x48));
    rrect(back_, f, rad, 0xFF171C27u);
    rrect_gradient(back_, {f.x, f.y, f.w, title_h()}, rad, 0xFF252C3Cu, 0xFF1B212Eu);
    fill(back_, {f.x, f.y + title_h() - rad, f.w, rad}, 0xFF1B212Eu);
    if (focused) rrect_outline_glow(back_, f, rad, 1, PHASE * 0.4f, 150);
    else rrect_outline(back_, f, rad, 1, 0x30FFFFFFu);
    const int th = font_h() * set_.scale;
    const std::string title = fit_text(w.title, f.w - S(130), set_.scale);
    draw_text(back_, f.x + S(16), f.y + (title_h() - th) / 2, title, focused ? kText : kTextDim, set_.scale);
    // title buttons: close, maximize, minimize (symbols + shape, not color alone)
    const char* names[3] = {"close", "max", "min"};
    for (int i = 0; i < 3; ++i) {
        const Rect b = title_btn(w, i);
        const bool over = b.has(mx_, my_);
        const u32 bg = i == 0 ? (over ? kDanger : 0xFF3A2A2Eu) : (over ? kGraphite500 : kGraphite600);
        circle(back_, b.x + b.w / 2, b.y + b.h / 2, b.w / 2, bg);
        const int cx = b.x + b.w / 2, cy = b.y + b.h / 2, q = b.w / 5;
        const int lt = std::max(1, S(1));
        if (i == 0) { line(back_, cx - q, cy - q, cx + q, cy + q, kText, lt); line(back_, cx - q, cy + q, cx + q, cy - q, kText, lt); }
        else if (i == 1) { rrect_outline(back_, {cx - q, cy - q, 2 * q + 1, 2 * q + 1}, 1, lt, kText); }
        else { line(back_, cx - q, cy + q / 2, cx + q, cy + q / 2, kText, lt); }
        (void)names;
    }
    // content
    const Rect c = {f.x + S(6), f.y + title_h(), w.cw, w.ch};
    const Rect save = back_.clip;
    back_.clip = save.intersect(c);
    fill(back_, c, kGraphite800);
    if (w.buf.px) {
        const int bw = std::min(w.buf.w, c.w), bh = std::min(w.buf.h, c.h);
        // copy only the part of each row that is inside the clip (a window may hang over a screen edge)
        const int x0 = std::max(c.x, std::max(back_.clip.x, 0));
        const int x1 = std::min(c.x + bw, std::min(back_.clip.right(), back_.w));
        if (x1 > x0)
            for (int y = 0; y < bh; ++y) {
                const int dy = c.y + y;
                if (dy < back_.clip.y || dy >= back_.clip.bottom() || dy < 0 || dy >= back_.h) continue;
                std::memcpy(back_.px + static_cast<std::size_t>(dy) * back_.stride + x0,
                            w.buf.px + static_cast<std::size_t>(y) * w.buf.stride + (x0 - c.x), static_cast<std::size_t>(x1 - x0) * 4);
            }
    }
    back_.clip = save;
    if (e < 1.0f) {  // fade-in veil
        rrect(back_, f, rad, with_alpha(0xFF0E1118u, static_cast<int>((1.0f - e) * 150.0f)));
    }
}

void Desktop::draw_windows() {
    for (const auto& w : wins_) {
        if (w->minimized) continue;
        draw_window(*w, w->id == focus_);
    }
}

// ----------------------------------------------------------------------------- dock
Rect Desktop::dock_rect() const {
    const int pad = S(kPad);
    int side = S(kDockSideWidth);
    const int d = S(kDockDWidth);
    side = std::min(side, (W_ - S(2 * kDockMargin) - d - 2 * pad) / 2);
    const int w = 2 * side + d + 2 * pad;
    const int h = S(kDockHeight);
    return {(W_ - w) / 2, H_ - S(kDockMargin) - h, w, h};
}

Rect Desktop::dock_d_rect() const {
    const Rect d = dock_rect();
    const int s = S(kDockDWidth) - S(6);
    return {d.x + d.w / 2 - s / 2, d.y + (d.h - s) / 2, s, s};
}

Rect Desktop::dock_left_rect() const {
    const Rect d = dock_rect();
    const int side = (d.w - S(kDockDWidth) - 2 * S(kPad)) / 2;
    return {d.x + S(kPad), d.y, side, d.h};
}

Rect Desktop::dock_right_rect() const {
    const Rect d = dock_rect();
    const int side = (d.w - S(kDockDWidth) - 2 * S(kPad)) / 2;
    return {d.right() - S(kPad) - side, d.y, side, d.h};
}

int Desktop::tray_count() const { return static_cast<int>(tray_ids(battery_present()).size()); }

Rect Desktop::tray_rect(int index) const {
    const auto ids = tray_ids(battery_present());
    const Rect a = dock_right_rect();
    int total = 0;
    std::vector<int> ws;
    for (const auto& id : ids) { const int w = id == "clock" ? S(58) : S(36); ws.push_back(w); total += w; }
    int gap = ids.size() > 1 ? std::min(S(4), (a.w - total) / static_cast<int>(ids.size() - 1)) : 0;
    if (gap < 0 || total > a.w) {  // not enough room (large UI scale, small screen): shrink the cells evenly
        const float f = static_cast<float>(a.w) / static_cast<float>(total);
        total = 0;
        for (auto& w : ws) { w = std::max(S(18), static_cast<int>(static_cast<float>(w) * f)); total += w; }
        gap = 0;
    }
    int x = a.right() - (total + gap * static_cast<int>(ids.size() - 1));
    if (x < a.x) x = a.x;
    for (int i = 0; i < index; ++i) x += ws[static_cast<std::size_t>(i)] + gap;
    const int h = S(44);
    return {x, a.y + (a.h - h) / 2, ws[static_cast<std::size_t>(index)], h};
}

static void tray_glyph(Surface& s, const std::string& id, int cx, int cy, int u, u32 c) {
    const int lt = std::max(1, u / 12);
    if (id == "tasks") {
        const int d = std::max(2, u / 6);
        for (int i = 0; i < 2; ++i)
            for (int j = 0; j < 2; ++j) rrect(s, {cx - d - d / 3 + i * (d + d / 2), cy - d - d / 3 + j * (d + d / 2), d, d}, 1, c);
    } else if (id == "net") {
        const Rect save = s.clip;
        s.clip = save.intersect({cx - u, cy - u, 2 * u, u + u / 6});
        for (int k = 1; k <= 3; ++k) ring(s, cx, cy + u / 4, k * u / 3, lt, c);
        s.clip = save;
        circle(s, cx, cy + u / 4, std::max(1, lt), c);
    } else if (id == "sound") {
        rrect(s, {cx - u / 2, cy - u / 6, u / 4, u / 3}, 1, c);
        line(s, cx - u / 4, cy - u / 6, cx, cy - u / 3, c, lt);
        line(s, cx - u / 4, cy + u / 6, cx, cy + u / 3, c, lt);
        line(s, cx, cy - u / 3, cx, cy + u / 3, c, lt);
        const Rect save = s.clip;
        s.clip = save.intersect({cx + u / 8, cy - u / 2, u, u});
        ring(s, cx + u / 8, cy, u / 3, lt, c);
        s.clip = save;
    } else if (id == "battery") {
        rrect_outline(s, {cx - u / 2, cy - u / 4, u, u / 2}, 2, lt, c);
        fill(s, {cx + u / 2, cy - u / 8, std::max(1, u / 10), u / 4}, c);
        fill(s, {cx - u / 2 + lt + 1, cy - u / 4 + lt + 1, u / 2, u / 2 - 2 * lt - 2}, c);
    } else if (id == "notify") {
        ring(s, cx, cy - u / 8, u / 3, lt, c);
        fill(s, {cx - u / 3, cy - u / 8, 2 * (u / 3) + 1, u / 4}, c);
        line(s, cx - u / 2, cy + u / 6, cx + u / 2, cy + u / 6, c, lt);
        circle(s, cx, cy + u / 3, std::max(1, lt), c);
    }
}

void Desktop::draw_tray(Rect) {
    const auto ids = tray_ids(battery_present());
    time_t tt = time(nullptr);
    struct tm tm;
    localtime_r(&tt, &tm);
    const auto bg = tasks_.background_tasks();
    int unseen = 0;
    {
        int idx = 0;
        for (const auto& e : tasks_.journal()) { if (idx >= notify_seen_ && e.alert) ++unseen; ++idx; }
    }
    for (std::size_t i = 0; i < ids.size(); ++i) {
        const std::string& id = ids[i];
        const Rect r = tray_rect(static_cast<int>(i));
        const std::string key = "tray:" + id;
        const float hv = hover_amount(key);
        const bool open = (id == "tasks" && popup(PopupKind::Tasks)) || (id == "net" && popup(PopupKind::Net)) || (id == "sound" && popup(PopupKind::Sound)) ||
                          (id == "clock" && popup(PopupKind::Calendar)) || (id == "notify" && popup(PopupKind::Notify)) || (id == "layout" && popup(PopupKind::Layout));
        if (hv > 0.02f || open) rrect(back_, r, S(12), with_alpha(0xFFFFFFFFu, open ? 34 : static_cast<int>(26.0f * hv)));
        const int cx = r.x + r.w / 2, cy = r.y + r.h / 2;
        const u32 col = open ? kAccentBlue : mix(kTextDim, kText, hv);
        if (id == "clock") {
            char hm[8], dm[16];
            std::strftime(hm, sizeof hm, "%H:%M", &tm);
            std::strftime(dm, sizeof dm, "%d.%m", &tm);
            const int th = font_h() * set_.scale;
            draw_text(back_, cx - text_width(hm, set_.scale) / 2, cy - th, hm, kText, set_.scale);
            draw_text(back_, cx - text_width(dm, set_.scale) / 2, cy, dm, kTextDim, set_.scale);
        } else if (id == "layout") {
            const std::string l = layout_ru_ ? "RU" : "EN";
            const int th = font_h() * set_.scale;
            draw_text(back_, cx - text_width(l, set_.scale) / 2, cy - th / 2, l, col, set_.scale);
        } else {
            tray_glyph(back_, id, cx, cy, std::min(S(22), r.w * 3 / 5), col);
        }
        if (id == "tasks" && !bg.empty()) {
            const std::string n = std::to_string(bg.size());
            draw_text(back_, r.right() - text_width(n, 1) - S(2), r.y + S(2), n, kAccentTeal, 1);
        }
        if (id == "notify" && unseen > 0) {
            circle(back_, r.right() - S(9), r.y + S(10), S(7), kDanger);
            const std::string n = std::to_string(std::min(unseen, 9));
            draw_text(back_, r.right() - S(9) - text_width(n, 1) / 2, r.y + S(10) - font_h() / 2, n, 0xFFFFFFFFu, 1);
        }
        std::string tip;
        if (id == "tasks") tip = t("Background tasks", "Фоновые задачи");
        else if (id == "net") tip = t("Network", "Сеть");
        else if (id == "sound") tip = t("Sound", "Звук");
        else if (id == "battery") tip = t("Power", "Питание");
        else if (id == "layout") tip = t("Keyboard layout (Alt+Shift)", "Раскладка (Alt+Shift)");
        else if (id == "clock") tip = t("Calendar", "Календарь");
        else if (id == "notify") tip = t("Notifications", "Уведомления");
        add_hit(r, [this, id, r](int) {
            if (id == "tasks") toggle_popup(PopupKind::Tasks, r);
            else if (id == "net") toggle_popup(PopupKind::Net, r);
            else if (id == "sound") toggle_popup(PopupKind::Sound, r);
            else if (id == "layout") toggle_popup(PopupKind::Layout, r);
            else if (id == "clock") toggle_popup(PopupKind::Calendar, r);
            else if (id == "notify") toggle_popup(PopupKind::Notify, r);
        }, key, tip);
    }
}

void Desktop::draw_dock() {
    build_dock_items();
    const Rect d = dock_rect();
    draw_glass(d, S(kRadiusDock), 20);

    // ---- user applications (running, started by the user, with a window)
    const Rect left = dock_left_rect();
    const int slot = S(kDockSlot);
    const int n = static_cast<int>(dock_items_.size());
    const int arrow = S(24);
    int cap = std::max(1, left.w / slot);
    const bool overflow = n > cap;
    Rect strip = left;
    if (overflow) {
        strip = {left.x + arrow, left.y, left.w - 2 * arrow, left.h};
        cap = std::max(1, strip.w / slot);
    }
    const float maxscroll = static_cast<float>(std::max(0, n - cap));
    dock_scroll_target_ = clampf(std::round(dock_scroll_target_), 0.0f, maxscroll);
    dock_scroll_ = clampf(dock_scroll_, 0.0f, maxscroll);
    const int icon = S(kDockIcon);
    if (n == 0) {
        const std::string hint = fit_text(t("Running apps appear here", "Здесь появятся запущенные приложения"), left.w - S(8), 1);
        draw_text(back_, left.x + S(8), left.y + (left.h - font_h()) / 2, hint, kTextFaint, 1);
    }
    const Rect save = back_.clip;
    back_.clip = save.intersect(strip);
    const int first_x = strip.x + (strip.w - std::min(n, cap) * slot) / 2;
    std::vector<float> hv(static_cast<std::size_t>(n), 0.0f);
    for (int i = 0; i < n; ++i) hv[static_cast<std::size_t>(i)] = hover_amount("dock:" + dock_items_[static_cast<std::size_t>(i)].app_id);
    for (int i = 0; i < n; ++i) {
        const DockItem& it = dock_items_[static_cast<std::size_t>(i)];
        const float x = static_cast<float>(first_x) + (static_cast<float>(i) - dock_scroll_) * static_cast<float>(slot);
        if (x + static_cast<float>(slot) < static_cast<float>(strip.x) || x > static_cast<float>(strip.right())) continue;
        const std::string key = "dock:" + it.app_id;
        const float h = hv[static_cast<std::size_t>(i)];
        const float pr = press_amount(key);
        const float sc = (1.0f + (kHoverScale - 1.0f) * h) * (1.0f - (1.0f - kPressScale) * pr);
        const int sz = static_cast<int>(static_cast<float>(icon) * sc);
        const int cx = static_cast<int>(x) + slot / 2;
        const int lift = static_cast<int>(h * kHoverLift * static_cast<float>(set_.scale));
        const Rect ir = {cx - sz / 2, d.y + (d.h - icon) / 2 - S(4) + (icon - sz) / 2 - lift, sz, sz};
        // soft light reflected from the neighbours
        const float nb = std::max(i > 0 ? hv[static_cast<std::size_t>(i - 1)] : 0.0f, i + 1 < n ? hv[static_cast<std::size_t>(i + 1)] : 0.0f);
        if (nb > 0.05f) rrect(back_, ir.inset(-S(3)), S(kRadiusIcon) + 3, with_alpha(glow_color(PHASE), static_cast<int>(26.0f * nb)));
        const AppDef* a = tasks_.app(it.app_id);
        draw_icon(ir, a ? a->icon : "gear", h, true);
        // running indicator (shape differs for the active window) + window count
        const int iy = d.y + d.h - S(11);
        if (it.active) rrect(back_, {cx - S(10), iy, S(20), S(4)}, S(2), kAccentBlue);
        else rrect(back_, {cx - S(3), iy, S(6), S(4)}, S(2), it.all_minimized ? kTextFaint : kTextDim);
        if (it.wins.size() > 1) {
            const std::string c = std::to_string(it.wins.size());
            circle(back_, ir.right() - S(2), ir.y + S(4), S(8), kAccentBlue);
            draw_text(back_, ir.right() - S(2) - text_width(c, 1) / 2, ir.y + S(4) - font_h() / 2, c, 0xFFFFFFFFu, 1);
        }
        const Rect hit = {static_cast<int>(x), d.y + S(4), slot, d.h - S(8)};
        const DockItem copy = it;
        const std::string app = it.app_id;
        add_hit(hit.intersect(strip), [this, copy, app, hit](int b) {
            if (b == 2) { close_all_popups(); open_popup(PopupKind::AppMenu, hit, app); }
            else activate_app(copy);
        }, key, a ? a->name(set_.ru) : app);
    }
    back_.clip = save;
    if (overflow) {  // arrows: plain buttons, the icons keep their size
        const Rect lr = {left.x, left.y + (left.h - S(36)) / 2, arrow, S(36)};
        const Rect rr = {left.right() - arrow, lr.y, arrow, lr.h};
        for (int k = 0; k < 2; ++k) {
            const Rect r = k == 0 ? lr : rr;
            const bool can = k == 0 ? dock_scroll_target_ > 0.0f : dock_scroll_target_ < maxscroll;
            const std::string key = std::string("dock:arrow") + (k == 0 ? "L" : "R");
            const float h = can ? hover_amount(key) : 0.0f;
            rrect(back_, r, S(8), with_alpha(0xFFFFFFFFu, can ? 18 + static_cast<int>(30.0f * h) : 6));
            const int cx = r.x + r.w / 2, cy = r.y + r.h / 2, q = S(5);
            const u32 c = can ? kText : kTextFaint;
            if (k == 0) { line(back_, cx + q / 2, cy - q, cx - q / 2, cy, c, 2); line(back_, cx - q / 2, cy, cx + q / 2, cy + q, c, 2); }
            else { line(back_, cx - q / 2, cy - q, cx + q / 2, cy, c, 2); line(back_, cx + q / 2, cy, cx - q / 2, cy + q, c, 2); }
            if (can) add_hit(r, [this, k](int) { dock_scroll_target_ += k == 0 ? -1.0f : 1.0f; dirty_ = true; }, key, k == 0 ? t("Scroll left", "Прокрутить влево") : t("Scroll right", "Прокрутить вправо"));
        }
    }

    // ---- the D button in the centre
    const Rect dr = dock_d_rect();
    draw_d_logo(dr, hover_amount("d"), press_amount("d"));
    if (popup(PopupKind::Menu)) rrect_outline(back_, dr.inset(-S(3)), S(24), 1, 0x60FFFFFFu);
    add_hit(dr, [this, dr](int) { toggle_popup(PopupKind::Menu, dr); }, "d", set_.winkey ? t("Applications (Win)", "Приложения (Win)") : t("Applications", "Приложения"));

    // thin separators between the three zones
    const int sy = d.y + S(16), sh = d.h - S(32);
    fill(back_, {dr.x - S(10), sy, 1, sh}, 0x24FFFFFFu);
    fill(back_, {dr.right() + S(10), sy, 1, sh}, 0x24FFFFFFu);
    draw_tray(dock_right_rect());
}

// --------------------------------------------------------------------------- popups
void Desktop::draw_popups() {
    for (auto& p : popups_) draw_popup(p);
}

void Desktop::draw_popup(Popup& p) {
    float e;
    if (set_.reduce_motion) e = p.closing ? 0.0f : 1.0f;
    else if (p.closing) e = 1.0f - ease_in_out(static_cast<float>(frame_ms_ - p.t_close) / static_cast<float>(kAnimPopup));
    else e = ease_out(static_cast<float>(frame_ms_ - p.t0) / static_cast<float>(kAnimPopup));
    if (e <= 0.01f) return;
    // grow from the button that opened it
    const float s = 0.30f + 0.70f * e;
    const int w = static_cast<int>(static_cast<float>(p.rect.w) * s), h = static_cast<int>(static_cast<float>(p.rect.h) * s);
    const float acx = static_cast<float>(p.anchor.x + p.anchor.w / 2), pcx = static_cast<float>(p.rect.x + p.rect.w / 2);
    const float abot = static_cast<float>(p.anchor.y - S(10)), pbot = static_cast<float>(p.rect.bottom());
    const int cx = static_cast<int>(acx + (pcx - acx) * e), bot = static_cast<int>(abot + (pbot - abot) * e);
    const Rect cur = {cx - w / 2, bot - h, w, h};
    draw_glass(cur, S(kRadiusPanel), 18);
    // pointer notch toward the source button
    if (e > 0.85f) {
        const int ax = clampi(p.anchor.x + p.anchor.w / 2, cur.x + S(28), cur.right() - S(28));
        const int ny = cur.bottom();
        for (int i = 0; i < S(9); ++i) fill(back_, {ax - (S(9) - i), ny + i - 1, 2 * (S(9) - i), 1}, 0xE0141925u);
    }
    content_alpha_ = clampf((e - 0.55f) / 0.45f, 0.0f, 1.0f);
    if (e < 0.6f) { content_alpha_ = 1.0f; return; }
    const Rect save = back_.clip;
    back_.clip = save.intersect(cur);
    const Rect r = p.rect;  // content is laid out in the final rectangle
    const std::size_t hits_before = hits_.size();
    switch (p.kind) {
        case PopupKind::Menu: draw_menu(p, r); break;
        case PopupKind::Tasks: draw_tasks(p, r); break;
        case PopupKind::Net: draw_net(r); break;
        case PopupKind::Sound: draw_sound(r); break;
        case PopupKind::Calendar: draw_calendar(r); break;
        case PopupKind::Notify: draw_notify(r); break;
        case PopupKind::Power: draw_power(r); break;
        case PopupKind::Layout: draw_layout(r); break;
        case PopupKind::Settings: draw_settings(r); break;
        case PopupKind::AppWindows: draw_app_windows(p, r); break;
        case PopupKind::AppMenu: draw_app_menu(p, r); break;
        case PopupKind::Confirm: draw_confirm(p, r); break;
    }
    back_.clip = save;
    if (p.closing || e < 0.9f) hits_.resize(hits_before);  // no clicks while it is animating or closing
    content_alpha_ = 1.0f;
}

static void heading(Desktop*, Surface& s, int x, int y, const std::string& text, u32 c, int scale) { draw_text(s, x, y, text, c, scale); }

void Desktop::draw_menu(Popup& p, Rect r) {
    const int pad = S(18), sc = set_.scale;
    const int th = font_h() * sc;
    int y = r.y + pad;
    heading(this, back_, r.x + pad, y + S(6), t("Applications", "Приложения"), ca_(kText), sc);
    // profile + power buttons (round)
    const int bd = S(30);
    const Rect pw = {r.right() - pad - bd, y, bd, bd}, pf = {pw.x - S(8) - bd, y, bd, bd};
    for (int k = 0; k < 2; ++k) {
        const Rect b = k == 0 ? pf : pw;
        const std::string key = k == 0 ? "menu:profile" : "menu:power";
        const float hv = hover_amount(key);
        circle(back_, b.x + b.w / 2, b.y + b.h / 2, b.w / 2, ca_(mix(kGraphite600, kGraphite500, hv)));
        ring(back_, b.x + b.w / 2, b.y + b.h / 2, b.w / 2, 1, ca_(with_alpha(0xFFFFFFFFu, 40 + static_cast<int>(60.0f * hv))));
        const int cx = b.x + b.w / 2, cy = b.y + b.h / 2, u = b.w / 3;
        if (k == 0) { circle(back_, cx, cy - u / 3, u / 2, ca_(kText)); rrect(back_, {cx - u, cy + u / 6, 2 * u, u}, u / 2, ca_(kText)); }
        else { ring(back_, cx, cy, u, std::max(1, S(2)), ca_(kText)); fill(back_, {cx - 1, cy - u - 2, std::max(2, S(2)), u + 2}, ca_(kGraphite600)); line(back_, cx, cy - u - 1, cx, cy - u / 3, ca_(kText), std::max(1, S(2))); }
        add_hit(b, [this, b, k](int) {
            if (k == 1) { if (!popup(PopupKind::Power)) open_popup(PopupKind::Power, b); else close_popup(PopupKind::Power); }
            else note_toast(t("User: difiw (uid 1000)", "Пользователь: difiw (uid 1000)"));
        }, key, k == 0 ? t("Profile", "Профиль") : t("Power", "Питание"));
    }
    y += bd + S(14);
    // search
    const Rect sb = {r.x + pad, y, r.w - 2 * pad, S(40)};
    rrect(back_, sb, S(20), ca_(0x90141925u));
    rrect_outline_glow(back_, sb, S(20), 1, PHASE * 0.3f, static_cast<int>(120.0f * content_alpha_));
    const int mx0 = sb.x + S(18), my0 = sb.y + sb.h / 2;
    ring(back_, mx0 - 1, my0 - 1, S(7), 2, ca_(kAccentBlue));
    line(back_, mx0 + S(4), my0 + S(4), mx0 + S(9), my0 + S(9), ca_(kAccentBlue), 2);
    if (menu_search_.empty()) draw_text(back_, sb.x + S(40), sb.y + (sb.h - th) / 2, fit_text(t("Search apps and settings...", "Поиск приложений и настроек..."), sb.w - S(60), sc), ca_(kTextFaint), sc);
    else {
        const int ex = draw_text(back_, sb.x + S(40), sb.y + (sb.h - th) / 2, fit_text(menu_search_, sb.w - S(70), sc), ca_(kText), sc);
        fill(back_, {ex + 1, sb.y + (sb.h - th) / 2, std::max(1, S(2)), th}, ca_(kAccentBlue));
    }
    y += sb.h + S(12);
    // category chips
    const auto cats = categories();
    int cx = r.x + pad;
    for (int i = 0; i < static_cast<int>(cats.size()); ++i) {
        const std::string label = category_label(i);
        const int w = text_width(label, 1) + S(22);
        if (cx + w > r.right() - pad) break;
        const Rect chip = {cx, y, w, S(28)};
        const bool on = i == menu_cat_;
        const std::string key = "menu:cat:" + std::to_string(i);
        const float hv = hover_amount(key);
        rrect(back_, chip, S(14), ca_(on ? 0xFF2F5FC0u : mix(kGraphite700, kGraphite500, hv)));
        if (on) rrect_outline(back_, chip, S(14), 1, ca_(kAccentBlue));
        draw_text(back_, chip.x + S(11), chip.y + (chip.h - font_h()) / 2, label, ca_(on ? 0xFFFFFFFFu : kText), 1);
        add_hit(chip, [this, i](int) { menu_cat_ = i; menu_page_ = 0; menu_sel_ = 0; dirty_ = true; }, key);
        cx += w + S(8);
    }
    y += S(28) + S(14);
    // application grid
    const auto apps = menu_apps();
    const int per = kMenuCols * 4;
    const int pages = std::max(1, (static_cast<int>(apps.size()) + per - 1) / per);
    menu_page_ = clampi(menu_page_, 0, pages - 1);
    const int cell_w = (r.w - 2 * pad) / kMenuCols, cell_h = S(92);
    if (apps.empty()) draw_text(back_, r.x + pad, y + S(20), t("Nothing found", "Ничего не найдено"), ca_(kTextDim), sc);
    for (int i = 0; i < per; ++i) {
        const int idx = menu_page_ * per + i;
        if (idx >= static_cast<int>(apps.size())) break;
        const AppDef* a = apps[static_cast<std::size_t>(idx)];
        const Rect cell = {r.x + pad + (i % kMenuCols) * cell_w, y + (i / kMenuCols) * cell_h, cell_w, cell_h};
        const std::string key = "menu:app:" + a->id;
        const float hv = hover_amount(key);
        const bool selected = idx == menu_sel_;
        if (hv > 0.02f || selected) rrect(back_, cell.inset(S(2)), S(14), ca_(with_alpha(0xFFFFFFFFu, selected ? 26 : static_cast<int>(20.0f * hv))));
        if (selected) {  // visible keyboard focus: an outline, not only a tint
            rrect_outline(back_, cell.inset(S(2)), S(14), 2, ca_(kAccentBlue));
        }
        const int isz = S(kMenuIcon);
        const Rect ir = {cell.x + (cell.w - isz) / 2, cell.y + S(8) - static_cast<int>(hv * static_cast<float>(S(3))), isz, isz};
        draw_icon(ir, a->icon, hv, true, S(15));
        const std::string name = fit_text(a->name(set_.ru), cell.w - S(6), 1);
        draw_text(back_, cell.x + (cell.w - text_width(name, 1)) / 2, ir.bottom() + S(8), name, ca_(kText), 1);
        const std::string id = a->id;
        add_hit(cell, [this, id](int) { launch_app(id); }, key);  // the name is already under the icon: no tooltip
    }
    // pagination
    if (pages > 1) {
        const int dy = r.bottom() - S(26);
        const int total = pages * S(14);
        for (int i = 0; i < pages; ++i) {
            const Rect dot = {r.x + (r.w - total) / 2 + i * S(14), dy - S(6), S(14), S(14)};
            circle(back_, dot.x + dot.w / 2, dot.y + dot.h / 2, i == menu_page_ ? S(4) : S(3), ca_(i == menu_page_ ? kAccentBlue : kTextFaint));
            add_hit(dot, [this, i](int) { menu_page_ = i; dirty_ = true; });
        }
    }
    (void)p;
}

static std::string state_word(bool ru, char s) {
    switch (s) {
        case 'R': return ru ? "работает" : "running";
        case 'S': return ru ? "ожидает" : "sleeping";
        case 'D': return ru ? "ждёт диск" : "disk wait";
        case 'Z': return ru ? "зомби" : "zombie";
        case 'T': return ru ? "остановлен" : "stopped";
        default: return ru ? "неизвестно" : "unknown";
    }
}

void Desktop::draw_tasks(Popup& p, Rect r) {
    const int pad = S(14), sc = set_.scale;
    const auto list = tasks_.background_tasks();
    draw_text(back_, r.x + pad, r.y + pad, t("Background tasks", "Фоновые задачи"), ca_(kText), sc);
    const std::string cnt = std::to_string(list.size());
    draw_text(back_, r.right() - pad - text_width(cnt, sc), r.y + pad, cnt, ca_(kAccentTeal), sc);
    const int top = r.y + pad + font_h() * sc + S(8);
    const int details_h = S(116);
    const Rect area = {r.x + S(8), top, r.w - S(16), r.bottom() - top - details_h - S(6)};
    const int cell_w = area.w / kTasksCols, cell_h = S(74);
    const int sec_head = S(24);
    const int sections = std::max(1, (static_cast<int>(list.size()) + kTasksPerSection - 1) / kTasksPerSection);
    const int rows_per_sec = kTasksPerSection / kTasksCols;
    const int sec_h = sec_head + rows_per_sec * cell_h;
    const int total_h = sections * sec_h;
    tasks_scroll_ = clampf(tasks_scroll_, 0.0f, static_cast<float>(std::max(0, total_h - area.h)));
    const Rect save = back_.clip;
    back_.clip = save.intersect(area);
    for (int s = 0; s < sections; ++s) {
        const int sy = area.y + s * sec_h - static_cast<int>(tasks_scroll_);
        const int from = s * kTasksPerSection + 1, to = std::min(static_cast<int>(list.size()), (s + 1) * kTasksPerSection);
        if (list.size() > static_cast<std::size_t>(kTasksPerSection) || s == 0)
            draw_text(back_, area.x + S(6), sy + S(4), t("Section ", "Раздел ") + std::to_string(s + 1) + "  (" + std::to_string(from) + "-" + std::to_string(to) + ")", ca_(kTextDim), 1);
        for (int i = 0; i < kTasksPerSection; ++i) {
            const int idx = s * kTasksPerSection + i;
            if (idx >= static_cast<int>(list.size())) break;
            const BgTask& bt = list[static_cast<std::size_t>(idx)];
            const Rect cell = {area.x + (i % kTasksCols) * cell_w, sy + sec_head + (i / kTasksCols) * cell_h, cell_w, cell_h};
            if (cell.bottom() < area.y || cell.y > area.bottom()) continue;
            const std::string key = "task:" + bt.key;
            const float hv = hover_amount(key);
            const bool sel = tasks_sel_ == bt.key;
            if (sel || hv > 0.02f) rrect(back_, cell.inset(S(2)), S(12), ca_(with_alpha(0xFFFFFFFFu, sel ? 28 : static_cast<int>(18.0f * hv))));
            if (sel) rrect_outline(back_, cell.inset(S(2)), S(12), 2, ca_(kAccentBlue));
            const int isz = S(38);
            const Rect ir = {cell.x + (cell.w - isz) / 2, cell.y + S(6), isz, isz};
            draw_icon(ir, bt.icon, hv, false, S(11));
            if (bt.critical) {  // lock badge: criticality is shown by shape, not only by color
                circle(back_, ir.right() - S(2), ir.y + S(2), S(8), ca_(kWarn));
                const int lx = ir.right() - S(2), ly = ir.y + S(3);
                rrect(back_, {lx - S(3), ly - S(1), S(6), S(5)}, 1, ca_(0xFF1D2330u));
                ring(back_, lx, ly - S(2), S(2), 1, ca_(0xFF1D2330u));
            }
            const std::string name = fit_text(bt.name, cell.w - S(4), 1);
            draw_text(back_, cell.x + (cell.w - text_width(name, 1)) / 2, ir.bottom() + S(6), name, ca_(kText), 1);
            const std::string key2 = bt.key;
            add_hit(cell.intersect(area), [this, key2](int) { tasks_sel_ = key2; dirty_ = true; }, key);
        }
    }
    back_.clip = save;
    if (total_h > area.h) {  // scroll bar
        const int th = std::max(S(20), area.h * area.h / total_h);
        const int ty = area.y + static_cast<int>(static_cast<float>(area.h - th) * tasks_scroll_ / static_cast<float>(total_h - area.h));
        rrect(back_, {area.right() - S(4), ty, S(3), th}, 2, ca_(0x60FFFFFFu));
    }
    // details of the selected task
    const Rect dt = {r.x + S(10), r.bottom() - details_h - S(4), r.w - S(20), details_h};
    rrect(back_, dt, S(14), ca_(0x70141925u));
    const BgTask* sel = nullptr;
    for (const auto& bt : list) if (bt.key == tasks_sel_) sel = &bt;
    if (!sel) {
        draw_text(back_, dt.x + S(12), dt.y + S(12), fit_text(t("Select a task to see details", "Выберите задачу, чтобы увидеть подробности"), dt.w - S(24), 1), ca_(kTextDim), 1);
        draw_text(back_, dt.x + S(12), dt.y + S(34), fit_text(t("Apps you started stay in the dock while they have a window.", "Приложения, которые вы запустили, остаются на панели, пока у них есть окно."), dt.w - S(24), 1), ca_(kTextFaint), 1);
        return;
    }
    draw_text(back_, dt.x + S(12), dt.y + S(10), fit_text(sel->name + (sel->service ? t("  (system service)", "  (системная служба)") : t("  (background app)", "  (фоновое приложение)")), dt.w - S(24), 1), ca_(kText), 1);
    char line1[96];
    std::snprintf(line1, sizeof line1, "%s: %s   PID %d   %s %ld KiB   CPU %.1f%%", t("State", "Состояние").c_str(), state_word(set_.ru, sel->state).c_str(),
                  static_cast<int>(sel->pid), t("RAM", "ОЗУ").c_str(), sel->rss_kb, static_cast<double>(sel->cpu));
    draw_text(back_, dt.x + S(12), dt.y + S(30), fit_text(line1, dt.w - S(24), 1), ca_(kTextDim), 1);
    if (!sel->detail.empty()) draw_text(back_, dt.x + S(12), dt.y + S(48), fit_text(sel->detail, dt.w - S(24), 1), ca_(kTextFaint), 1);
    const Rect kb = {dt.right() - S(12) - S(130), dt.bottom() - S(38), S(130), S(28)};
    if (sel->critical) {
        draw_text(back_, dt.x + S(12), dt.bottom() - S(34), fit_text(t("Critical system process: it cannot be ended.", "Критический системный процесс: завершить нельзя."), dt.w - S(24), 1), ca_(kWarn), 1);
    } else {
        const BgTask copy = *sel;
        draw_button(kb, t("End task", "Завершить"), [this, kb, copy](int) {
            confirm(kb, t("End \"", "Завершить «") + copy.name + t("\"? Unsaved data may be lost.", "»? Несохранённые данные могут быть потеряны."), [this, copy]() {
                if (copy.iid) tasks_.ask_close(copy.iid); else tasks_.terminate_pid(copy.pid, false);
                tasks_sel_.clear();
                note_toast(t("Asked to end ", "Завершение: ") + copy.name);
            });
        }, false, true);
    }
    (void)p;
}

void Desktop::draw_net(Rect r) {
    const int pad = S(16), sc = set_.scale;
    draw_text(back_, r.x + pad, r.y + pad, t("Network", "Сеть"), ca_(kText), sc);
    int y = r.y + pad + font_h() * sc + S(12);
    DIR* d = opendir("/sys/class/net");
    int shown = 0;
    if (d) {
        const int fd = socket(AF_INET, SOCK_DGRAM, 0);
        while (struct dirent* e = readdir(d)) {
            if (e->d_name[0] == '.') continue;
            std::string st = "?";
            std::ifstream f(std::string("/sys/class/net/") + e->d_name + "/operstate");
            std::getline(f, st);
            char ip[32] = "-";
            if (fd >= 0) {
                struct ifreq ifr;
                std::memset(&ifr, 0, sizeof ifr);
                std::strncpy(ifr.ifr_name, e->d_name, IFNAMSIZ - 1);
                if (ioctl(fd, SIOCGIFADDR, &ifr) == 0) inet_ntop(AF_INET, &reinterpret_cast<struct sockaddr_in*>(&ifr.ifr_addr)->sin_addr, ip, sizeof ip);
            }
            const bool up = st == "up" || st == "unknown";
            draw_text(back_, r.x + pad, y, std::string(e->d_name) + (up ? "  [on]" : "  [off]"), ca_(up ? kOk : kTextDim), 1);
            draw_text(back_, r.x + pad + S(130), y, ip, ca_(kText), 1);
            y += S(24);
            ++shown;
        }
        if (fd >= 0) close(fd);
        closedir(d);
    }
    if (shown <= 1) draw_text(back_, r.x + pad, y + S(6), fit_text(t("No network connection.", "Нет сетевого подключения."), r.w - 2 * pad, 1), ca_(kTextDim), 1);
}

void Desktop::draw_sound(Rect r) {
    const int pad = S(16), sc = set_.scale;
    draw_text(back_, r.x + pad, r.y + pad, t("Sound", "Звук"), ca_(kText), sc);
    const bool have = access("/proc/asound/cards", F_OK) == 0;
    draw_text(back_, r.x + pad, r.y + pad + font_h() * sc + S(14), fit_text(have ? t("Sound device found.", "Звуковое устройство найдено.") : t("No sound device.", "Звукового устройства нет."), r.w - 2 * pad, 1), ca_(kTextDim), 1);
    draw_text(back_, r.x + pad, r.y + pad + font_h() * sc + S(36), fit_text(t("Audio is not part of this build.", "Звук в этой сборке не поддерживается."), r.w - 2 * pad, 1), ca_(kTextFaint), 1);
}

void Desktop::draw_calendar(Rect r) {
    const int pad = S(16), sc = set_.scale;
    time_t tt = time(nullptr);
    struct tm now;
    localtime_r(&tt, &now);
    static const char* const mru[] = {"Январь", "Февраль", "Март", "Апрель", "Май", "Июнь", "Июль", "Август", "Сентябрь", "Октябрь", "Ноябрь", "Декабрь"};
    static const char* const men[] = {"January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November", "December"};
    char head[64];
    std::snprintf(head, sizeof head, "%s %d", set_.ru ? mru[now.tm_mon] : men[now.tm_mon], now.tm_year + 1900);
    draw_text(back_, r.x + pad, r.y + pad, head, ca_(kText), sc);
    char hm[16];
    std::strftime(hm, sizeof hm, "%H:%M:%S", &now);
    draw_text(back_, r.right() - pad - text_width(hm, 1), r.y + pad + S(2), hm, ca_(kTextDim), 1);
    const char* wd = set_.ru ? "ПнВтСрЧтПтСбВс" : "MoTuWeThFrSaSu";
    const int cw = (r.w - 2 * pad) / 7, top = r.y + pad + font_h() * sc + S(14);
    for (int i = 0; i < 7; ++i) {
        std::string d(wd + i * 2 * (set_.ru ? 2 : 1), set_.ru ? 4 : 2);
        d = set_.ru ? std::string(wd).substr(static_cast<std::size_t>(i) * 4, 4) : std::string(wd).substr(static_cast<std::size_t>(i) * 2, 2);
        draw_text(back_, r.x + pad + i * cw + (cw - text_width(d, 1)) / 2, top, d, ca_(kTextFaint), 1);
    }
    struct tm first = now;
    first.tm_mday = 1;
    mktime(&first);
    const int offset = (first.tm_wday + 6) % 7;
    static const int dim[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    int days = dim[now.tm_mon];
    const int yr = now.tm_year + 1900;
    if (now.tm_mon == 1 && ((yr % 4 == 0 && yr % 100 != 0) || yr % 400 == 0)) days = 29;
    for (int day = 1; day <= days; ++day) {
        const int pos = offset + day - 1;
        const Rect c = {r.x + pad + (pos % 7) * cw, top + S(24) + (pos / 7) * S(34), cw, S(32)};
        const std::string s = std::to_string(day);
        const bool today = day == now.tm_mday;
        if (today) { rrect(back_, c.inset(S(2)), S(10), ca_(0xFF2F5FC0u)); rrect_outline(back_, c.inset(S(2)), S(10), 1, ca_(kAccentBlue)); }
        draw_text(back_, c.x + (c.w - text_width(s, 1)) / 2, c.y + (c.h - font_h()) / 2, s, ca_(today ? 0xFFFFFFFFu : kText), 1);
    }
}

void Desktop::draw_notify(Rect r) {
    const int pad = S(16), sc = set_.scale;
    draw_text(back_, r.x + pad, r.y + pad, t("Notifications", "Уведомления"), ca_(kText), sc);
    int y = r.y + pad + font_h() * sc + S(12);
    const auto& j = tasks_.journal();
    if (j.empty()) draw_text(back_, r.x + pad, y, t("No notifications.", "Уведомлений нет."), ca_(kTextDim), 1);
    int shown = 0;
    for (auto it = j.rbegin(); it != j.rend() && y + S(26) < r.bottom() - S(8); ++it, ++shown) {
        const long ago = (now_ms() - it->t_ms) / 1000;
        char when[24];
        std::snprintf(when, sizeof when, ago < 60 ? "%lds" : "%ldm", ago < 60 ? ago : ago / 60);
        draw_text(back_, r.x + pad, y, std::string(it->alert ? "(!) " : "    ") + fit_text(it->text, r.w - 2 * pad - S(70), 1), ca_(it->alert ? kWarn : kTextDim), 1);
        draw_text(back_, r.right() - pad - text_width(when, 1), y, when, ca_(kTextFaint), 1);
        y += S(24);
    }
}

void Desktop::draw_power(Rect r) {
    const int pad = S(16), bh = S(38);
    int y = r.y + pad;
    struct Item { std::string label; int kind; };
    const Item items[3] = {{t("Power off", "Выключение"), 0}, {t("Reboot", "Перезагрузка"), 1}, {t("Leave to console", "Выход в консоль"), 2}};
    for (const Item& it : items) {
        const Rect b = {r.x + pad, y, r.w - 2 * pad, bh};
        const int kind = it.kind;
        draw_button(b, it.label, [this, b, kind](int) {
            if (kind == 2) { leave_desktop(); return; }
            confirm(b, kind == 0 ? t("Power off the computer?", "Выключить компьютер?") : t("Reboot the computer?", "Перезагрузить компьютер?"),
                    [this, kind]() { do_power(kind == 1); });
        }, false, kind == 0);
        y += bh + S(10);
    }
}

void Desktop::draw_layout(Rect r) {
    const int pad = S(16), bh = S(34);
    draw_text(back_, r.x + pad, r.y + pad, t("Keyboard layout", "Раскладка клавиатуры"), ca_(kText), 1);
    for (int k = 0; k < 2; ++k) {
        const Rect b = {r.x + pad + k * ((r.w - 2 * pad) / 2 + S(4)), r.y + pad + S(30), (r.w - 2 * pad) / 2 - S(4), bh};
        const bool cur = (k == 1) == layout_ru_;
        draw_button(b, std::string(k == 0 ? "EN" : "RU") + (cur ? " *" : ""), [this, k](int) { layout_ru_ = k == 1; dirty_ = true; }, cur);
    }
}

void Desktop::draw_settings(Rect r) {
    const int pad = S(18), sc = set_.scale, rh = S(40);
    draw_text(back_, r.x + pad, r.y + pad, t("Settings", "Параметры"), ca_(kText), sc);
    int y = r.y + pad + font_h() * sc + S(14);
    auto row = [&](const std::string& label, const std::string& value, std::function<void(int)> fn) {
        draw_text(back_, r.x + pad, y + (S(32) - font_h()) / 2, fit_text(label, r.w - 2 * pad - S(110), 1), ca_(kText), 1);
        const Rect b = {r.right() - pad - S(100), y, S(100), S(32)};
        draw_button(b, value, std::move(fn));
        y += rh;
    };
    row(t("Reduce motion", "Меньше движения"), set_.reduce_motion ? t("On", "Вкл") : t("Off", "Выкл"), [this](int) { set_.reduce_motion = !set_.reduce_motion; apply_settings(); });
    row(t("Transparency", "Прозрачность"), set_.transparency ? t("On", "Вкл") : t("Off", "Выкл"), [this](int) { set_.transparency = !set_.transparency; apply_settings(); });
    row(t("Interface scale", "Масштаб интерфейса"), set_.scale == 1 ? "100%" : "200%", [this](int) { set_.scale = set_.scale == 1 ? 2 : 1; close_all_popups(); apply_settings(); });
    row(t("Language", "Язык"), set_.ru ? "RU" : "EN", [this](int) { set_.ru = !set_.ru; apply_settings(); });
    row(t("Win key opens menu", "Клавиша Win открывает меню"), set_.winkey ? t("On", "Вкл") : t("Off", "Выкл"), [this](int) { set_.winkey = !set_.winkey; apply_settings(); });
    draw_text(back_, r.x + pad, y + S(6), fit_text(t("Saved in /etc/difiw/desktop.conf", "Сохраняется в /etc/difiw/desktop.conf"), r.w - 2 * pad, 1), ca_(kTextFaint), 1);
}

void Desktop::draw_app_windows(Popup& p, Rect r) {
    const int pad = S(12);
    int y = r.y + pad;
    p.rect.h = S(24) + 0;
    std::vector<int> ids;
    for (auto& in : tasks_.instances()) if (in.app_id == p.app_id) for (int id : in.wins) ids.push_back(id);
    draw_text(back_, r.x + pad, y, t("Windows", "Окна"), ca_(kTextDim), 1);
    y += S(24);
    for (int id : ids) {
        Window* w = win(id);
        if (!w) continue;
        const Rect b = {r.x + pad, y, r.w - 2 * pad, S(30)};
        draw_button(b, fit_text(w->title, b.w - S(24), set_.scale) + (w->minimized ? " _" : ""), [this, id](int) {
            if (Window* ww = win(id)) { if (ww->minimized) restore(*ww); else set_focus(id); }
            close_all_popups();
        }, id == focus_);
        y += S(36);
    }
    (void)r;
}

void Desktop::draw_app_menu(Popup& p, Rect r) {
    const int pad = S(14), bh = S(34);
    const AppDef* a = tasks_.app(p.app_id);
    draw_text(back_, r.x + pad, r.y + pad, fit_text(a ? a->name(set_.ru) : p.app_id, r.w - 2 * pad, 1), ca_(kText), 1);
    int y = r.y + pad + S(26);
    const std::string app = p.app_id;
    auto item = [&](const std::string& label, std::function<void(int)> fn, bool danger = false) {
        draw_button({r.x + pad, y, r.w - 2 * pad, bh}, label, std::move(fn), false, danger);
        y += bh + S(6);
    };
    item(t("New window", "Новое окно"), [this, app](int) { launch_app(app); });
    item(t("Close window", "Закрыть окно"), [this, app](int) { close_app_windows(app); close_all_popups(); });
    item(t("Quit app", "Закрыть приложение"), [this, app](int) { close_app(app); close_all_popups(); }, true);
}

void Desktop::draw_confirm(Popup& p, Rect r) {
    const int pad = S(16);
    // wrap the question
    std::string rest = p.text;
    const int cols = std::max(8, (r.w - 2 * pad) / font_w());
    int y = r.y + pad;
    while (!rest.empty() && y < r.bottom() - S(56)) {
        std::size_t cut = 0;
        for (int i = 0; i < cols && cut < rest.size(); ++i) cut = u8_next(rest, cut);
        if (cut < rest.size()) {  // break at a space when possible
            const std::size_t sp = rest.rfind(' ', cut);
            if (sp != std::string::npos && sp > 0) cut = sp;
        }
        draw_text(back_, r.x + pad, y, rest.substr(0, cut), ca_(kText), 1);
        rest.erase(0, cut);
        while (!rest.empty() && rest[0] == ' ') rest.erase(0, 1);
        y += S(20);
    }
    const int bw = S(110), bh = S(32);
    const Rect no = {r.right() - pad - bw, r.bottom() - pad - bh, bw, bh}, yes = {no.x - S(8) - bw, no.y, bw, bh};
    draw_button(no, t("Cancel", "Отмена"), [&p, this](int) { p.closing = true; p.t_close = now_ms(); dirty_ = true; });
    auto act = p.on_yes;
    draw_button(yes, t("Yes", "Да"), [this, act](int) {
        close_all_popups();
        if (act) act();
    }, true);
}

// ----------------------------------------------------------------------- toast / tip
void Desktop::draw_toast() {
    if (toast_.empty()) return;
    const int w = text_width(toast_, set_.scale) + S(40), h = S(40);
    const Rect d = dock_rect();
    const Rect r = {(W_ - w) / 2, d.y - S(70), w, h};
    draw_glass(r, S(20), 10);
    draw_text(back_, r.x + S(20), r.y + (h - font_h() * set_.scale) / 2, toast_, kText, set_.scale);
}

void Desktop::draw_tip() {
    if (tip_.empty() || hover_.empty()) return;
    for (const auto& p : popups_) if (!p.closing && p.rect.has(mx_, my_)) { /* tips are drawn above popups too */ }
    const int w = text_width(tip_, 1) + S(20), h = S(28);
    int x = tip_anchor_.x + tip_anchor_.w / 2 - w / 2;
    x = clampi(x, S(6), W_ - w - S(6));
    int y = tip_anchor_.y - h - S(8);
    if (y < S(6)) y = tip_anchor_.bottom() + S(8);
    const Rect r = {x, y, w, h};
    rrect(back_, r, S(10), 0xF0141925u);
    rrect_outline(back_, r, S(10), 1, 0x40FFFFFFu);
    draw_text(back_, r.x + S(10), r.y + (h - font_h()) / 2, tip_, kText, 1);
}

// ---------------------------------------------------------------------------- render
void Desktop::render() {
    frame_ms_ = now_ms();
    hits_.clear();
    back_.reset_clip();
    std::memcpy(back_.px, base_.px.data(), base_.px.size() * 4);  // wallpaper + scrim, prepared once
struct timespec t0, t1, t2, t3;    clock_gettime(CLOCK_MONOTONIC, &t0);    draw_windows();    clock_gettime(CLOCK_MONOTONIC, &t1);    draw_dock();    clock_gettime(CLOCK_MONOTONIC, &t2);    draw_popups();    clock_gettime(CLOCK_MONOTONIC, &t3);    auto us = [](const timespec& a, const timespec& b) { return (b.tv_sec - a.tv_sec) * 1000000L + (b.tv_nsec - a.tv_nsec) / 1000L; };    perf_win_us_ = us(t0, t1); perf_dock_us_ = us(t1, t2); perf_pop_us_ = us(t2, t3);
    draw_toast();
    if (show_perf_) {
        char b[96];
        std::snprintf(b, sizeof b, "render %.1f ms  video write %.1f ms  windows %d", static_cast<double>(perf_render_us_) / 1000.0, static_cast<double>(perf_present_us_) / 1000.0, static_cast<int>(wins_.size()));
        const Rect r = {S(8), S(8), text_width(b, 1) + S(20), S(26)};
        rrect(back_, r, S(8), 0xE0141925u);
        draw_text(back_, r.x + S(10), r.y + (r.h - font_h()) / 2, b, kAccentTeal, 1);
    }
    if (!hover_.empty() && popups_.empty()) draw_tip();
    else if (!hover_.empty()) draw_tip();
}

}  // namespace dg
