// Difiw desktop wire protocol: fixed-size messages over a Unix SOCK_SEQPACKET socket.
// Window buffers are shared memory (memfd) passed with SCM_RIGHTS; pixels are 0xAARRGGBB.
// SPDX-License-Identifier: MIT
#pragma once

#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstring>

namespace dg::proto {

constexpr const char* kSocketPath = "/run/difiw/desktop.sock";
constexpr const char* kCtlPath = "/run/difiw/control.sock";

enum Type : std::uint32_t {
    // client -> desktop
    C_HELLO = 1,    // s = instance token
    C_CREATE = 2,   // i0=w i1=h, s=title; carries the buffer fd
    C_ATTACH = 3,   // i0=win i1=w i2=h; carries the new buffer fd (answer to S_CONFIGURE)
    C_COMMIT = 4,   // i0=win: the buffer content changed
    C_TITLE = 5,    // i0=win, s=title
    C_DESTROY = 6,  // i0=win
    C_REQUEST = 7,  // i0=Request, s=argument (needs the matching manifest permission)
    // desktop -> client
    S_WELCOME = 100,    // i0=ui scale, i1=reduced motion, i2=transparency
    S_CREATED = 101,    // i0=win id, i1=w, i2=h
    S_CONFIGURE = 102,  // i0=win id, i1=w, i2=h
    S_MOUSE = 103,      // i0=win i1=x i2=y i3=buttons i4=kind(0 move,1 down,2 up,3 wheel) i5=wheel
    S_KEY = 104,        // i0=win i1=keycode i2=state(0 up,1 down,2 repeat) i3=mods(1 shift,2 ctrl,4 alt) s=text
    S_FOCUS = 105,      // i0=win i1=focused
    S_CLOSE = 106,      // i0=win: the user asked to close the window
    S_DENIED = 107,     // i0=Request
    // control channel (difiwctl) <-> desktop
    X_CMD = 200,   // s = command line
    X_REPLY = 201, // s = one line of output; i0=1 when this is the last line
};

enum Request : int { RQ_QUIT_DESKTOP = 1, RQ_POWEROFF = 2, RQ_REBOOT = 3, RQ_SET = 4, RQ_NET = 5 };  // RQ_SET: s = "key=value" (settings)

struct Msg {
    std::uint32_t type = 0;
    std::int32_t i[6] = {0, 0, 0, 0, 0, 0};
    char s[120] = {0};
};

inline void set_str(Msg& m, const char* src) {
    std::strncpy(m.s, src, sizeof m.s - 1);
    m.s[sizeof m.s - 1] = 0;
}

// Sends one message, optionally with a file descriptor. Returns true on success.
inline bool send_msg(int sock, const Msg& m, int pass_fd = -1, int flags = 0) {
    struct iovec iov = {const_cast<Msg*>(&m), sizeof m};
    char ctl[CMSG_SPACE(sizeof(int))];
    std::memset(ctl, 0, sizeof ctl);
    struct msghdr mh;
    std::memset(&mh, 0, sizeof mh);
    mh.msg_iov = &iov;
    mh.msg_iovlen = 1;
    if (pass_fd >= 0) {
        mh.msg_control = ctl;
        mh.msg_controllen = sizeof ctl;
        struct cmsghdr* c = CMSG_FIRSTHDR(&mh);
        c->cmsg_level = SOL_SOCKET;
        c->cmsg_type = SCM_RIGHTS;
        c->cmsg_len = CMSG_LEN(sizeof(int));
        std::memcpy(CMSG_DATA(c), &pass_fd, sizeof(int));
    }
    for (;;) {
        const ssize_t n = sendmsg(sock, &mh, MSG_NOSIGNAL | flags);
        if (n == static_cast<ssize_t>(sizeof m)) return true;
        if (n < 0 && errno == EINTR) continue;
        return false;
    }
}

// Receives one message. Returns 1 on success, 0 on orderly close, -1 on error / would block.
// A received descriptor is stored in *fd_out (or -1).
inline int recv_msg(int sock, Msg& m, int* fd_out, bool nonblock = false) {
    struct iovec iov = {&m, sizeof m};
    char ctl[CMSG_SPACE(sizeof(int))];
    struct msghdr mh;
    std::memset(&mh, 0, sizeof mh);
    mh.msg_iov = &iov;
    mh.msg_iovlen = 1;
    mh.msg_control = ctl;
    mh.msg_controllen = sizeof ctl;
    if (fd_out) *fd_out = -1;
    ssize_t n;
    do { n = recvmsg(sock, &mh, (nonblock ? MSG_DONTWAIT : 0) | MSG_CMSG_CLOEXEC); } while (n < 0 && errno == EINTR);
    if (n == 0) return 0;
    if (n != static_cast<ssize_t>(sizeof m)) return -1;
    for (struct cmsghdr* c = CMSG_FIRSTHDR(&mh); c; c = CMSG_NXTHDR(&mh, c))
        if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS) {
            int fd;
            std::memcpy(&fd, CMSG_DATA(c), sizeof fd);
            if (fd_out) *fd_out = fd; else close(fd);
        }
    m.s[sizeof m.s - 1] = 0;
    return 1;
}

}  // namespace dg::proto
