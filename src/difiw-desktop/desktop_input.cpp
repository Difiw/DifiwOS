// SPDX-License-Identifier: MIT
// Desktop input routing and shell actions (launch, activate, popups, keyboard layout).
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#include "desktop.hpp"

namespace dg {

using namespace proto;

// ----------------------------------------------------------------- keyboard layout
namespace {
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
    static bool done = false;
    if (done) return;
    done = true;
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

bool is_letter(const std::string& s) {
    if (s.empty()) return false;
    std::size_t i = 0;
    const std::uint32_t cp = u8_codepoint(s, i);
    return (cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z') || (cp >= 0x410 && cp <= 0x44F) || cp == 0x451 || cp == 0x401;
}
}  // namespace

std::string Desktop::key_text(int code) const {
    init_keys();
    if (code < 0 || code >= 128 || ctrl_ || alt_) return "";
    const std::string& base = layout_ru_ ? ru_n[code] : lat_n[code];
    if (base.empty()) return "";
    bool sh = shift_;
    if (caps_ && is_letter(base)) sh = !sh;
    return sh ? (layout_ru_ ? ru_s[code] : lat_s[code]) : base;
}

// ----------------------------------------------------------------------- geometry
Rect Desktop::title_btn(const Window& w, int idx) const {
    const Rect f = frame_of(w);
    const int d = S(20), gap = S(8);
    const int x = f.right() - S(14) - d - idx * (d + gap);
    return {x, f.y + (title_h() - d) / 2, d, d};
}

int Desktop::edge_at(const Window& w, int x, int y) const {
    if (w.maximized) return 0;
    const Rect f = frame_of(w);
    const int g = S(tok::kResizeGrip);
    int e = 0;
    if (x < f.x + g) e |= 1;
    if (x >= f.right() - g) e |= 2;
    if (y < f.y + g / 2) e |= 4;
    if (y >= f.bottom() - g) e |= 8;
    return e;
}

Rect Desktop::anchor_of(const std::string& key) const {
    for (const auto& h : hits_) if (h.hover_key == key) return h.r;
    return dock_rect();
}

Hit* Desktop::hit_at(int x, int y) {
    for (std::size_t i = hits_.size(); i-- > 0;)
        if (hits_[i].r.has(x, y)) return &hits_[i];
    return nullptr;
}

// ------------------------------------------------------------------------ pointer
void Desktop::route_mouse_to_window(Window& w, int kind, int wheel) {
    if (kind == 0) {  // pointer motion is coalesced: a busy app must not be flooded
        const long now = now_ms();
        if (now - last_move_send_ms_ < 10) return;
        last_move_send_ms_ = now;
    }
    const Rect c = content_of(w);
    Msg m;
    m.type = S_MOUSE;
    m.i[0] = w.id; m.i[1] = mx_ - c.x; m.i[2] = my_ - c.y; m.i[3] = buttons_; m.i[4] = kind; m.i[5] = wheel;
    send_msg(w.client, m, -1, MSG_DONTWAIT);
}

void Desktop::on_pointer_move(int nx, int ny) {
    const int ox = mx_, oy = my_;
    mx_ = clampi(nx, 0, W_ - 1);
    my_ = clampi(ny, 0, H_ - 1);
    if (mx_ == ox && my_ == oy) return;

    if (drag_ == Drag::Move) {
        if (Window* w = win(drag_win_)) { w->x = mx_ - drag_dx_; w->y = my_ - drag_dy_; clamp_window(*w); dirty_ = true; }
    } else if (drag_ == Drag::Resize) {
        if (Window* w = win(drag_win_)) {
            Rect r = drag_orig_;
            const int dx = mx_ - drag_dx_, dy = my_ - drag_dy_;
            const int minw = S(tok::kMinWindowW) + 2 * S(6), minh = title_h() + S(tok::kMinWindowH) + S(6);
            if (drag_edge_ & 1) { const int nxl = std::min(r.x + dx, r.right() - minw); r.w = r.right() - nxl; r.x = nxl; }
            if (drag_edge_ & 2) r.w = std::max(minw, r.w + dx);
            if (drag_edge_ & 4) { const int nyt = std::min(r.y + dy, r.bottom() - minh); r.h = r.bottom() - nyt; r.y = nyt; }
            if (drag_edge_ & 8) r.h = std::max(minh, r.h + dy);
            w->x = r.x; w->y = r.y;
            w->cw = r.w - 2 * S(6);
            w->ch = r.h - title_h() - S(6);
            dirty_ = true;
        }
    } else if (drag_ == Drag::Content) {
        if (Window* w = win(drag_win_)) route_mouse_to_window(*w, 0, 0);
    } else {
        // hover tracking (shell elements) and pointer events for the window under the cursor
        Hit* h = hit_at(mx_, my_);
        const std::string key = h ? h->hover_key : "";
        const std::string tip = h ? h->tip : "";
        if (key != hover_ || tip != tip_) { hover_ = key; tip_ = tip; if (h) tip_anchor_ = h->r; dirty_ = true; }
        if (!h && popups_.empty()) {
            if (Window* w = top_window_at(mx_, my_)) {
                if (content_of(*w).has(mx_, my_)) route_mouse_to_window(*w, 0, 0);
            }
        }
    }
    // cheap cursor update when nothing else changed
    if (!dirty_ && fb_.mem) {
        blit_region(ox, oy, 12, 19);
        draw_cursor();
    }
}

void Desktop::on_wheel(int delta) {
    if (!popups_.empty()) {
        Popup& p = popups_.back();
        if (p.rect.has(mx_, my_)) {
            if (p.kind == PopupKind::Tasks) tasks_scroll_ = std::max(0.0f, tasks_scroll_ - static_cast<float>(delta) * static_cast<float>(S(34)));
            else if (p.kind == PopupKind::Menu) menu_page_ = std::max(0, menu_page_ - delta);
            dirty_ = true;
            return;
        }
    }
    if (dock_left_rect().has(mx_, my_)) { dock_scroll_target_ = std::max(0.0f, dock_scroll_target_ - static_cast<float>(delta)); dirty_ = true; return; }
    if (Window* w = top_window_at(mx_, my_))
        if (content_of(*w).has(mx_, my_)) route_mouse_to_window(*w, 3, delta);
}

void Desktop::on_button(int button, bool down) {
    if (button == 1) buttons_ = down ? (buttons_ | 1) : (buttons_ & ~1);
    else if (button == 2) buttons_ = down ? (buttons_ | 2) : (buttons_ & ~2);
    dirty_ = true;

    if (!down) {
        if (drag_ == Drag::Content) {
            if (Window* w = win(drag_win_)) route_mouse_to_window(*w, 2, 0);
        } else if (drag_ == Drag::Resize) {
            if (Window* w = win(drag_win_)) configure(*w, w->cw, w->ch);
        }
        drag_ = Drag::None;
        return;
    }

    // 1. shell widgets (popups are registered last, so they win over the dock)
    Hit* h = hit_at(mx_, my_);
    if (h) {
        if (!h->hover_key.empty()) press_t_[h->hover_key] = now_ms();
        auto fn = h->fn;
        if (fn) fn(button);
        return;
    }
    // a click on a popup's empty area keeps it open; anywhere else closes popups
    for (const auto& p : popups_) if (!p.closing && p.rect.has(mx_, my_)) return;
    if (!popups_.empty()) close_all_popups();

    // 2. windows
    if (button != 1) return;
    Window* w = top_window_at(mx_, my_);
    if (!w) return;
    const int id = w->id;
    set_focus(id);
    w = win(id);
    if (!w) return;
    if (title_btn(*w, 0).has(mx_, my_)) { request_close_window(*w); return; }
    if (title_btn(*w, 1).has(mx_, my_)) { toggle_maximize(*w); return; }
    if (title_btn(*w, 2).has(mx_, my_)) { minimize(*w); return; }
    const int edge = edge_at(*w, mx_, my_);
    if (edge) {
        drag_ = Drag::Resize; drag_win_ = id; drag_edge_ = edge;
        drag_orig_ = frame_of(*w); drag_dx_ = mx_; drag_dy_ = my_;
        return;
    }
    const Rect f = frame_of(*w);
    if (my_ < f.y + title_h()) {
        const long t = now_ms();
        if (last_title_click_win_ == id && t - last_title_click_ms_ < tok::kDoubleClickMs) { toggle_maximize(*w); last_title_click_win_ = 0; return; }
        last_title_click_win_ = id; last_title_click_ms_ = t;
        if (w->maximized) return;
        drag_ = Drag::Move; drag_win_ = id; drag_dx_ = mx_ - w->x; drag_dy_ = my_ - w->y;
        return;
    }
    if (content_of(*w).has(mx_, my_)) {
        drag_ = Drag::Content; drag_win_ = id;
        route_mouse_to_window(*w, 1, 0);
    }
}

// ---------------------------------------------------------------------- keyboard
void Desktop::send_key_to_focus(int code, int state, const std::string& text) {
    Window* w = win(focus_);
    if (!w) return;
    Msg m;
    m.type = S_KEY;
    m.i[0] = w->id; m.i[1] = code; m.i[2] = state;
    m.i[3] = (shift_ ? 1 : 0) | (ctrl_ ? 2 : 0) | (alt_ ? 4 : 0);
    set_str(m, text.c_str());
    send_msg(w->client, m, -1, MSG_DONTWAIT);
}

bool Desktop::handle_popup_key(int code, int state, const std::string& text) {
    if (popups_.empty() || state == 0) return !popups_.empty() && state == 0 ? true : false;
    Popup& p = popups_.back();
    if (code == KEY_ESC) { p.closing = true; p.t_close = now_ms(); dirty_ = true; return true; }
    if (p.kind == PopupKind::Confirm) {
        if (code == KEY_ENTER) { auto yes = p.on_yes; p.closing = true; p.t_close = now_ms(); if (yes) yes(); }
        dirty_ = true;
        return true;
    }
    if (p.kind != PopupKind::Menu) return false;
    const auto apps = menu_apps();
    const int n = static_cast<int>(apps.size());
    const int per = tok::kMenuCols * 4;
    if (code == KEY_ENTER) {
        const int idx = menu_sel_ >= 0 ? menu_sel_ : 0;
        if (idx < n) launch_app(apps[static_cast<std::size_t>(idx)]->id);
    } else if (code == KEY_BACKSPACE) {
        if (!menu_search_.empty()) menu_search_.erase(u8_prev(menu_search_, menu_search_.size()));
        menu_sel_ = 0; menu_page_ = 0;
    } else if (code == KEY_RIGHT) { if (menu_sel_ + 1 < n) ++menu_sel_; }
    else if (code == KEY_LEFT) { if (menu_sel_ > 0) --menu_sel_; }
    else if (code == KEY_DOWN) { if (menu_sel_ + tok::kMenuCols < n) menu_sel_ += tok::kMenuCols; }
    else if (code == KEY_UP) { if (menu_sel_ - tok::kMenuCols >= 0) menu_sel_ -= tok::kMenuCols; }
    else if (code == KEY_TAB) { menu_cat_ = (menu_cat_ + 1) % static_cast<int>(categories().size()); menu_sel_ = 0; menu_page_ = 0; }
    else if (!text.empty() && text != " " ) { menu_search_ += text; menu_sel_ = 0; menu_page_ = 0; }
    else if (text == " " && !menu_search_.empty()) { menu_search_ += text; }
    if (menu_sel_ >= 0) menu_page_ = menu_sel_ / per;
    dirty_ = true;
    return true;
}

void Desktop::on_key(int code, int state) {
    const bool down = state != 0;
    switch (code) {
        case KEY_LEFTSHIFT: case KEY_RIGHTSHIFT: shift_ = down; if (state == 1 && alt_) { layout_ru_ = !layout_ru_; dirty_ = true; } return;
        case KEY_LEFTCTRL: case KEY_RIGHTCTRL: ctrl_ = down; return;
        case KEY_LEFTALT: case KEY_RIGHTALT: alt_ = down; if (state == 1 && shift_) { layout_ru_ = !layout_ru_; dirty_ = true; } return;
        case KEY_CAPSLOCK: if (state == 1) caps_ = !caps_; return;
        case KEY_LEFTMETA: case KEY_RIGHTMETA:
            if (state == 1) toggle_popup(PopupKind::Menu, dock_d_rect());
            return;
        default: break;
    }
    if (state == 1 && code == KEY_F12) { show_perf_ = !show_perf_; dirty_ = true; return; }
    if (down && alt_ && code == KEY_TAB) { focus_next(); return; }
    if (down && alt_ && code == KEY_F4) { if (Window* w = win(focus_)) request_close_window(*w); return; }
    const std::string text = down ? key_text(code) : "";
    if (!popups_.empty() && handle_popup_key(code, state, text)) { dirty_ = true; return; }
    send_key_to_focus(code, state, text);
}

// ---------------------------------------------------------------------- shell actions
void Desktop::note_toast(const std::string& text) {
    toast_ = text;
    toast_until_ = now_ms() + 3200;
    dirty_ = true;
}

void Desktop::build_dock_items() {
    dock_items_.clear();
    for (auto& in : tasks_.instances()) {
        if (in.wins.empty()) continue;
        DockItem* item = nullptr;
        for (auto& d : dock_items_) if (d.app_id == in.app_id) item = &d;
        if (!item) { dock_items_.push_back(DockItem()); item = &dock_items_.back(); item->app_id = in.app_id; }
        for (int id : in.wins) item->wins.push_back(id);
    }
    for (auto& d : dock_items_) {
        bool all_min = true;
        for (int id : d.wins) {
            Window* w = win(id);
            if (!w) continue;
            if (!w->minimized) all_min = false;
            if (id == focus_) d.active = true;
        }
        d.all_minimized = all_min;
    }
}

void Desktop::launch_app(const std::string& app_id) {
    close_all_popups();
    if (app_id == "@settings") { open_popup(PopupKind::Settings, dock_d_rect()); return; }
    std::string err;
    const int iid = tasks_.launch(app_id, err);
    const AppDef* a = tasks_.app(app_id);
    if (iid < 0) { note_toast(t("Cannot start: ", "Не удалось запустить: ") + err); return; }
    // single-instance apps: bring the existing window forward instead of starting another
    if (Instance* in = tasks_.instance(iid))
        if (!in->wins.empty()) { if (Window* w = win(in->wins.back())) { if (w->minimized) restore(*w); else set_focus(w->id); } }
    note_toast(t("Starting ", "Запуск: ") + (a ? a->name(set_.ru) : app_id));
}

void Desktop::activate_app(const DockItem& it) {
    if (it.wins.size() == 1) {
        Window* w = win(it.wins[0]);
        if (!w) return;
        if (w->minimized) restore(*w);
        else if (w->id == focus_) minimize(*w);
        else set_focus(w->id);
        return;
    }
    toggle_popup(PopupKind::AppWindows, anchor_of("dock:" + it.app_id));
    if (Popup* p = popup(PopupKind::AppWindows)) p->app_id = it.app_id;
}

void Desktop::close_app(const std::string& app_id) {
    for (auto& in : tasks_.instances()) if (in.app_id == app_id) tasks_.ask_close(in.iid);
}

void Desktop::close_app_windows(const std::string& app_id) {
    std::vector<int> ids;
    for (auto& in : tasks_.instances()) if (in.app_id == app_id) for (int id : in.wins) ids.push_back(id);
    for (int id : ids) if (Window* w = win(id)) request_close_window(*w);
}

// ------------------------------------------------------------------------- popups
Popup* Desktop::popup(PopupKind k) {
    for (auto& p : popups_) if (p.kind == k && !p.closing) return &p;
    return nullptr;
}

Rect Desktop::place_popup(Rect anchor, int w, int h) const {
    const int m = S(10);
    w = std::min(w, W_ - 2 * m);
    h = std::min(h, std::max(S(80), anchor.y - S(22) - m));
    int x = anchor.x + anchor.w / 2 - w / 2;
    x = clampi(x, m, W_ - w - m);
    const int y = std::max(m, anchor.y - h - S(12));
    return {x, y, w, h};
}

void Desktop::open_popup(PopupKind k, Rect anchor, const std::string& app_id) {
    Popup p;
    p.kind = k;
    p.anchor = anchor;
    p.t0 = now_ms();
    p.app_id = app_id;
    int w = 300, h = 220;
    switch (k) {
        case PopupKind::Menu: {
            menu_search_.clear(); menu_cat_ = 0; menu_page_ = 0; menu_sel_ = 0;
            const int n = static_cast<int>(menu_apps().size());
            const int rows = clampi((n + tok::kMenuCols - 1) / tok::kMenuCols, 2, 4);
            w = 36 + tok::kMenuCols * tok::kMenuCellW;
            h = 188 + rows * 92 + (n > tok::kMenuCols * 4 ? 34 : 14);
            break;
        }
        case PopupKind::Tasks: w = 28 + tok::kTasksCols * 70; h = 440; tasks_scroll_ = 0; tasks_sel_.clear(); break;
        case PopupKind::Net: w = 320; h = 230; break;
        case PopupKind::Sound: w = 290; h = 150; break;
        case PopupKind::Calendar: w = 310; h = 330; break;
        case PopupKind::Notify: w = 360; h = 320; notify_seen_ = static_cast<int>(tasks_.journal().size()); break;
        case PopupKind::Power: w = 230; h = 200; break;
        case PopupKind::Layout: w = 210; h = 120; break;
        case PopupKind::Settings: w = 360; h = 360; break;
        case PopupKind::AppWindows: w = 280; h = 70; break;
        case PopupKind::AppMenu: w = 220; h = 150; break;
        case PopupKind::Confirm: w = 340; h = 140; break;
    }
    p.rect = place_popup(anchor, S(w), S(h));
    popups_.push_back(p);
    dirty_ = true;
}

void Desktop::close_popup(PopupKind k) {
    for (auto& p : popups_) if (p.kind == k && !p.closing) { p.closing = true; p.t_close = now_ms(); }
    dirty_ = true;
}

void Desktop::close_all_popups() {
    for (auto& p : popups_) if (!p.closing) { p.closing = true; p.t_close = now_ms(); }
    dirty_ = true;
}

void Desktop::toggle_popup(PopupKind k, Rect anchor) {
    if (popup(k)) { close_all_popups(); return; }
    close_all_popups();
    open_popup(k, anchor);
}

void Desktop::confirm(Rect anchor, const std::string& text, std::function<void()> yes) {
    Popup p;
    p.kind = PopupKind::Confirm;
    p.anchor = anchor;
    p.t0 = now_ms();
    p.text = text;
    p.on_yes = std::move(yes);
    p.rect = place_popup(anchor, S(340), S(140));
    popups_.push_back(p);
    dirty_ = true;
}

}  // namespace dg
