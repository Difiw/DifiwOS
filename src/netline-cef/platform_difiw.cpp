// Difiw desktop backend for Netline: a window of the Difiw compositor (shared-memory buffer, evdev key codes).
// SPDX-License-Identifier: MIT
#include <linux/input.h>
#include <time.h>

#include <deque>

#include "../difiw-desktop/client.hpp"
#include "platform.hpp"

namespace nl {
namespace {
int evdev_to_vk(int c) {
    static const char* rows[] = {"QWERTYUIOP", "ASDFGHJKL", "ZXCVBNM"};
    static const int start[] = {KEY_Q, KEY_A, KEY_Z};
    for (int r = 0; r < 3; r++) {
        int n = (int)std::string(rows[r]).size();
        if (c >= start[r] && c < start[r] + n) return rows[r][c - start[r]];
    }
    if (c >= KEY_1 && c <= KEY_9) return '1' + (c - KEY_1);
    if (c == KEY_0) return '0';
    if (c >= KEY_F1 && c <= KEY_F10) return 0x70 + (c - KEY_F1);
    switch (c) {
        case KEY_F11: return 0x7A; case KEY_F12: return 0x7B;
        case KEY_BACKSPACE: return 8; case KEY_TAB: return 9; case KEY_ENTER: case KEY_KPENTER: return 13; case KEY_ESC: return 27;
        case KEY_SPACE: return 32; case KEY_PAGEUP: return 33; case KEY_PAGEDOWN: return 34; case KEY_END: return 35;
        case KEY_HOME: return 36; case KEY_LEFT: return 37; case KEY_UP: return 38; case KEY_RIGHT: return 39; case KEY_DOWN: return 40;
        case KEY_INSERT: return 45; case KEY_DELETE: return 46;
        case KEY_EQUAL: return 0xBB; case KEY_MINUS: return 0xBD; case KEY_COMMA: return 0xBC; case KEY_DOT: return 0xBE;
        case KEY_SLASH: return 0xBF; case KEY_SEMICOLON: return 0xBA; case KEY_APOSTROPHE: return 0xDE;
        default: return 0;
    }
}

class DifiwPlatform : public Platform {
public:
    bool init(int w, int h, const std::string& title) override {
        if (!cl_.connect()) return false;
        win_ = cl_.create_window(w, h, title);
        return win_ >= 0;
    }
    dg::Surface& surface() override { return cl_.surface(win_); }
    bool wait(PEvent& out, int timeout_ms) override {
        if (!queue_.empty()) { out = queue_.front(); queue_.pop_front(); return true; }
        dg::Event e;
        if (!cl_.next_event(e, timeout_ms)) return false;
        switch (e.kind) {
            case dg::Event::Disconnected: case dg::Event::Close: out.kind = PEvent::Quit; return true;
            case dg::Event::Configure: out.kind = PEvent::Resize; return true;
            case dg::Event::Mouse: {
                out.x = e.x; out.y = e.y; out.mods = mods_;
                if (e.mouse_kind == 3) { out.kind = PEvent::Wheel; out.wheel = e.wheel; return true; }
                const int diff = e.buttons ^ buttons_;
                buttons_ = e.buttons;
                if (e.mouse_kind == 1 || e.mouse_kind == 2) {
                    out.kind = e.mouse_kind == 1 ? PEvent::MouseDown : PEvent::MouseUp;
                    out.button = (diff & 2) ? 3 : 1;
                    return true;
                }
                out.kind = PEvent::MouseMove;
                return true;
            }
            case dg::Event::Key: {
                mods_ = ((e.mods & 1) ? MOD_SHIFT : 0) | ((e.mods & 2) ? MOD_CTRL : 0) | ((e.mods & 4) ? MOD_ALT : 0);
                const int vk = evdev_to_vk(e.code);
                out.kind = PEvent::None;
                if (vk) { out.kind = PEvent::Key; out.vk = vk; out.mods = mods_; out.down = e.state != 0; }
                if (e.state != 0 && !e.text.empty() && !(mods_ & (MOD_CTRL | MOD_ALT)) && static_cast<unsigned char>(e.text[0]) >= 32) {
                    PEvent t;
                    t.kind = PEvent::Text; t.text = e.text; t.mods = mods_;
                    if (out.kind == PEvent::None) { out = t; return true; }
                    queue_.push_back(t);
                }
                return out.kind != PEvent::None;
            }
            default: return false;
        }
    }
    void present() override { cl_.commit(win_); }
    uint32_t ticks() override {
        timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        return static_cast<uint32_t>(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
    }
    void set_title(const std::string& t) override { cl_.set_title(win_, t); }

private:
    dg::Client cl_;
    int win_ = -1, buttons_ = 0, mods_ = 0;
    std::deque<PEvent> queue_;
};
}  // namespace

Platform* make_platform() { return new DifiwPlatform; }
}  // namespace nl
