# Лицензии

**Собственный код Difiw OS** (Difiw Shell, скрипты, конфигурация, документация этого репозитория) — лицензия
MIT, см. `LICENSE`. MIT **не распространяется** на сторонние компоненты.

| Компонент | Лицензия (основная) |
|---|---|
| Linux kernel | GPL-2.0 (с syscall exception) |
| Buildroot | GPL-2.0-or-later |
| BusyBox | GPL-2.0-only |
| musl libc | MIT |
| GCC runtime (libgcc/libstdc++ при статической линковке) | GPL-3.0-or-later с GCC Runtime Library Exception |
| GRUB 2 (в ISO) | GPL-3.0-or-later |

Difiw Shell линкуется статически с musl и частями runtime GCC, поэтому в бинарнике присутствует код под
этими лицензиями.

## Обязанности при распространении ISO
- сохранять тексты лицензий и уведомления;
- предоставлять соответствующие исходники (corresponding source) для GPL-компонентов, включая патчи и
  конфигурации сборки.

Команда `make legal-info` (`scripts/legal-info.sh`) запускает Buildroot `legal-info` и копирует манифест,
тексты лицензий и исходные архивы в `artifacts/legal-info/`.

**Это не юридический аудит.** Перед публичным распространением ISO нужен отдельный license audit.

## stb_image 2.30 (src/difiw-desktop/third_party/stb_image.h)
Public domain (Unlicense) / MIT, автор Sean Barrett. Используется браузером Netline для показа JPEG/PNG/GIF/BMP.

## NetSurf 3.11 (Difiw OS 0.5.5)
NetSurf (GPL-2.0) and its libraries libcss, libdom, libhubbub, libparserutils, libwapcaplet, libnsfb, libnsgif, libnsbmp, libsvgtiny, libnslog, libnsutils, libnspsl, libutf8proc (MIT / compatible) are built from `netsurf-all-3.11.tar.gz`
(https://download.netsurf-browser.org/netsurf/releases/source-full/). Difiw changes: `src/netsurf/difiw_surface.c` (replaces libnsfb's ram surface) and a small Unicode key patch in the framebuffer front end (`scripts/netsurf-build.sh`).
