// Difiw desktop: compositor (windows) + shell (dock, menu, popups, background tasks).
// SPDX-License-Identifier: MIT
#pragma once

#include <linux/input.h>

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "gfx.hpp"
#include "proto.hpp"
#include "tasks.hpp"
#include "tokens.hpp"

namespace dg {

struct Settings {
    int scale = 1;               // 1 or 2 (100% / 200%)
    bool reduce_motion = false;  // no animations
    bool transparency = true;    // glass panels; off = opaque graphite
    bool ru = true;              // interface language
    bool winkey = false;         // the Win/Super key opens the D menu (off: it does nothing)
};

struct Options {
    bool headless = false;       // no framebuffer / evdev (development and tests on the host)
    int hw = 1024, hh = 768;     // headless screen size
    std::string rundir = "/run/difiw";
    std::string datadir = "/usr/share/difiw";
    std::string etcdir = "/etc/difiw";
};

struct Window {
    int id = 0, iid = 0, client = -1;
    std::string title;
    int x = 0, y = 0;            // top-left of the outer frame
    int cw = 0, ch = 0;          // content size the window wants / has
    int fd = -1;
    u32* map = nullptr;
    std::size_t len = 0;
    Surface buf;                 // wraps the shared buffer (size bw x bh)
    bool minimized = false, maximized = false;
    Rect restore;                // frame before maximizing
    long opened_ms = 0;
    bool attached = false;
};

enum class PopupKind { Menu, Tasks, Net, Sound, Calendar, Notify, Power, Layout, Settings, AppWindows, AppMenu, Confirm };

struct Popup {
    PopupKind kind = PopupKind::Menu;
    Rect anchor;                 // the button that opened it
    Rect rect;                   // final panel rectangle
    long t0 = 0;
    bool closing = false;
    long t_close = 0;
    std::string app_id;          // AppWindows / AppMenu
    std::string text;            // Confirm: question
    std::function<void()> on_yes;
};

struct Hit {
    Rect r;
    std::function<void(int button)> fn;   // button: 1 left, 2 right
    std::string tip;
    std::string hover_key;                // for hover animation (empty = none)
    std::string label;                    // text of a button (used by `difiwctl button`)
};

struct DockItem {
    std::string app_id;
    std::vector<int> wins;       // visible + minimized windows of all instances of the app
    bool active = false;         // one of its windows has the focus
    bool all_minimized = false;
};

class Desktop {
public:
    bool init(const Options& opt);
    int run();
    ~Desktop();

    // ---- control commands (difiwctl); returns reply lines
    std::vector<std::string> control(const std::string& line);

private:
    // ---- core
    Options opt_;
    Settings set_;
    TaskModel tasks_;
    Surface back_;               // the frame being composed
    Image wall_, wall_blur_;
    int W_ = 1024, H_ = 768;
    bool quit_ = false;
    int exit_code_ = 0;
    long frame_ms_ = 0;
    bool dirty_ = true;

    // framebuffer
    struct Fb {
        int fd = -1;
        unsigned char* mem = nullptr;
        std::size_t len = 0;
        int stride = 0, bpp = 32, ro = 16, go = 8, bo = 0, rl = 8, gl = 8, bl = 8;
    } fb_;
    int tty_ = -1;
    int old_kbmode_ = 3;
    bool gfx_mode_ = false;
    bool open_fb();
    void enter_console_graphics();
    void leave_console_graphics();
    void present();
    void blit_region(int x, int y, int w, int h);
    void draw_cursor();

    // ---- input devices
    struct Dev {
        int fd = -1;
        bool kbd = false, abs = false, rel = false;
        int minx = 0, maxx = 1, miny = 0, maxy = 1;
    };
    std::vector<Dev> devs_;
    void scan_devices();
    void handle_dev_event(const Dev& d, const struct input_event& e);
    int mx_ = 0, my_ = 0, buttons_ = 0;
    bool shift_ = false, ctrl_ = false, alt_ = false, caps_ = false, layout_ru_ = false;
    long last_scan_ms_ = 0;

    // ---- clients and windows
    int listen_fd_ = -1, ctl_fd_ = -1;
    struct ClientConn { int fd = -1; int iid = -1; bool hello = false; };
    std::vector<ClientConn> clients_;
    std::vector<std::unique_ptr<Window>> wins_;  // z-order, back to front
    int focus_ = 0;
    int next_win_ = 1;
    void accept_clients();
    void handle_client_msg(ClientConn& c, const proto::Msg& m, int fd);
    void drop_client(int fd);
    void create_window(ClientConn& c, const proto::Msg& m, int fd);
    bool attach_buffer(Window& w, int fd, int bw, int bh);
    void free_buffer(Window& w);
    void destroy_window(int id);
    Window* win(int id);
    Window* top_window_at(int x, int y, bool include_frame = true);
    void raise(int id);
    void set_focus(int id);
    void focus_next();
    void toggle_maximize(Window& w);
    void minimize(Window& w);
    void restore(Window& w);
    void send_to_client(int fd, const proto::Msg& m);
    void request_close_window(Window& w);
    void configure(Window& w, int cw, int ch);
    bool instance_has_windows(int iid) const;
    Rect work_area() const;
    // geometry (scaled)
    int S(int v) const { return v * set_.scale; }
    int title_h() const { return S(tok::kTitleBar); }
    Rect frame_of(const Window& w) const;
    Rect content_of(const Window& w) const;
    void clamp_window(Window& w);

    // ---- window interaction (mouse capture)
    enum class Drag { None, Move, Resize, Content } drag_ = Drag::None;
    int drag_win_ = 0, drag_dx_ = 0, drag_dy_ = 0, drag_edge_ = 0;
    Rect drag_orig_;
    long last_title_click_ms_ = 0;
    int last_title_click_win_ = 0;

    // ---- shell state
    std::vector<Popup> popups_;
    std::vector<Hit> hits_;
    std::string hover_;                      // hover_key currently under the pointer
    std::map<std::string, float> hov_;       // animated hover amount per key
    std::map<std::string, long> press_t_;    // press animation start per key
    std::vector<DockItem> dock_items_;
    float dock_scroll_ = 0, dock_scroll_target_ = 0;
    std::string menu_search_;
    int menu_cat_ = 0, menu_page_ = 0, menu_sel_ = -1;
    float tasks_scroll_ = 0;
    std::string tasks_sel_;
    std::string tip_;
    Rect tip_anchor_;
    int notify_seen_ = 0;
    long last_sample_ms_ = 0;
    std::string toast_;
    long toast_until_ = 0;
    float content_alpha_ = 1.0f;
    // performance: last presented frame (to blit only what changed), cached wallpaper+scrim, timings
    std::vector<u32> prev_;
    Image base_;
    long last_render_ms_ = 0, last_move_send_ms_ = 0;
    long perf_render_us_ = 0, perf_present_us_ = 0, perf_win_us_ = 0, perf_dock_us_ = 0, perf_pop_us_ = 0;
    bool show_perf_ = false;   // F12: frame timings overlay

    void note_toast(const std::string& text);
    void build_dock_items();
    // popups
    Popup* popup(PopupKind k);
    void open_popup(PopupKind k, Rect anchor, const std::string& app_id = "");
    void close_popup(PopupKind k);
    void close_all_popups();
    void toggle_popup(PopupKind k, Rect anchor);
    Rect place_popup(Rect anchor, int w, int h) const;
    void confirm(Rect anchor, const std::string& text, std::function<void()> yes);
    // actions
    void launch_app(const std::string& app_id);
    void activate_app(const DockItem& it);
    void close_app(const std::string& app_id);
    void close_app_windows(const std::string& app_id);
    void do_power(bool reboot);
    void leave_desktop();
    void apply_settings();
    void save_settings();
    Rect dock_rect() const;
    Rect dock_d_rect() const;
    Rect dock_left_rect() const;
    Rect dock_right_rect() const;
    Rect tray_rect(int index) const;
    int tray_count() const;
    bool battery_present() const;

    // ---- drawing (desktop_ui.cpp)
    void render();
    void draw_windows();
    void draw_window(const Window& w, bool focused);
    void draw_dock();
    void draw_tray(Rect area);
    void draw_popups();
    void draw_popup(Popup& p);
    void draw_menu(Popup& p, Rect r);
    void draw_tasks(Popup& p, Rect r);
    void draw_net(Rect r);
    void draw_sound(Rect r);
    void draw_calendar(Rect r);
    void draw_notify(Rect r);
    void draw_power(Rect r);
    void draw_layout(Rect r);
    void draw_settings(Rect r);
    void draw_app_windows(Popup& p, Rect r);
    void draw_app_menu(Popup& p, Rect r);
    void draw_confirm(Popup& p, Rect r);
    void draw_toast();
    void draw_tip();
    void draw_icon(Rect r, const std::string& kind, float hover, bool glow, int radius = -1);
    void draw_d_logo(Rect r, float hover, float press);
    void draw_glass(Rect r, int radius, int shadow = 14);
    void draw_button(Rect r, const std::string& label, std::function<void(int)> fn, bool primary = false, bool danger = false, bool enabled = true);
    void add_hit(Rect r, std::function<void(int)> fn, const std::string& hover_key = "", const std::string& tip = "");
    u32 ca_(u32 c) const;  // color with the current content alpha
    std::string t(const char* en, const char* ru) const { return set_.ru ? ru : en; }
    float hover_amount(const std::string& key) const;
    float press_amount(const std::string& key) const;
    bool animating() const;
    void update_animations(long dt_ms);
    std::vector<const AppDef*> menu_apps() const;
    std::string category_label(int idx) const;
    std::vector<std::string> categories() const;

    // ---- input routing (desktop_input.cpp)
    void on_pointer_move(int nx, int ny);
    void on_button(int button, bool down);
    void on_wheel(int delta);
    void on_key(int code, int state);
    void route_mouse_to_window(Window& w, int kind, int wheel);
    std::string key_text(int code) const;
    int edge_at(const Window& w, int x, int y) const;
    Rect title_btn(const Window& w, int idx) const;     // 0 close, 1 maximize, 2 minimize
    Rect anchor_of(const std::string& hover_key) const;
    bool handle_popup_key(int code, int state, const std::string& text);
    void send_key_to_focus(int code, int state, const std::string& text);
    Hit* hit_at(int x, int y);

    // ---- control socket
    struct CtlConn { int fd = -1; };
    void accept_ctl();
    void write_screenshot(const std::string& path);

    friend int desktop_main(int argc, char** argv);
};

}  // namespace dg
