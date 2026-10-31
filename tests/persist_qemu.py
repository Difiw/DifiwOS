#!/usr/bin/env python3
"""QEMU test of persistence (0.5.1): an empty disk becomes the data disk, settings and open apps survive.

Usage: persist_qemu.py <iso> <log-file>
SPDX-License-Identifier: MIT
"""
import os
import re
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import smoke_qemu as sq  # noqa: E402


def ctl(vm, *args):
    out = vm.run("difiwctl " + " ".join(args), 30)
    return [l.strip() for l in out.replace("\r", "").split("\n") if l.strip() and not l.strip().startswith("difiw:/")]


def start_desktop(vm):
    vm.run("sh -c 'difiw-desktop > /tmp/desktop.out 2>&1 &'")
    for _ in range(40):
        time.sleep(0.5)
        r = ctl(vm, "screen")
        if r and re.match(r"\d+x\d+", r[0]):
            return True
    return False


def boot(iso, img, log):
    vm = sq.Vm(iso, ["-vga", "std", "-drive", f"file={img},format=raw,if=ide,index=0"], log)
    vm.wait_for(sq.PROMPT, 150)
    return vm


def main():
    iso, logp = sys.argv[1], sys.argv[2]
    tmp = tempfile.mkdtemp(prefix="difiw-persist-")
    img = os.path.join(tmp, "disk.img")
    with open(img, "wb") as f:
        f.truncate(128 * 1024 * 1024)  # completely empty disk
    with open(logp, "w") as log:
        vm = boot(iso, img, log)
        try:
            mnt = vm.run("sh -c 'mount | grep /data'")
            sq.check("an empty disk is prepared and mounted as the data disk", "/data" in mnt and "ext4" in mnt, mnt)
            sq.check("the desktop starts", start_desktop(vm))
            ctl(vm, "set", "wallpaper", "sunset")
            ctl(vm, "pin", "calc")
            ctl(vm, "launch", "notes")
            time.sleep(3)
            ctl(vm, "quit")  # leaving the desktop keeps the open apps for the next start
            time.sleep(3)
            vm.run("sync")
            vm.p.stdin.write(b"poweroff\r"); vm.p.stdin.flush()
            vm.wait_exit(60)
        finally:
            vm.kill()
        vm = boot(iso, img, log)
        try:
            conf = vm.run("read /etc/difiw/desktop.conf")
            sq.check("settings survive a reboot (wallpaper)", "wallpaper=sunset" in conf, conf)
            sq.check("pinned apps survive a reboot", "pinned=calc" in conf, conf)
        finally:
            vm.kill()
    ok = sum(1 for _, r in sq.results if r)
    print(f"passed: {ok}, failed: {len(sq.results) - ok}")
    sys.exit(0 if ok == len(sq.results) else 1)


if __name__ == "__main__":
    main()
