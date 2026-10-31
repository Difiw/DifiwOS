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

## Chromium Embedded Framework 156.0.14 / Chromium 156.0.8078.48 (Difiw OS 0.5.6)
CEF — BSD-3-Clause (Marshall Aubrey Greenblatt, The Chromium Embedded Framework Authors); Chromium и его сторонние компоненты — BSD-подобные и другие
лицензии, полный перечень лежит в образе: `/opt/netline/CREDITS.html` и `/opt/netline/LICENSE.txt`. Бинарная сборка скачивается скриптом
`scripts/fetch-cef.sh` с https://cef-builds.spotifycdn.com/ (минимальный дистрибутив linux64, SHA-1 `d091c82a8655113a34616c08cb340bf92310298d`) и проверяется.
Проприетарные кодеки (H.264/AAC) и DRM в этой сборке CEF **не** включены. Netline (`src/netline-cef`) — MIT.
