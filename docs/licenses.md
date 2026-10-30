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
