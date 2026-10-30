// difiw-desktop entry point.
//   difiw-desktop                   run on the framebuffer (real system)
//   difiw-desktop --headless WxH    no framebuffer / evdev (development and tests on the host)
// Exit status: 0 normal, 11 = reboot requested, 12 = power off requested
// (on the real system as root the desktop performs the action itself after restoring the console).
// SPDX-License-Identifier: MIT
#include <sys/reboot.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "desktop.hpp"

int main(int argc, char** argv) {
    dg::Options opt;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--headless" && i + 1 < argc) {
            opt.headless = true;
            std::sscanf(argv[++i], "%dx%d", &opt.hw, &opt.hh);
        } else if (a == "--rundir" && i + 1 < argc) opt.rundir = argv[++i];
        else if (a == "--datadir" && i + 1 < argc) opt.datadir = argv[++i];
        else if (a == "--etcdir" && i + 1 < argc) opt.etcdir = argv[++i];
        else { std::fprintf(stderr, "usage: difiw-desktop [--headless WxH] [--rundir D] [--datadir D] [--etcdir D]\n"); return 2; }
    }
    int rc = 0;
    {
        dg::Desktop d;  // its destructor restores the text console
        if (!d.init(opt)) return 1;
        rc = d.run();
    }
    if (!opt.headless && geteuid() == 0 && (rc == 11 || rc == 12)) {
        sync();
        reboot(rc == 11 ? RB_AUTOBOOT : RB_POWER_OFF);
    }
    return rc;
}
