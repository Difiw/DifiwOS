#!/usr/bin/env python3
"""QEMU test of the Difiw desktop: the scenarios of section 14 of the task.

Usage: desktop_qemu.py <iso> <log-file> <screenshot-dir>
The desktop is started in the background from the serial shell, so `difiwctl` can be used while it runs.
SPDX-License-Identifier: MIT
"""
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import smoke_qemu as sq  # noqa: E402
import gui_qemu as gq    # noqa: E402  (Qmp, read_ppm, pixel, near)

results = sq.results


def parse_kv(line):
    return dict(re.findall(r"(\w+)=(\S+)", line))


def rect(s):
    return tuple(int(v) for v in s.split(","))


class Session:
    def __init__(self, iso, log, shots, extra_iso_cfg=None):
        self.shots = shots
        self.tmp = tempfile.mkdtemp(prefix="difiw-desk-")
        self.sock = os.path.join(self.tmp, "qmp.sock")
        self.vm = sq.Vm(iso, ["-vga", "std", "-qmp", f"unix:{self.sock},server,nowait", "-device", "qemu-xhci",
                              "-device", "usb-tablet", "-device", "usb-kbd"], log)
        self.vm.wait_for(sq.PROMPT, 150)
        self.qmp = gq.Qmp(self.sock)

    def sh(self, cmd, timeout=30):
        return self.vm.run(cmd, timeout)

    def ctl(self, *args, timeout=30):
        out = self.sh("difiwctl " + " ".join(args), timeout)
        return [l.strip() for l in out.replace("\r", "").split("\n") if l.strip() and not l.strip().startswith("difiw:/")]

    def start_desktop(self):
        self.sh("sh -c 'difiw-desktop > /tmp/desktop.out 2>&1 &'")
        for _ in range(40):
            time.sleep(0.5)
            r = self.ctl("screen")
            if r and re.match(r"\d+x\d+", r[0]):
                return r[0]
        return None

    def shot(self, name):
        p = os.path.join(self.shots, name + ".ppm")
        self.qmp.shot(p)
        return gq.read_ppm(p)

    def click_key(self, key):
        r = self.ctl("locate", key)
        m = re.search(r"rect=(\S+)", " ".join(r))
        if not m:
            return False
        x, y, w, h = rect(m.group(1))
        self.qmp.click(x + w // 2, y + h // 2)
        return True

    def close(self):
        try:
            self.vm.kill()
        finally:
            shutil.rmtree(self.tmp, ignore_errors=True)


def check(name, cond, detail=""):
    sq.check(name, cond, detail)


def run(iso, logpath, shots):
    os.makedirs(shots, exist_ok=True)
    with open(logpath, "w", encoding="utf-8", errors="replace") as log:
        s = None
        try:
            s = Session(iso, log, shots)
            scr = s.start_desktop()
            check("desktop starts (difiw-desktop on /dev/fb0)", scr == "1024x768 scale=1", str(scr))
            s.ctl("set", "lang", "en")  # test labels are English
            img = s.shot("01-desktop")
            # wallpaper: the upper area is the aurora sky, clearly not black and blue-dominant
            px = gq.pixel(img, 500, 120)
            check("default wallpaper is shown", sum(px) > 80 and px[2] >= px[0], str(px))
            d = s.ctl("dock")
            check("panel has no apps before anything is launched", any(l.startswith("overflow=0") and "items=0" in l for l in d), str(d))
            t = s.ctl("tasks")
            check("system services are listed as background tasks, not in the panel", any("name=syslogd" in l for l in t) and not any(l.startswith("item") for l in d), str(t))

            # launch real apps (separate processes, separate users)
            out = s.ctl("launch", "notes")
            out2 = s.ctl("launch", "calc")
            check("apps start", "ok" in " ".join(out) and "ok" in " ".join(out2), f"{out} {out2}")
            time.sleep(3)
            d = s.ctl("dock")
            items = [l for l in d if l.startswith("item")]
            check("launched apps appear in the panel", len(items) == 2 and "item notes" in items[0], str(d))
            w = s.ctl("windows")
            check("each app has its own window", len(w) == 2, str(w))
            inst = s.ctl("instances")
            check("apps run as different unprivileged users", len({parse_kv(l)["uid"] for l in inst}) == 2 and all(int(parse_kv(l)["uid"]) >= 20000 for l in inst), str(inst))

            # one window has the focus; click the icon of the OTHER app -> its window gets the focus
            def focused_app(wl):
                for l in wl:
                    if "focus=1" in l:
                        return "calc" if "title=Calculator" in l else "notes"
                return None
            other = "calc" if focused_app(w) == "notes" else "notes"
            other_title = "Calculator" if other == "calc" else "Notes"
            s.click_key("dock:" + other); time.sleep(0.6)
            w = s.ctl("windows")
            check("clicking a panel icon switches focus to its window", any(f"title={other_title}" in l and "focus=1" in l for l in w), str(w))
            # click the active app icon -> minimise, click again -> restore
            s.click_key("dock:" + other); time.sleep(0.6)
            w = s.ctl("windows")
            check("clicking the active icon minimises the window", any(f"title={other_title}" in l and "min=1" in l for l in w), str(w))
            s.click_key("dock:" + other); time.sleep(0.6)
            w = s.ctl("windows")
            check("clicking a minimised icon restores the window", any(f"title={other_title}" in l and "min=0" in l and "focus=1" in l for l in w), str(w))
            if focused_app(w) != "notes":  # make Notes the active window for the next steps
                s.click_key("dock:notes"); time.sleep(0.6)
            w = s.ctl("windows")
            s.shot("02-windows")

            # window operations with the mouse: move by the title bar
            f = rect(re.search(r"title=Notes frame=(\S+)", " ".join(w)).group(1))

            def drag(x0, y0, x1, y1):
                def mv(x, y):
                    s.qmp.cmd("input-send-event", {"events": [{"type": "abs", "data": {"axis": "x", "value": int(x * 32767 / 1023)}},
                                                              {"type": "abs", "data": {"axis": "y", "value": int(y * 32767 / 767)}}]})
                    time.sleep(0.2)
                mv(x0, y0)
                s.qmp.cmd("input-send-event", {"events": [{"type": "btn", "data": {"down": True, "button": "left"}}]})
                time.sleep(0.2)
                for k in range(1, 5):
                    mv(x0 + (x1 - x0) * k // 4, y0 + (y1 - y0) * k // 4)
                s.qmp.cmd("input-send-event", {"events": [{"type": "btn", "data": {"down": False, "button": "left"}}]})
                time.sleep(0.6)

            drag(f[0] + 260, f[1] + 12, f[0] + 360, f[1] + 62)
            w2 = s.ctl("windows")
            f2 = rect(re.search(r"title=Notes frame=(\S+)", " ".join(w2)).group(1))
            check("a window can be moved by its title bar", f2[0] - f[0] >= 80 and f2[1] - f[1] >= 40, f"{f} -> {f2}")
            # resize by the bottom-right corner (the app re-creates its buffer)
            drag(f2[0] + f2[2] - 3, f2[1] + f2[3] - 3, f2[0] + f2[2] + 77, f2[1] + f2[3] + 47)
            w3 = s.ctl("windows")
            f3 = rect(re.search(r"title=Notes frame=(\S+)", " ".join(w3)).group(1))
            check("a window can be resized by its corner", f3[2] - f2[2] >= 50 and f3[3] - f2[3] >= 30, f"{f2} -> {f3}")
            # maximise / restore with the title button
            wid = re.search(r"win=(\d+) iid=\d+ title=Notes", " ".join(w3)).group(1)
            r = re.search(r"rect=(\S+)", " ".join(s.ctl("winbtn", wid, "max"))).group(1)
            bx, by, bw, bh = rect(r)
            s.qmp.click(bx + bw // 2, by + bh // 2); time.sleep(0.8)
            w4 = s.ctl("windows")
            check("a window can be maximised", any("title=Notes" in l and "max=1" in l for l in w4), str(w4))
            s.shot("03-maximised")
            r = re.search(r"rect=(\S+)", " ".join(s.ctl("winbtn", wid, "max"))).group(1)
            bx, by, bw, bh = rect(r)
            s.qmp.click(bx + bw // 2, by + bh // 2); time.sleep(0.8)
            w4 = s.ctl("windows")
            check("a maximised window can be restored", any("title=Notes" in l and "max=0" in l for l in w4), str(w4))
            # minimise with the title button, restore from the panel
            r = re.search(r"rect=(\S+)", " ".join(s.ctl("winbtn", wid, "min"))).group(1)
            bx, by, bw, bh = rect(r)
            s.qmp.click(bx + bw // 2, by + bh // 2); time.sleep(0.6)
            w4 = s.ctl("windows")
            check("a window can be minimised with its button", any("title=Notes" in l and "min=1" in l for l in w4), str(w4))
            s.click_key("dock:notes"); time.sleep(0.6)
            # Alt+Tab switches between windows
            before = [l for l in s.ctl("windows") if "focus=1" in l]
            s.qmp.key("alt-tab"); time.sleep(0.6)
            after = [l for l in s.ctl("windows") if "focus=1" in l]
            check("Alt+Tab switches the focused window", bool(before and after) and before[0].split(" title=")[1][:6] != after[0].split(" title=")[1][:6], f"{before} -> {after}")

            # the D button opens the menu directly above itself
            s.click_key("d"); time.sleep(0.8)
            p = s.ctl("popups")
            m = re.search(r"popup menu anchor=(\S+) rect=(\S+)", " ".join(p))
            ok = False
            if m:
                a, r = rect(m.group(1)), rect(m.group(2))
                ok = r[1] + r[3] <= a[1] and abs((r[0] + r[2] // 2) - (a[0] + a[2] // 2)) < 120 and r[0] >= 0 and r[0] + r[2] <= 1024
            check("D button opens the menu right above itself", ok, str(p))
            s.shot("04-menu")
            s.click_key("d"); time.sleep(0.6)
            # keyboard: Super opens it, typing filters, Enter launches
            s.qmp.key("meta_l"); time.sleep(0.8)
            s.qmp.text("mon"); time.sleep(0.4)
            s.shot("05-menu-search")
            s.qmp.key("ret"); time.sleep(3)
            inst = s.ctl("instances")
            check("menu search + keyboard launches an app", any("app=monitor" in l for l in inst), str(inst))

            # background tasks popup: above its own button, inside the screen
            t = s.ctl("popup", "tasks")
            m = re.search(r"popup tasks anchor=(\S+) rect=(\S+)", " ".join(t))
            ok = False
            if m:
                a, r = rect(m.group(1)), rect(m.group(2))
                ok = r[1] + r[3] <= a[1] and r[0] >= 0 and r[0] + r[2] <= 1024
            check("background tasks panel opens above its own button", ok, str(t))
            s.shot("06-tasks")
            s.ctl("popup", "close")

            # every small popup stays inside the screen
            inside = True
            for kind in ("net", "sound", "calendar", "notify", "layout", "settings"):
                t = s.ctl("popup", kind)
                m = re.search(r"rect=(\S+)", " ".join(t))
                if not m:
                    inside = False
                    continue
                r = rect(m.group(1))
                if not (r[0] >= 0 and r[1] >= 0 and r[0] + r[2] <= 1024 and r[1] + r[3] <= 768):
                    inside = False
                s.ctl("popup", "close")
            check("all pop-up panels stay inside the screen", inside)

            # overflow: bigger UI scale leaves room for fewer icons -> arrows + scrolling
            s.ctl("launch", "terminal"); s.ctl("launch", "probe"); time.sleep(3)
            s.ctl("set", "scale", "2"); time.sleep(0.6)
            d = s.ctl("dock")
            check("too many icons switch the panel to arrows and scrolling", any(l.startswith("overflow=1") for l in d), str(d))
            s.shot("07-overflow-scale2")
            check("interface works after changing the scale", any(l.startswith("item") for l in d))
            s.ctl("set", "scale", "1"); time.sleep(0.4)

            # >25 background tasks wrap to the next section (real processes)
            for _ in range(30):
                s.ctl("launch", "worker")
            time.sleep(3)
            t = s.ctl("tasks")
            last = [l for l in t if l.startswith("count=")]
            kv = parse_kv(last[0]) if last else {}
            check("more than 25 background tasks go to a second section", int(kv.get("count", 0)) > 25 and int(kv.get("sections", 0)) >= 2, str(last))
            workers = [l for l in s.ctl("dock") if l.startswith("item")]
            check("background workers never appear among the user's apps", not any("worker" in l for l in workers))
            s.ctl("popup", "tasks"); time.sleep(0.8)
            s.shot("08-tasks-30")
            s.ctl("popup", "close")

            # isolation: the probe app tried to read another home and change /system
            time.sleep(1)
            res = s.sh("read /homes/user/apps/probe/probe-result.txt")
            check("isolation probe: all checks passed", "PASS" in res and "FAIL" not in res, res)
            check("an app cannot read another app's home", "чтение дома" in res and "Permission denied" in res, res)
            check("an app cannot change /usr or /etc", res.count("запись в /") >= 2 and "FAIL" not in res, res)

            check("other users' processes are hidden from an app (hidepid)", "чужие процессы не видны" in res and "FAIL" not in res, res)
            # an app without the permission is refused when it asks to quit the desktop
            s.ctl("launch", "reqtest"); time.sleep(3)
            lg = " ".join(s.ctl("log"))
            alive = s.ctl("screen")
            check("a request without the manifest permission is refused and logged", "permission denied app=reqtest" in lg and alive and alive[0].startswith("1024x768"), f"{lg[-200:]} {alive}")

            # critical services cannot be ended
            r = s.ctl("kill-task", "svc:init")
            check("a critical system service cannot be ended", any("refused" in l for l in r), str(r))
            r = s.ctl("kill-task", "svc:difiw-desktop")
            check("the desktop itself is protected too", any("refused" in l for l in r), str(r))

            # closing an app removes its icon
            iid = parse_kv([l for l in s.ctl("instances") if "app=calc" in l][0])["iid"]
            s.ctl("close", iid); time.sleep(3.5)
            d = s.ctl("dock")
            check("closing an app removes its icon from the panel", not any("item calc" in l for l in d), str(d))

            # right click -> app menu
            s.qmp.cmd("input-send-event", {"events": [{"type": "abs", "data": {"axis": "x", "value": 1}}]})
            r = s.ctl("locate", "dock:notes")
            m = re.search(r"rect=(\S+)", " ".join(r))
            if m:
                x, y, w_, h_ = rect(m.group(1))
                s.ctl("click", str(x + w_ // 2), str(y + h_ // 2), "2")
                time.sleep(0.6)
                p = s.ctl("popups")
                check("right click on an app icon opens its context menu", any("popup appmenu" in l for l in p), str(p))
                s.shot("09-appmenu")
                s.ctl("popup", "close")

            # animations: a hovered icon glows; with reduced motion the picture stays still
            r = s.ctl("locate", "dock:notes")
            m = re.search(r"rect=(\S+)", " ".join(r))
            x, y, w_, h_ = rect(m.group(1))
            s.qmp.click(x + w_ // 2, y + h_ // 2 - 40)  # empty desktop area (also deselects popups)
            s.ctl("move", str(x + w_ // 2), str(y + h_ // 2)); time.sleep(0.5)
            a = s.shot("10-hover-a"); time.sleep(0.4); b = s.shot("10-hover-b")
            region = (x - 6, y - 14, w_ + 12, h_ + 20)
            changed = 0
            for yy in range(region[1], region[1] + region[3], 2):
                for xx in range(region[0], region[0] + region[2], 2):
                    if gq.pixel(a, xx, yy) != gq.pixel(b, xx, yy):
                        changed += 1
            check("a hovered icon has a moving light", changed > 5, f"changed={changed}")
            s.ctl("set", "reduce", "on"); time.sleep(0.6)
            a = s.shot("11-still-a"); time.sleep(0.5); b = s.shot("11-still-b")
            changed2 = sum(1 for yy in range(region[1], region[1] + region[3], 2) for xx in range(region[0], region[0] + region[2], 2)
                           if gq.pixel(a, xx, yy) != gq.pixel(b, xx, yy))
            check("reduced motion stops the animations", changed2 == 0, f"changed={changed2}")
            s.ctl("set", "reduce", "off")
            # transparency off: the panel becomes opaque graphite
            s.ctl("move", "5", "5"); time.sleep(0.8)
            s.ctl("set", "transparency", "on"); time.sleep(1.2)
            dock = rect(re.search(r"dockrect=(\S+)", " ".join(s.ctl("dock"))).group(1))
            on = s.shot("12-glass-on")
            s.ctl("set", "transparency", "off"); time.sleep(1.2)
            off = s.shot("12-glass-off")
            p1 = gq.pixel(on, dock[0] + 20, dock[1] + dock[3] // 2)
            p2 = gq.pixel(off, dock[0] + 20, dock[1] + dock[3] // 2)
            check("transparency can be switched off (opaque panel)", gq.near(p2, (20, 25, 37), 4) and p1 != p2, f"on={p1} off={p2}")
            s.ctl("set", "transparency", "on")

            # power buttons: confirm dialog, then the machine really powers off
            s.ctl("popup", "settings"); s.ctl("popup", "close")
            s.click_key("d"); time.sleep(0.6)
            r = s.ctl("popup", "menu")
            # the power button inside the menu
            ok = s.click_key("menu:power"); time.sleep(0.6)
            pw = s.ctl("popups")
            check("the menu has a power pop-up (power off / reboot / console)", any("popup power" in l for l in pw), str(pw))
            s.shot("13-power")
            s.ctl("button", "Power off"); time.sleep(0.8)
            s.ctl("button", "Yes")
            rc = s.vm.wait_exit(90)
            check("power off from the desktop really powers the VM off", rc == 0 and "Power down" in s.vm.buf, f"rc={rc}")
        except Exception as e:  # noqa: BLE001
            check("desktop test run completed", False, repr(e))
        finally:
            if s:
                s.close()
    failed = [n for n, ok in results if not ok]
    print(f"passed: {len(results) - len(failed)}, failed: {len(failed)}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(run(sys.argv[1], sys.argv[2], sys.argv[3] if len(sys.argv) > 3 else tempfile.mkdtemp()))
