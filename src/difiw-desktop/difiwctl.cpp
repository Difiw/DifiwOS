// difiwctl: control client of the running desktop (root only: the control socket is 0600).
//   difiwctl launch notes | apps | instances | windows | tasks | dock | popup menu | shot /tmp/x.ppm | quit ...
// SPDX-License-Identifier: MIT
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "proto.hpp"

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: difiwctl <command> [args]   (e.g. launch notes, windows, tasks, shot FILE)\n"); return 2; }
    std::string line;
    for (int i = 1; i < argc; ++i) { if (i > 1) line += ' '; line += argv[i]; }
    const char* dir = std::getenv("DIFIW_RUNDIR");
    const std::string path = std::string(dir ? dir : "/run/difiw") + "/control.sock";
    const int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    struct sockaddr_un sa;
    std::memset(&sa, 0, sizeof sa);
    sa.sun_family = AF_UNIX;
    std::strncpy(sa.sun_path, path.c_str(), sizeof sa.sun_path - 1);
    if (fd < 0 || connect(fd, reinterpret_cast<struct sockaddr*>(&sa), sizeof sa) != 0) {
        std::fprintf(stderr, "difiwctl: cannot connect to %s (is the desktop running? are you root?)\n", path.c_str());
        return 1;
    }
    dg::proto::Msg m;
    m.type = dg::proto::X_CMD;
    dg::proto::set_str(m, line.c_str());
    if (!dg::proto::send_msg(fd, m)) return 1;
    int rc = 0;
    for (;;) {
        dg::proto::Msg r;
        if (dg::proto::recv_msg(fd, r, nullptr) != 1) break;
        if (r.type != dg::proto::X_REPLY) break;
        if (r.s[0]) {
            std::printf("%s\n", r.s);
            if (std::strncmp(r.s, "error", 5) == 0 || std::strncmp(r.s, "refused", 7) == 0) rc = 1;
        }
        if (r.i[0]) break;
    }
    close(fd);
    return rc;
}
