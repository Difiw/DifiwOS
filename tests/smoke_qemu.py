#!/usr/bin/env python3
"""Automated QEMU serial smoke test for the Difiw OS ISO.

Phases: boot + commands + poweroff, boot + reboot, persistence disk round trip.
Usage: smoke_qemu.py <iso> <log-file>
SPDX-License-Identifier: MIT
"""
import codecs
import os
import queue
import re
import shutil
import subprocess
import sys
import tempfile
import threading
import time

PROMPT = "difiw:/root> "
ANY_PROMPT = re.compile(r"difiw:/[^\s>]*> ")


class Vm:
    def __init__(self, iso, extra=None, log=None):
        args = ["qemu-system-x86_64", "-m", "512", "-cdrom", iso, "-boot", "d",
                "-display", "none", "-serial", "stdio", "-monitor", "none", "-no-reboot"]
        if os.access("/dev/kvm", os.W_OK):
            args += ["-accel", "kvm", "-cpu", "host"]
        args += extra or []
        self.p = subprocess.Popen(args, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  stderr=subprocess.STDOUT)
        self.buf = ""
        self.q = queue.Queue()
        self.log = log
        threading.Thread(target=self._pump, daemon=True).start()

    def _pump(self):
        dec = codecs.getincrementaldecoder("utf-8")(errors="replace")  # keeps multibyte chars intact
        while True:
            b = self.p.stdout.read1(4096)
            if not b:
                self.q.put(None)
                return
            self.q.put(dec.decode(b))

    def _drain(self, timeout):
        try:
            s = self.q.get(timeout=timeout)
        except queue.Empty:
            return True
        if s is None:
            return False
        self.buf += s
        if self.log:
            self.log.write(s)
            self.log.flush()
        return True

    def wait_for(self, text, timeout=120, start=0):
        end = time.time() + timeout
        while time.time() < end:
            i = self.buf.find(text, start)
            if i >= 0:
                return i + len(text)
            if not self._drain(0.5):
                break
        i = self.buf.find(text, start)
        if i >= 0:
            return i + len(text)
        raise TimeoutError(f"timeout waiting for {text!r}")

    def run(self, cmd, timeout=20):
        start = len(self.buf)
        self.p.stdin.write((cmd + "\r").encode())
        self.p.stdin.flush()
        # wait for the echoed command, then for the next prompt
        pos = self.wait_for(cmd, timeout, start)
        deadline = time.time() + timeout
        while True:
            m = ANY_PROMPT.search(self.buf, pos)
            if m:
                return self.buf[pos:m.start()]
            if time.time() > deadline or not self._drain(0.5):
                m = ANY_PROMPT.search(self.buf, pos)
                if m:
                    return self.buf[pos:m.start()]
                raise TimeoutError(f"no prompt after {cmd!r}")

    def wait_exit(self, timeout=40):
        end = time.time() + timeout
        while time.time() < end:
            if self.p.poll() is not None:
                while self._drain(0.2):
                    if self.q.empty():
                        break
                return self.p.returncode
            self._drain(0.5)
        raise TimeoutError("VM did not exit")

    def kill(self):
        if self.p.poll() is None:
            self.p.kill()
            self.p.wait()


results = []


def check(name, cond, detail=""):
    results.append((name, bool(cond)))
    print(("ok   - " if cond else "FAIL - ") + name + ("" if cond else f"   [{detail[:200]!r}]"))


def boot(iso, log, extra=None):
    vm = Vm(iso, extra, log)
    vm.wait_for(PROMPT, 150)
    return vm


def main():
    iso, logpath = sys.argv[1], sys.argv[2]
    tmp = tempfile.mkdtemp(prefix="difiw-qemu-")
    with open(logpath, "w", encoding="utf-8", errors="replace") as log:
        try:
            # ---- phase 1: boot and commands, then poweroff
            log.write("\n===== PHASE 1: boot + commands + poweroff =====\n")
            vm = boot(iso, log)
            check("GRUB menu skipped (timeout 0), kernel booted straight away", "GNU GRUB" not in vm.buf)
            check("kernel started (banner shows -difiw kernel)", re.search(r"Kernel Linux \S+-difiw", vm.buf) is not None)
            check("no kernel panic", "Kernel panic" not in vm.buf, vm.buf[-300:])
            check("Difiw Shell banner", "Difiw OS 0.4.0 Core" in vm.buf)
            out = vm.run("help");           check("help", "poweroff" in out and "reboot" in out, out)
            out = vm.run("info");           check("info", "Difiw OS 0.4.0 Core" in out and "-difiw" in out, out)
            out = vm.run("mem");            check("mem", "Total:" in out and "Available:" in out, out)
            out = vm.run("disk /");         check("disk", "Filesystem of /" in out, out)
            out = vm.run("time");           check("time", re.search(r"\d\d:\d\d:\d\d \(UTC", out) is not None, out)
            out = vm.run("date");           check("date", re.search(r"\d{4}-\d\d-\d\d", out) is not None, out)
            out = vm.run("mkdir work");     check("mkdir", "difiw:" not in out and "error" not in out, out)
            out = vm.run("touch work/a.txt"); check("touch", "No such" not in out, out)
            out = vm.run("ls work");        check("ls", "a.txt" in out, out)
            out = vm.run("dir");            check("dir (Windows-style)", "Directory of /root" in out and "<DIR>" in out and "work" in out, out)
            out = vm.run("cd work");        check("cd work", "No such" not in out, out)
            out = vm.run("cd..");           check("cd.. goes up", "No such" not in out, out)
            out = vm.run("cd \\root\\work"); check("cd with backslashes", "No such" not in out, out)
            out = vm.run("cd ..")
            out = vm.run("size work");      check("size", "work: " in out and "in 1 files" in out, out)
            out = vm.run("cd work");        check("cd", "No such" not in out, out)
            check("prompt follows cd", "difiw:/root/work> " in vm.buf)
            vm.run("cd /")
            check("prompt shows root dir", "difiw:/> " in vm.buf)
            vm.run("cd /root")
            out = vm.run("read /etc/os-release"); check("read", 'NAME="Difiw OS"' in out, out)
            out = vm.run("read nothere");   check("read missing file is an error", "No such file" in out, out)
            out = vm.run("del work");       check("del refuses non-empty dir", "not empty" in out, out)
            out = vm.run("del /etc/passwd"); check("del protects system files", "protected" in out, out)
            out = vm.run("del work/a.txt"); check("del file", "deleted" in out, out)
            out = vm.run("del work");       check("del empty dir", "deleted" in out, out)
            out = vm.run("frobnicate");     check("unknown command is reported", "command not found" in out, out)
            out = vm.run("clear");          check("clear", "\x1b[" in out, out)
            out = vm.run("ru");             check("ru switches language", "Язык интерфейса: русский" in out, out)
            out = vm.run("help");           check("help in Russian", "встроенные команды" in out, out)
            out = vm.run("echo тест привет"); check("Cyrillic text in echo", "тест привет" in out, out)
            out = vm.run("en");             check("en switches back", "Interface language: English" in out, out)
            out = vm.run("info");           check("shell alive after errors", "Difiw OS" in out, out)
            vm.p.stdin.write(b"poweroff\r"); vm.p.stdin.flush()
            rc = vm.wait_exit(60)
            check("poweroff powers off the VM", rc == 0 and ("Power down" in vm.buf or "reboot: Power" in vm.buf), f"rc={rc} {vm.buf[-200:]}")
            vm.kill()

            # ---- phase 2: reboot
            log.write("\n===== PHASE 2: reboot =====\n")
            vm = boot(iso, log)
            vm.p.stdin.write(b"reboot\r"); vm.p.stdin.flush()
            rc = vm.wait_exit(60)
            check("reboot restarts the VM (QEMU -no-reboot exits)", "Restarting system" in vm.buf or "reboot: Restarting" in vm.buf, f"rc={rc} {vm.buf[-200:]}")
            vm.kill()

            # ---- phase 3: persistence disk
            log.write("\n===== PHASE 3: DIFIWDATA disk =====\n")
            img = os.path.join(tmp, "data.img")
            if shutil.which("mkfs.ext4"):
                subprocess.run(["truncate", "-s", "64M", img], check=True)
                subprocess.run(["mkfs.ext4", "-q", "-F", "-L", "DIFIWDATA", img], check=True)
                extra = ["-drive", f"file={img},format=raw,if=virtio"]
                vm = boot(iso, log, extra)
                out = vm.run("info");   check("data disk detected", "/data is mounted" in out, out)
                out = vm.run("touch /data/keep.txt"); check("write to /data", "denied" not in out and "No such" not in out, out)
                vm.p.stdin.write(b"poweroff\r"); vm.p.stdin.flush(); vm.wait_exit(60); vm.kill()
                vm = boot(iso, log, extra)
                out = vm.run("ls /data");  check("data survives reboot", "keep.txt" in out, out)
                vm.p.stdin.write(b"poweroff\r"); vm.p.stdin.flush(); vm.wait_exit(60); vm.kill()
            else:
                print("skip - persistence phase (mkfs.ext4 missing)")
        except Exception as e:  # noqa: BLE001
            check("test run completed", False, repr(e))
            try:
                vm.kill()
            except Exception:  # noqa: BLE001
                pass
        finally:
            shutil.rmtree(tmp, ignore_errors=True)
    failed = [n for n, ok in results if not ok]
    print(f"passed: {len(results) - len(failed)}, failed: {len(failed)}")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
