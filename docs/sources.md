# Источники и версии (Difiw OS 0.1.0 Core)

Дата получения исходников: 2026-10-30.

| Компонент | Версия | Откуда | Контроль целостности |
|---|---|---|---|
| Buildroot | 2026.08.1 | https://buildroot.org/downloads/buildroot-2026.08.1.tar.xz | SHA-256 `34fb7bea8a213ffde39cc02e61703391560f26e0850f03138c722d0b09880f2b` из подписанного объявления релиза (`.tar.xz.sign`); закреплён в `versions.env`, проверяется `scripts/fetch-buildroot.sh` |
| Linux kernel | 7.1.13 (`7.1.13-difiw`) | https://cdn.kernel.org/pub/linux/kernel/v7.x/ через Buildroot | хэш из пакета Buildroot (`linux.hash`) |
| BusyBox | 1.38.0 | https://busybox.net/ через Buildroot | хэш из Buildroot |
| musl libc | 1.2.6 | https://musl.libc.org/ через Buildroot | хэш из Buildroot |
| GCC (toolchain Buildroot) | 15.3.0 | через Buildroot | хэш из Buildroot |
| binutils | 2.45.1 | через Buildroot | хэш из Buildroot |
| GRUB 2 (BIOS, i386-pc) | 2.12 (Ubuntu 24.04 пакет `grub-pc-bin 2.12-1ubuntu7.3`) | пакет дистрибутива build-хоста | менеджер пакетов Ubuntu |
| xorriso | 1.5.6 (Ubuntu пакет `1:1.5.6-1.1ubuntu3`) | пакет дистрибутива build-хоста | менеджер пакетов Ubuntu |

Версия ядра: Buildroot 2026.08.1 берёт «последнюю» ядерную ветку (`BR2_LINUX_KERNEL_LATEST_VERSION`), поэтому
точная версия определяется релизом Buildroot; она зафиксирована самим Buildroot и записана в
`artifacts/build-report.txt` после сборки.

Примечания:
- GRUB собирается не Buildroot, а берётся с build-хоста (`grub-mkrescue`). Для повторяемости документированы
  версии пакетов Ubuntu 24.04.
- Исходный код ядра не изменялся. Брендинг — только через `CONFIG_LOCALVERSION="-difiw"`, `/etc/os-release`,
  `/etc/issue`, hostname и оболочку.
- Исходники и лицензии компонентов: `make legal-info` (см. `docs/licenses.md`).
