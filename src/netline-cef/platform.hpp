// Netline platform layer: one window, a pixel surface, input events. Two backends: Difiw desktop client and SDL2 (development).
// SPDX-License-Identifier: MIT
#pragma once
#include <cstdint>
#include <string>

#include "../difiw-desktop/gfx.hpp"

namespace nl {

// Key codes are Windows virtual-key codes (what Chromium expects): letters 'A'-'Z', digits '0'-'9', 8 Backspace, 9 Tab,
// 13 Enter, 27 Esc, 32 Space, 33/34 PgUp/PgDn, 35/36 End/Home, 37-40 arrows, 45 Ins, 46 Del, 0x70.. F1.., 0xBB '=', 0xBD '-'.
enum Mod { MOD_SHIFT = 2, MOD_CTRL = 4, MOD_ALT = 8 };  // same values as CEF EVENTFLAG_*_DOWN

struct PEvent {
    enum Kind { None, Quit, Resize, Expose, Key, Text, MouseMove, MouseDown, MouseUp, Wheel } kind = None;
    int x = 0, y = 0;         // mouse position (window coordinates)
    int button = 0;           // 1 left, 2 middle, 3 right
    int clicks = 1;
    int wheel = 0;            // >0 scrolls the page up
    int vk = 0, mods = 0;     // Key (mods: Mod bits, also set for mouse/wheel events)
    bool down = false;        // Key
    std::string text;         // Text (UTF-8)
};

class Platform {
public:
    virtual ~Platform() {}
    virtual bool init(int w, int h, const std::string& title) = 0;
    virtual dg::Surface& surface() = 0;                       // target of drawing; size follows the window
    virtual bool wait(PEvent& ev, int timeout_ms) = 0;        // false: timeout
    virtual void present() = 0;
    virtual void set_cursor(int cef_cursor_type) {}
    virtual std::string clipboard() { return clip_; }
    virtual void set_clipboard(const std::string& s) { clip_ = s; }
    virtual uint32_t ticks() = 0;
    virtual void set_title(const std::string&) {}
protected:
    std::string clip_;
};

Platform* make_platform();  // the backend selected at build time

}  // namespace nl
