// SPDX-License-Identifier: MIT
// Desktop panels for the network and the sound devices (the tray pop-ups) and the helpers behind them.
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>

#include "desktop.hpp"

namespace dg {

using namespace tok;

// ------------------------------------------------------------------ helpers
void Desktop::refresh_audio() {
    out_vol_ = sys::vol_get(false);
    in_vol_ = sys::vol_get(true);
}

void Desktop::refresh_net(bool check) {
    nets_ = sys::net_ifs();
    net_refresh_ms_ = now_ms();
    if (!check) return;
    net_check_ = 3;
    unlink((opt_.rundir + "/net.state").c_str());
    // an ordinary request tells whether the internet is really reachable (an address alone proves nothing)
    const std::string cmd = "(c=$(curl -4 -s -m 6 -o /dev/null -w %{http_code} http://example.com/); echo $c > " + opt_.rundir + "/net.state) >/dev/null 2>&1 &";
    if (std::system(cmd.c_str()) != 0) {}
}

void Desktop::net_action(const std::string& action, const std::string& ifn) {
    if (action == "check") { refresh_net(true); return; }
    for (char c : ifn) if (!std::isalnum(static_cast<unsigned char>(c))) return;  // only plain adapter names reach the shell
    if (ifn.empty()) return;
    const std::string down = "kill $(cat /run/udhcpc-" + ifn + ".pid 2>/dev/null) 2>/dev/null; ip addr flush dev " + ifn + "; ip link set " + ifn + " down";
    const std::string up = "ip link set " + ifn + " up; udhcpc -i " + ifn + " -b -t 8 -T 2 -p /run/udhcpc-" + ifn + ".pid -s /etc/difiw/udhcpc.script";
    std::string cmd;
    if (action == "down") cmd = down;
    else if (action == "up") cmd = up;
    else if (action == "renew") cmd = down + "; " + up;
    else if (action == "check") { refresh_net(true); return; }
    else return;
    if (std::system(("sh -c '" + cmd + "' >/dev/null 2>&1 &").c_str()) != 0) {}
    net_check_ = action == "down" ? 2 : 3;
    net_refresh_ms_ = 0;
    if (action != "down") {  // check again once the new address had time to arrive
        if (std::system(("sh -c 'sleep 6; echo > " + opt_.rundir + "/net.state; c=$(curl -4 -s -m 6 -o /dev/null -w %{http_code} http://example.com/); echo $c > " + opt_.rundir + "/net.state' >/dev/null 2>&1 &").c_str()) != 0) {}
    }
    dirty_ = true;
}

void Desktop::open_settings(const std::string& page) {
    const std::string p = opt_.rundir + "/settings-page";
    if (FILE* f = std::fopen(p.c_str(), "w")) { std::fputs(page.c_str(), f); std::fclose(f); chmod(p.c_str(), 0666); }
    close_all_popups();
    launch_app("settings");
}

void Desktop::slider_set(const std::string& key, int pct, bool final_value) {
    pct = clampi(pct, 0, 100);
    sys::Vol& v = key == "out" ? out_vol_ : in_vol_;
    v.pct = pct;
    const long now = now_ms();
    if (final_value || now - vol_sent_ms_ > 90) {
        sys::vol_set(v.ctl, pct);
        vol_sent_ms_ = now;
    }
    dirty_ = true;
}

void Desktop::draw_slider(Rect r, const std::string& key, int pct, bool on) {
    const int tr = S(8);
    const Rect track = {r.x, r.y + (r.h - tr) / 2, r.w, tr};
    rrect(back_, track, tr / 2, ca_(0xFF2A3142u));
    const int fw = std::max(tr, track.w * pct / 100);
    rrect(back_, {track.x, track.y, fw, tr}, tr / 2, ca_(on ? kAccentBlue : kTextFaint));
    const int kx = track.x + track.w * pct / 100;
    const std::string hk = "slider:" + key;
    const float hv = hover_amount(hk) + (slider_key_ == key && drag_ == Drag::Slider ? 1.0f : 0.0f);
    circle(back_, kx, r.y + r.h / 2, S(9) + static_cast<int>(hv * static_cast<float>(S(1))), ca_(0xFFF4F7FFu));
    add_hit(r.inset(-S(4)), [this, key, r](int) {
        slider_key_ = key;
        slider_rect_ = r;
        drag_ = Drag::Slider;
        slider_set(key, (mx_ - r.x) * 100 / std::max(1, r.w), false);
    }, hk);
}

// ------------------------------------------------------------------ network panel
void Desktop::draw_net(Rect r) {
    const int pad = S(16), sc = set_.scale;
    if (now_ms() - net_refresh_ms_ > 2000) refresh_net(false);
    if (net_check_ == 3) {
        std::ifstream f(opt_.rundir + "/net.state");
        std::string s;
        std::getline(f, s);
        if (!s.empty()) net_check_ = (s == "200" || s == "301" || s == "302" || s == "204") ? 1 : 2;
    }
    draw_text(back_, r.x + pad, r.y + pad, t("Network", "Сеть"), ca_(kText), sc);
    int y = r.y + pad + font_h() * sc + S(14);
    if (nets_.empty()) {
        draw_text(back_, r.x + pad, y, fit_text(t("No network adapter found.", "Сетевой адаптер не найден."), r.w - 2 * pad, 1), ca_(kTextDim), 1);
        draw_text(back_, r.x + pad, y + S(24), fit_text(t("VirtualBox: Settings > Network.", "VirtualBox: Настройки > Сеть."), r.w - 2 * pad, 1), ca_(kTextFaint), 1);
        return;
    }
    int shown = 0;
    for (const auto& n : nets_) {
        if (++shown > 1) break;
        const Rect card = {r.x + pad, y, r.w - 2 * pad, S(182)};
        rrect(back_, card, S(14), ca_(0x40FFFFFFu));
        rrect_outline(back_, card, S(14), 1, ca_(0x30FFFFFFu));
        const bool connected = n.up && !n.ip.empty();
        // adapter glyph: a plug-like screen
        const int gx = card.x + S(14), gy = card.y + S(14), gs = S(28);
        rrect(back_, {gx, gy, gs, gs * 3 / 4}, S(5), ca_(connected ? kAccentBlue : kTextFaint));
        fill(back_, {gx + gs / 2 - S(2), gy + gs * 3 / 4, S(4), S(5)}, ca_(connected ? kAccentBlue : kTextFaint));
        fill(back_, {gx + gs / 4, gy + gs * 3 / 4 + S(5), gs / 2, S(3)}, ca_(connected ? kAccentBlue : kTextFaint));
        draw_text(back_, card.x + S(54), card.y + S(12), fit_text(t("Wired connection", "Проводное подключение") + std::string("  (") + n.name + ")", card.w - S(66), 1), ca_(kText), 1);
        std::string st;
        u32 col = kTextDim;
        if (!n.up) { st = t("Disconnected", "Отключено"); }
        else if (n.ip.empty()) { st = t("Connecting... (waiting for an address)", "Подключение... (ожидание адреса)"); col = kWarn; }
        else if (net_check_ == 1) { st = t("Connected, internet available", "Подключено, доступ в интернет есть"); col = kOk; }
        else if (net_check_ == 2) { st = t("Connected, no internet access", "Подключено, без доступа в интернет"); col = kWarn; }
        else { st = t("Connected, checking internet...", "Подключено, проверка интернета..."); col = kAccentTeal; }
        draw_text(back_, card.x + S(54), card.y + S(32), fit_text(st, card.w - S(66), 1), ca_(col), 1);
        int ty = card.y + S(58);
        auto kv = [&](const std::string& k, const std::string& v) {
            draw_text(back_, card.x + S(14), ty, k, ca_(kTextFaint), 1);
            draw_text(back_, card.x + S(14) + S(80), ty, fit_text(v.empty() ? "-" : v, card.w - S(100), 1), ca_(kTextDim), 1);
            ty += S(16);
        };
        kv("IP", n.ip);
        kv(t("Gateway", "Шлюз"), n.gw);
        kv("DNS", sys::dns_servers());
        kv("MAC", n.mac);
        const int bw = (card.w - S(14) * 2 - S(8)) / 2, bh = S(28), by = card.bottom() - bh - S(8);
        const std::string ifn = n.name;
        const bool up = n.up;
        draw_button({card.x + S(14), by, bw, bh}, up ? t("Disconnect", "Отключить") : t("Connect", "Подключить"), [this, ifn, up](int) { net_action(up ? "down" : "up", ifn); }, !up, false);
        draw_button({card.x + S(14) + bw + S(8), by, bw, bh}, t("Reconnect", "Переподключить"), [this, ifn](int) { net_action("renew", ifn); }, false, false, up);
        y += card.h + S(10);
    }
    // what the adapter really is: the host's connection, Wi-Fi included, reaches the virtual machine as a wire
    const std::string note = t("Virtual network adapter: the internet of the host computer (including its Wi-Fi) reaches this system as a wired connection.",
                               "Виртуальный сетевой адаптер: интернет компьютера-хоста (в том числе его Wi-Fi) приходит в систему как проводное подключение.");
    std::string rest = note;
    const int cols = std::max(10, (r.w - 2 * pad) / font_w());
    int ny = y;
    for (int line = 0; line < 4 && !rest.empty(); ++line) {
        std::size_t cut = 0;
        for (int i = 0; i < cols && cut < rest.size(); ++i) cut = u8_next(rest, cut);
        if (cut < rest.size()) { const std::size_t sp = rest.rfind(' ', cut); if (sp != std::string::npos && sp > 0) cut = sp; }
        draw_text(back_, r.x + pad, ny, rest.substr(0, cut), ca_(kTextFaint), 1);
        rest.erase(0, cut);
        while (!rest.empty() && rest[0] == ' ') rest.erase(0, 1);
        ny += S(16);
    }
    draw_button({r.x + pad, r.bottom() - pad - S(32), r.w - 2 * pad, S(32)}, t("Network settings", "Параметры сети"), [this](int) { open_settings("network"); });
}

// ------------------------------------------------------------------ sound panel
void Desktop::draw_sound(Rect r) {
    const int pad = S(16), sc = set_.scale;
    draw_text(back_, r.x + pad, r.y + pad, t("Sound", "Звук"), ca_(kText), sc);
    int y = r.y + pad + font_h() * sc + S(14);
    const std::string card = sys::card_name();
    if (card.empty()) {
        draw_text(back_, r.x + pad, y, fit_text(t("No sound device found.", "Звуковое устройство не найдено."), r.w - 2 * pad, 1), ca_(kTextDim), 1);
        draw_text(back_, r.x + pad, y + S(24), fit_text(t("VirtualBox: Settings > Audio: enable output and input.", "VirtualBox: Настройки > Звук: включите вывод и ввод."), r.w - 2 * pad, 1), ca_(kTextFaint), 1);
        return;
    }
    for (int k = 0; k < 2; ++k) {
        const bool in = k == 1;
        sys::Vol& v = in ? in_vol_ : out_vol_;
        const std::string pcm = sys::pcm_name(in);
        const Rect box = {r.x + pad, y, r.w - 2 * pad, S(104)};
        rrect(back_, box, S(14), ca_(0x40FFFFFFu));
        rrect_outline(back_, box, S(14), 1, ca_(0x30FFFFFFu));
        draw_text(back_, box.x + S(14), box.y + S(10), in ? t("Input (microphone)", "Ввод (микрофон)") : t("Output (speakers)", "Вывод (динамики)"), ca_(kText), 1);
        draw_text(back_, box.x + S(14), box.y + S(30), fit_text((in ? t("Microphone", "Микрофон") : t("Speakers", "Динамики")) + " - " + (pcm.empty() ? card : pcm), box.w - S(28), 1), ca_(kTextDim), 1);
        if (!v.ok) {
            draw_text(back_, box.x + S(14), box.y + S(60), fit_text(t("No volume control for this device.", "Для этого устройства нет регулятора громкости."), box.w - S(28), 1), ca_(kTextFaint), 1);
        } else {
            const std::string pct = std::to_string(v.on ? v.pct : 0) + "%";
            draw_slider({box.x + S(14), box.y + S(50), box.w - S(28) - S(50), S(26)}, in ? "in" : "out", v.pct, v.on);
            draw_text(back_, box.right() - S(14) - text_width(pct, 1), box.y + S(50) + (S(26) - font_h()) / 2, pct, ca_(v.on ? kText : kTextFaint), 1);
            const std::string ctl = v.ok ? v.ctl : "";
            draw_button({box.x + S(14), box.bottom() - S(30), S(150), S(24)}, v.on ? (in ? t("Mute microphone", "Выключить микрофон") : t("Mute sound", "Выключить звук")) : (in ? t("Unmute microphone", "Включить микрофон") : t("Unmute sound", "Включить звук")),
                        [this, ctl, in](int) { sys::vol_mute(ctl); (in ? in_vol_ : out_vol_).on = !(in ? in_vol_ : out_vol_).on; dirty_ = true; });
        }
        y += box.h + S(10);
    }
    const int bw = (r.w - 2 * pad - S(8)) / 2, bh = S(32), by = r.bottom() - pad - bh;
    draw_button({r.x + pad, by, bw, bh}, t("Test sound", "Проверить звук"), [](int) { sys::play_test(); });
    draw_button({r.x + pad + bw + S(8), by, bw, bh}, t("Sound settings", "Параметры звука"), [this](int) { open_settings("sound"); });
}

}  // namespace dg
