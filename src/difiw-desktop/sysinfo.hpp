// Read-only system information and sound-mixer helpers shared by the desktop shell (panels) and the Settings app.
// SPDX-License-Identifier: MIT
#pragma once

#include <arpa/inet.h>
#include <dirent.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace dg::sys {

struct NetIf {
    std::string name, state, ip, mac, gw;
    bool up = false;
};

inline std::string read_line(const std::string& path) {
    std::ifstream f(path);
    std::string l;
    std::getline(f, l);
    while (!l.empty() && (l.back() == '\r' || l.back() == ' ')) l.pop_back();
    return l;
}

// Default gateway of an interface from /proc/net/route (little-endian hex).
inline std::string gateway_of(const std::string& ifn) {
    std::ifstream f("/proc/net/route");
    std::string l;
    std::getline(f, l);
    while (std::getline(f, l)) {
        std::istringstream is(l);
        std::string name, dest, gw;
        is >> name >> dest >> gw;
        if (name == ifn && dest == "00000000") {
            const unsigned long v = std::strtoul(gw.c_str(), nullptr, 16);
            char b[32];
            std::snprintf(b, sizeof b, "%lu.%lu.%lu.%lu", v & 255, (v >> 8) & 255, (v >> 16) & 255, (v >> 24) & 255);
            return b;
        }
    }
    return "";
}

inline std::vector<NetIf> net_ifs() {
    std::vector<NetIf> out;
    DIR* d = opendir("/sys/class/net");
    if (!d) return out;
    const int fd = socket(AF_INET, SOCK_DGRAM, 0);
    while (struct dirent* e = readdir(d)) {
        if (e->d_name[0] == '.' || std::strcmp(e->d_name, "lo") == 0) continue;
        NetIf n;
        n.name = e->d_name;
        n.state = read_line("/sys/class/net/" + n.name + "/operstate");
        n.mac = read_line("/sys/class/net/" + n.name + "/address");
        struct ifreq ifr;
        std::memset(&ifr, 0, sizeof ifr);
        std::strncpy(ifr.ifr_name, e->d_name, IFNAMSIZ - 1);
        if (fd >= 0 && ioctl(fd, SIOCGIFFLAGS, &ifr) == 0) n.up = (ifr.ifr_flags & IFF_UP) != 0;
        std::memset(&ifr, 0, sizeof ifr);
        std::strncpy(ifr.ifr_name, e->d_name, IFNAMSIZ - 1);
        if (fd >= 0 && ioctl(fd, SIOCGIFADDR, &ifr) == 0) {
            char b[32];
            inet_ntop(AF_INET, &reinterpret_cast<struct sockaddr_in*>(&ifr.ifr_addr)->sin_addr, b, sizeof b);
            n.ip = b;
        }
        n.gw = gateway_of(n.name);
        out.push_back(n);
    }
    if (fd >= 0) close(fd);
    closedir(d);
    std::sort(out.begin(), out.end(), [](const NetIf& a, const NetIf& b) { return a.name < b.name; });
    return out;
}

inline std::string dns_servers() {
    std::ifstream f("/etc/resolv.conf");
    std::string l, out;
    while (std::getline(f, l)) {
        if (l.compare(0, 11, "nameserver ") != 0) continue;
        std::string ip = l.substr(11);
        const std::size_t sp = ip.find(' ');
        if (sp != std::string::npos) ip.erase(sp);
        if (out.find(ip) != std::string::npos) continue;
        out += (out.empty() ? "" : ", ") + ip;
    }
    return out;
}

// ------------------------------------------------------------------- sound
struct Vol {
    bool ok = false, on = true;
    int pct = 0;
    std::string ctl;
};

inline std::string run_capture(const std::string& cmd) {
    FILE* p = popen(cmd.c_str(), "r");
    if (!p) return "";
    std::string out;
    char b[512];
    while (std::fgets(b, sizeof b, p)) out += b;
    pclose(p);
    return out;
}

// The first mixer control from `names` that exists. Output: Master/PCM/Speaker, input: Capture/Mic.
inline Vol vol_get(bool capture) {
    static const char* const play[] = {"Master", "PCM", "Speaker", "Headphone"};
    static const char* const rec[] = {"Capture", "Mic", "Input"};
    Vol v;
    const char* const* names = capture ? rec : play;
    const int n = capture ? 3 : 4;
    for (int i = 0; i < n; ++i) {
        const std::string out = run_capture(std::string("amixer sget ") + names[i] + " 2>/dev/null");
        const std::size_t p = out.find('[');
        const std::size_t pc = out.find("%]");
        if (p == std::string::npos || pc == std::string::npos) continue;
        v.ok = true;
        v.ctl = names[i];
        const std::size_t s = out.rfind('[', pc);
        v.pct = std::atoi(out.c_str() + s + 1);
        v.on = out.find("[off]") == std::string::npos;
        return v;
    }
    return v;
}

inline void vol_set(const std::string& ctl, int pct) {
    if (ctl.empty()) return;
    if (std::system(("amixer -q sset " + ctl + " " + std::to_string(std::max(0, std::min(100, pct))) + "% >/dev/null 2>&1 &").c_str()) != 0) {}
}

inline void vol_mute(const std::string& ctl) {
    if (ctl.empty()) return;
    if (std::system(("amixer -q sset " + ctl + " toggle >/dev/null 2>&1 &").c_str()) != 0) {}
}

inline void play_test() { if (std::system("aplay -q /usr/share/difiw/sounds/test.wav >/dev/null 2>&1 &") != 0) {} }

// "HDA-Intel - HDA Intel" -> "HDA Intel"
inline std::string card_name() {
    std::ifstream f("/proc/asound/cards");
    std::string l;
    while (std::getline(f, l)) {
        const std::size_t c = l.find("]: ");
        if (c == std::string::npos) continue;
        std::string n = l.substr(c + 3);
        const std::size_t d = n.find(" - ");
        if (d != std::string::npos) n = n.substr(d + 3);
        return n;
    }
    return "";
}

// Name of the first playback / capture device from /proc/asound/pcm ("ALC269 Analog").
inline std::string pcm_name(bool capture) {
    std::ifstream f("/proc/asound/pcm");
    std::string l;
    while (std::getline(f, l)) {
        if (l.find(capture ? "capture" : "playback") == std::string::npos) continue;
        const std::size_t c = l.find(": ");
        if (c == std::string::npos) continue;
        std::string n = l.substr(c + 2);
        const std::size_t d = n.find(" : ");
        if (d != std::string::npos) n.erase(d);
        return n;
    }
    return "";
}

// ------------------------------------------------------------------- about
inline std::string cpu_model() {
    std::ifstream f("/proc/cpuinfo");
    std::string l;
    int cores = 0;
    std::string name;
    while (std::getline(f, l)) {
        if (l.compare(0, 10, "model name") == 0 && name.empty()) { const std::size_t c = l.find(':'); if (c != std::string::npos) name = l.substr(c + 2); }
        if (l.compare(0, 9, "processor") == 0) ++cores;
    }
    return (name.empty() ? std::string("CPU") : name) + " (" + std::to_string(cores) + ")";
}

inline long mem_total_mb() {
    std::ifstream f("/proc/meminfo");
    std::string k;
    long kb = 0;
    f >> k >> kb;
    return kb / 1024;
}

inline std::string uptime_text() {
    std::ifstream f("/proc/uptime");
    double up = 0;
    f >> up;
    const long s = static_cast<long>(up);
    char b[48];
    std::snprintf(b, sizeof b, "%ldh %02ldm", s / 3600, (s / 60) % 60);
    return b;
}

}  // namespace dg::sys
