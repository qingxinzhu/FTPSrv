# FTPSrv · Windows 局域网 FTP 服务器

Windows 上一个小巧的 FTP 服务器，专门做给**不想碰命令行**的人用：双击 exe → 点「启动服务」，
这台电脑的全部硬盘（或指定的一个文件夹）就能被同一局域网里的手机 / 另一台电脑访问。

|  |  |
|---|---|
| **ftpsrv-gui.exe** | 图形界面版，双击即用，**不弹黑框**，带托盘图标 |
| **ftpsrv.exe** | 命令行版，同一个引擎，参数驱动 |
| 体积 / 依赖 | 约 1 MB 静态 exe，只依赖系统 DLL（ws2_32 / kernel32 / msvcrt / user32…） |
| 默认 | 端口 `2121`，账号 `admin` / `admin` |

## 功能要点

- **共享范围** —— 全部硬盘当虚拟根（`/C:/`、`/D:/`），或只共享一个文件夹
- **独立设置页** —— 端口、账号、匿名、只读、空闲超时、TCP 保活、数据端口范围、IP 白名单、日志、开机自启
- **实时速度行** —— 只有一两行原地刷新，不刷屏；传输结束留一条带平均速度的汇总
- **大文件稳** —— 每个会话只开一个被动监听（不烧端口）、双通道 TCP 保活、1 MB 套接字缓冲、256 KB 传输块、`REST` 断点续传
- **多线程客户端** —— MT 管理器式的分段/多线程上传不会再互相截断
- **可诊断** —— 每个会话都记录**为什么断**，传输失败带 Winsock 错误码；小于 8 MB 的客户端校验读取只记一行，不刷噪音
- **托盘** —— 点 × 缩到托盘继续跑；双击打开，右键停止 / 退出
- **图标 + 版本信息** —— 那只小鲸鱼就在 exe 里（`icon.rc`，资源 id 1）

## 怎么用

0. **下载**：[Releases](https://github.com/shatangji/FTPSrv/releases/latest) 里两个 exe 直接下（也可以直接点本仓库里的文件）
1. `设置…` → 选「全盘共享」或「指定文件夹」，把账号改成自己的 → 确定
2. `启动服务` → 窗口会显示形如 `ftp://192.168.1.10:2121/` 的地址
3. 手机 / 另一台电脑上随便一个 FTP 客户端（资源管理器、FileZilla、MT 管理器、Cx…）：
   主机 = 那个 IP，**端口 2121**，账号 = 你刚设的

第一次运行 Windows 可能问防火墙 —— 勾「专用网络」允许即可。

## 编译

```sh
x86_64-w64-mingw32-windres -i icon.rc -O coff -o icon.res
x86_64-w64-mingw32-g++ -O2 -std=c++17 -static -Wall -o ftpsrv.exe ftpsrv.cpp icon.res -lws2_32
x86_64-w64-mingw32-g++ -O2 -std=c++17 -static -Wall -mwindows \
    -o ftpsrv-gui.exe ftpsrv_gui.cpp icon.res -lws2_32 -lshell32 -lole32 -luuid -lcomdlg32
```

`sh build.sh` 就是上面这套（windres 缺失会自动跳过图标）；Windows 下用 `build_win.bat`。
一份引擎（`ftpsrv_engine.inc`）+ 两个壳，不带任何运行时依赖。

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

A tiny FTP server for Windows, made for people who do not want a command line: run the exe,
click **Start**, and every drive (or one folder) of that PC becomes reachable from a phone or
another computer on the same LAN.

|  |  |
|---|---|
| **ftpsrv-gui.exe** | windowed build — double-click, no console box, tray icon |
| **ftpsrv.exe** | console build, same engine, driven by flags |
| size / deps | ~1 MB static exe, only system DLLs (ws2_32, kernel32, msvcrt, user32…) |
| defaults | port `2121`, login `admin` / `admin` |

## What's in it

- **Share mode** — every drive as a virtual root (`/C:/`, `/D:/`) or a single folder
- **Settings page** — port, account, anonymous, read-only, idle timeout, keep-alive,
  passive data-port range, IP allow list, log file, autostart
- **Live speed line** — one or two rewriting lines instead of a scrolling wall of text;
  a transfer leaves one summary line with the average speed
- **Stable big transfers** — one passive listener reused per session (no port churn),
  TCP keep-alive on both channels, 1 MB socket buffers, 256 KB blocks, `REST` resume
- **Multi-thread clients** — MT Manager style segmented uploads no longer truncate each other
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

Windows may ask about the firewall the first time — allow the private network.

## Build

Same two commands as in the Chinese section above, or `sh build.sh` / `build_win.bat`.
One engine (`ftpsrv_engine.inc`) + two thin front ends — no runtime, no dependencies.

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