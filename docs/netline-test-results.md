# Netline (CEF) test results

Environment: WSL2 Ubuntu 24.04, SDL development backend, CEF 156.0.14 / Chromium 156.0.8078.48, headless Ozone, software rendering. Command: `tests/netline/smoke.sh`.

| Check | Result |
|-------|--------|
| Local stand: JS, WebAssembly object, localStorage, cookies, IndexedDB, fetch, WebSocket, Cyrillic, canvas | PASS |
| https://ya.ru, https://yandex.ru (redirects to dzen.ru), https://vk.com (redirects to vk.ru), https://github.com, https://www.youtube.com, https://ru.wikipedia.org – page loads, title correct | PASS |
| Address bar: free text becomes a Yandex search | PASS |
| Invalid certificate (expired.badssl.com) is refused with an error page | PASS |
| Keyboard input in the SDL development window under WSLg | NOT TESTED (no text events from xdotool) |
| Running inside the Difiw OS ISO (glibc build 0.5.6) | NOT TESTED yet |
| Memory/CPU measurements, 10 tabs, 30 min idle, 50 open/close cycles, renderer crash | NOT TESTED |
| Video playback, sound, fullscreen on YouTube | NOT TESTED |
| Favicons, permission prompts, background tab unloading, print/PDF | NOT IMPLEMENTED |

Known issue: one Chromium child process sometimes aborts at start-up ("stack smashing detected"); the browser keeps working. Cause not found.
