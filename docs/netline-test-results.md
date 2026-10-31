# Netline (CEF) test results

Environment: WSL2 Ubuntu 24.04, SDL development backend, CEF 156.0.14 / Chromium 156.0.8078.48, headless Ozone, software rendering. Command: `tests/netline/smoke.sh`.

| Check | Result |
|-------|--------|
| Local stand: JS, WebAssembly object, localStorage, cookies, IndexedDB, fetch, WebSocket, Cyrillic, canvas | PASS |
| https://ya.ru, https://yandex.ru (redirects to dzen.ru), https://vk.com (redirects to vk.ru), https://github.com, https://www.youtube.com, https://ru.wikipedia.org – page loads, title correct | PASS |
| Address bar: free text becomes a Yandex search | PASS |
| Invalid certificate (expired.badssl.com) is refused with an error page | PASS |
| Keyboard input in the SDL development window under WSLg | NOT TESTED (no text events from xdotool) |
| Netline starts inside the Difiw OS 0.5.6 ISO (VirtualBox, 4 GB RAM, glibc, headless Ozone, software rendering), start page renders, a click on the Yandex tile opens https://dzen.ru with JS content (manual check, screenshots in `docs/screenshots/netline-in-difiw-*.png`) | PASS |
| Keyboard typing, mouse wheel, tabs, downloads inside the ISO | NOT TESTED |
| Memory/CPU measurements, 10 tabs, 30 min idle, 50 open/close cycles, renderer crash | NOT TESTED |
| Video playback, sound, fullscreen on YouTube | NOT TESTED |
| Favicons, permission prompts, background tab unloading, print/PDF | NOT IMPLEMENTED |

Known issue: one Chromium child process sometimes aborts at start-up ("stack smashing detected"); the browser keeps working. Cause not found.
