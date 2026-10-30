#!/usr/bin/env python3
"""QEMU test of the graphical shell: `screen`, screenshots, mouse click, keyboard, save, `terminal`.

Usage: gui_qemu.py <iso> <log-file> [screenshot-dir]
SPDX-License-Identifier: MIT
"""
import json
import os
import re
import shutil
import socket
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import smoke_qemu as sq  # noqa: E402  (Vm helper, PROMPT, check())

results = sq.results


class Qmp:
    def __init__(self, path):
        for _ in range(100):
            try:
                self.s = socket.socket(socket.AF_UNIX)
                self.s.connect(path)
                break
            except OSError:
                self.s.close()
                time.sleep(0.2)
        else:
            raise RuntimeError("cannot connect to QMP")
        self.f = self.s.makefile("rw")
        self.f.readline()  # greeting
        self.cmd("qmp_capabilities")

    def cmd(self, name, args=None):
        self.f.write(json.dumps({"execute": name, "arguments": args or {}}) + "\n")
        self.f.flush()
        while True:
            r = json.loads(self.f.readline())
            if "return" in r or "error" in r:
                return r

    def key(self, k):
        self.cmd("human-monitor-command", {"command-line": "sendkey " + k})
        time.sleep(0.12)

    def text(self, s):
        for ch in s:
            self.key({" ": "spc", "/": "slash", ".": "dot", "-": "minus"}.get(ch, ch))

    def click(self, x, y, w=1024, h=768):
        ev = [{"type": "abs", "data": {"axis": "x", "value": int(x * 32767 / (w - 1))}},
              {"type": "abs", "data": {"axis": "y", "value": int(y * 32767 / (h - 1))}}]
        self.cmd("input-send-event", {"events": ev})
        time.sleep(0.3)
        for down in (True, False):
            self.cmd("input-send-event", {"events": [{"type": "btn", "data": {"down": down, "button": "left"}}]})
            time.sleep(0.15)
        time.sleep(0.4)

    def shot(self, path):
        self.cmd("screendump", {"filename": path})
        for _ in range(50):
            if os.path.exists(path) and os.path.getsize(path) > 1000:
                time.sleep(0.2)
                break
            time.sleep(0.1)


def read_ppm(path):
    data = open(path, "rb").read()
    parts = data.split(b"\n", 3)
    w, h = map(int, parts[1].split())
    return w, h, parts[3]


def pixel(img, x, y):
    w, _, pix = img
    o = (y * w + x) * 3
    return tuple(pix[o:o + 3])


def near(a, b, tol=6):
    return all(abs(p - q) <= tol for p, q in zip(a, b))


def main():
    iso, logpath = sys.argv[1], sys.argv[2]
    shots = sys.argv[3] if len(sys.argv) > 3 else tempfile.mkdtemp(prefix="difiw-shots-")
    os.makedirs(shots, exist_ok=True)
    tmp = tempfile.mkdtemp(prefix="difiw-gui-")
    sock = os.path.join(tmp, "qmp.sock")
    vm = None
    with open(logpath, "w", encoding="utf-8", errors="replace") as log:
        try:
            vm = sq.Vm(iso, ["-vga", "std", "-qmp", f"unix:{sock},server,nowait",
                             "-device", "qemu-xhci", "-device", "usb-tablet", "-device", "usb-kbd"], log)
            vm.wait_for(sq.PROMPT, 150)
            sq.check("text terminal is the default after boot", "Difiw OS 0.3.0 Core" in vm.buf)
            out = vm.run("terminal")
            sq.check("terminal command in text mode", "already in the text terminal" in out, out)
            qmp = Qmp(sock)

            vm.p.stdin.write(b"screen\r"); vm.p.stdin.flush()
            ok = False
            img = None
            for _ in range(40):  # up to ~20 s
                time.sleep(0.5)
                p = os.path.join(shots, "1-desktop.ppm")
                qmp.shot(p)
                img = read_ppm(p)
                if img[0] >= 640 and near(pixel(img, 700, 3), (42, 48, 66)):
                    ok = True
                    break
            sq.check("screen starts the graphical shell (title bar visible)", ok, str(img and (img[0], img[1])))
            sq.check("graphics mode is 1024x768", img is not None and (img[0], img[1]) == (1024, 768), str(img and (img[0], img[1])))

            # mouse: click "Новый файл" (Files tab: buttons row at y ~ 87)
            qmp.click(378, 87)
            qmp.shot(os.path.join(shots, "2-dialog.ppm"))
            img2 = read_ppm(os.path.join(shots, "2-dialog.ppm"))
            sq.check("mouse click opens the 'New file' dialog", near(pixel(img2, 512, 384 - 60), (42, 48, 66), 10) or near(pixel(img2, 300, 330), (42, 48, 66), 10),
                     str(pixel(img2, 512, 324)))
            qmp.text("a.txt")
            qmp.key("ret")
            time.sleep(0.5)
            qmp.text("hello")
            qmp.key("ret")
            qmp.key("alt-shift")  # switch to the Russian layout
            qmp.text("ghbdtn")    # -> привет
            qmp.key("alt-shift")  # back to Latin
            qmp.key("ctrl-s")
            time.sleep(0.5)
            qmp.shot(os.path.join(shots, "3-editor.ppm"))

            qmp.key("f3")  # GUI terminal
            qmp.text("mkdir folder1"); qmp.key("ret"); time.sleep(0.8)
            qmp.text("dir"); qmp.key("ret"); time.sleep(1.2)
            qmp.shot(os.path.join(shots, "4-terminal.ppm"))
            qmp.text("terminal"); qmp.key("ret")
            out = ""
            try:
                vm.wait_for(sq.PROMPT, 30, len(vm.buf) - 200 if len(vm.buf) > 200 else 0)
                sq.check("'terminal' inside the GUI returns to the text console", True)
            except TimeoutError:
                sq.check("'terminal' inside the GUI returns to the text console", False, vm.buf[-200:])
            out = vm.run("read /root/a.txt")
            sq.check("file created in the GUI has the typed text (Latin)", "hello" in out, out)
            sq.check("Russian text typed with Alt+Shift is saved", "привет" in out, out)
            out = vm.run("dir")
            sq.check("folder created from the GUI terminal exists", "folder1" in out, out)

            # `screen` again, then leave through the 'Консоль' button
            vm.p.stdin.write(b"screen\r"); vm.p.stdin.flush()
            time.sleep(6)
            qmp.click(718, 20)  # "Консоль" button in the top bar (approx.)
            try:
                vm.wait_for(sq.PROMPT, 25, max(0, len(vm.buf) - 300))
                sq.check("'screen' can be started again and left with a button", True)
            except TimeoutError:
                sq.check("'screen' can be started again and left with a button", False, vm.buf[-200:])
            vm.p.stdin.write(b"poweroff\r"); vm.p.stdin.flush()
            rc = vm.wait_exit(60)
            sq.check("poweroff after GUI session", rc == 0, f"rc={rc}")
        except Exception as e:  # noqa: BLE001
            sq.check("GUI test run completed", False, repr(e))
        finally:
            if vm:
                vm.kill()
            shutil.rmtree(tmp, ignore_errors=True)
    failed = [n for n, ok in results if not ok]
    print(f"passed: {len(results) - len(failed)}, failed: {len(failed)}")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
