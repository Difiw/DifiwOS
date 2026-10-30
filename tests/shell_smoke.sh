#!/usr/bin/env bash
# Host smoke test for Difiw Shell: feeds commands via stdin, checks output.
# Usage: tests/shell_smoke.sh <path-to-difiw-shell>
# SPDX-License-Identifier: MIT
set -euo pipefail

SHELL_BIN="${1:?usage: shell_smoke.sh <difiw-shell binary>}"
WORK="$(mktemp -d "${TMPDIR:-/tmp}/difiw-smoke.XXXXXX")"
trap 'rm -rf -- "$WORK"' EXIT
pass=0; failed=0

run() { "$SHELL_BIN" 2>&1 <<<"$1" || true; }
expect() { # name, input, regex
    local out; out="$(run "$2")"
    if grep -Eq -- "$3" <<<"$out"; then pass=$((pass+1)); echo "ok   - $1"
    else failed=$((failed+1)); echo "FAIL - $1"; echo "       output was: $out"; fi
}

cd "$WORK"
expect "help lists commands"      "help"                       'poweroff'
expect "help of one command"      "help del"                   'usage: del'
expect "unknown command"          "frobnicate"                 'command not found'
expect "empty input is fine"      $'\n\n'                      '^$'
expect "unterminated quote"       'echo "abc'                  'syntax error'
expect "time format"              "time"                       '[0-9]{2}:[0-9]{2}:[0-9]{2} \(UTC'
expect "date format"              "date"                       '[0-9]{4}-[0-9]{2}-[0-9]{2}'
expect "info"                     "info"                       'Difiw OS 0.3.0 Core'
expect "mem"                      "mem"                        'Total:'
expect "disk"                     "disk /"                     'Filesystem of /'
expect "disk missing path"        "disk /nonexistent"          'No such file'
expect "mkdir + ls"               $'mkdir d1\nls'               'd .* d1/'
expect "mkdir -p"                 $'mkdir -p a/b/c\nls a/b'    'c/'
expect "touch + ls"               $'touch f.txt\nls'           'f.txt'
expect "size file"                $'touch s.txt\nsize s.txt'   's.txt: 0 B'
expect "size dir"                 $'mkdir sd\ntouch sd/x\nsize sd' 'in 1 files'
expect "size missing"             "size nothere"               'No such file'
expect "cd missing"               "cd /nonexistent"            'No such file'
expect "cd + pwd"                 $'mkdir cdt\ncd cdt\npwd'    'cdt$'
expect "read file"                $'touch r.txt\nread /etc/os-release' 'NAME='
expect "read directory"           "read ."                     'Is a directory'
expect "read missing"             "read nothere"               'No such file'
expect "del file"                 $'touch gone\ndel gone\nls'  'deleted .*gone'
expect "del nonempty dir refused" $'mkdir full\ntouch full/x\ndel full' 'not empty'
expect "del -r without tty"       $'mkdir full2\ntouch full2/x\ndel -r full2' 'needs confirmation'
expect "del -r -y"                $'mkdir full3\ntouch full3/x\ndel -r -y full3' 'deleted 2 objects'
expect "del protected /"          "del /"                      'protected'
expect "del protected /etc"       "del /etc/passwd"            'protected'
expect "del empty arg"            'del ""'                     'empty path refused'
expect "del wrong args"           "del"                        'usage: del'
expect "unknown option"           "ls -z"                      'unknown option'
expect "ls too many args"         "ls a b"                     'invalid arguments'
expect "quoted names"             $'touch "two words"\nls'     'two words'
long="$(head -c 5000 /dev/zero | tr '\0' 'a')"
expect "too long input"           "$long"                      'input too long'
expect "dir listing"              $'mkdir dd\ntouch ff.txt\ndir'  '<DIR> +dd$'
expect "dir counts"               $'touch q1\ndir'           '[0-9]+ File\(s\)'
expect "dir of missing path"      "dir /nonexistent"           'No such file'
expect "cd .."                    $'mkdir up\ncd up\ncd ..\npwd' 'smoke\.[A-Za-z0-9]+$'
expect "cd.. without space"       $'mkdir up2\ncd up2\ncd..\npwd' 'smoke\.[A-Za-z0-9]+$'
expect "cd with backslash"        $'cd \\tmp\npwd'              '^/tmp$'
expect "cd \\ to root"            $'cd\\\npwd'                  '^/$'
expect "dir with backslash path"  $'dir ..\\..\\tmp'            'Directory of /tmp'
expect "md / rd / type / cls"     $'md m1\nrd m1\ntouch t.txt\ntype t.txt\ncls\nver' 'Difiw OS 0.3.0 Core'
expect "lang shows current"       "lang"                       'Language: en'
expect "lang switch en/ru"        $'ru\nlang\nen\nlang'        'Язык: ru'
expect "lang bad argument"        "lang de"                    'invalid arguments'
expect "terminal in text mode"    "terminal"                   'already in the text terminal'
expect "screen without /dev/fb0"  "screen"                     'fb0'
expect "help lists screen"        "help"                       'screen'
expect "help ru (one-off Russian)" "help ru"                   'встроенные команды'
expect "help ru shows <путь>"     "help ru"                    'read <файл>'
expect "help cd ru"               "help cd ru"                 'формат: cd \[путь\]'
expect "help de en stays English" "help cd en"                 'usage: cd \[path\]'
expect "help en after ru"         $'ru\nhelp en'               'built-in commands'
expect "one-off does not stick"   $'help ru\nhelp cd'          'usage: cd \[path\]'
expect "--ru on any command"      "ls /nonexistent --ru"       'Нет такого файла'
expect "usage error in Russian"   "del --ru"                   'формат: del \[-r \[-y\]\] <путь>'
expect "info ru"                  "info ru"                    'Ядро:'
out="$(DIFIW_LANG=ru "$SHELL_BIN" 2>&1 <<<$'help\nls /nonexistent\ndel /etc/passwd\ninfo')"
if grep -q 'встроенные команды' <<<"$out" && grep -q 'Нет такого файла' <<<"$out" && grep -q 'защищённый' <<<"$out" && grep -q 'Язык: *ru' <<<"$out"; then
    pass=$((pass+1)); echo "ok   - Russian interface messages"
else failed=$((failed+1)); echo "FAIL - Russian interface messages"; echo "       $out"; fi
out="$(DIFIW_LANG=ru "$SHELL_BIN" 2>&1 <<<$'echo тест привет\ntouch "файл номер один.txt"\nls')"
if grep -q 'тест привет' <<<"$out" && grep -q 'файл номер один.txt' <<<"$out"; then
    pass=$((pass+1)); echo "ok   - Cyrillic arguments and file names"
else failed=$((failed+1)); echo "FAIL - Cyrillic arguments"; echo "       $out"; fi
expect "shell survives"          $'ls /nonexistent\ninfo'     'Difiw OS 0.3.0 Core'

echo "passed: $pass, failed: $failed"
[ "$failed" -eq 0 ]
