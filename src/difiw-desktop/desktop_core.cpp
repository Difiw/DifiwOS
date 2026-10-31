// SPDX-License-Identifier: MIT
// Desktop core: initialisation, main loop, framebuffer, input devices, clients, windows, control.
#include <arpa/inet.h>
#include <dirent.h>
#include <fcntl.h>
#include <net/if.h>
#include <linux/fb.h>
#include <linux/kd.h>
#include <poll.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/reboot.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <termios.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <sstream>

#include "desktop.hpp"

namespace dg {

using namespace proto;

namespace {
Desktop* g_self = nullptr;
volatile sig_atomic_t g_term = 0;
long now_us() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000000L + ts.tv_nsec / 1000L;
}
void on_term(int) { g_term = 1; }
}  // namespace

// ---------------------------------------------------------------------------- init
static std::vector<std::pair<std::string, std::string>> read_conf(const std::string& path) {
    std::vector<std::pair<std::string, std::string>> kv;
    std::ifstream f(path);
    std::string line;
    while (std::getline(f, line)) {
        const std::size_t eq = line.find('=');
        if (line.empty() || line[0] == '#' || eq == std::string::npos) continue;
        kv.emplace_back(line.substr(0, eq), line.substr(eq + 1));
    }
    return kv;
}

// Wallpaper: the shipped photo, a procedural gradient, or a .ppm chosen by the user. Also recomputes the blurred copy
// (glass panels) and the wallpaper + scrim under the dock.
void Desktop::build_wallpaper() {
    struct Grad { const char* id; u32 top, bottom, glow; };
    static const Grad grads[] = {{"graphite", 0xFF141821, 0xFF2A3142, 0xFF4C8DFF}, {"ocean", 0xFF0B2545, 0xFF13A4B8, 0xFF8DE0F2},
                                 {"sunset", 0xFF2B1B4F, 0xFFE0703A, 0xFFFFC27A}, {"forest", 0xFF0C1F17, 0xFF2F8F5B, 0xFFA3E8B8}};
    bool done = false;
    if (set_.wallpaper.compare(0, 5, "file:") == 0) {
        Image raw;
        if (load_ppm(set_.wallpaper.substr(5).c_str(), raw)) { wall_ = scale_cover(raw, W_, H_); done = true; }
    } else {
        for (const Grad& g : grads) {
            if (set_.wallpaper != g.id) continue;
            wall_.w = W_; wall_.h = H_; wall_.px.resize(static_cast<std::size_t>(W_) * H_);
            for (int y = 0; y < H_; ++y)
                for (int x = 0; x < W_; ++x) {
                    const float dx = static_cast<float>(x - W_ * 3 / 4) / static_cast<float>(W_), dy = static_cast<float>(y - H_ / 4) / static_cast<float>(H_);
                    const float glow = std::max(0.0f, 1.0f - std::sqrt(dx * dx + dy * dy) * 1.6f);
                    wall_.px[static_cast<std::size_t>(y) * W_ + x] = mix(mix(g.top, g.bottom, static_cast<float>(y) / static_cast<float>(H_)), g.glow, glow * glow * 0.55f) | 0xFF000000u;
                }
            done = true;
        }
    }
    if (!done) {
        Image raw;
        if (load_ppm((opt_.datadir + "/wallpaper.ppm").c_str(), raw)) wall_ = scale_cover(raw, W_, H_);
        else {  // fallback: calm graphite-to-blue gradient
            wall_.w = W_; wall_.h = H_; wall_.px.resize(static_cast<std::size_t>(W_) * H_);
            for (int y = 0; y < H_; ++y)
                for (int x = 0; x < W_; ++x)
                    wall_.px[static_cast<std::size_t>(y) * W_ + x] = mix(0xFF0E1118u, 0xFF1B2A4Au, static_cast<float>(y) / static_cast<float>(H_));
        }
    }
    wall_blur_ = wall_;
    box_blur(wall_blur_, 12, 2);
    base_ = wall_;
    const int sh = std::min(H_, 240);
    for (int i = 0; i < sh; ++i) {
        const int y = H_ - sh + i;
        const float a = static_cast<float>(i * 130 / sh) / 255.0f;
        for (int x = 0; x < W_; ++x) {
            u32& p = base_.px[static_cast<std::size_t>(y) * W_ + x];
            p = mix(p, 0xFF05070Au, a) | 0xFF000000u;
        }
    }
    dirty_ = true;
}

bool Desktop::init(const Options& opt) {
    opt_ = opt;
    g_self = this;
    if (!load_font((opt_.datadir + "/font.psf").c_str())) {
        std::fprintf(stderr, "difiw-desktop: cannot load %s/font.psf\n", opt_.datadir.c_str());
        return false;
    }
    for (const auto& [k, v] : read_conf(opt_.etcdir + "/desktop.conf")) {
        if (k == "scale") set_.scale = std::atoi(v.c_str()) >= 2 ? 2 : 1;
        else if (k == "reduce_motion") set_.reduce_motion = v == "1";
        else if (k == "transparency") set_.transparency = v != "0";
        else if (k == "lang") set_.ru = v != "en";
        else if (k == "winkey") set_.winkey = v == "1";
        else if (k == "tips") set_.tips = v != "0";
        else if (k == "menu") set_.menu_list = v == "list";
        else if (k == "wallpaper" && !v.empty()) set_.wallpaper = v;
        else if (k == "pinned") { std::istringstream ps(v); std::string id; while (std::getline(ps, id, ',')) if (!id.empty()) pinned_.push_back(id); }
    }
    if (opt_.headless) {
        W_ = opt_.hw; H_ = opt_.hh;
    } else if (!open_fb()) {
        std::fprintf(stderr, "difiw-desktop: no usable /dev/fb0 (graphics mode not set by the bootloader)\n");
        return false;
    }
    back_.alloc(W_, H_);

    build_wallpaper();

    if (!tasks_.load(opt_.datadir + "/apps", opt_.etcdir + "/services.d")) {
        std::fprintf(stderr, "difiw-desktop: no applications registered in %s/apps\n", opt_.datadir.c_str());
        return false;
    }
    tasks_.set_window_checker([](int iid) { return g_self && g_self->instance_has_windows(iid); });

    // sockets
    mkdir(opt_.rundir.c_str(), 0755);
    const std::string sock = opt_.rundir + "/desktop.sock", ctl = opt_.rundir + "/control.sock";
    tasks_.socket_path = sock;
    tasks_.datadir = opt_.datadir;
    auto make_listener = [](const std::string& path, mode_t mode) -> int {
        unlink(path.c_str());
        const int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (fd < 0) return -1;
        struct sockaddr_un sa;
        std::memset(&sa, 0, sizeof sa);
        sa.sun_family = AF_UNIX;
        std::strncpy(sa.sun_path, path.c_str(), sizeof sa.sun_path - 1);
        if (bind(fd, reinterpret_cast<struct sockaddr*>(&sa), sizeof sa) != 0 || listen(fd, 16) != 0) { close(fd); return -1; }
        chmod(path.c_str(), mode);
        return fd;
    };
    listen_fd_ = make_listener(sock, 0666);
    ctl_fd_ = make_listener(ctl, 0600);
    if (listen_fd_ < 0 || ctl_fd_ < 0) { std::fprintf(stderr, "difiw-desktop: cannot create sockets in %s\n", opt_.rundir.c_str()); return false; }

    tasks_.ensure_homes();
    tasks_.sample(now_ms());
    mx_ = W_ / 2;
    my_ = H_ / 3;
    if (!opt_.headless) {
        enter_console_graphics();
        scan_devices();
    }
    save_settings();  // also writes /run/difiw-lang, so apps start in the desktop's language
    restore_session();
    tasks_.log("desktop started " + std::to_string(W_) + "x" + std::to_string(H_) + " scale=" + std::to_string(set_.scale));
    return true;
}

Desktop::~Desktop() {
    for (auto& w : wins_) free_buffer(*w);
    for (auto& c : clients_) if (c.fd >= 0) close(c.fd);
    if (listen_fd_ >= 0) close(listen_fd_);
    if (ctl_fd_ >= 0) close(ctl_fd_);
    leave_console_graphics();
    if (fb_.mem) munmap(fb_.mem, fb_.len);
    if (fb_.fd >= 0) close(fb_.fd);
}

// ------------------------------------------------------------------- framebuffer
bool Desktop::open_fb() {
    fb_.fd = open("/dev/fb0", O_RDWR | O_CLOEXEC);
    if (fb_.fd < 0) return false;
    struct fb_var_screeninfo v;
    struct fb_fix_screeninfo f;
    if (ioctl(fb_.fd, FBIOGET_VSCREENINFO, &v) != 0 || ioctl(fb_.fd, FBIOGET_FSCREENINFO, &f) != 0) return false;
    if (v.bits_per_pixel != 32 && v.bits_per_pixel != 24 && v.bits_per_pixel != 16) return false;
    W_ = static_cast<int>(v.xres);
    H_ = static_cast<int>(v.yres);
    fb_.bpp = static_cast<int>(v.bits_per_pixel);
    fb_.stride = static_cast<int>(f.line_length);
    fb_.ro = static_cast<int>(v.red.offset); fb_.rl = static_cast<int>(v.red.length);
    fb_.go = static_cast<int>(v.green.offset); fb_.gl = static_cast<int>(v.green.length);
    fb_.bo = static_cast<int>(v.blue.offset); fb_.bl = static_cast<int>(v.blue.length);
    fb_.len = f.smem_len ? f.smem_len : static_cast<std::size_t>(fb_.stride) * static_cast<std::size_t>(H_);
    void* m = mmap(nullptr, fb_.len, PROT_READ | PROT_WRITE, MAP_SHARED, fb_.fd, 0);
    if (m == MAP_FAILED) return false;
    fb_.mem = static_cast<unsigned char*>(m);
    return true;
}

void Desktop::enter_console_graphics() {
    // The console to put into graphics mode must be a virtual terminal. When started from a serial line or in the
    // background the controlling terminal is not one: use the active VT (/dev/tty0), otherwise the kernel keeps
    // drawing its text (and cursor) over our pixels.
    tty_ = open("/dev/tty", O_RDWR | O_CLOEXEC);
    char kt = 0;
    if (tty_ < 0 || ioctl(tty_, KDGKBTYPE, &kt) != 0) {
        if (tty_ >= 0) close(tty_);
        tty_ = open("/dev/tty0", O_RDWR | O_CLOEXEC);
    }
    if (tty_ < 0) tty_ = STDIN_FILENO;
    if (ioctl(tty_, KDGKBMODE, &old_kbmode_) != 0) old_kbmode_ = K_UNICODE;
    ioctl(tty_, KDSKBMODE, K_OFF);
    gfx_mode_ = ioctl(tty_, KDSETMODE, KD_GRAPHICS) == 0;
    const char* s = "\033[9;0]\033[?25l";
    if (write(tty_, s, std::strlen(s)) < 0) {}
}

void Desktop::leave_console_graphics() {
    for (auto& d : devs_) if (d.fd >= 0) { ioctl(d.fd, EVIOCGRAB, 0); close(d.fd); }
    devs_.clear();
    if (tty_ >= 0) {
        if (gfx_mode_) ioctl(tty_, KDSETMODE, KD_TEXT);
        if (gfx_mode_ || old_kbmode_ != 3) ioctl(tty_, KDSKBMODE, old_kbmode_);
        const char* clr = "\033[?25h\033[2J\033[H";
        if (write(tty_, clr, std::strlen(clr)) < 0) {}
        tty_ = -1;
    }
}

static inline std::uint32_t pack_px(const Desktop* /*d*/, u32 c, int bpp, int ro, int go, int bo, int rl, int gl, int bl) {
    const std::uint32_t r = (c >> 16) & 0xff, g = (c >> 8) & 0xff, b = c & 0xff;
    if (bpp == 16) return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
    return ((r >> (8 - rl)) << ro) | ((g >> (8 - gl)) << go) | ((b >> (8 - bl)) << bo);
}

void Desktop::blit_region(int x0, int y0, int w, int h) {
    if (!fb_.mem) return;
    x0 = std::max(0, x0); y0 = std::max(0, y0);
    w = std::min(w, W_ - x0); h = std::min(h, H_ - y0);
    if (w <= 0 || h <= 0) return;
    const bool fast32 = fb_.bpp == 32 && fb_.ro == 16 && fb_.go == 8 && fb_.bo == 0;
    const bool fast24 = fb_.bpp == 24 && fb_.ro == 16 && fb_.go == 8 && fb_.bo == 0;
    // The row is converted in ordinary memory and copied with one wide memcpy: many small stores straight into
    // video memory are very slow inside a virtual machine.
    static std::vector<unsigned char> tmp;
    const std::size_t bytes = static_cast<std::size_t>(w) * static_cast<std::size_t>(fb_.bpp / 8);
    if (tmp.size() < bytes) tmp.resize(bytes + 16);
    for (int y = y0; y < y0 + h; ++y) {
        const u32* src = back_.px + static_cast<std::size_t>(y) * back_.stride + x0;
        unsigned char* dst = fb_.mem + static_cast<std::size_t>(y) * fb_.stride + static_cast<std::size_t>(x0) * static_cast<std::size_t>(fb_.bpp / 8);
        if (fast32) {
            std::memcpy(dst, src, bytes);
            continue;
        }
        unsigned char* t = tmp.data();
        if (fast24) {
            for (int x = 0; x < w; ++x) {
                const u32 p = src[x];
                t[x * 3] = static_cast<unsigned char>(p);
                t[x * 3 + 1] = static_cast<unsigned char>(p >> 8);
                t[x * 3 + 2] = static_cast<unsigned char>(p >> 16);
            }
        } else if (fb_.bpp == 24) {
            for (int x = 0; x < w; ++x) {
                t[x * 3 + fb_.ro / 8] = static_cast<unsigned char>(src[x] >> 16);
                t[x * 3 + fb_.go / 8] = static_cast<unsigned char>(src[x] >> 8);
                t[x * 3 + fb_.bo / 8] = static_cast<unsigned char>(src[x]);
            }
        } else if (fb_.bpp == 16) {
            for (int x = 0; x < w; ++x)
                reinterpret_cast<std::uint16_t*>(t)[x] = static_cast<std::uint16_t>(pack_px(this, src[x], 16, fb_.ro, fb_.go, fb_.bo, fb_.rl, fb_.gl, fb_.bl));
        } else {
            for (int x = 0; x < w; ++x)
                reinterpret_cast<std::uint32_t*>(t)[x] = pack_px(this, src[x], 32, fb_.ro, fb_.go, fb_.bo, fb_.rl, fb_.gl, fb_.bl);
        }
        std::memcpy(dst, t, bytes);
    }
}

static const char* const kCursor[] = {
    "X           ", "XX          ", "X.X         ", "X..X        ", "X...X       ", "X....X      ", "X.....X     ",
    "X......X    ", "X.......X   ", "X........X  ", "X.........X ", "X......XXXXX", "X...X..X    ", "X..XX..X    ",
    "X.X  X..X   ", "XX   X..X   ", "X     X..X  ", "      X..X  ", "       XX   "};
static constexpr int kCurW = 12, kCurH = 19;

void Desktop::draw_cursor() {
    if (!fb_.mem) return;
    for (int y = 0; y < kCurH; ++y)
        for (int x = 0; x < kCurW; ++x) {
            const char c = kCursor[y][x];
            if (c == ' ') continue;
            const int px = mx_ + x, py = my_ + y;
            if (px < 0 || py < 0 || px >= W_ || py >= H_) continue;
            const u32 rgb = c == 'X' ? 0x000000u : 0xFFFFFFu;
            unsigned char* dst = fb_.mem + static_cast<std::size_t>(py) * fb_.stride + static_cast<std::size_t>(px) * static_cast<std::size_t>(fb_.bpp / 8);
            if (fb_.bpp == 24) {
                dst[fb_.ro / 8] = static_cast<unsigned char>(rgb >> 16);
                dst[fb_.go / 8] = static_cast<unsigned char>(rgb >> 8);
                dst[fb_.bo / 8] = static_cast<unsigned char>(rgb);
            } else if (fb_.bpp == 16) {
                *reinterpret_cast<std::uint16_t*>(dst) = static_cast<std::uint16_t>(pack_px(this, rgb, 16, fb_.ro, fb_.go, fb_.bo, fb_.rl, fb_.gl, fb_.bl));
            } else {
                *reinterpret_cast<std::uint32_t*>(dst) = pack_px(this, rgb, 32, fb_.ro, fb_.go, fb_.bo, fb_.rl, fb_.gl, fb_.bl);
            }
        }
}

// Copies only what changed since the previous frame to the framebuffer. Writing to video memory is by far the
// most expensive step in a virtual machine, so a still desktop costs nothing and small animations stay small.
void Desktop::present() {
    if (!fb_.mem) return;
    const std::size_t n = static_cast<std::size_t>(W_) * static_cast<std::size_t>(H_);
    if (prev_.size() != n) prev_.assign(n, 0);
    for (int y = 0; y < H_; ++y) {
        const u32* a = back_.px + static_cast<std::size_t>(y) * back_.stride;
        u32* b = prev_.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(W_);
        if (std::memcmp(a, b, static_cast<std::size_t>(W_) * 4) == 0) continue;
        int x0 = 0, x1 = W_ - 1;
        while (a[x0] == b[x0]) ++x0;
        while (a[x1] == b[x1]) --x1;
        blit_region(x0, y, x1 - x0 + 1, 1);
        std::memcpy(b + x0, a + x0, static_cast<std::size_t>(x1 - x0 + 1) * 4);
    }
    draw_cursor();
}

// ------------------------------------------------------------------ input devices
static inline bool testbit(const unsigned long* a, int n) { return (a[n / (8 * sizeof(long))] >> (n % (8 * sizeof(long)))) & 1; }

void Desktop::scan_devices() {
    for (int i = 0; i < 32; ++i) {
        char path[64];
        std::snprintf(path, sizeof path, "/dev/input/event%d", i);
        const int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) continue;
        struct stat sb;
        fstat(fd, &sb);
        bool dup = false;
        for (const auto& o : devs_) {
            struct stat s2;
            if (fstat(o.fd, &s2) == 0 && s2.st_rdev == sb.st_rdev) { dup = true; break; }
        }
        if (dup) { close(fd); continue; }
        unsigned long ev[(EV_MAX + 8 * sizeof(long)) / (8 * sizeof(long))] = {0};
        unsigned long key[(KEY_MAX + 8 * sizeof(long)) / (8 * sizeof(long))] = {0};
        unsigned long absb[(ABS_MAX + 8 * sizeof(long)) / (8 * sizeof(long))] = {0};
        unsigned long relb[(REL_MAX + 8 * sizeof(long)) / (8 * sizeof(long))] = {0};
        ioctl(fd, EVIOCGBIT(0, sizeof ev), ev);
        ioctl(fd, EVIOCGBIT(EV_KEY, sizeof key), key);
        ioctl(fd, EVIOCGBIT(EV_ABS, sizeof absb), absb);
        ioctl(fd, EVIOCGBIT(EV_REL, sizeof relb), relb);
        Dev d;
        d.fd = fd;
        d.kbd = testbit(ev, EV_KEY) && testbit(key, KEY_A) && testbit(key, KEY_ENTER);
        d.abs = testbit(ev, EV_ABS) && testbit(absb, ABS_X) && testbit(absb, ABS_Y) && (testbit(key, BTN_LEFT) || testbit(key, BTN_TOUCH));
        d.rel = testbit(ev, EV_REL) && testbit(relb, REL_X) && testbit(relb, REL_Y) && testbit(key, BTN_LEFT);
        if (d.abs) {
            struct input_absinfo ai;
            if (ioctl(fd, EVIOCGABS(ABS_X), &ai) == 0) { d.minx = ai.minimum; d.maxx = std::max(ai.maximum, ai.minimum + 1); }
            if (ioctl(fd, EVIOCGABS(ABS_Y), &ai) == 0) { d.miny = ai.minimum; d.maxy = std::max(ai.maximum, ai.minimum + 1); }
        }
        if (!d.kbd && !d.abs && !d.rel) { close(fd); continue; }
        if (d.kbd) ioctl(fd, EVIOCGRAB, 1);
        devs_.push_back(d);
    }
}

void Desktop::handle_dev_event(const Dev& d, const struct input_event& e) {
    if (e.type == EV_KEY) {
        if (e.code == BTN_LEFT || e.code == BTN_TOUCH) on_button(1, e.value != 0);
        else if (e.code == BTN_RIGHT) on_button(2, e.value != 0);
        else if (d.kbd && e.code < 256) on_key(e.code, e.value);
    } else if (e.type == EV_REL) {
        if (e.code == REL_X) on_pointer_move(mx_ + e.value, my_);
        else if (e.code == REL_Y) on_pointer_move(mx_, my_ + e.value);
        else if (e.code == REL_WHEEL) on_wheel(e.value);
    } else if (e.type == EV_ABS && d.abs) {
        if (e.code == ABS_X) on_pointer_move((e.value - d.minx) * (W_ - 1) / (d.maxx - d.minx), my_);
        else if (e.code == ABS_Y) on_pointer_move(mx_, (e.value - d.miny) * (H_ - 1) / (d.maxy - d.miny));
    }
}

// ----------------------------------------------------------------------- windows
Window* Desktop::win(int id) {
    for (auto& w : wins_) if (w->id == id) return w.get();
    return nullptr;
}

bool Desktop::instance_has_windows(int iid) const {
    for (const auto& w : wins_) if (w->iid == iid) return true;
    return false;
}

Rect Desktop::work_area() const {
    const int dock = S(tok::kDockHeight + 2 * tok::kDockMargin);
    return {0, 0, W_, H_ - dock};
}

Rect Desktop::frame_of(const Window& w) const {
    const int p = S(6);
    return {w.x, w.y, w.cw + 2 * p, title_h() + w.ch + p};
}

Rect Desktop::content_of(const Window& w) const {
    const int p = S(6);
    return {w.x + p, w.y + title_h(), w.cw, w.ch};
}

void Desktop::clamp_window(Window& w) {
    const Rect f = frame_of(w);
    const Rect wa = work_area();
    w.x = clampi(w.x, 0, std::max(0, W_ - f.w));
    w.y = clampi(w.y, 0, std::max(0, wa.h - title_h()));
}

void Desktop::free_buffer(Window& w) {
    if (w.map) munmap(w.map, w.len);
    if (w.fd >= 0) close(w.fd);
    w.map = nullptr; w.fd = -1; w.len = 0;
    w.buf = Surface();
}

bool Desktop::attach_buffer(Window& w, int fd, int bw, int bh) {
    if (bw < 1 || bh < 1 || bw > 4096 || bh > 4096) return false;
    struct stat sb;
    const std::size_t need = static_cast<std::size_t>(bw) * static_cast<std::size_t>(bh) * 4;
    if (fstat(fd, &sb) != 0 || static_cast<std::size_t>(sb.st_size) < need) return false;
    void* m = mmap(nullptr, need, PROT_READ, MAP_SHARED, fd, 0);
    if (m == MAP_FAILED) return false;
    free_buffer(w);
    w.fd = fd;
    w.map = static_cast<u32*>(m);
    w.len = need;
    w.buf.wrap(w.map, bw, bh);
    w.attached = true;
    return true;
}

// Never block the desktop on a slow application: when its queue is full the event is dropped.
void Desktop::send_to_client(int fd, const Msg& m) { send_msg(fd, m, -1, MSG_DONTWAIT); }

void Desktop::create_window(ClientConn& c, const Msg& m, int fd) {
    Msg r;
    r.type = S_CREATED;
    if (!c.hello || fd < 0) { if (fd >= 0) close(fd); send_to_client(c.fd, r); return; }
    auto w = std::make_unique<Window>();
    int cw = clampi(m.i[0], tok::kMinWindowW, std::max(tok::kMinWindowW, W_ - S(40)));
    int ch = clampi(m.i[1], tok::kMinWindowH, std::max(tok::kMinWindowH, work_area().h - title_h() - S(30)));
    if (!attach_buffer(*w, fd, m.i[0], m.i[1])) { close(fd); send_to_client(c.fd, r); return; }
    w->id = next_win_++;
    w->iid = c.iid;
    w->client = c.fd;
    w->title = m.s;
    w->cw = cw; w->ch = ch;
    const int n = static_cast<int>(wins_.size());
    w->x = S(70) + (n % 8) * S(34);
    w->y = S(40) + (n % 8) * S(30);
    clamp_window(*w);
    w->opened_ms = now_ms();
    r.i[0] = w->id; r.i[1] = cw; r.i[2] = ch;
    Instance* in = tasks_.instance(c.iid);
    if (in) in->wins.push_back(w->id);
    const int id = w->id;
    wins_.push_back(std::move(w));
    send_to_client(c.fd, r);
    set_focus(id);
    dirty_ = true;
}

void Desktop::destroy_window(int id) {
    for (std::size_t i = 0; i < wins_.size(); ++i)
        if (wins_[i]->id == id) {
            Instance* in = tasks_.instance(wins_[i]->iid);
            if (in) in->wins.erase(std::remove(in->wins.begin(), in->wins.end(), id), in->wins.end());
            free_buffer(*wins_[i]);
            wins_.erase(wins_.begin() + static_cast<std::ptrdiff_t>(i));
            break;
        }
    if (focus_ == id) { focus_ = 0; focus_next(); }
    if (drag_win_ == id) drag_ = Drag::None;
    dirty_ = true;
}

void Desktop::raise(int id) {
    for (std::size_t i = 0; i < wins_.size(); ++i)
        if (wins_[i]->id == id && i + 1 < wins_.size()) {
            auto w = std::move(wins_[i]);
            wins_.erase(wins_.begin() + static_cast<std::ptrdiff_t>(i));
            wins_.push_back(std::move(w));
            break;
        }
}

void Desktop::set_focus(int id) {
    Window* nw = win(id);
    if (nw && nw->minimized) return;
    if (focus_ == id) return;
    if (Window* old = win(focus_)) { Msg m; m.type = S_FOCUS; m.i[0] = old->id; m.i[1] = 0; send_to_client(old->client, m); }
    focus_ = nw ? id : 0;
    if (nw) {
        raise(id);
        Msg m; m.type = S_FOCUS; m.i[0] = id; m.i[1] = 1;
        send_to_client(nw->client, m);
    }
    dirty_ = true;
}

void Desktop::focus_next() {  // Alt+Tab: the window below the top one comes forward
    std::vector<int> order;
    for (const auto& w : wins_) if (!w->minimized) order.push_back(w->id);
    if (order.empty()) { focus_ = 0; return; }
    if (focus_ == 0 || order.size() == 1) { set_focus(order.back()); return; }
    set_focus(order[order.size() - 2]);
}

void Desktop::toggle_maximize(Window& w) {
    if (w.maximized) {
        w.maximized = false;
        w.x = w.restore.x; w.y = w.restore.y;
        configure(w, w.restore.w - 2 * S(6), w.restore.h - title_h() - S(6));
    } else {
        w.restore = frame_of(w);
        w.restore.x = w.x; w.restore.y = w.y;
        w.maximized = true;
        const Rect wa = work_area();
        w.x = wa.x; w.y = wa.y;
        configure(w, wa.w - 2 * S(6), wa.h - title_h() - S(6));
    }
    dirty_ = true;
}

void Desktop::minimize(Window& w) {
    w.minimized = true;
    if (focus_ == w.id) { focus_ = 0; focus_next(); }
    dirty_ = true;
}

void Desktop::restore(Window& w) {
    w.minimized = false;
    set_focus(w.id);
    dirty_ = true;
}

void Desktop::configure(Window& w, int cw, int ch) {
    w.cw = std::max(tok::kMinWindowW, cw);
    w.ch = std::max(tok::kMinWindowH, ch);
    Msg m;
    m.type = S_CONFIGURE;
    m.i[0] = w.id; m.i[1] = w.cw; m.i[2] = w.ch;
    send_to_client(w.client, m);
}

void Desktop::request_close_window(Window& w) {
    Msg m;
    m.type = S_CLOSE;
    m.i[0] = w.id;
    send_to_client(w.client, m);
}

Window* Desktop::top_window_at(int x, int y, bool) {
    for (std::size_t i = wins_.size(); i-- > 0;) {
        Window& w = *wins_[i];
        if (!w.minimized && frame_of(w).has(x, y)) return &w;
    }
    return nullptr;
}

// ----------------------------------------------------------------------- clients
void Desktop::accept_clients() {
    for (;;) {
        const int fd = accept4(listen_fd_, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (fd < 0) break;
        ClientConn c;
        c.fd = fd;
        clients_.push_back(c);
    }
}

void Desktop::drop_client(int fd) {
    for (std::size_t i = 0; i < clients_.size(); ++i)
        if (clients_[i].fd == fd) {
            std::vector<int> ids;
            for (const auto& w : wins_) if (w->client == fd) ids.push_back(w->id);
            for (int id : ids) destroy_window(id);
            close(fd);
            clients_.erase(clients_.begin() + static_cast<std::ptrdiff_t>(i));
            return;
        }
}

void Desktop::handle_client_msg(ClientConn& c, const Msg& m, int fd) {
    Instance* in = c.iid >= 0 ? tasks_.instance(c.iid) : nullptr;
    switch (m.type) {
        case C_HELLO: {
            Instance* cand = tasks_.by_token(m.s);
            bool ok = cand != nullptr;
            if (ok && geteuid() == 0) {  // the connecting process must really be that app's user
                struct ucred cred;
                socklen_t len = sizeof cred;
                ok = getsockopt(c.fd, SOL_SOCKET, SO_PEERCRED, &cred, &len) == 0 && static_cast<int>(cred.uid) == cand->uid;
            }
            if (!ok) { tasks_.log("rejected connection with a bad token", true); close(c.fd); c.fd = -2; break; }
            c.iid = cand->iid; c.hello = true; cand->connected = true;
            Msg r;
            r.type = S_WELCOME;
            r.i[0] = set_.scale; r.i[1] = set_.reduce_motion ? 1 : 0; r.i[2] = set_.transparency ? 1 : 0;
            send_to_client(c.fd, r);
            break;
        }
        case C_CREATE: create_window(c, m, fd); fd = -1; break;
        case C_ATTACH: {
            Window* w = win(m.i[0]);
            if (w && w->iid == c.iid && fd >= 0 && attach_buffer(*w, fd, m.i[1], m.i[2])) fd = -1;
            dirty_ = true;
            break;
        }
        case C_COMMIT: if (win(m.i[0]) && win(m.i[0])->iid == c.iid) dirty_ = true; break;
        case C_TITLE: { Window* w = win(m.i[0]); if (w && w->iid == c.iid) { w->title = m.s; dirty_ = true; } break; }
        case C_DESTROY: { Window* w = win(m.i[0]); if (w && w->iid == c.iid) destroy_window(w->id); break; }
        case C_REQUEST: {
            const AppDef* a = in ? tasks_.app(in->app_id) : nullptr;
            const int rq = m.i[0];
            const char* need = rq == RQ_QUIT_DESKTOP ? "desktop.quit" : rq == RQ_SET ? "settings" : rq == RQ_NET ? "network" : "power";
            if (!a || !a->has_perm(need)) {
                tasks_.log("permission denied app=" + (in ? in->app_id : std::string("?")) + " request=" + std::to_string(rq), true);
                { const AppDef* da = in ? tasks_.app(in->app_id) : nullptr; notify((da ? da->name(set_.ru) : std::string("?")) + t(": request refused (no permission).", ": запрос отклонён (нет разрешения)."), true); }
                Msg r; r.type = S_DENIED; r.i[0] = rq; send_to_client(c.fd, r);
            } else if (rq == RQ_NET) {
                const std::string kv = m.s;
                const std::size_t sp = kv.find(' ');
                if (sp != std::string::npos) net_action(kv.substr(0, sp), kv.substr(sp + 1));
            } else if (rq == RQ_SET) {
                const std::string kv = m.s;
                const std::size_t eq = kv.find('=');
                if (eq != std::string::npos) apply_kv(kv.substr(0, eq), kv.substr(eq + 1));
            } else if (rq == RQ_QUIT_DESKTOP) {
                leave_desktop();
            } else if (rq == RQ_POWEROFF) {
                do_power(false);
            } else if (rq == RQ_REBOOT) {
                do_power(true);
            }
            break;
        }
        default: break;
    }
    if (fd >= 0) close(fd);
}

void Desktop::leave_desktop() { save_session(); quit_ = true; exit_code_ = 0; }

void Desktop::do_power(bool rb) {
    tasks_.log(rb ? "reboot requested" : "power off requested");
    quit_ = true;
    exit_code_ = rb ? 11 : 12;  // the launcher (`screen`) performs the real action after the desktop is gone
}

// ----------------------------------------------------------------- control socket
void Desktop::accept_ctl() {
    for (;;) {
        const int fd = accept4(ctl_fd_, nullptr, nullptr, SOCK_CLOEXEC);
        if (fd < 0) break;
        struct timeval tv = {1, 0};
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        Msg m;
        if (recv_msg(fd, m, nullptr) == 1 && m.type == X_CMD) {
            const std::vector<std::string> lines = control(m.s);
            for (std::size_t i = 0; i < lines.size(); ++i) {
                Msg r;
                r.type = X_REPLY;
                set_str(r, lines[i].c_str());
                r.i[0] = i + 1 == lines.size() ? 1 : 0;
                send_msg(fd, r);
            }
            if (lines.empty()) { Msg r; r.type = X_REPLY; r.i[0] = 1; send_msg(fd, r); }
        }
        close(fd);
    }
}

void Desktop::write_screenshot(const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return;
    std::fprintf(f, "P6\n%d %d\n255\n", W_, H_);
    std::vector<unsigned char> row(static_cast<std::size_t>(W_) * 3);
    for (int y = 0; y < H_; ++y) {
        for (int x = 0; x < W_; ++x) {
            const u32 c = back_.px[static_cast<std::size_t>(y) * back_.stride + x];
            row[static_cast<std::size_t>(x) * 3] = static_cast<unsigned char>(c >> 16);
            row[static_cast<std::size_t>(x) * 3 + 1] = static_cast<unsigned char>(c >> 8);
            row[static_cast<std::size_t>(x) * 3 + 2] = static_cast<unsigned char>(c);
        }
        std::fwrite(row.data(), 1, row.size(), f);
    }
    std::fclose(f);
}

static std::string rect_str(const Rect& r) {
    return std::to_string(r.x) + "," + std::to_string(r.y) + "," + std::to_string(r.w) + "," + std::to_string(r.h);
}

static const char* popup_name(PopupKind k) {
    switch (k) {
        case PopupKind::Menu: return "menu";
        case PopupKind::Tasks: return "tasks";
        case PopupKind::Net: return "net";
        case PopupKind::Sound: return "sound";
        case PopupKind::Calendar: return "calendar";
        case PopupKind::Notify: return "notify";
        case PopupKind::Power: return "power";
        case PopupKind::Layout: return "layout";
        case PopupKind::AppWindows: return "appwindows";
        case PopupKind::AppMenu: return "appmenu";
        case PopupKind::Confirm: return "confirm";
    }
    return "?";
}

std::vector<std::string> Desktop::control(const std::string& line) {
    std::vector<std::string> out;
    std::istringstream in(line);
    std::vector<std::string> a;
    std::string tok;
    while (in >> tok) a.push_back(tok);
    if (a.empty()) return out;
    const std::string& cmd = a[0];
    auto need = [&](std::size_t n) { if (a.size() < n) { out.push_back("error: missing argument"); return false; } return true; };

    if (cmd == "launch") {
        if (!need(2)) return out;
        std::string err;
        const int iid = tasks_.launch(a[1], err);
        out.push_back(iid < 0 ? "error: " + err : "ok iid=" + std::to_string(iid));
    } else if (cmd == "apps") {
        for (const auto& ap : tasks_.apps()) out.push_back(ap.id + " uid=" + std::to_string(ap.uid) + " cat=" + ap.category + " home=" + ap.home);
    } else if (cmd == "instances") {
        for (auto& i : tasks_.instances())
            out.push_back("iid=" + std::to_string(i.iid) + " app=" + i.app_id + " pid=" + std::to_string(i.pid) + " uid=" + std::to_string(i.uid) +
                          " windows=" + std::to_string(i.wins.size()) + " connected=" + (i.connected ? "1" : "0"));
    } else if (cmd == "windows") {
        for (const auto& w : wins_)
            out.push_back("win=" + std::to_string(w->id) + " iid=" + std::to_string(w->iid) + " title=" + w->title + " frame=" + rect_str(frame_of(*w)) +
                          " content=" + rect_str(content_of(*w)) + " min=" + (w->minimized ? "1" : "0") + " max=" + (w->maximized ? "1" : "0") +
                          " focus=" + (w->id == focus_ ? "1" : "0"));
    } else if (cmd == "tasks") {
        for (const auto& t : tasks_.background_tasks())
            out.push_back(t.key + " name=" + t.name + " critical=" + (t.critical ? "1" : "0") + " pid=" + std::to_string(t.pid) + " rss_kb=" + std::to_string(t.rss_kb));
        { const int n = static_cast<int>(tasks_.background_tasks().size()); out.push_back("count=" + std::to_string(n) + " sections=" + std::to_string(std::max(1, (n + tok::kTasksPerSection - 1) / tok::kTasksPerSection))); }
    } else if (cmd == "dock") {
        build_dock_items();
        for (const auto& d : dock_items_)
            out.push_back("item " + d.app_id + " windows=" + std::to_string(d.wins.size()) + " active=" + (d.active ? "1" : "0"));
        const Rect l = dock_left_rect();
        const int cap = std::max(1, l.w / S(tok::kDockSlot));
        out.push_back("overflow=" + std::string(static_cast<int>(dock_items_.size()) > cap ? "1" : "0") + " capacity=" + std::to_string(cap) +
                      " items=" + std::to_string(dock_items_.size()) + " dockrect=" + rect_str(dock_rect()) + " d=" + rect_str(dock_d_rect()));
    } else if (cmd == "popup") {
        if (!need(2)) return out;
        const std::string& k = a[1];
        if (k == "close") { close_all_popups(); return out; }
        dirty_ = true;
        render();
        for (const auto& h : hits_) {
            if ((k == "menu" && h.hover_key == "d") || (k == "tasks" && h.hover_key == "tray:tasks") || (k == "calendar" && h.hover_key == "tray:clock") ||
                (k == "net" && h.hover_key == "tray:net") || (k == "sound" && h.hover_key == "tray:sound") || (k == "notify" && h.hover_key == "tray:notify") ||
                (k == "layout" && h.hover_key == "tray:layout")) {
                const PopupKind pk = k == "menu" ? PopupKind::Menu : k == "tasks" ? PopupKind::Tasks : k == "calendar" ? PopupKind::Calendar :
                                     k == "net" ? PopupKind::Net : k == "sound" ? PopupKind::Sound : k == "notify" ? PopupKind::Notify : PopupKind::Layout;
                close_all_popups();
                open_popup(pk, h.r);
                break;
            }
        }
        render();
        if (Popup* p = popups_.empty() ? nullptr : &popups_.back())
            out.push_back(std::string("popup ") + popup_name(p->kind) + " anchor=" + rect_str(p->anchor) + " rect=" + rect_str(p->rect));
        else out.push_back("error: popup not opened");
    } else if (cmd == "popups") {
        for (const auto& p : popups_) out.push_back(std::string("popup ") + popup_name(p.kind) + " anchor=" + rect_str(p.anchor) + " rect=" + rect_str(p.rect));
    } else if (cmd == "locate") {
        if (!need(2)) return out;
        dirty_ = true; render();
        for (const auto& h : hits_) if (h.hover_key == a[1]) { out.push_back("rect=" + rect_str(h.r)); return out; }
        out.push_back("error: no such element");
    } else if (cmd == "click") {
        if (!need(3)) return out;
        on_pointer_move(std::atoi(a[1].c_str()), std::atoi(a[2].c_str()));
        const int b = a.size() > 3 ? std::atoi(a[3].c_str()) : 1;
        on_button(b, true);
        on_button(b, false);
        out.push_back("ok");
    } else if (cmd == "button") {
        const std::size_t sp = line.find(' ');
        const std::string label = sp == std::string::npos ? "" : line.substr(sp + 1);
        dirty_ = true; render();
        bool done = false;
        for (std::size_t i = hits_.size(); i-- > 0 && !done;)
            if (!hits_[i].label.empty() && hits_[i].label == label) { auto fn = hits_[i].fn; if (fn) fn(1); done = true; }
        out.push_back(done ? "ok" : "error: no such button");
    } else if (cmd == "winbtn") {
        if (!need(3)) return out;
        Window* w = win(std::atoi(a[1].c_str()));
        if (!w) { out.push_back("error: no such window"); return out; }
        const int idx = a[2] == "close" ? 0 : a[2] == "max" ? 1 : 2;
        out.push_back("rect=" + rect_str(title_btn(*w, idx)));
    } else if (cmd == "mousedown" || cmd == "mouseup") {
        on_button(a.size() > 1 ? std::atoi(a[1].c_str()) : 1, cmd == "mousedown");
        out.push_back("ok");
    } else if (cmd == "perf") {
        out.push_back("windows_us=" + std::to_string(perf_win_us_) + " dock_us=" + std::to_string(perf_dock_us_) + " popups_us=" + std::to_string(perf_pop_us_));
        out.push_back("render_us=" + std::to_string(perf_render_us_) + " present_us=" + std::to_string(perf_present_us_) +
                      " windows=" + std::to_string(wins_.size()) + " fb=" + (fb_.mem ? "yes" : "no"));
    } else if (cmd == "fbdiff") {
        // Exact check of the video memory against the composed frame, ignoring the pointer image itself.
        if (!fb_.mem || (fb_.bpp != 24 && fb_.bpp != 32)) { out.push_back("fbdiff=n/a"); return out; }
        dirty_ = false; render(); present();
        long bad = 0; int bx0 = W_, by0 = H_, bx1 = -1, by1 = -1;
        for (int y = 0; y < H_; ++y) {
            const unsigned char* row = fb_.mem + static_cast<std::size_t>(y) * fb_.stride;
            for (int x = 0; x < W_; ++x) {
                if (x >= mx_ && x < mx_ + 12 && y >= my_ && y < my_ + 19) continue;
                const unsigned char* p = row + static_cast<std::size_t>(x) * static_cast<std::size_t>(fb_.bpp / 8);
                const u32 want = back_.px[static_cast<std::size_t>(y) * back_.stride + x] & 0xFFFFFFu;
                const u32 got = (static_cast<u32>(p[fb_.ro / 8]) << 16) | (static_cast<u32>(p[fb_.go / 8]) << 8) | p[fb_.bo / 8];
                if (want != got) { ++bad; bx0 = std::min(bx0, x); by0 = std::min(by0, y); bx1 = std::max(bx1, x); by1 = std::max(by1, y); }
            }
        }
        out.push_back("fbdiff=" + std::to_string(bad) + " bbox=" + std::to_string(bx0) + "," + std::to_string(by0) + "," + std::to_string(bx1) + "," + std::to_string(by1) + " cursor=" + std::to_string(mx_) + "," + std::to_string(my_));
    } else if (cmd == "move") {
        if (!need(3)) return out;
        on_pointer_move(std::atoi(a[1].c_str()), std::atoi(a[2].c_str()));
        out.push_back("ok");
    } else if (cmd == "key") {
        if (!need(2)) return out;
        on_key(std::atoi(a[1].c_str()), 1);
        on_key(std::atoi(a[1].c_str()), 0);
        out.push_back("ok");
    } else if (cmd == "type") {
        const std::size_t p = line.find(' ');
        const std::string text = p == std::string::npos ? "" : line.substr(p + 1);
        std::size_t i = 0;
        while (i < text.size()) {
            const std::size_t j = u8_next(text, i);
            send_key_to_focus(0, 1, text.substr(i, j - i));
            i = j;
        }
        out.push_back("ok");
    } else if (cmd == "close") {
        if (!need(2)) return out;
        tasks_.ask_close(std::atoi(a[1].c_str()));
        out.push_back("ok");
    } else if (cmd == "kill-task") {
        if (!need(2)) return out;
        bool done = false;
        for (const auto& t : tasks_.background_tasks())
            if (t.key == a[1]) {
                if (t.critical) { out.push_back("refused: critical system process"); tasks_.log("refused to end critical task " + t.name, true); return out; }
                if (t.iid) tasks_.ask_close(t.iid); else tasks_.terminate_pid(t.pid, false);
                done = true;
            }
        out.push_back(done ? "ok" : "error: no such task");
    } else if (cmd == "focus") {
        if (!need(2)) return out;
        if (Window* w = win(std::atoi(a[1].c_str()))) { if (w->minimized) restore(*w); else set_focus(w->id); }
        out.push_back("ok");
    } else if (cmd == "shot") {
        if (!need(2)) return out;
        dirty_ = true; render();
        write_screenshot(a[1]);
        out.push_back("ok");
    } else if (cmd == "set") {
        if (!need(3)) return out;
        out.push_back(apply_kv(a[1], a[2]) ? "ok" : "error: unknown setting");
    } else if (cmd == "pin") {
        if (!need(2)) return out;
        if (tasks_.app(a[1])) { toggle_pin(a[1]); out.push_back(is_pinned(a[1]) ? "pinned" : "unpinned"); }
        else out.push_back("error: no such app");
    } else if (cmd == "screen") {
        out.push_back(std::to_string(W_) + "x" + std::to_string(H_) + " scale=" + std::to_string(set_.scale));
    } else if (cmd == "log") {
        for (const auto& e : tasks_.journal()) out.push_back(e.text);
    } else if (cmd == "quit") {
        quit_ = true;
        out.push_back("ok");
    } else {
        out.push_back("error: unknown command");
    }
    dirty_ = true;
    return out;
}

// One place that changes a setting: used by the control socket (difiwctl set ...) and by the Settings app (RQ_SET).
bool Desktop::apply_kv(const std::string& k, const std::string& v) {
    const bool on = v == "on" || v == "1";
    if (k == "scale") { const int n = std::atoi(v.c_str()) >= 2 ? 2 : 1; if (n != set_.scale) close_all_popups(); set_.scale = n; }
    else if (k == "reduce") set_.reduce_motion = on;
    else if (k == "transparency") set_.transparency = v != "off" && v != "0";
    else if (k == "lang") set_.ru = v != "en";
    else if (k == "winkey") set_.winkey = on;
    else if (k == "tips") set_.tips = v != "off" && v != "0";
    else if (k == "menu") set_.menu_list = v == "list";
    else if (k == "wallpaper" && !v.empty()) { set_.wallpaper = v; build_wallpaper(); }
    else return false;
    apply_settings();
    return true;
}

bool Desktop::is_pinned(const std::string& id) const { return std::find(pinned_.begin(), pinned_.end(), id) != pinned_.end(); }

void Desktop::toggle_pin(const std::string& id) {
    const AppDef* a = tasks_.app(id);
    if (is_pinned(id)) {
        pinned_.erase(std::remove(pinned_.begin(), pinned_.end(), id), pinned_.end());
        note_toast(t("Unpinned: ", "Откреплено: ") + (a ? a->name(set_.ru) : id));
    } else {
        pinned_.push_back(id);
        if (std::find(dock_order_.begin(), dock_order_.end(), id) == dock_order_.end()) dock_order_.push_back(id);
        note_toast(t("Pinned to the panel: ", "Закреплено в панели: ") + (a ? a->name(set_.ru) : id));
    }
    save_settings();
    dirty_ = true;
}

// Leaving to the text console keeps the list of open apps; they come back when the desktop starts again.
void Desktop::save_session() {
    FILE* f = std::fopen((opt_.rundir + "/session").c_str(), "w");
    if (!f) return;
    for (const auto& in : tasks_.instances())
        if (!in.wins.empty()) std::fprintf(f, "%s\n", in.app_id.c_str());
    std::fclose(f);
}

void Desktop::restore_session() {
    const std::string path = opt_.rundir + "/session";
    std::ifstream f(path);
    std::string id;
    int n = 0;
    while (std::getline(f, id)) {
        if (id.empty() || !tasks_.app(id)) continue;
        std::string err;
        if (tasks_.launch(id, err) >= 0) ++n;
    }
    f.close();
    unlink(path.c_str());
    if (n) notify(t("Your open apps were restored.", "Открытые приложения восстановлены."));
}

// The first time an address appears (or changes) the user is told; no technical messages.
void Desktop::check_network() {
    std::string ip;
    if (DIR* d = opendir("/sys/class/net")) {
        const int fd = socket(AF_INET, SOCK_DGRAM, 0);
        while (struct dirent* e = readdir(d)) {
            if (e->d_name[0] == '.' || std::strcmp(e->d_name, "lo") == 0 || fd < 0) continue;
            struct ifreq ifr;
            std::memset(&ifr, 0, sizeof ifr);
            std::strncpy(ifr.ifr_name, e->d_name, IFNAMSIZ - 1);
            if (ioctl(fd, SIOCGIFADDR, &ifr) == 0) {
                char b[32];
                inet_ntop(AF_INET, &reinterpret_cast<struct sockaddr_in*>(&ifr.ifr_addr)->sin_addr, b, sizeof b);
                ip = b;
            }
        }
        if (fd >= 0) close(fd);
        closedir(d);
    }
    if (ip != last_ip_) {
        if (!ip.empty()) notify(t("Network connected: ", "Подключено к сети: ") + ip);
        else if (!last_ip_.empty()) notify(t("Network connection lost.", "Сетевое подключение потеряно."), true);
        last_ip_ = ip;
    }
}

void Desktop::notify(const std::string& text, bool alert) {
    notes_.push_back({now_ms(), text, alert});
    if (notes_.size() > 60) notes_.erase(notes_.begin());
    ++notify_unseen_;
    dirty_ = true;
}

void Desktop::apply_settings() {
    for (auto& w : wins_) {
        clamp_window(*w);
        if (w->maximized) { const Rect wa = work_area(); w->x = wa.x; w->y = wa.y; configure(*w, wa.w - 2 * S(6), wa.h - title_h() - S(6)); }
    }
    Msg m;
    m.type = S_WELCOME;
    m.i[0] = set_.scale; m.i[1] = set_.reduce_motion ? 1 : 0; m.i[2] = set_.transparency ? 1 : 0;
    for (const auto& c : clients_) if (c.hello) send_to_client(c.fd, m);
    save_settings();
    dirty_ = true;
}

void Desktop::save_settings() {
    mkdir(opt_.etcdir.c_str(), 0755);
    FILE* f = std::fopen((opt_.etcdir + "/desktop.conf").c_str(), "w");
    if (f) {
        std::string pins;
        for (const auto& p : pinned_) pins += (pins.empty() ? "" : ",") + p;
        std::fprintf(f, "scale=%d\nreduce_motion=%d\ntransparency=%d\nlang=%s\nwinkey=%d\ntips=%d\nmenu=%s\nwallpaper=%s\npinned=%s\n", set_.scale,
                     set_.reduce_motion ? 1 : 0, set_.transparency ? 1 : 0, set_.ru ? "ru" : "en", set_.winkey ? 1 : 0, set_.tips ? 1 : 0,
                     set_.menu_list ? "list" : "grid", set_.wallpaper.c_str(), pins.c_str());
        std::fclose(f);
    }
    if (FILE* lf = std::fopen("/run/difiw-lang", "w")) { std::fputs(set_.ru ? "ru\n" : "en\n", lf); std::fclose(lf); }  // apps follow the language
}

// -------------------------------------------------------------------- main loop
int Desktop::run() {
    struct sigaction sa;
    std::memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_term;
    sigaction(SIGTERM, &sa, nullptr);
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGHUP, &sa, nullptr);
    signal(SIGPIPE, SIG_IGN);

    long last = now_ms();
    render();
    present();
    while (!quit_ && !g_term) {
        std::vector<struct pollfd> pf;
        pf.push_back({listen_fd_, POLLIN, 0});
        pf.push_back({ctl_fd_, POLLIN, 0});
        for (const auto& c : clients_) pf.push_back({c.fd, POLLIN, 0});
        const std::size_t ndev0 = pf.size();
        for (const auto& d : devs_) pf.push_back({d.fd, POLLIN, 0});
        const int timeout = dirty_ ? 4 : (animating() ? 16 : 250);  // idle: wake up rarely
        poll(pf.data(), static_cast<nfds_t>(pf.size()), timeout);

        if (pf[0].revents & POLLIN) accept_clients();
        if (pf[1].revents & POLLIN) accept_ctl();
        // client messages (iterate over a snapshot of fds: handlers may drop clients)
        std::vector<int> ready;
        for (std::size_t i = 2; i < ndev0; ++i) if (pf[i].revents & (POLLIN | POLLHUP | POLLERR)) ready.push_back(pf[i].fd);
        for (int fd : ready) {
            for (std::size_t k = 0; k < clients_.size(); ++k) {
                if (clients_[k].fd != fd) continue;
                for (int n = 0; n < 32; ++n) {
                    Msg m;
                    int passed = -1;
                    const int r = recv_msg(fd, m, &passed, true);
                    if (r == 0) { drop_client(fd); break; }
                    if (r < 0) break;
                    handle_client_msg(clients_[k], m, passed);
                    if (clients_[k].fd == -2) { drop_client(-2); break; }
                }
                break;
            }
        }
        for (std::size_t i = 0; i < devs_.size() && ndev0 + i < pf.size(); ++i) {
            if (!(pf[ndev0 + i].revents & POLLIN)) continue;
            struct input_event evs[64];
            const ssize_t n = read(devs_[i].fd, evs, sizeof evs);
            for (ssize_t k = 0; k < n / static_cast<ssize_t>(sizeof(struct input_event)); ++k) handle_dev_event(devs_[i], evs[k]);
        }

        const long now = now_ms();
        if (!opt_.headless) {
            bool kb = false, ptr = false;
            for (const auto& d : devs_) { kb |= d.kbd; ptr |= d.abs || d.rel; }
            if ((!kb || !ptr) && now - last_scan_ms_ > 1000) { last_scan_ms_ = now; scan_devices(); }
        }
        // finished apps
        for (const Instance& done : tasks_.reap()) {
            std::vector<int> ids;
            for (const auto& w : wins_) if (w->iid == done.iid) ids.push_back(w->id);
            for (int id : ids) destroy_window(id);
            for (auto& c : clients_) if (c.iid == done.iid) { close(c.fd); c.fd = -2; }
            clients_.erase(std::remove_if(clients_.begin(), clients_.end(), [](const ClientConn& c) { return c.fd == -2; }), clients_.end());
            const AppDef* a = tasks_.app(done.app_id);
            if (!done.closing && (done.exit_signal || done.exit_code)) {
                note_toast((a ? a->name(set_.ru) : done.app_id) + t(" stopped unexpectedly", " неожиданно завершилось"));
                notify((a ? a->name(set_.ru) : done.app_id) + t(" stopped unexpectedly.", " неожиданно завершилось."), true);
            }
            dirty_ = true;
        }
        tasks_.tick_kills(now);
        if (now - last_sample_ms_ >= tok::kTaskRefreshMs) { last_sample_ms_ = now; tasks_.sample(now); dirty_ = true; }
        if (toast_until_ && now > toast_until_) { toast_.clear(); toast_until_ = 0; dirty_ = true; }
        if (now - last_ip_check_ms_ >= 3000) { last_ip_check_ms_ = now; check_network(); }
        // drop closed popups
        for (std::size_t i = 0; i < popups_.size();)
            if (popups_[i].closing && now - popups_[i].t_close > (set_.reduce_motion ? 0 : tok::kAnimPopup)) { popups_.erase(popups_.begin() + static_cast<std::ptrdiff_t>(i)); dirty_ = true; }
            else ++i;
        update_animations(now - last);
        last = now;
        frame_ms_ = now;
        static int last_min = -1;
        time_t tt = time(nullptr);
        struct tm tm;
        localtime_r(&tt, &tm);
        if (tm.tm_min != last_min) { last_min = tm.tm_min; dirty_ = true; }
        // at most ~120 frames/s for input, ~30 frames/s for pure animation; the picture only costs time when it changes
        if ((dirty_ && now - last_render_ms_ >= 8) || (animating() && now - last_render_ms_ >= 33)) {
            dirty_ = false;
            const long t0 = now_us();
            render();
            const long t1 = now_us();
            present();
            const long t2 = now_us();
            perf_render_us_ = (perf_render_us_ * 7 + (t1 - t0)) / 8;
            perf_present_us_ = (perf_present_us_ * 7 + (t2 - t1)) / 8;
            last_render_ms_ = now_ms();
        }
    }
    // shutting down: ask every app to leave, then force
    for (auto& i : tasks_.instances()) kill(i.pid, SIGTERM);
    const long until = now_ms() + 1200;
    while (now_ms() < until && !tasks_.instances().empty()) { tasks_.reap(); usleep(30000); }
    for (auto& i : tasks_.instances()) kill(i.pid, SIGKILL);
    tasks_.reap();
    tasks_.log("desktop stopped");
    return exit_code_;
}

}  // namespace dg
