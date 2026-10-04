
Windows 上一个小巧的 FTP 服务器，给**不想碰命令行**的人做的：双击 exe → 点一下「启动服务」，
这台电脑的硬盘（或者你指定的某个文件夹），就能被同一个 WiFi / 网线里的手机、别的电脑看到：能翻目录、能下载、也能往里传。

|  |  |
|---|---|
| **ftpsrv-gui.exe** | 图形界面版，双击即用，**不弹黑框**，带托盘图标 |
| **ftpsrv.exe** | 命令行版，同一个引擎，参数驱动 |
| 体积 / 依赖 | 约 1 MB 静态 exe，只依赖系统 DLL（ws2_32 / kernel32 / msvcrt / user32…） |
| 默认 | 端口 `2121`，账号 `admin` / `admin` |

## 两个版本，怎么选

一份引擎（`ftpsrv_engine.inc`）+ 两个壳：**干活的代码、稳定性、日志都是同一套**，
区别只在**你看到的样子** —— 一个是有窗口、能用鼠标点的，一个是在黑窗口里敲命令的。
两个功能完全一样，挑顺手的那个就行。

### 图形版 `ftpsrv-gui.exe`（推荐）

双击运行，出来一个小窗口（**不弹黑框**，任务栏里显示的是 `FTPSrv - LAN FTP file server`）：

- **主窗口**：状态 / 当前地址（点「复制地址」一键拷走）、启动服务、停止服务、放行防火墙、
  实时速度行、日志框（谁连过、传了什么、为什么断，一眼看到）
- **设置…**：共享范围（全盘 / 指定文件夹）、账号密码、端口、并发上限、空闲超时、TCP 保活、
  数据端口范围、IP 白名单、只读、日志、开机自启、托盘行为 —— 所有能调的都在这一个页面里
- **托盘**：点窗口右上角的 × 缩到右下角**继续跑**；双击托盘图标打开，右键停止 / 退出
- 设置写进同目录的 `ftpsrv.ini`，下次打开自动带出来

### 命令行版 `ftpsrv.exe`

双击也会跑（全盘共享 + 默认账号），但它真正的价值是**带参数启动**：

| 参数 | 说明 |
|---|---|
| `--port N` | 监听端口，默认 `2121`（填 21 就能省掉地址里的端口号，需管理员/未被占用） |
| `--root PATH` | 只共享某个目录，不写 = 全盘共享 |
| `--user NAME` / `--pass WORD` | 登录账号 / 密码，默认 `admin` / `admin` |
| `--anon` / `--readonly` | 允许匿名登录（匿名只读）/ 全部只读 |
| `--allow LIST` | 只允许这些 IP / 网段连，如 `192.168.1.0/24,10.0.0.5` |
| `--pasv-range L-H` | 数据通道端口范围，默认 `50000-50100` |
| `--keepalive SEC` / `--idle SEC` | TCP 保活（默认 30，0 = 关）/ 空闲超时（默认 900） |
| `--maxclients N` | 并发连接上限，默认 `64` |
| `--log FILE` / `--noprogress` | 日志同时落盘 / 不画实时进度行 |
| `--flush` | 上传完成后强制落盘再回 226（慢盘别开，见手册 FAQ） |
| `--addfw` | 添加防火墙放行规则（需管理员） |

`ftpsrv.exe --help` 看全部。传输时窗口底部**最多五行**原地刷新速度（超过五个并发时末行显示省略号 + 总速率），Ctrl+C 停服。

## 功能要点

- **共享范围** —— 全部硬盘当虚拟根（`/C:/`、`/D:/`），或只共享一个文件夹
- **独立设置页** —— 端口、账号、匿名、只读、空闲超时、TCP 保活、数据端口范围、IP 白名单、日志、开机自启
- **实时速度行** —— **最多五行**原地刷新，不刷屏；并发的传输超过五个时，末行变成 `... +N more running, total X/s`（总速率）；传输结束留一条带平均速度的汇总
- **大文件稳** —— 每个会话只开一个被动监听（不烧端口）、双通道 TCP 保活、1 MB 套接字缓冲、256 KB 传输块、`REST` 断点续传
- **适配多线程传输** —— MT 管理器式的分段/多线程上传不会再互相截断
- **可诊断** —— 每个会话都记录**为什么断**，传输失败带 Winsock 错误码；小于 8 MB 的客户端校验读取只记一行，不刷噪音
- **托盘** —— 点 × 缩到托盘继续跑；双击打开，右键停止 / 退出
- **图标 + 版本信息** —— 那只小鲸鱼就在 exe 里（`icon.rc`，资源 id 1）

## 怎么用

0. **下载**：[Releases](https://github.com/shatangji/FTPSrv/releases/latest) 里两个 exe 直接下（也可以直接点本仓库里的文件）
1. `设置…` → 选「全盘共享」或「指定文件夹」，把账号改成自己的 → 确定
2. `启动服务` → 窗口会显示形如 `ftp://192.168.1.10:2121/` 的地址
3. 手机 / 另一台电脑上随便一个 FTP 客户端（资源管理器、FileZilla、MT 管理器、Cx…）：
   主机 = 那个 IP，**端口 2121**，账号 = 你刚设的

第一次运行 Windows 可能问防火墙 —— 勾「专用网络」允许即可（图形版也可以直接点「放行防火墙」按钮）。

## 编译

```sh
x86_64-w64-mingw32-windres -i icon.rc -O coff -o icon.res
x86_64-w64-mingw32-g++ -O2 -std=c++17 -static -Wall -o ftpsrv.exe ftpsrv.cpp icon.res -lws2_32
x86_64-w64-mingw32-g++ -O2 -std=c++17 -static -Wall -mwindows \
    -o ftpsrv-gui.exe ftpsrv_gui.cpp icon.res -lws2_32 -lshell32 -lole32 -luuid -lcomdlg32
```

`sh build.sh` 就是上面这套（windres 缺失会自动跳过图标）；Windows 下用 `build_win.bat`。
一份引擎 + 两个壳，不带任何运行时依赖。

## 测试

```sh
cd test && sh run_test.sh          # 进度行            14 条断言
cd test && sh run_segment_test.sh  # 分段上传截断保护   9 条
cd test && sh run_allow_test.sh    # IP 白名单 CIDR    16 条
```

## 文档

- [`docs/MANUAL-zh.md`](docs/MANUAL-zh.md) —— 完整中文手册：每一项设置、防火墙、
  为什么数据通道是另一个端口、常见问题

## 现状

- **命令行版**已在真实 Windows 电脑上端到端验证：登录、被动模式、列盘符、`226`，
  一次会话连续上传 27.6 GB 零重传。
- **图形版**与本轮新增的稳定性改动（被动端口复用、保活、分段上传、IP 白名单、托盘、图标）
  编译零警告、测试全绿，但**还没在真机上跑过**。有问题欢迎开 issue。

## 说明

- 明文 FTP，不加密 —— 只在可信局域网里用；能只读就只读，能用 IP 白名单就用上
- 仅 IPv4，Windows x64

MIT — 见 [LICENSE](LICENSE)。

---

# FTPSrv (English)

A small FTP server for Windows, made for people who do not want to touch a command line:
double-click the exe, press **Start**, and the drives of that PC (or one folder you pick) show up
on any phone or computer on the same Wi-Fi / LAN — browse it, download from it, upload to it.

|  |  |
|---|---|
| **ftpsrv-gui.exe** | windowed build — double-click, no console box, tray icon |
| **ftpsrv.exe** | console build, same engine, driven by flags |
| size / deps | ~1 MB static exe, only system DLLs (ws2_32, kernel32, msvcrt, user32…) |
| defaults | port `2121`, login `admin` / `admin` |

## Two builds, pick one

One engine (`ftpsrv_engine.inc`) plus two thin shells: **the same code does the work, the same
stability work and the same logs** — the only difference is what you see: a window you click, or
a console you type into. Both do the same job, so just pick whichever feels natural.

### The windowed build `ftpsrv-gui.exe` (recommended)

Double-click it and you get a small window (no console box; the task manager shows it as
`FTPSrv - LAN FTP file server`):

- **Main window** — status, the current address (one click to copy), Start / Stop,
  add firewall rules, a live speed line, and a log box that says who connected and why a
  session ended
- **Settings…** — share scope (all drives / one folder), account, port, concurrency, idle
  timeout, TCP keep-alive, passive data-port range, IP allow list, read-only, log file,
  autostart, tray behaviour: everything adjustable lives on that one page
- **Tray** — the × hides the window and the server keeps running; double-click to reopen,
  right-click to stop / quit
- Settings are stored in `ftpsrv.ini` next to the exe and reloaded next time

### The console build `ftpsrv.exe`

Double-clicking also works (all drives, default account), but it really shines with flags:

| Flag | Meaning |
|---|---|
| `--port N` | listen port, default `2121` (use 21 to drop the port from URLs) |
| `--root PATH` | share one directory instead of every drive |
| `--user NAME` / `--pass WORD` | account, default `admin` / `admin` |
| `--anon` / `--readonly` | allow anonymous (read-only) / make everything read-only |
| `--allow LIST` | only these IPs / CIDR blocks may connect, e.g. `192.168.1.0/24` |
| `--pasv-range L-H` | passive data-port range, default `50000-50100` |
| `--keepalive SEC` / `--idle SEC` | TCP keep-alive (30, 0 = off) / idle timeout (900) |
| `--maxclients N` | concurrent connection limit, default `64` |
| `--log FILE` / `--noprogress` | also write a log file / no live speed line |
| `--flush` | flush uploads to disk before the `226` (skip it on slow disks) |
| `--addfw` | add the Windows Firewall rules (needs administrator) |

`ftpsrv.exe --help` prints the whole list. While transferring, the console rewrites up to five
lines in place (past five transfers the last line shows "... +N more" plus the total rate);
Ctrl+C stops the server.

## What's in it

- **Share mode** — every drive as a virtual root (`/C:/`, `/D:/`) or a single folder
- **Settings page** — port, account, anonymous, read-only, idle timeout, keep-alive,
  passive data-port range, IP allow list, log file, autostart
- **Live speed line** — up to five rewriting lines instead of a scrolling wall of text; with
  more than five transfers at once the last line becomes `... +N more running, total X/s`;
  a transfer leaves one summary line with the average speed
- **Stable big transfers** — one passive listener reused per session (no port churn),
  TCP keep-alive on both channels, 1 MB socket buffers, 256 KB blocks, `REST` resume
- **Multi-threaded transfers** — MT Manager style segmented uploads no longer truncate each other
- **Diagnosable** — every session logs *why* it ended, failed transfers carry the Winsock
  error code, and small client-side verify reads are logged as a single quiet line
- **Tray** — closing the window keeps serving; double-click to reopen, right-click to stop / quit
- **Icon + version info** — the little whale is inside the exe (`icon.rc`, resource id 1)

## Use it

0. **Download**: grab either exe from [Releases](https://github.com/shatangji/FTPSrv/releases/latest)
1. `设置…` → pick *all drives* or one folder, set your own account → OK
2. `启动服务` → the window shows an address like `ftp://192.168.1.10:2121/`
3. On the phone / other PC use any FTP client (Explorer, FileZilla, MT Manager, Cx …):
   host = that IP, **port 2121**, the account you just set

Windows may ask about the firewall the first time — allow the private network
(the GUI also has an "add firewall rules" button).

## Build

Same two commands as in the Chinese section above, or `sh build.sh` / `build_win.bat`.
One engine + two thin front ends — no runtime, no dependencies.

## Test

```sh
cd test && sh run_test.sh          # live progress line            14 asserts
cd test && sh run_segment_test.sh  # segmented-upload truncation    9 asserts
cd test && sh run_allow_test.sh    # IP allow-list CIDR           16 asserts
```

## Docs

- [`docs/MANUAL-zh.md`](docs/MANUAL-zh.md) — full manual (Chinese): every knob, the firewall,
  why the data channel uses a second port, troubleshooting

## Status

The console build has been verified end-to-end on a real Windows PC — login, passive mode,
listing drives, the `226` at the end of a transfer, and a session that pushed 27.6 GB without
a retry. The GUI build and the later stability work compile clean and pass their tests, but
have **not** been run on real hardware yet. Bug reports are welcome.

## Notes

- plain FTP, nothing encrypted — trusted LAN only; prefer read-only and the IP allow list
- IPv4 only, Windows x64

MIT — see [LICENSE](LICENSE).
