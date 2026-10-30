# Difiw OS 0.2.0 Core

Минимальный дистрибутив на основе Linux для x86_64: загрузочный ISO (BIOS, GRUB 2) с собственной
командной оболочкой **Difiw Shell** (C++17).

## Что это и чем пока не является
- Это **Linux-based** система: официальное ядро Linux (без правок), BusyBox, musl и сборка через Buildroot.
  Своё — конфигурация сборки, init/rootfs, оформление, документация и Difiw Shell.
- Это **не** новое ядро, не полноценный дистрибутив и не установщик. Установки на диск, пакетного менеджера,
  сети «из коробки» и UEFI-загрузки в 0.2.0 нет.

## Статус 0.2.0
Фактически проверено (журнал: `docs/verification-log.md`, свежий прогон: `artifacts/verify-report.log`):
- сборка ISO с нуля в WSL2/Ubuntu 24.04;
- загрузка в QEMU и в VirtualBox 7.2 (BIOS), приглашение Difiw Shell на VGA и serial;
- все обязательные команды, `dir`/`cd ..`, `help ru`, Alt+Shift и русский ввод, `reboot` и `poweroff` (QEMU и VirtualBox);
- необязательный диск данных `DIFIWDATA` (ext4) в QEMU.

Не проверено: реальное железо, UEFI, сеть, другие версии VirtualBox.

## Требования
- Linux или Windows 11 + WSL2 (Ubuntu 24.04). Buildroot официально рассчитан на Linux.
- 20+ ГБ свободного места, 4+ ГБ RAM (8 ГБ рекомендуется), интернет для первой сборки.
- Пакеты (Ubuntu/Debian):
  ```bash
  sudo apt-get install -y build-essential git wget curl rsync cpio bc perl python3 unzip file xz-utils bzip2 patch \
       libncurses-dev libssl-dev libelf-dev flex bison xorriso grub-pc-bin grub-common mtools qemu-system-x86 e2fsprogs
  ```

### Windows / WSL2
```powershell
wsl --install -d Ubuntu-24.04     # нужна перезагрузка Windows
```
Сборка идёт в **`~/difiw-build` внутри WSL** (быстро, корректные права и symlink); готовые файлы
копируются в `artifacts/` проекта. Не собирайте на `/mnt/c`/`/mnt/w`. Запуск из PowerShell:
```powershell
powershell -File scripts\wsl.ps1 build
```
Если WSL добавляет в `PATH` каталоги Windows с пробелами, скрипты сами их отбрасывают (Buildroot иначе падает).

## Быстрый старт
В Linux/WSL, из корня проекта:
```bash
./scripts/check-host.sh   # проверка хоста           (make check)
./scripts/build.sh        # получить Buildroot, настроить, собрать, создать ISO   (make build)
./scripts/verify.sh       # проверки + загрузка в QEMU                             (make verify)
```
Результат: `artifacts/difiw-os-0.2.0-x86_64.iso`, `artifacts/SHA256SUMS`, `artifacts/build-report.txt`.
Первая сборка — около 1 часа (toolchain + ядро), повторные — минуты.

Отдельные шаги: `make fetch` (скачать и проверить Buildroot), `make configure` (применить defconfig),
`make run` (запуск в QEMU, выход Ctrl-A X), `make test` (юнит-тесты оболочки на хосте), `make clean`
(удалить output и зеркало; `scripts/clean.sh --all --artifacts` — всё, включая загрузки и ISO),
`make legal-info`.

Скрипт `clean.sh` удаляет только внутри `~/difiw-build`, помеченного файлом `.difiw-build-root`.

## Загрузочная цепочка
```
BIOS -> GRUB 2 (ISO, El Torito) -> Linux bzImage + initramfs (rootfs.cpio.gz в RAM)
     -> /init (BusyBox) -> /etc/inittab -> S10difiw-boot -> difiw-session -> Difiw Shell
```
- GRUB: `console=tty0 console=ttyS0,115200 loglevel=5`, таймаут 0 — меню не показывается, система грузится
  сразу (вернуть меню: `set timeout=3` в `buildroot-external/board/difiw/x86_64/grub.cfg`). Вывод дублируется на
  serial, ввод — только клавиатура (чтобы «плавающий» COM1 в VirtualBox не прерывал загрузку).
- Сессия Difiw Shell запускается от root без пароля на `tty1`; на `ttyS0` — только если есть реальный UART
  (QEMU). Это осознанный выбор для локального прототипа: нет сети и нет входа извне.
- Если Difiw Shell не запускается или падает, `difiw-session` печатает причину и запускает `/bin/sh`.
- Выход из оболочки (`exit`, Ctrl+D) запускает новую сессию.

## Структура проекта
```
README.md, LICENSE, VERSION, versions.env, Makefile
scripts/            check-host, fetch-buildroot, sync, configure, build, verify, run-qemu, clean, legal-info, wsl.ps1
src/difiw-shell/    исходный код оболочки (C++17), Makefile
tests/              юнит-тесты, smoke-тест оболочки, автотест QEMU
buildroot-external/ br2-external: defconfig, пакет difiw-shell, board/difiw/x86_64 (overlay, fragments, GRUB, post-*.sh)
docs/               sources.md, licenses.md, verification-log.md
artifacts/          ISO, SHA256SUMS, отчёты (ISO не коммитится)
```
Buildroot не хранится в репозитории: скачивается и проверяется по SHA-256 из `versions.env`.

## Выбор конфигурации
- **musl**, а не glibc: меньше размер и проще статическая линковка оболочки. Минус — меньшая совместимость
  с чужими бинарниками.
- **initramfs**: rootfs целиком в RAM, ISO остаётся read-only, нет зависимости от драйверов CD-ROM.
- **Только BIOS**: UEFI запланирован на 0.3.0. В VirtualBox отключите EFI.
- Ядро: `x86_64_defconfig` + `board/difiw/x86_64/linux.fragment`; BusyBox: стандартная конфигурация Buildroot +
  `busybox.fragment`. Конфигурация Buildroot: `buildroot-external/configs/difiw_x86_64_defconfig`
  (обновить: `scripts/configure.sh --save`).

## Difiw Shell
Все команды — **встроенные** (реализованы в самой оболочке). Неизвестная команда запускается как внешняя
программа из `PATH` (BusyBox), иначе: `command not found`.

| Команда | Описание | Пример |
|---|---|---|
| `help [команда]` | список команд / справка; `help ru` — на русском, `help en` — на английском | `help del`, `help ru`, `help cd ru` |
| `dir [-a] [путь]` | список как в Windows (даты, размеры, `<DIR>`, итоги) | `dir`, `dir ..\etc` |
| `time` | время `HH:MM:SS (UTC±hh:mm)` | `time` |
| `date` | дата `YYYY-MM-DD, день` | `date` |
| `info` | версия ОС, ядро, hostname, uptime, режим хранения | `info` |
| `mem` | RAM: всего / занято / доступно / свободно | `mem` |
| `disk [path]` | ёмкость файловой системы пути | `disk /data` |
| `size <path>` | размер файла или каталога (рекурсивно, symlink не обходятся) | `size /etc` |
| `ls [-a] [path]` | список; `d` каталог, `-` файл, `l` ссылка | `ls -a /root` |
| `cd [path]` | сменить каталог: `cd ..`, `cd..`, `cd \tmp` (без аргумента — `/root`) | `cd /tmp` |
| `pwd`, `echo`, `ver`, `exit` | вспомогательные | `echo "hi there"` |
| `lang [en\|ru]`, `en`, `ru` | язык сообщений (см. ниже) | `ru` |
| `read <file>` | вывод текстового файла (до 64 КиБ; бинарные отклоняются) | `read /etc/os-release` |
| `clear` | очистка экрана | |
| `mkdir [-p] <path>...` | создать каталоги | `mkdir -p a/b` |
| `touch <path>...` | создать файл / обновить время | `touch notes.txt` |
| `del [-r [-y]] <path>` | удалить файл или пустой каталог; `-r` — с содержимым (с подтверждением) | `del notes.txt` |
| `reboot` | `sync` и перезагрузка | |
| `poweroff` | `sync` и выключение | |

Защита `del`: нельзя удалять `/`, `/root`, `/tmp`, `/data` (сами каталоги), `/etc`, `/usr`, `/bin`, `/sbin`,
`/lib`, `/boot`, `/dev`, `/proc`, `/sys` и всё внутри них (проверяется и по реальному пути, так что symlink не
обойдёт защиту); пустой путь отклоняется; нельзя удалить текущий каталог; `del -r` требует `y` либо флаг `-y`
(без терминала — только с `-y`).

Псевдонимы как в Windows: `cls` = `clear`, `type` = `read`, `md` = `mkdir`, `rd`/`rmdir` = `del`, `chdir` = `cd`.
В путях подходят и `/`, и `\` (`cd \tmp`, `dir ..\etc`); `cd /d путь` тоже принимается.

### Язык и клавиатура
- **Язык сообщений**: команда `ru` (русский), `en` (английский) или `lang ru|en`. Выбор помнится до перезагрузки.
- **Один раз на команду**: `help ru`, `info ru`, `mem en` и т. п.; у любой встроенной команды можно в конце дописать
  `--ru` или `--en` (`ls /nope --ru`). В справке на русском аргументы показаны как `<файл>`, `[путь]`;
  `<x>` — обязательный аргумент, `[x]` — необязательный. (Справка по самой команде `ru`: `help ru ru`.)
- **Русская клавиатура**: на консоли VirtualBox загружается шрифт с кириллицей и раскладка ЙЦУКЕН (как в Windows).
  **Alt+Shift** переключает латиницу и русскую раскладку в любой момент, в том числе в середине строки:
  наберите `echo `, нажмите Alt+Shift и пишите `тест`. Раскладка привязана к физическим клавишам, поэтому язык
  вашей Windows не важен: клавиша с английской `T` даёт `е`. Названия команд остаются латинскими.
- На serial-терминале (QEMU) раскладка не переключается, там действует раскладка вашего терминала.

Парсер: пробелы разделяют аргументы; `'одинарные'` и `"двойные"` кавычки (без escape-последовательностей: кавычку
внутри берите кавычкой другого вида); `\` — обычный символ; `#` в начале слова — комментарий. **Нет** конвейеров,
перенаправлений, переменных и подстановок (`*`). Строка — до 4095 символов. Ошибки печатаются как `difiw: <команда>: <причина>`; оболочка не завершается.
Коды: 0 успех, 1 ошибка операции, 2 неверные аргументы, 3 отказ защиты.

## Постоянное хранение (честно)
- **По умолчанию всё в RAM**: файлы в `/root`, `/tmp` и т. д. пропадают после `reboot`. Это проверено.
- Подключённый диск сам по себе ничего не сохраняет. Реализовано **опциональное расширение**: если есть диск
  с ext4 и меткой `DIFIWDATA`, при загрузке он монтируется в `/data`, и там данные переживают перезагрузку
  (проверено в QEMU; в VirtualBox не проверялось). Подготовка диска на хосте:
  ```bash
  truncate -s 256M data.img && mkfs.ext4 -L DIFIWDATA data.img
  DATA_DISK=data.img ./scripts/run-qemu.sh
  ```
  В VirtualBox: сконвертируйте образ (`VBoxManage convertfromraw`) и подключите как диск. В ОС нет `mkfs`
  и установки на диск.

## Запуск в QEMU
```bash
./scripts/run-qemu.sh                  # serial в терминале, выход: Ctrl-A X
```
`./scripts/verify.sh` автоматически загружает ISO, выполняет команды, проверяет `poweroff`, `reboot` и диск данных.

## Запуск в VirtualBox
1. Создать VM: тип **Linux**, версия **Other Linux (64-bit)**.
2. 1–2 CPU, 512 МБ–1 ГБ RAM, жёсткий диск не нужен.
3. Система → **отключить EFI** (только BIOS).
4. Носители: подключить `artifacts/difiw-os-0.2.0-x86_64.iso` как оптический диск; порядок загрузки — оптический первым.
5. Запустить. Ожидается: сразу сообщения ядра (меню GRUB не показывается) → логотип и приглашение `difiw:/root>`.
6. `poweroff` выключает VM, `reboot` перезапускает.

## Известные ограничения
- Только BIOS, только x86_64; нет установки на диск и сети по умолчанию.
- Данные в RAM (кроме опционального `/data`); размер tmpfs = половина RAM.
- Вход root без пароля — только для локального прототипа.
- Нет истории и редактирования строки (только возможности терминала), нет конвейеров.
- Часовой пояс — UTC; время берётся из RTC виртуальной машины и может отличаться от хостовых часов.
- Ядро Linux — «последнее» из Buildroot (7.1.x), не LTS.
- Если ядро печатает `regulatory.db` / `amd_pstate` — это безвредные сообщения виртуального окружения.

## Устранение неполадок
- **`Your PATH contains spaces`** — запускайте через `scripts/*.sh` (они чистят `PATH`) или уберите Windows-пути из PATH WSL.
- **`Temporary failure in name resolution`** при загрузке исходников — временный сбой DNS в WSL; повторите `./scripts/build.sh` (продолжит с места остановки). `wsl --shutdown` и повтор помогают при постоянных сбоях.
- **`grub-mkrescue: not found` / `xorriso` / `mformat`** — установите `grub-pc-bin grub-common xorriso mtools`.
- **Медленная сборка / ошибки прав** — проект собирается в `~/difiw-build`, а не на `/mnt/*`.
- **VirtualBox: после выбора пункта GRUB чёрный экран, мигает курсор, ничего не появляется** — VM создана
  с типом `Other/Unknown` (32-битный режим). Ядро Difiw OS — x86_64. Настройки → Общие → Основные → Тип
  **Linux**, версия **Other Linux (64-bit)**. Проверка: `VBoxManage showvminfo "<имя>" --machinereadable | findstr longmode`
  должно быть `longmode="on"`.
- **VirtualBox показывает `no bootable medium`** — проверьте, что ISO подключён и EFI отключён.
- **VirtualBox не стартует при включённом Hyper-V/WSL2** — используйте VirtualBox 7+, он работает поверх Hyper-V; при VERR_NEM_* обновите VirtualBox.
- Чистая пересборка: `./scripts/clean.sh && ./scripts/build.sh`.

## Дальнейшее развитие (0.3.0+)
UEFI, установка на диск, история команд/редактирование строки, конвейеры и перенаправления, сеть,
автоматическая разметка постоянного хранилища, аудит лицензий перед публикацией ISO.

## Лицензия
Собственный код — MIT (`LICENSE`). Linux, Buildroot, BusyBox, musl, GRUB и др. — под своими лицензиями
(`docs/licenses.md`, `make legal-info`). Перед публичным распространением ISO нужен отдельный license audit.
