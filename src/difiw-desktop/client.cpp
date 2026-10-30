// SPDX-License-Identifier: MIT
#include "client.hpp"

#include <poll.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace dg {

using namespace proto;

Client::~Client() {
    for (auto& [id, w] : wins_) free_buffer(w);
    if (fd_ >= 0) close(fd_);
}

bool Client::make_buffer(Win& w, int width, int height) {
    if (width < 1 || height < 1 || width > 4096 || height > 4096) return false;
    const std::size_t len = static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4;
    const int fd = memfd_create("difiw-window", MFD_CLOEXEC);
    if (fd < 0) return false;
    if (ftruncate(fd, static_cast<off_t>(len)) != 0) { close(fd); return false; }
    void* m = mmap(nullptr, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (m == MAP_FAILED) { close(fd); return false; }
    w.fd = fd;
    w.map = static_cast<u32*>(m);
    w.len = len;
    w.surf.wrap(w.map, width, height);
    fill(w.surf, {0, 0, width, height}, 0xFF151A24u);
    return true;
}

void Client::free_buffer(Win& w) {
    if (w.map) munmap(w.map, w.len);
    if (w.fd >= 0) close(w.fd);
    w.map = nullptr;
    w.fd = -1;
}

bool Client::connect() {
    const char* token = std::getenv("DIFIW_TOKEN");
    const char* path = std::getenv("DIFIW_SOCK");
    if (!token) return false;
    fd_ = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (fd_ < 0) return false;
    struct sockaddr_un sa;
    std::memset(&sa, 0, sizeof sa);
    sa.sun_family = AF_UNIX;
    std::strncpy(sa.sun_path, path ? path : kSocketPath, sizeof sa.sun_path - 1);
    if (::connect(fd_, reinterpret_cast<struct sockaddr*>(&sa), sizeof sa) != 0) return false;
    Msg hello;
    hello.type = C_HELLO;
    set_str(hello, token);
    if (!send_msg(fd_, hello)) return false;
    Msg r;
    if (recv_msg(fd_, r, nullptr) != 1 || r.type != S_WELCOME) return false;
    scale = std::max(1, r.i[0]);
    reduced_motion = r.i[1] != 0;
    transparency = r.i[2] != 0;
    return true;
}

int Client::create_window(int w, int h, const std::string& title) {
    Win win;
    if (!make_buffer(win, w, h)) return -1;
    Msg m;
    m.type = C_CREATE;
    m.i[0] = w;
    m.i[1] = h;
    set_str(m, title.c_str());
    if (!send_msg(fd_, m, win.fd)) { free_buffer(win); return -1; }
    Msg r;
    for (;;) {  // the answer may be preceded by other events; keep it simple: wait for S_CREATED
        if (recv_msg(fd_, r, nullptr) != 1) { free_buffer(win); return -1; }
        if (r.type == S_CREATED) break;
    }
    const int id = r.i[0];
    if (id <= 0) { free_buffer(win); return -1; }
    // the desktop may have chosen a different size; honour it with a fresh buffer
    if (r.i[1] != w || r.i[2] != h) {
        free_buffer(win);
        if (!make_buffer(win, r.i[1], r.i[2])) return -1;
        Msg a;
        a.type = C_ATTACH;
        a.i[0] = id; a.i[1] = r.i[1]; a.i[2] = r.i[2];
        send_msg(fd_, a, win.fd);
    }
    wins_[id] = std::move(win);
    return id;
}

Surface& Client::surface(int win) { return wins_.at(win).surf; }

void Client::commit(int win) {
    Msg m;
    m.type = C_COMMIT;
    m.i[0] = win;
    send_msg(fd_, m);
}

void Client::set_title(int win, const std::string& title) {
    Msg m;
    m.type = C_TITLE;
    m.i[0] = win;
    set_str(m, title.c_str());
    send_msg(fd_, m);
}

void Client::destroy_window(int win) {
    Msg m;
    m.type = C_DESTROY;
    m.i[0] = win;
    send_msg(fd_, m);
    auto it = wins_.find(win);
    if (it != wins_.end()) { free_buffer(it->second); wins_.erase(it); }
}

void Client::request(int code, const std::string& arg) {
    Msg m;
    m.type = C_REQUEST;
    m.i[0] = code;
    set_str(m, arg.c_str());
    send_msg(fd_, m);
}

bool Client::next_event(Event& ev, int timeout_ms) {
    ev = Event();
    struct pollfd pf = {fd_, POLLIN, 0};
    const int pr = poll(&pf, 1, timeout_ms);
    if (pr <= 0) return false;
    Msg m;
    const int r = recv_msg(fd_, m, nullptr);
    if (r <= 0) { ev.kind = Event::Disconnected; return true; }
    switch (m.type) {
        case S_MOUSE:
            ev.kind = Event::Mouse; ev.win = m.i[0]; ev.x = m.i[1]; ev.y = m.i[2]; ev.buttons = m.i[3];
            ev.mouse_kind = m.i[4]; ev.wheel = m.i[5];
            break;
        case S_KEY:
            ev.kind = Event::Key; ev.win = m.i[0]; ev.code = m.i[1]; ev.state = m.i[2]; ev.mods = m.i[3]; ev.text = m.s;
            break;
        case S_FOCUS: ev.kind = Event::Focus; ev.win = m.i[0]; ev.focused = m.i[1]; break;
        case S_CLOSE: ev.kind = Event::Close; ev.win = m.i[0]; break;
        case S_DENIED: ev.kind = Event::Denied; ev.code = m.i[0]; break;
        case S_CONFIGURE: {
            ev.kind = Event::Configure; ev.win = m.i[0]; ev.w = m.i[1]; ev.h = m.i[2];
            auto it = wins_.find(ev.win);
            if (it != wins_.end()) {
                Win nw;
                if (make_buffer(nw, ev.w, ev.h)) {
                    free_buffer(it->second);
                    it->second = std::move(nw);
                    Msg a;
                    a.type = C_ATTACH;
                    a.i[0] = ev.win; a.i[1] = ev.w; a.i[2] = ev.h;
                    send_msg(fd_, a, it->second.fd);
                }
            }
            break;
        }
        case S_WELCOME:
            scale = std::max(1, m.i[0]); reduced_motion = m.i[1] != 0; transparency = m.i[2] != 0;
            return next_event(ev, 0);
        default: return next_event(ev, 0);
    }
    return true;
}

}  // namespace dg
