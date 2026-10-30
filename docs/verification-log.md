# Журнал проверки Difiw OS 0.1.0 Core

Среда: Windows 11 + WSL2 (Ubuntu 24.04, 8 CPU, ~7,8 ГБ RAM), VirtualBox 7.2.22, QEMU из Ubuntu 24.04.
Дата: 2026-10-30.

## Ход сборки (что реально происходило)
1. `wsl --install` потребовал перезагрузки Windows, затем `wsl --install -d Ubuntu-24.04`.
2. Первая сборка упала: `Your PATH contains spaces` (WSL добавляет Windows-пути). Исправлено в `scripts/lib.sh`.
3. Вторая сборка упала на загрузке ядра: временный сбой DNS в WSL. Повтор продолжил сборку с места остановки.
4. Первая проверка QEMU: загрузка доходила до init, но сессия на `ttyS0` не стартовала — `/proc/tty/driver/serial`
   отсутствует в ядре 7.1; определение UART заменено на `/sys/class/tty/ttyS0/type`. Лишний `mount devtmpfs`
   убран (устройство уже смонтировано: «Resource busy»).
5. Ошибки в самом тесте (поиск «Linux version» при `loglevel=5`, формат путей `./boot/...` в листинге xorriso,
   синхронизация с приглашением после `cd`) исправлены в `tests/smoke_qemu.py` и `scripts/verify.sh`.
6. Логотип в банере был искажён экранированием — заменён на raw-строку.

## Итоговая проверка (`scripts/verify.sh`, `artifacts/verify-report.log`): VERIFY: PASSED
- Юнит-тесты парсера/форматирования (ASan+UBSan) и 35 smoke-проверок Difiw Shell на хосте;
- ISO: размер 22 290 432 байт, ISO 9660 (bootable), El Torito, `SHA256SUMS`, наличие `/boot/bzImage`,
  `/boot/rootfs.cpio.gz`, `/boot/grub/grub.cfg`;
- конфигурация ядра: ELF, initrd, devtmpfs, tmpfs, proc, sysfs, VT/VGA/serial console, PCI, ACPI, RTC, AHCI, ext4,
  virtio-blk, клавиатура, `-difiw`;
- QEMU (31 проверка): GRUB, ядро, отсутствие panic, банер, `help`, `info`, `mem`, `disk`, `time`, `date`,
  `mkdir`, `touch`, `ls`, `size`, `cd`, `read`, `del` (включая защиту и отказы), неизвестная команда, `clear`,
  `poweroff` (VM выключается), `reboot` (QEMU `-no-reboot` завершается), диск `DIFIWDATA`: запись в `/data` и
  сохранение после перезагрузки.

## VirtualBox 7.2.22 (ручная автоматизация через VBoxManage)
Временная VM `difiw-os-test` (Linux 64-bit, 512 МБ, BIOS, ISO на IDE). Проверено по скриншотам:
- загрузка до приглашения `difiw:/root>`, банер с версией ядра `7.1.13-difiw`;
- `info`, `mem`, `mkdir`, `touch`, `ls`, `del /etc/passwd` (отказ «protected system path»);
- `poweroff` → состояние VM `poweroff`;
- `reboot` → заново загрузка, uptime сброшен, файл, созданный до перезагрузки, исчез (данные только в RAM).
VM `difiw-os-test` и её файлы удалены; VM «Ubuntu Linux» не изменялась.
Этот прогон выполнен на сборке ISO до исправления логотипа; логика не менялась, логотип проверен только на
хосте/QEMU-прогоне итоговой сборки.

## Не проверено
Реальное железо, UEFI (не реализован), сеть, диск `DIFIWDATA` в VirtualBox, другие версии VirtualBox/QEMU,
юридический аудит лицензий.

---

# Версия 0.2.0 Core (2026-10-30)

Новое: `dir`, `cd ..` / `cd..` / пути с `\`, псевдонимы `cls type md rd`, `ver`; GRUB без меню (timeout 0);
языки `ru`/`en` (`lang`), справка `help ru`, `--ru`/`--en` для любой команды; кириллический шрифт
(`cyr-sun16`) и раскладка `ruwin_alt_sh-UTF-8` с переключением Alt+Shift на консоли VGA; пакет `kbd` (урезан в post-build).

Найденные и исправленные ошибки:
- Buildroot не пересобирает локальный пакет `difiw-shell` при правке `src/` — `build.sh` теперь делает `difiw-shell-dirclean`.
- post-build удалял ненужные раскладки из инкрементального `target`; теперь недостающие файлы берутся из исходников kbd.
- Тест QEMU: меню GRUB при timeout 0 не печатается; декодирование UTF-8 по границам чтения.

Результаты `scripts/verify.sh` на итоговом ISO: **VERIFY: PASSED** (58 проверок оболочки на хосте, 39 в QEMU, включая
`dir`, `cd..`, `cd \root\work`, `ru`/`help` по-русски, `echo тест привет`, `poweroff`, `reboot`, диск `DIFIWDATA`).

VirtualBox 7.2.22 (временная VM, затем удалена): загрузка без меню, `dir`, `cd ..`, `ru` (шрифт + подсказка Alt+Shift),
`echo` + Alt+Shift + русский текст в одной строке (вывод: «тест привет мир»), `help ru`, `help en`, `cd ru --ru`.
Не проверено: Caps Lock в русской раскладке, Ctrl+Shift, реальное железо.
