// Difiw task model: applications (installed), instances (one launch), system services and
// background tasks. The UI is built on this model; it never parses `ps` output.
// SPDX-License-Identifier: MIT
#pragma once

#include <sys/types.h>

#include <deque>
#include <string>
#include <vector>

namespace dg {

struct AppDef {
    std::string id;           // stable app-id, e.g. "notes"
    std::string name_en, name_ru;
    std::string category;     // work | utility | system | test
    std::string icon;         // icon kind drawn by the shell
    std::string exec, args;
    std::string home;         // private home directory (default /homes/user/apps/<id>)
    int uid = 0;              // unprivileged account the app runs as
    bool hidden = false;      // not listed in the menu (still launchable)
    bool single = false;      // only one instance at a time
    std::vector<std::string> perms;  // e.g. desktop.quit, power
    bool has_perm(const std::string& p) const;
    const std::string& name(bool ru) const { return ru && !name_ru.empty() ? name_ru : name_en; }
};

struct Instance {
    int iid = 0;              // instance-id, unique while the desktop runs
    std::string app_id;
    pid_t pid = 0;
    int uid = 0;
    std::string token;        // secret handed to the process, proves which instance connects
    long start_ms = 0;
    std::vector<int> wins;    // window ids owned by this instance
    bool connected = false;   // completed the HELLO handshake
    bool closing = false;     // user asked to close it
    long kill_at_ms = 0;      // SIGKILL deadline after SIGTERM
    bool dead = false;
    int exit_code = 0, exit_signal = 0;
    // sampled resources
    char state = '?';
    long rss_kb = 0;
    float cpu = 0;
    unsigned long long jiffies = 0;
};

struct Service {
    std::string name, desc, match;  // match = process name (comm)
    bool critical = false;
    pid_t pid = 0;                  // 0 = not running
    char state = '?';
    long rss_kb = 0;
    float cpu = 0;
    unsigned long long jiffies = 0;
};

// One row of the background-tasks panel.
struct BgTask {
    std::string key;          // stable id: "svc:<name>" or "app:<iid>"
    std::string name, detail;
    std::string icon;
    bool critical = false;
    bool service = false;
    int iid = 0;              // for user background apps
    pid_t pid = 0;
    char state = '?';
    long rss_kb = 0;
    float cpu = 0;
};

struct LogEntry {
    long t_ms = 0;
    std::string text;
    bool alert = false;
};

class TaskModel {
public:
    bool load(const std::string& apps_dir, const std::string& services_dir);
    const std::vector<AppDef>& apps() const { return apps_; }
    const AppDef* app(const std::string& id) const;
    std::vector<Instance>& instances() { return inst_; }
    Instance* instance(int iid);
    Instance* by_token(const std::string& token);
    Instance* by_pid(pid_t pid);
    std::vector<Service>& services() { return svc_; }

    // Starts an app as its own unprivileged user in its own home. Returns instance-id or -1.
    int launch(const std::string& app_id, std::string& error);
    void ask_close(int iid);      // polite SIGTERM, then SIGKILL after a grace period
    void kill_now(int iid);
    bool terminate_pid(pid_t pid, bool force);
    // Collects exited children. Returns the instances that finished since the last call.
    std::vector<Instance> reap();
    void tick_kills(long now_ms);
    void sample(long now_ms);     // refresh /proc based numbers (instances + services)
    void set_window_checker(bool (*fn)(int iid)) { has_windows_ = fn; }
    std::vector<BgTask> background_tasks() const;

    void log(const std::string& text, bool alert = false);
    const std::deque<LogEntry>& journal() const { return journal_; }
    void ensure_homes();
    std::string socket_path = "/run/difiw/desktop.sock";
    std::string datadir = "/usr/share/difiw";

private:
    std::vector<AppDef> apps_;
    std::vector<Instance> inst_;
    std::vector<Service> svc_;
    std::deque<LogEntry> journal_;
    int next_iid_ = 1;
    long last_sample_ms_ = 0;
    bool (*has_windows_)(int) = nullptr;
};

long now_ms();

}  // namespace dg
