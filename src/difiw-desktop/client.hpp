// Difiw client library: how an application talks to the desktop (windows + events).
// SPDX-License-Identifier: MIT
#pragma once

#include <map>
#include <string>

#include "gfx.hpp"
#include "proto.hpp"

namespace dg {

struct Event {
    enum Kind { None, Mouse, Key, Focus, Close, Configure, Denied, Disconnected } kind = None;
    int win = 0;
    int x = 0, y = 0, buttons = 0, mouse_kind = 0, wheel = 0;  // Mouse (mouse_kind: 0 move, 1 down, 2 up, 3 wheel)
    int code = 0, state = 0, mods = 0;                         // Key
    std::string text;                                          // Key (UTF-8, may be empty)
    int focused = 0;                                           // Focus
    int w = 0, h = 0;                                          // Configure
};

class Client {
public:
    ~Client();
    // Connects using DIFIW_TOKEN / DIFIW_SOCK from the environment.
    bool connect();
    int create_window(int w, int h, const std::string& title);  // returns window id or -1
    Surface& surface(int win);
    void commit(int win);
    void set_title(int win, const std::string& title);
    void destroy_window(int win);
    void request(int code, const std::string& arg = "");
    int fd() const { return fd_; }
    // Waits up to timeout_ms (-1 = forever) for the next event. A Configure event has already
    // re-created the buffer: the application only has to redraw and commit.
    bool next_event(Event& ev, int timeout_ms);

    int scale = 1;
    bool reduced_motion = false;
    bool transparency = true;

private:
    struct Win {
        int fd = -1;
        u32* map = nullptr;
        std::size_t len = 0;
        Surface surf;
    };
    bool make_buffer(Win& w, int width, int height);
    void free_buffer(Win& w);
    int fd_ = -1;
    std::map<int, Win> wins_;
};

}  // namespace dg
