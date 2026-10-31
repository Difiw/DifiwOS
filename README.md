<div align="center">

<img src="docs/screenshots/netline-icon.png" width="96" alt="Difiw OS">

# Difiw OS

**A small Linux-based operating system with its own desktop, shell and apps.**

🇬🇧 English · [🇷🇺 Русский](README.ru.md) · [📘 Manual (RU)](docs/manual.ru.md)

![version](https://img.shields.io/badge/version-0.5.5-3d7bff) ![arch](https://img.shields.io/badge/arch-x86__64-555) ![license](https://img.shields.io/badge/license-MIT-green)

</div>

![Difiw Desktop](docs/screenshots/10-new-apps.png)

Difiw OS boots from a single ISO (BIOS, GRUB 2) in VirtualBox, QEMU or on real hardware. It is built with [Buildroot](https://buildroot.org/) on top of the official Linux kernel; the desktop, compositor, shell and apps are written from scratch in C++.

## ⬇ Download

Pick a version and click **Download** (one file, no need to clone the repository). Checksums: [SHA256SUMS](https://github.com/Difiw/DifiwOS/raw/iso-releases/SHA256SUMS). All images live in the [`iso-releases`](../../tree/iso-releases) branch.

| Version | What's inside | Size | Link |
|---------|---------------|------|------|
| **0.5.5** | Latest stable: Yandex search in Netline, saved cookies | 36.0 MB | [⬇ Download](https://github.com/Difiw/DifiwOS/raw/iso-releases/difiw-os-0.5.5-x86_64.iso) |
| **0.5.4** | Netline search fixes | 30.0 MB | [⬇ Download](https://github.com/Difiw/DifiwOS/raw/iso-releases/difiw-os-0.5.4-x86_64.iso) |
| **0.5.3** | Network panel, sound devices, Settings pages | 30.0 MB | [⬇ Download](https://github.com/Difiw/DifiwOS/raw/iso-releases/difiw-os-0.5.3-x86_64.iso) |
| **0.5.2** | First Netline browser | 29.8 MB | [⬇ Download](https://github.com/Difiw/DifiwOS/raw/iso-releases/difiw-os-0.5.2-x86_64.iso) |
| **0.5.1** | Settings app, notifications | 29.7 MB | [⬇ Download](https://github.com/Difiw/DifiwOS/raw/iso-releases/difiw-os-0.5.1-x86_64.iso) |
| **0.5.0** | Network, sound, Files, Recorder, Camera | 25.6 MB | [⬇ Download](https://github.com/Difiw/DifiwOS/raw/iso-releases/difiw-os-0.5.0-x86_64.iso) |
| **0.4.0** | Difiw Desktop (compositor, dock) | 24.0 MB | [⬇ Download](https://github.com/Difiw/DifiwOS/raw/iso-releases/difiw-os-0.4.0-x86_64.iso) |
| **0.3.0** | Graphical shell, files/editor/terminal | 21.8 MB | [⬇ Download](https://github.com/Difiw/DifiwOS/raw/iso-releases/difiw-os-0.3.0-x86_64.iso) |
| **0.2.0** | Difiw Shell, en/ru | 21.5 MB | [⬇ Download](https://github.com/Difiw/DifiwOS/raw/iso-releases/difiw-os-0.2.0-x86_64.iso) |

> Work in progress: **0.5.6** (Netline on Chromium) is developed in the [`0.5.6`](../../tree/0.5.6) branch and is not released yet.

## What you get

- **Difiw Desktop** – own compositor with a floating dock, task model, per-app unprivileged users and home folders.
- **Difiw Shell** – command shell with English/Russian UI (Alt+Shift switches the keyboard layout).
- **Apps** – Files, Notepad, Terminal, Monitor, Voice Recorder, Camera, Settings (network, sound, personalisation).
- **Network and sound** – DHCP networking, ALSA audio, USB webcam.
- **Netline** – the built-in web browser (tabs, bookmarks, downloads, Yandex as default search). In 0.5.5 it uses a lightweight engine **without JavaScript**, so web apps do not work yet.

![Settings](docs/screenshots/053-p3-settings-net.png)

## Run it in VirtualBox

1. Create a VM: **Type** Linux, **Version** Other Linux (64-bit), 1–2 CPUs, 1 GB RAM. No hard disk is needed (an empty disk is formatted once and keeps your settings).
2. System → **disable EFI** (BIOS only). System → Pointing device → **USB Tablet**. Network: NAT.
3. Storage → attach the downloaded ISO to the optical drive and boot from it.
4. At the `difiw:/root>` prompt type `screen` to start the graphical desktop. `poweroff` / `reboot` work as expected.

### QEMU

```bash
qemu-system-x86_64 -m 1024 -cdrom difiw-os-0.5.5-x86_64.iso -vga std -netdev user,id=n0 -device e1000,netdev=n0
```

## Build from source

Needs Linux (or WSL2 with Ubuntu 24.04), about 30 GB of disk and an hour or more for the first build.

```bash
./scripts/build.sh     # downloads Buildroot, builds the ISO into artifacts/
./scripts/verify.sh    # boots it in QEMU and checks the basics
```

## Repository layout

| Path | Contents |
|------|----------|
| `src/difiw-desktop` | compositor, desktop, apps, client library |
| `src/difiw-shell` | Difiw Shell |
| `buildroot-external` | Buildroot defconfig, packages, overlay, kernel fragment |
| `scripts` | build, run and verification scripts |
| `tests`, `docs` | QEMU tests, licenses, logs, screenshots |

## License

Difiw OS code is MIT licensed; third-party components keep their own licenses, see [docs/licenses.md](docs/licenses.md).
