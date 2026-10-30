// SPDX-License-Identifier: MIT
#include "tasks.hpp"

#include <dirent.h>
#include <fcntl.h>
#include <grp.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

namespace dg {

long now_ms() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

bool AppDef::has_perm(const std::string& p) const { return std::find(perms.begin(), perms.end(), p) != perms.end(); }

namespace {

std::string trim(const std::string& s) {
    std::size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r')) --b;
    return s.substr(a, b - a);
}

std::vector<std::pair<std::string, std::string>> read_kv(const std::string& path) {
    std::vector<std::pair<std::string, std::string>> kv;
    std::ifstream f(path);
    std::string line;
    while (std::getline(f, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#') continue;
        const std::size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        kv.emplace_back(trim(line.substr(0, eq)), trim(line.substr(eq + 1)));
    }
    return kv;
}

std::vector<std::string> list_files(const std::string& dir, const std::string& suffix) {
    std::vector<std::string> out;
    DIR* d = opendir(dir.c_str());
    if (!d) return out;
    while (struct dirent* e = readdir(d)) {
        const std::string n = e->d_name;
        if (n.size() > suffix.size() && n.compare(n.size() - suffix.size(), suffix.size(), suffix) == 0) out.push_back(dir + "/" + n);
    }
    closedir(d);
    std::sort(out.begin(), out.end());
    return out;
}

std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == sep) { if (!cur.empty()) out.push_back(cur); cur.clear(); }
        else cur += c;
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

std::string random_token() {
    unsigned char b[12] = {0};
    const int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd >= 0) { if (read(fd, b, sizeof b) < 0) b[0] = 1; close(fd); }
    char hex[32];
    for (int i = 0; i < 12; ++i) std::snprintf(hex + i * 2, 3, "%02x", b[i]);
    return hex;
}

struct ProcStat {
    bool ok = false;
    char state = '?';
    unsigned long long jiffies = 0;
    long rss_kb = 0;
};

ProcStat read_proc(pid_t pid) {
    ProcStat r;
    char path[64];
    std::snprintf(path, sizeof path, "/proc/%d/stat", static_cast<int>(pid));
    std::ifstream f(path);
    std::string line;
    if (!std::getline(f, line)) return r;
    const std::size_t rp = line.rfind(')');
    if (rp == std::string::npos) return r;
    std::istringstream in(line.substr(rp + 2));
    std::string tok;
    // fields after ")": state ppid pgrp session tty tpgid flags minflt cminflt majflt cmajflt utime stime ...
    std::vector<std::string> v;
    while (in >> tok) v.push_back(tok);
    if (v.size() < 13) return r;
    r.state = v[0][0];
    r.jiffies = std::strtoull(v[11].c_str(), nullptr, 10) + std::strtoull(v[12].c_str(), nullptr, 10);
    std::snprintf(path, sizeof path, "/proc/%d/statm", static_cast<int>(pid));
    std::ifstream sm(path);
    long size = 0, resident = 0;
    if (sm >> size >> resident) r.rss_kb = resident * (sysconf(_SC_PAGESIZE) / 1024);
    r.ok = true;
    return r;
}

std::string read_comm(pid_t pid) {
    char path[64];
    std::snprintf(path, sizeof path, "/proc/%d/comm", static_cast<int>(pid));
    std::ifstream f(path);
    std::string s;
    std::getline(f, s);
    return s;
}

}  // namespace

bool TaskModel::load(const std::string& apps_dir, const std::string& services_dir) {
    apps_.clear();
    svc_.clear();
    for (const auto& file : list_files(apps_dir, ".app")) {
        AppDef a;
        for (const auto& [k, v] : read_kv(file)) {
            if (k == "id") a.id = v;
            else if (k == "name") a.name_en = v;
            else if (k == "name_ru") a.name_ru = v;
            else if (k == "category") a.category = v;
            else if (k == "icon") a.icon = v;
            else if (k == "exec") a.exec = v;
            else if (k == "args") a.args = v;
            else if (k == "home") a.home = v;
            else if (k == "uid") a.uid = std::atoi(v.c_str());
            else if (k == "hidden") a.hidden = v == "1";
            else if (k == "single") a.single = v == "1";
            else if (k == "perms") a.perms = split(v, ',');
        }
        if (a.id.empty() || a.exec.empty() || a.uid <= 0) continue;  // a manifest without an unprivileged uid is refused
        if (a.home.empty()) a.home = "/homes/user/apps/" + a.id;
        if (a.name_en.empty()) a.name_en = a.id;
        apps_.push_back(a);
    }
    for (const auto& file : list_files(services_dir, ".svc")) {
        Service s;
        for (const auto& [k, v] : read_kv(file)) {
            if (k == "name") s.name = v;
            else if (k == "desc") s.desc = v;
            else if (k == "match") s.match = v;
            else if (k == "critical") s.critical = v == "1";
        }
        if (!s.name.empty() && !s.match.empty()) svc_.push_back(s);
    }
    return !apps_.empty();
}

const AppDef* TaskModel::app(const std::string& id) const {
    for (const auto& a : apps_) if (a.id == id) return &a;
    return nullptr;
}

Instance* TaskModel::instance(int iid) {
    for (auto& i : inst_) if (i.iid == iid && !i.dead) return &i;
    return nullptr;
}

Instance* TaskModel::by_token(const std::string& token) {
    for (auto& i : inst_) if (!i.dead && i.token == token) return &i;
    return nullptr;
}

Instance* TaskModel::by_pid(pid_t pid) {
    for (auto& i : inst_) if (!i.dead && i.pid == pid) return &i;
    return nullptr;
}

void TaskModel::log(const std::string& text, bool alert) {
    journal_.push_back({now_ms(), text, alert});
    if (journal_.size() > 40) journal_.pop_front();
    // The journal records events only: never passwords or file contents.
    mkdir("/var/log", 0755);
    FILE* f = std::fopen("/var/log/difiw-desktop.log", "a");
    if (f) {
        std::fprintf(f, "[%ld.%03ld] %s\n", now_ms() / 1000, now_ms() % 1000, text.c_str());
        std::fclose(f);
    }
}

static void make_dir(const std::string& path, mode_t mode, uid_t uid, gid_t gid) {
    std::string cur;
    for (const auto& part : split(path, '/')) {
        cur += "/" + part;
        mkdir(cur.c_str(), 0755);
    }
    if (geteuid() == 0) {
        if (chown(path.c_str(), uid, gid) != 0) {}
        chmod(path.c_str(), mode);
    }
}

void TaskModel::ensure_homes() {
    // /homes and /homes/user are traversable but not listable by apps; each app home is 0700.
    make_dir("/homes", 0755, 0, 0);
    make_dir("/homes/user", 0711, 0, 0);
    make_dir("/homes/user/apps", 0711, 0, 0);
    make_dir("/run/difiw", 0755, 0, 0);
    for (const auto& a : apps_) {
        make_dir(a.home, 0700, static_cast<uid_t>(a.uid), static_cast<gid_t>(a.uid));
        make_dir("/run/difiw/" + a.id, 0700, static_cast<uid_t>(a.uid), static_cast<gid_t>(a.uid));
        make_dir("/run/difiw/" + a.id + "/tmp", 0700, static_cast<uid_t>(a.uid), static_cast<gid_t>(a.uid));
        // a marker so other apps have something real to be refused
        const std::string marker = a.home + "/home-marker.txt";
        if (access(marker.c_str(), F_OK) != 0) {
            FILE* f = std::fopen(marker.c_str(), "w");
            if (f) {
                std::fprintf(f, "private data of %s\n", a.id.c_str());
                std::fclose(f);
                if (geteuid() == 0 && chown(marker.c_str(), static_cast<uid_t>(a.uid), static_cast<gid_t>(a.uid)) != 0) {}
                chmod(marker.c_str(), 0600);
            }
        }
    }
}

int TaskModel::launch(const std::string& app_id, std::string& error) {
    const AppDef* a = app(app_id);
    if (!a) { error = "unknown app: " + app_id; return -1; }
    if (a->single)
        for (auto& i : inst_)
            if (!i.dead && i.app_id == app_id) { return i.iid; }
    ensure_homes();
    Instance in;
    in.iid = next_iid_++;
    in.app_id = app_id;
    in.uid = a->uid;
    in.token = random_token();
    in.start_ms = now_ms();

    std::vector<std::string> argv_s = {a->exec};
    for (const auto& t : split(a->args, ' ')) argv_s.push_back(t);

    const pid_t pid = fork();
    if (pid < 0) { error = std::string("fork: ") + std::strerror(errno); return -1; }
    if (pid == 0) {
        // ---- child: become the unprivileged app user, then exec
        setsid();
        for (int fd = 3; fd < 256; ++fd) close(fd);
        const std::string tmp = "/run/difiw/" + a->id + "/tmp";
        if (geteuid() == 0) {
            if (setgroups(0, nullptr) != 0 || setgid(static_cast<gid_t>(a->uid)) != 0 || setuid(static_cast<uid_t>(a->uid)) != 0) _exit(126);
            if (setuid(0) == 0) _exit(126);  // must not be able to regain root
        }
        prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0);
        if (chdir(a->home.c_str()) != 0 && chdir("/") != 0) _exit(126);
        setenv("HOME", a->home.c_str(), 1);
        setenv("TMPDIR", tmp.c_str(), 1);
        setenv("DIFIW_TOKEN", in.token.c_str(), 1);
        setenv("DIFIW_SOCK", socket_path.c_str(), 1);
        setenv("DIFIW_APP", a->id.c_str(), 1);
        setenv("DIFIW_DATADIR", datadir.c_str(), 1);
        setenv("USER", ("difiw-" + a->id).c_str(), 1);
        setenv("PATH", "/usr/bin:/bin", 1);
        unsetenv("LD_PRELOAD");
        const int nul = open("/dev/null", O_RDWR);
        if (nul >= 0) { dup2(nul, 0); }
        const std::string logp = "/run/difiw/" + a->id + "/app.log";
        const int lg = open(logp.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0600);
        if (lg >= 0) { dup2(lg, 1); dup2(lg, 2); }
        std::vector<char*> argv;
        for (auto& s : argv_s) argv.push_back(const_cast<char*>(s.c_str()));
        argv.push_back(nullptr);
        execv(argv[0], argv.data());
        _exit(127);
    }
    in.pid = pid;
    inst_.push_back(in);
    log("launch app=" + app_id + " iid=" + std::to_string(in.iid) + " pid=" + std::to_string(pid) + " uid=" + std::to_string(a->uid));
    return in.iid;
}

void TaskModel::ask_close(int iid) {
    Instance* i = instance(iid);
    if (!i) return;
    i->closing = true;
    i->kill_at_ms = now_ms() + 2500;
    kill(i->pid, SIGTERM);
    log("close requested iid=" + std::to_string(iid) + " app=" + i->app_id);
}

void TaskModel::kill_now(int iid) {
    Instance* i = instance(iid);
    if (i) kill(i->pid, SIGKILL);
}

bool TaskModel::terminate_pid(pid_t pid, bool force) {
    if (pid <= 1) return false;
    return kill(pid, force ? SIGKILL : SIGTERM) == 0;
}

void TaskModel::tick_kills(long now) {
    for (auto& i : inst_)
        if (!i.dead && i.closing && i.kill_at_ms && now >= i.kill_at_ms) {
            kill(i.pid, SIGKILL);
            i.kill_at_ms = 0;
            log("app did not exit, killed iid=" + std::to_string(i.iid) + " app=" + i.app_id, true);
        }
}

std::vector<Instance> TaskModel::reap() {
    std::vector<Instance> done;
    for (;;) {
        int st = 0;
        const pid_t p = waitpid(-1, &st, WNOHANG);
        if (p <= 0) break;
        Instance* i = by_pid(p);
        if (!i) continue;  // not an app (e.g. a helper child)
        i->dead = true;
        i->exit_code = WIFEXITED(st) ? WEXITSTATUS(st) : 0;
        i->exit_signal = WIFSIGNALED(st) ? WTERMSIG(st) : 0;
        const bool crashed = !i->closing && (i->exit_signal != 0 || i->exit_code != 0);
        log("exit app=" + i->app_id + " iid=" + std::to_string(i->iid) + " code=" + std::to_string(i->exit_code) +
                " signal=" + std::to_string(i->exit_signal),
            crashed);
        done.push_back(*i);
    }
    inst_.erase(std::remove_if(inst_.begin(), inst_.end(), [](const Instance& i) { return i.dead; }), inst_.end());
    return done;
}

void TaskModel::sample(long now) {
    const long dt = last_sample_ms_ ? now - last_sample_ms_ : 0;
    const double hz = static_cast<double>(sysconf(_SC_CLK_TCK));
    auto cpu_of = [&](unsigned long long prev, unsigned long long cur) -> float {
        if (dt <= 0 || prev == 0) return 0.0f;
        return static_cast<float>(static_cast<double>(cur - prev) / hz / (static_cast<double>(dt) / 1000.0) * 100.0);
    };
    for (auto& i : inst_) {
        const ProcStat p = read_proc(i.pid);
        if (!p.ok) continue;
        i.cpu = cpu_of(i.jiffies, p.jiffies);
        i.jiffies = p.jiffies; i.state = p.state; i.rss_kb = p.rss_kb;
    }
    // services: look the processes up by name
    DIR* d = opendir("/proc");
    std::vector<std::pair<pid_t, std::string>> procs;
    if (d) {
        while (struct dirent* e = readdir(d)) {
            const pid_t pid = static_cast<pid_t>(std::atoi(e->d_name));
            if (pid <= 0) continue;
            procs.emplace_back(pid, read_comm(pid));
        }
        closedir(d);
    }
    for (auto& s : svc_) {
        pid_t found = 0;
        for (const auto& [pid, comm] : procs) if (comm == s.match) { found = pid; break; }
        if (s.match == "difiw-desktop") found = getpid();
        if (found != s.pid) { s.pid = found; s.jiffies = 0; }
        if (!found) { s.state = '-'; s.rss_kb = 0; s.cpu = 0; continue; }
        const ProcStat p = read_proc(found);
        if (!p.ok) continue;
        s.cpu = cpu_of(s.jiffies, p.jiffies);
        s.jiffies = p.jiffies; s.state = p.state; s.rss_kb = p.rss_kb;
    }
    last_sample_ms_ = now;
}

// Background tasks = system services + user-launched apps that currently have no window.
std::vector<BgTask> TaskModel::background_tasks() const {
    std::vector<BgTask> out;
    for (const auto& s : svc_) {
        if (!s.pid) continue;
        BgTask t;
        t.key = "svc:" + s.name;
        t.name = s.name;
        t.detail = s.desc;
        t.icon = s.critical ? "shield" : "gear";
        t.critical = s.critical;
        t.service = true;
        t.pid = s.pid; t.state = s.state; t.rss_kb = s.rss_kb; t.cpu = s.cpu;
        out.push_back(t);
    }
    for (const auto& i : inst_) {
        if (i.dead) continue;
        if (has_windows_ && has_windows_(i.iid)) continue;
        const AppDef* a = nullptr;
        for (const auto& x : apps_) if (x.id == i.app_id) a = &x;
        BgTask t;
        t.key = "app:" + std::to_string(i.iid);
        t.name = (a ? a->id : i.app_id) + "#" + std::to_string(i.iid);  // unique and short enough for the grid
        t.detail = a ? a->name_en + " / " + a->name_ru : "";
        t.icon = a ? a->icon : "gear";
        t.iid = i.iid; t.pid = i.pid; t.state = i.state; t.rss_kb = i.rss_kb; t.cpu = i.cpu;
        out.push_back(t);
    }
    return out;
}

}  // namespace dg
