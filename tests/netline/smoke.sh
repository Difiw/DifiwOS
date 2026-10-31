#!/bin/bash
# Netline (CEF) smoke test: opens real pages in the real browser binary and checks the page title/URL it reports.
# Run inside Linux/WSL:  tests/netline/smoke.sh <dir-with-netline-binary>
# SPDX-License-Identifier: MIT
BIN="${1:-$HOME/nl-src-all/netline-cef/build}"; HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="${NETLINE_TEST_OUT:-/tmp/netline-test}"; mkdir -p "$OUT"
PORT=18700; python3 "$HERE/server.py" $PORT & SRV=$!; trap 'kill $SRV 2>/dev/null' EXIT; sleep 1
pass=0; fail=0
run() {  # name, arg, expected-substring-in-title-or-url, wait-ms
  local name="$1" arg="$2" want="$3" ms="${4:-12000}" line
  line=$(cd "$BIN" && NETLINE_SHOT="$OUT/$name.bmp:$ms" NETLINE_EXIT_AFTER_SHOT=1 timeout 90 ./netline --private "$arg" 2>/dev/null | grep '^TAB' | head -1)
  if [[ "$line" == *"$want"* ]]; then echo "PASS  $name   $line"; pass=$((pass+1)); else echo "FAIL  $name   want[$want] got[$line]"; fail=$((fail+1)); fi
}
run local        "http://127.0.0.1:$PORT/local.html" "RESULT js=ok localStorage=ok cookie=ok indexedDB=ok fetch=ok websocket=ok cyrillic=ok canvas=ok" 6000
run ya.ru        https://ya.ru/                        "Яндекс"
run yandex.ru    https://yandex.ru/                    "dzen.ru"   # yandex.ru redirects to dzen.ru (site behaviour)
run vk.com       https://vk.com/                       "ВКонтакте"
run github.com   https://github.com/                   "GitHub"
run youtube.com  https://www.youtube.com/              "YouTube"
run wikipedia    https://ru.wikipedia.org/             "Википедия"
run search       "привет мир"                          "yandex.ru/search/?text=%D0%BF%D1%80%D0%B8%D0%B2%D0%B5%D1%82"
run badcert      https://expired.badssl.com/           "url=[data:text/html"
echo "passed=$pass failed=$fail   screenshots in $OUT"
