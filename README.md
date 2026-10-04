# FTPSrv

A small FTP server for Windows, meant to be handed to someone who does not want a
command line: run the exe, click **Start**, and every drive (or one folder) of that
PC becomes reachable from a phone or another computer on the same LAN.

|  |  |
|---|---|
| **ftpsrv-gui.exe** | windowed build — double-click, no console box, tray icon |
| **ftpsrv.exe** | console build, same engine, driven by flags |
| size / deps | ~1 MB static exe, only system DLLs (ws2_32, kernel32, msvcrt, user32…) |
| defaults | port `2121`, login `admin` / `admin` |

## What's in it

- **Share mode** — every drive as a virtual root (`/C:/`, `/D:/`) or a single folder
- **Settings page** — port, account, anonymous, read-only, idle timeout, TCP keep-alive,
  passive data-port range, IP allow list, log file, autostart
- **Live speed line** — one or two *rewriting* lines instead of a scrolling wall of text;
  a transfer leaves one summary line with the average speed when it ends
- **Stable big transfers** — one passive listener reused per session (no port churn),
  TCP keep-alive on both channels, 1 MB socket buffers, 256 KB blocks, `REST` resume
- **Multi-thread clients** — MT Manager style segmented uploads no longer truncate each other
- **Diagnostics** — every session logs *why* it ended, failed transfers carry the Winsock error code
- **Tray** — closing the window keeps serving; double-click to reopen, right-click to stop / quit
- **Icon + version info** — the little whale is inside the exe (`icon.rc`, resource id 1)

## Use it

1. `设置…` → pick *all drives* or one folder, set your own account → OK
2. `启动服务` → the window shows an address like `ftp://192.168.1.10:2121/`
3. On the phone / other PC use any FTP client (Explorer, FileZilla, MT Manager, Cx …):
   host = that IP, **port 2121**, the account you just set

Windows may ask about the firewall the first time — allow the private network.

## Build

```sh
x86_64-w64-mingw32-windres -i icon.rc -O coff -o icon.res
x86_64-w64-mingw32-g++ -O2 -std=c++17 -static -Wall -o ftpsrv.exe ftpsrv.cpp icon.res -lws2_32
x86_64-w64-mingw32-g++ -O2 -std=c++17 -static -Wall -mwindows \
    -o ftpsrv-gui.exe ftpsrv_gui.cpp icon.res -lws2_32 -lshell32 -lole32 -luuid -lcomdlg32
```

`sh build.sh` does exactly that (and skips the icon if windres is missing);
`build_win.bat` is the same thing on Windows with mingw-w64.
One engine (`ftpsrv_engine.inc`) + two thin front ends — no runtime, no dependencies.

## Test

```sh
cd test && sh run_test.sh          # live progress line            14 asserts
cd test && sh run_segment_test.sh  # segmented-upload truncation    9 asserts
cd test && sh run_allow_test.sh    # IP allow-list CIDR           16 asserts
```

## Docs

- [`docs/MANUAL-zh.md`](docs/MANUAL-zh.md) — full manual (Chinese): every knob, the
  firewall, why the data channel uses a second port, troubleshooting

## Status

The console build has been verified end-to-end on a real Windows PC — login, passive
mode, listing drives, the `226` at the end of a transfer, and a session that pushed
several GB without a hiccup. The GUI build and the later stability work (passive-port
reuse, keep-alive, segmented uploads, IP allow list, tray, icon) compile clean and pass
their tests, but have **not** been run on real hardware yet. Bug reports are welcome.

## Notes

- plain FTP: no TLS, nothing encrypted — use it on a trusted LAN, and prefer
  read-only or the IP allow list when you can
- IPv4 only, Windows x64

MIT — see [LICENSE](LICENSE).
