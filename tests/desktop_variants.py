#!/usr/bin/env python3
"""QEMU tests of the desktop in other conditions: a different screen resolution, autostart at boot,
the Terminal app leaving the desktop. Builds a variant ISO with another GRUB command line.

Usage: desktop_variants.py <iso> <log-file> <screenshot-dir>
SPDX-License-Identifier: MIT
"""
import os
import re
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import smoke_qemu as sq  # noqa: E402
import desktop_qemu as dq  # noqa: E402
import gui_qemu as gq  # noqa: E402

results = sq.results
check = sq.check

GRUB = """set default=0
set timeout=0
serial --unit=0 --speed=115200
terminal_input console
terminal_output console serial
menuentry "Difiw OS variant" {
    linux /boot/bzImage console=tty0 console=ttyS0,115200 loglevel=5 %s
    initrd /boot/rootfs.cpio.gz
}
"""


def variant_iso(iso, workdir, args):
    cfg = os.path.join(workdir, "grub.cfg")
    out = os.path.join(workdir, "variant.iso")
    with open(cfg, "w") as f:
        f.write(GRUB % args)
    subprocess.run(["xorriso", "-indev", iso, "-outdev", out, "-map", cfg, "/boot/grub/grub.cfg", "-boot_image", "any", "replay"],
                   check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    return out


def popup_rects(s, W, H):
    bad = []
    for kind in ("menu", "tasks", "net", "sound", "calendar", "notify", "layout", "settings"):
        t = s.ctl("popup", kind)
        m = re.search(r"anchor=(\S+) rect=(\S+)", " ".join(t))
        if not m:
            bad.append(kind + ":missing")
            continue
        a, r = dq.rect(m.group(1)), dq.rect(m.group(2))
        if not (r[0] >= 0 and r[1] >= 0 and r[0] + r[2] <= W and r[1] + r[3] <= H and r[1] + r[3] <= a[1]):
            bad.append(f"{kind}:{r}")
        s.ctl("popup", "close")
    return bad


def run(iso, logpath, shots):
    os.makedirs(shots, exist_ok=True)
    work = tempfile.mkdtemp(prefix="difiw-var-")
    with open(logpath, "w", encoding="utf-8", errors="replace") as log:
        s = None
        try:
            # ---- 800x600, desktop started by the kernel option difiw.desktop=1 (no manual start)
            vi = variant_iso(iso, work, "vga=789 difiw.desktop=1")
            s = dq.Session(vi, log, shots)
            scr = None
            for _ in range(60):
                time.sleep(0.5)
                r = s.ctl("screen")
                if r and re.match(r"\d+x\d+", r[0]):
                    scr = r[0]
                    break
            check("the desktop starts at boot without a manual command (difiw.desktop=1)", scr is not None, str(scr))
            check("the desktop adapts to another resolution (800x600)", scr == "800x600 scale=1", str(scr))
            img = s.shot("v1-800x600")
            check("800x600: the wallpaper and panel are drawn", img[0] == 800 and sum(gq.pixel(img, 400, 120)) > 80)
            s.ctl("set", "lang", "en")
            for app in ("notes", "calc", "terminal", "monitor", "probe"):
                s.ctl("launch", app)
            time.sleep(4)
            d = s.ctl("dock")
            check("800x600: the panel holds the running apps", sum(1 for l in d if l.startswith("item")) == 5, str(d))
            bad = popup_rects(s, 800, 600)
            check("800x600: every pop-up stays inside the screen and above its button", not bad, str(bad))
            s.ctl("set", "scale", "2"); time.sleep(0.8)
            bad = popup_rects(s, 800, 600)
            check("800x600 at 200% scale: every pop-up still fits", not bad, str(bad))
            s.shot("v2-800x600-scale2")
            s.ctl("set", "scale", "1"); time.sleep(0.5)

            # ---- the Terminal app: `terminal` closes the desktop and returns to the text console
            iid = None
            for l in s.ctl("instances"):
                if "app=terminal" in l:
                    iid = sq.re.search(r"iid=(\d+)", l).group(1)
            ws = s.ctl("windows")
            wid = None
            for l in ws:
                if "title=Terminal" in l:
                    wid = re.search(r"win=(\d+)", l).group(1)
            if wid:
                s.ctl("focus", wid)
            time.sleep(0.5)
            s.qmp.text("terminal"); s.qmp.key("ret")
            gone = False
            for _ in range(30):
                time.sleep(0.5)
                r = s.ctl("screen")
                if r and "cannot connect" in " ".join(r) or (r and "error" in " ".join(r)):
                    gone = True
                    break
            check("typing `terminal` in the desktop terminal closes the desktop", gone, str(r))
            out = s.sh("read /var/log/difiw-desktop.log")
            check("the journal records starts, exits and requests", "launch app=notes" in out and "desktop stopped" in out, out[-300:])
            check("apps were shut down together with the desktop", "exit app=" in out, out[-300:])
            s.sh("poweroff", 5) if False else None
            s.vm.p.stdin.write(b"poweroff\r"); s.vm.p.stdin.flush()
            s.vm.wait_exit(60)
        except Exception as e:  # noqa: BLE001
            check("variant test run completed", False, repr(e))
        finally:
            if s:
                s.close()
    failed = [n for n, ok in results if not ok]
    print(f"passed: {len(results) - len(failed)}, failed: {len(failed)}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(run(sys.argv[1], sys.argv[2], sys.argv[3] if len(sys.argv) > 3 else tempfile.mkdtemp()))
