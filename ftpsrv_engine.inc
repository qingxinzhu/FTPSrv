// ============================================================================
//  FTPSrv  -  Windows LAN FTP file server  (single file, C++17, no runtime deps)
//
//  build (cross, on Linux):
//    x86_64-w64-mingw32-g++ -O2 -std=c++17 -static
//      -o ftpsrv.exe ftpsrv.cpp -lws2_32
//  build (on Windows, mingw-w64):
//    g++ -O2 -std=c++17 -static -o ftpsrv.exe ftpsrv.cpp -lws2_32
//  build (MSVC):
//    cl /EHsc /O2 /std:c++17 ftpsrv.cpp ws2_32.lib
//
//  usage:
//    ftpsrv.exe                         全盘共享，端口 2121，账号 admin/admin
//    ftpsrv.exe --root D:\share         只共享某个目录
//    ftpsrv.exe --port 21 --user u --pass p
//    ftpsrv.exe --anon --readonly       允许匿名 + 只读
//    ftpsrv.exe --addfw                 添加防火墙放行规则（需管理员）
// ============================================================================

#define WIN32_LEAN_AND_MEAN
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <mutex>
#include <cstdio>
#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <cctype>
#include <algorithm>

#ifdef _MSC_VER
#pragma comment(lib, "ws2_32.lib")
#endif

// ---------------------------------------------------------------------------
// encoding helpers
// ---------------------------------------------------------------------------
static std::wstring mbToW(const std::string& s, UINT cp)
{
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(cp, 0, s.data(), (int)s.size(), nullptr, 0);
    if (n <= 0) return std::wstring();
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(cp, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}

static std::string wToMb(const std::wstring& w, UINT cp)
{
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(cp, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return std::string();
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(cp, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

static std::wstring u8ToW(const std::string& s) { return mbToW(s, CP_UTF8); }
static std::string  wToU8(const std::wstring& w) { return wToMb(w, CP_UTF8); }

// ---------------------------------------------------------------------------
// passive data port range.  Keeping the data channel inside a known range is
// what makes a firewall rule able to describe it at all - an OS-chosen
// ephemeral port cannot be written into a rule on the client side.
// ---------------------------------------------------------------------------
#define PASV_PORT_LOW   50000
#define PASV_PORT_HIGH  50100

// ---------------------------------------------------------------------------
// config / globals
// ---------------------------------------------------------------------------
struct Config {
    int          port      = 2121;
    std::wstring rootDir;            // non-empty  ->  single directory mode
    bool         allDrives = true;   // true       ->  share every local drive
    std::string  user      = "admin";
    std::string  pass      = "admin";
    bool         anon      = false;
    bool         readonly  = false;
    std::string  logPath;
    int          idleSec   = 900;
    int          dataIdleSec = 60;   // data channel: no bytes for this long = peer gone
    int          maxClients = 64;   // multi-threaded clients use several at once
    int          keepAliveSec = 30;   // TCP keep-alive idle time (0 = off)
    int          blockKB   = 256;    // read/write block in KB (bigger = fewer disk seeks)
    int          pasvLow  = PASV_PORT_LOW;   // passive data port range (data channel)
    int pasvHigh = PASV_PORT_HIGH;
    bool flushOnFinish = false;      // false: reply 226 at once (slow disks would stall)
    std::string allowList;           // optional IP allow list (CIDR, comma separated)
};

static Config             g_cfg;
static std::atomic<int>   g_active(0);
static std::atomic<bool>  g_run(true);
static std::string        g_primaryIp = "127.0.0.1";
static int                g_lastSockErr = 0;   // last send/recv error, for the log

// read/write block: 16 KB .. 4 MB (one buffer per transfer)
static void clampBlock()
{
    if (g_cfg.blockKB < 16)   g_cfg.blockKB = 16;
    if (g_cfg.blockKB > 4096) g_cfg.blockKB = 4096;
}
static size_t blockBytes()
{
    return (size_t)(g_cfg.blockKB > 0 ? g_cfg.blockKB : 256) * 1024;
}
// keep the data-port range inside sane bounds (also used by the firewall rule)
// true when the socket has data (or closed) within ms
static bool waitReadableMs(SOCKET d, int ms)
{
    fd_set rd;
    FD_ZERO(&rd);
    FD_SET(d, &rd);
    timeval tv;
    tv.tv_sec  = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    return select(0, &rd, nullptr, nullptr, &tv) > 0;
}
static void clampPasvRange()
{
    if (g_cfg.pasvLow  < 1024)     g_cfg.pasvLow  = PASV_PORT_LOW;
    if (g_cfg.pasvHigh > 65535)    g_cfg.pasvHigh = PASV_PORT_HIGH;
    if (g_cfg.pasvLow >= g_cfg.pasvHigh) {
        g_cfg.pasvLow  = PASV_PORT_LOW;
        g_cfg.pasvHigh = PASV_PORT_HIGH;
    }
    if (g_cfg.pasvHigh - g_cfg.pasvLow > 4096)      // don't probe forever
        g_cfg.pasvHigh = g_cfg.pasvLow + 4096;
}


// ---------------------------------------------------------------------------
// frontend hooks - the console build draws a rewriting text line, the GUI
// build updates its own widgets.  Both must provide these four.
// ---------------------------------------------------------------------------
static void logf(const char* fmt, ...);                     // one timestamped line
static int  progBegin(bool upload, const std::string& label, unsigned long long total);
static void progTick(int slot, unsigned long long done);
static void progEnd(int slot, unsigned long long done, bool ok);

// ---------------------------------------------------------------------------
// live-transfer accounting (shared by both frontends)
//
// Every file transfer registers here, so a frontend can tell the user how many
// transfers are running in total and what they add up to in bytes/second - even
// the ones that have no line of their own on screen.  The clock is passed in by
// the caller, which keeps this block free of any Windows API (so it can be unit
// tested on its own).
// ---------------------------------------------------------------------------
#define XFER_MAX 64

struct XferRec {
    bool               used = false;
    unsigned long long done = 0;
};

static XferRec             g_xfer[XFER_MAX];
static std::mutex          g_xferMx;
static int                 g_xferLive   = 0;
static double              g_xferSpeed  = 0;   // bytes/second, smoothed
static double              g_xferLastMs = 0;
static unsigned long long  g_xferLastSum = 0;

static int xferBegin()
{
    std::lock_guard<std::mutex> lk(g_xferMx);
    for (int i = 0; i < XFER_MAX; ++i) {
        if (!g_xfer[i].used) {
            g_xfer[i].used = true;
            g_xfer[i].done = 0;
            ++g_xferLive;
            return i;
        }
    }
    ++g_xferLive;                  // still count it, it just cannot be summed
    return -1;
}

static void xferSet(int h, unsigned long long done, double nowMs)
{
    std::lock_guard<std::mutex> lk(g_xferMx);
    if (h >= 0 && h < XFER_MAX && g_xfer[h].used) g_xfer[h].done = done;

    unsigned long long sum  = 0;
    for (int i = 0; i < XFER_MAX; ++i)
        if (g_xfer[i].used) sum += g_xfer[i].done;

    if (g_xferLastMs <= 0.0) { g_xferLastMs = nowMs; g_xferLastSum = sum; return; }

    double dt = (nowMs - g_xferLastMs) / 1000.0;
    if (dt < 0.25) return;

    double inst = (sum >= g_xferLastSum) ? (double)(sum - g_xferLastSum) / dt : 0.0;
    g_xferSpeed = (g_xferSpeed > 0.0) ? (g_xferSpeed * 0.65 + inst * 0.35) : inst;
    g_xferLastSum = sum;
    g_xferLastMs  = nowMs;
}

static void xferEnd(int h)
{
    std::lock_guard<std::mutex> lk(g_xferMx);
    if (h >= 0 && h < XFER_MAX && g_xfer[h].used) {
        g_xfer[h].used = false;
        g_xfer[h].done = 0;
    }
    if (g_xferLive > 0) --g_xferLive;
    if (g_xferLive <= 0) {         // nothing running: start the next batch clean
        g_xferLive    = 0;
        g_xferSpeed   = 0.0;
        g_xferLastMs  = 0.0;
        g_xferLastSum = 0;
    }
}

static int xferLiveCount()
{
    std::lock_guard<std::mutex> lk(g_xferMx);
    return g_xferLive;
}

static double xferSpeedNow()
{
    std::lock_guard<std::mutex> lk(g_xferMx);
    return g_xferSpeed;
}
// end of live-transfer accounting
// ---------------------------------------------------------------------------

// live session sockets, so engineStop() can kick idle clients off the port
static std::mutex          g_sessMx;
static std::vector<SOCKET> g_sessSocks;
static SOCKET              g_listen = INVALID_SOCKET;

static void registerSession(SOCKET s)
{
    std::lock_guard<std::mutex> lk(g_sessMx);
    g_sessSocks.push_back(s);
}

static void unregisterSession(SOCKET s)
{
    std::lock_guard<std::mutex> lk(g_sessMx);
    for (size_t i = 0; i < g_sessSocks.size(); ++i)
        if (g_sessSocks[i] == s) { g_sessSocks.erase(g_sessSocks.begin() + (long)i); break; }
}
// socket helpers
// ---------------------------------------------------------------------------
static bool sendAll(SOCKET sk, const char* p, size_t n)
{
    int interruptions = 0;
    while (n > 0) {
        size_t chunk = (n > (1u << 20)) ? (1u << 20) : n;
        int k = send(sk, p, (int)chunk, 0);
        if (k <= 0) {
            int e = WSAGetLastError();
            if (e == WSAEINTR && interruptions < 100) {   // signal while blocked
                ++interruptions;
                Sleep(10);
                continue;
            }
            g_lastSockErr = e;
            return false;
        }
        p += k;
        n -= (size_t)k;
    }
    return true;
}

static bool sendAll(SOCKET sk, const std::string& s) { return sendAll(sk, s.data(), s.size()); }

static std::string ipv4ToStr(const in_addr& a)
{
    char b[32] = {0};
    inet_ntop(AF_INET, &a, b, sizeof(b) - 1);
    return std::string(b);
}

// ---------------------------------------------------------------------------
// session
// ---------------------------------------------------------------------------
struct Session {
    SOCKET      ctrl   = INVALID_SOCKET;
    sockaddr_in peer {};
    std::string peerIp;
    std::string user;
    bool        authed = false;
    bool        anonUser = false;
    bool        utf8   = false;          // client speaks UTF-8 (OPTS UTF8 ON)
    bool        binMode = false;         // TYPE I
    std::string cwd    = "/";            // internal UTF-8 virtual path
    long long   rest   = 0;
    std::string rnfr;                    // internal UTF-8
    SOCKET      pasv   = INVALID_SOCKET;
    sockaddr_in act {};
    bool        hasAct = false;
    bool        kill   = false;
    bool        idleTimeout = false;     // control connection hit the idle limit
    std::string lastCmd;                 // for the disconnect log line
    unsigned long long alloTotal = 0;    // size hint from ALLO (uploads only)
    int  dataMode   = -1;                // -1 unknown, 0 = passive (client dials in), 1 = active (we dial out)
    int  xferFails  = 0;                 // failed transfers on this control connection
    int  slowBlocks = 0;                 // writes that took far longer than they should
    bool slowDiskLogged = false;         // "the disk is the bottleneck" warning already printed
    unsigned long long slowMsTotal = 0;  // time this session wasted on slow disk writes
    int  slowWrites = 0;                 // how many of them
    bool modeHintLogged = false;         // the "try active mode" hint is printed once per connection
};

// convert internal UTF-8 <-> client character set
static std::string toClient(const Session& s, const std::string& in8)
{
    if (s.utf8) return in8;
    return wToMb(u8ToW(in8), CP_ACP);
}

static std::string toInternal(const Session& s, const std::string& raw)
{
    if (s.utf8) return raw;
    return wToU8(mbToW(raw, CP_ACP));
}

static void replyRaw(Session& s, const std::string& line8)
{
    std::string out = toClient(s, line8);
    out += "\r\n";
    sendAll(s.ctrl, out);
}

static void reply(Session& s, const char* fmt, ...)
{
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    replyRaw(s, std::string(buf));
}

// ---------------------------------------------------------------------------
// path resolution
// ---------------------------------------------------------------------------
static bool driveExists(const std::string& d)
{
    if (d.size() < 2 || d[1] != ':') return false;
    char c = (char)toupper((unsigned char)d[0]);
    if (c < 'A' || c > 'Z') return false;
    DWORD m = GetLogicalDrives();
    return (m & (1u << (c - 'A'))) != 0;
}

static std::vector<std::string> listDrives()
{
    std::vector<std::string> v;
    DWORD m = GetLogicalDrives();
    for (int i = 0; i < 26; ++i) {
        if (m & (1u << i)) {
            char d[3] = { (char)('A' + i), ':', 0 };
            std::wstring w = L"A:\\";
            w[0] = (wchar_t)('A' + i);
            UINT t = GetDriveTypeW(w.c_str());
            if (t == DRIVE_FIXED || t == DRIVE_REMOVABLE || t == DRIVE_REMOTE ||
                t == DRIVE_RAMDISK) {
                v.push_back(std::string(d));
            }
        }
    }
    return v;
}

struct Resolved {
    bool                     ok       = false;
    bool                     virtRoot = false;
    std::vector<std::string> toks;      // internal UTF-8 tokens, [0] may be "C:"
    std::wstring             phys;      // physical Windows path (empty when virtRoot)
    std::string              err;
};

static void splitTokens(const std::string& in, std::vector<std::string>& toks)
{
    size_t i = 0;
    while (i < in.size()) {
        if (in[i] == '/' || in[i] == '\\') { ++i; continue; }
        size_t j = i;
        while (j < in.size() && in[j] != '/' && in[j] != '\\') ++j;
        std::string t = in.substr(i, j - i);
        i = j;
        if (t.empty() || t == ".") continue;
        if (t == "..") { if (!toks.empty()) toks.pop_back(); continue; }
        toks.push_back(t);
    }
}

static std::string virtualOf(const std::vector<std::string>& toks)
{
    if (toks.empty()) return "/";
    std::string r;
    for (size_t i = 0; i < toks.size(); ++i) { r += "/"; r += toks[i]; }
    return r;
}

// Windows file APIs stop at 260 characters unless the path carries the \\?\ prefix.
// Every path this server hands to Win32 is absolute, so adding the prefix here is
// safe (and it also passes names with trailing dots/spaces through unchanged).
static const wchar_t kBs = (wchar_t)92;                 // backslash
static const std::wstring kLongPrefix = std::wstring(2, kBs) + L"?" + kBs;        // two Bs + ? + B
static const std::wstring kLongUnc    = kLongPrefix + L"UNC" + kBs;              // ... + UNC + B

// Windows file APIs stop at 260 characters unless the path carries the long-path
// prefix (two backslashes, a question mark, a backslash).  Every path this
// server hands to Win32 is absolute, so adding it here is safe - and it also lets
// names with trailing dots or spaces through unchanged.  Spelled with (wchar_t)92
// on purpose: no escape sequences to get wrong.
static std::wstring longPath(const std::wstring& abs)
{
    if (abs.size() >= 4 && abs[0] == kBs && abs[1] == kBs && abs[2] == '?' && abs[3] == kBs)
        return abs;                                       // already prefixed
    if (abs.size() >= 2 && abs[1] == ':')
        return kLongPrefix + abs;                         // C:... -> prefixed C:...
    if (abs.size() >= 2 && abs[0] == kBs && abs[1] == kBs)
        return kLongUnc + abs.substr(2);                  // UNC share -> prefixed UNC
    return abs;                                           // relative: leave it alone
}
// Short, human readable hint for the GetLastError() values that show up here.
static const char* winErrHint(DWORD e)
{
    switch (e) {
    case 2:   return "file not found";
    case 3:   return "a folder in the path is missing, or the path is too long";
    case 5:   return "access denied";
    case 32:  return "sharing violation - another program is holding the file";
    case 123: return "invalid name";
    case 206: return "filename or extension too long";
    default:  return "";
    }
}

static Resolved resolvePath(Session& s, const std::string& rawInternal)
{
    Resolved r;
    std::string p = rawInternal;
    if (p.empty()) p = s.cwd;
    else if (p[0] != '/' && p[0] != '\\') {
        if (s.cwd == "/") p = "/" + p;
        else p = s.cwd + "/" + p;
    }

    splitTokens(p, r.toks);

    if (g_cfg.allDrives) {
        if (r.toks.empty()) { r.virtRoot = true; r.ok = true; return r; }

        std::string d0 = r.toks[0];
        bool looksDrive = false;
        if (d0.size() == 2 && d0[1] == ':' && isalpha((unsigned char)d0[0])) looksDrive = true;
        else if (d0.size() == 1 && isalpha((unsigned char)d0[0])) looksDrive = true;

        if (!looksDrive) { r.err = "no such directory"; return r; }

        std::string drive = std::string(1, (char)toupper((unsigned char)d0[0])) + ":";
        if (!driveExists(drive)) { r.err = "drive not available"; return r; }
        r.toks[0] = drive;

        std::wstring w;
        w += (wchar_t)drive[0];
        w += L":";
        for (size_t i = 1; i < r.toks.size(); ++i) { w += L"\\"; w += u8ToW(r.toks[i]); }
        if (w.size() == 2) w += L"\\";   // "C:" -> "C:\"
        r.phys = longPath(w);
        r.ok = true;
        return r;
    }

    // single directory mode
    std::wstring w = g_cfg.rootDir;
    while (!w.empty() && (w.back() == L'\\' || w.back() == L'/')) w.pop_back();
    for (size_t i = 0; i < r.toks.size(); ++i) { w += L"\\"; w += u8ToW(r.toks[i]); }
    r.phys = longPath(w);
    r.ok = true;
    return r;
}

// ---------------------------------------------------------------------------
// listing
// ---------------------------------------------------------------------------
static void fileTimeStr(const FILETIME& ft, char* out, size_t outsz)
{
    static const char* mon[] = { "Jan","Feb","Mar","Apr","May","Jun",
                                 "Jul","Aug","Sep","Oct","Nov","Dec" };
    FILETIME localFt;
    FileTimeToLocalFileTime(&ft, &localFt);
    SYSTEMTIME st;
    FileTimeToSystemTime(&localFt, &st);

    ULARGE_INTEGER a, b;
    a.LowPart = ft.dwLowDateTime;
    a.HighPart = ft.dwHighDateTime;
    FILETIME nowFt;
    GetSystemTimeAsFileTime(&nowFt);
    b.LowPart = nowFt.dwLowDateTime;
    b.HighPart = nowFt.dwHighDateTime;

    bool recent = false;
    if (a.QuadPart <= b.QuadPart) {
        unsigned long long diffSec = (b.QuadPart - a.QuadPart) / 10000000ULL;
        recent = (diffSec < 180ULL * 24ULL * 3600ULL);
    }
    int mi = (st.wMonth >= 1 && st.wMonth <= 12) ? (st.wMonth - 1) : 0;
    if (recent)
        snprintf(out, outsz, "%s %2d %02d:%02d", mon[mi], st.wDay, st.wHour, st.wMinute);
    else
        snprintf(out, outsz, "%s %2d  %4d", mon[mi], st.wDay, st.wYear);
}

static void listLine(const std::string& name8, bool isDir, unsigned long long size,
                     const FILETIME& ft, std::string& out)
{
    char ts[32] = {0};
    fileTimeStr(ft, ts, sizeof(ts));
    char buf[1024];
    snprintf(buf, sizeof(buf), "%s   1 ftp      ftp   %12llu %s %s\r\n",
             isDir ? "drwxrwxrwx" : "-rw-rw-rw-",
             (unsigned long long)(isDir ? 0 : size), ts, name8.c_str());
    out += buf;
}

static bool buildListing(const std::wstring& phys, const std::string& physU8Name,
                         bool namesOnly, std::string& out, std::string& err)
{
    DWORD attr = GetFileAttributesW(phys.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES) { err = "not found"; return false; }

    if (!(attr & FILE_ATTRIBUTE_DIRECTORY)) {
        WIN32_FILE_ATTRIBUTE_DATA fad{};
        if (!GetFileAttributesExW(phys.c_str(), GetFileExInfoStandard, &fad)) {
            err = "stat failed";
            return false;
        }
        unsigned long long sz = ((unsigned long long)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;
        if (namesOnly) out += physU8Name + "\r\n";
        else listLine(physU8Name, false, sz, fad.ftLastWriteTime, out);
        return true;
    }

    std::wstring pat = phys;
    if (!pat.empty() && pat.back() != L'\\') pat += L"\\";
    pat += L"*";

    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW(pat.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) { err = "cannot open directory"; return false; }
    do {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
        bool isDir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        std::string n8 = wToU8(fd.cFileName);
        if (namesOnly) {
            out += n8 + "\r\n";
        } else {
            unsigned long long sz = ((unsigned long long)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
            listLine(n8, isDir, sz, fd.ftLastWriteTime, out);
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return true;
}

// ---------------------------------------------------------------------------
// data connection
// ---------------------------------------------------------------------------
static std::string localAddrOf(Session& s)
{
    sockaddr_in a{};
    int l = sizeof(a);
    if (getsockname(s.ctrl, (sockaddr*)&a, &l) == 0 && a.sin_addr.s_addr != 0) {
        std::string ip = ipv4ToStr(a.sin_addr);
        if (ip != "0.0.0.0") return ip;
    }
    return g_primaryIp;
}

// ---------------------------------------------------------------------------
// TCP keep-alive.  A multi-hour transfer leaves the control connection idle,
// and cheap routers / NAT boxes happily drop an idle connection after 5-30 min
// - which is exactly how "big file stops halfway" happens.  Probing every
// keepAliveSec keeps the NAT mapping alive and also spots dead peers early.
// ---------------------------------------------------------------------------
struct FtpKeepAlive {
    u_long onoff;
    u_long keepalivetime;      // ms idle before the first probe
    u_long keepaliveinterval;  // ms between probes
};
#ifndef SIO_KEEPALIVE_VALS
#define SIO_KEEPALIVE_VALS _WSAIOW(IOC_VENDOR, 4)
#endif

static void applyKeepAlive(SOCKET sk, int idleSec)
{
    BOOL on = TRUE;
    setsockopt(sk, SOL_SOCKET, SO_KEEPALIVE, (const char*)&on, sizeof(on));
    if (idleSec <= 0) return;

    FtpKeepAlive ka;
    ka.onoff             = 1;
    ka.keepalivetime     = (u_long)idleSec * 1000u;
    ka.keepaliveinterval = 5000u;
    DWORD ret = 0;
    WSAIoctl(sk, SIO_KEEPALIVE_VALS, &ka, sizeof(ka), nullptr, 0, &ret, nullptr, nullptr);
}

// ---------------------------------------------------------------------------
// TCP_NODELAY.  FTP is a request/response protocol on the control channel, and
// clients that pipeline commands (MT's file manager fires several commands per
// burst) otherwise sit behind Nagle + delayed ACK for up to ~200 ms per reply.
// Nagle also hurts the data channel: it merges tail packets of a transfer.
// ---------------------------------------------------------------------------
static void applyNoDelay(SOCKET sk)
{
    BOOL on = TRUE;
    setsockopt(sk, IPPROTO_TCP, TCP_NODELAY, (const char*)&on, sizeof(on));
}

// ---------------------------------------------------------------------------
// ONE passive listener per session, reused for every file in the batch.
// Creating a fresh listening socket per transfer burns two ephemeral ports each
// time (the listener + the connection, both parked in TIME_WAIT) - that is what
// makes a "thousands of files" job die in the middle with 425/426.
// ---------------------------------------------------------------------------
static bool ensurePassive(Session& s, unsigned short& port, std::string& err)
{
    if (s.pasv != INVALID_SOCKET) {
        sockaddr_in a{};
        int al = sizeof(a);
        if (getsockname(s.pasv, (sockaddr*)&a, &al) == 0) {
            port = ntohs(a.sin_port);          // same port as last time
            return true;
        }
        closesocket(s.pasv);
        s.pasv = INVALID_SOCKET;
    }

    SOCKET l = socket(AF_INET, SOCK_STREAM, 0);
    if (l == INVALID_SOCKET) { err = "socket() failed"; return false; }

    // NOTE: deliberately NO SO_REUSEADDR here.  With a fixed port range we would
    // rather fail and try the next port than let two sessions hijack one port.
    sockaddr_in la{};
    la.sin_family = AF_INET;
    inet_pton(AF_INET, localAddrOf(s).c_str(), &la.sin_addr);

    bool bound = false;
    static unsigned seed = (unsigned)GetTickCount();
    int span = g_cfg.pasvHigh - g_cfg.pasvLow + 1;
    if (span < 1) span = 1;
    for (int attempt = 0; attempt < 24 && !bound; ++attempt) {
        int tryPort = 0;                       // 0 = let the OS pick anything free
        if (attempt < 20) {                    // first try the configured data range
            seed = seed * 1103515245u + 12345u;
            tryPort = g_cfg.pasvLow + (int)((seed >> 16) % (unsigned)span);
        }
        la.sin_port = htons((unsigned short)tryPort);
        if (bind(l, (sockaddr*)&la, sizeof(la)) == 0) bound = true;
    }
    if (!bound) {
        closesocket(l);
        err = "cannot bind data port";
        return false;
    }
    if (listen(l, 8) != 0) {                   // deeper backlog: clients pipeline transfers
        closesocket(l);
        err = "cannot listen on data port";
        return false;
    }

    int llen = sizeof(la);
    if (getsockname(l, (sockaddr*)&la, &llen) != 0) {
        closesocket(l);
        err = "getsockname failed";
        return false;
    }
    port = ntohs(la.sin_port);
    s.pasv = l;
    return true;
}

static SOCKET tuneDataSocket(SOCKET d)
{
    // Data channels get their own (much shorter) silence timeout: a control
    // connection may sit idle for minutes, but a transfer that moves no bytes
    // for a minute means the peer is gone.  Without this a stalled upload kept
    // its thread (and the client's task) hanging until the 900 s control limit.
    int ds = (g_cfg.dataIdleSec > 0) ? g_cfg.dataIdleSec : (g_cfg.idleSec > 0 ? g_cfg.idleSec : 60);
    int tv = ds * 1000;
    setsockopt(d, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
    setsockopt(d, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tv, sizeof(tv));

    // 1 MB socket buffers: on a 1 Gbps LAN the 64 KB default throttles big files
    int bufsz = 1 << 20;
    setsockopt(d, SOL_SOCKET, SO_SNDBUF, (const char*)&bufsz, sizeof(bufsz));
    setsockopt(d, SOL_SOCKET, SO_RCVBUF, (const char*)&bufsz, sizeof(bufsz));

    applyKeepAlive(d, g_cfg.keepAliveSec);
    applyNoDelay(d);
    return d;
}

static SOCKET acceptWithTimeout(SOCKET l, int sec)
{
    fd_set rf;
    FD_ZERO(&rf);
    FD_SET(l, &rf);
    timeval tv;
    tv.tv_sec = sec;
    tv.tv_usec = 0;
    int r = select(0, &rf, nullptr, nullptr, &tv);
    if (r <= 0) return INVALID_SOCKET;
    return accept(l, nullptr, nullptr);
}

// A queued passive connection can be a leftover from a transfer the client
// cancelled (it connected, then gave up).  Writing into such a socket dies with
// 10054 after a few MB, so detect it before we start.
static bool isDeadDataConn(SOCKET d)
{
    fd_set rf;
    FD_ZERO(&rf);
    FD_SET(d, &rf);
    timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = 0;
    int r = select(0, &rf, nullptr, nullptr, &tv);
    if (r <= 0) return false;              // nothing pending: a live, quiet connection

    char c = 0;
    int n = recv(d, &c, 1, MSG_PEEK);
    if (n == 0) return true;               // peer already closed (FIN queued)
    if (n < 0) {
        int e = WSAGetLastError();
        return (e != WSAEWOULDBLOCK && e != WSAETIMEDOUT);
    }
    return false;                          // real data pending (upload): keep it
}

// A multi-threaded reader (MT's file manager, most download managers that use
// range requests) opens a data connection, reads one slice, closes it and comes
// back for the next slice.  It closes while our send buffer still holds data,
// so the peer's stack answers with RST and we see WSAECONNRESET - even though
// nothing is broken.  Such a client keeps the CONTROL connection open and
// waiting; a real network drop normally takes both channels down.
// Used to answer 226 instead of 426 for that deliberate case: clients like MT
// treat 426 as a hard failure and then retry the same slice again and again.
static bool peerClosedDataOnPurpose(Session& s, bool ok, int err)
{
    if (ok) return false;
    if (err != WSAECONNRESET && err != WSAECONNABORTED) return false;

    fd_set rd;
    FD_ZERO(&rd);
    FD_SET(s.ctrl, &rd);
    timeval tv;
    tv.tv_sec  = 0;
    tv.tv_usec = 0;
    int n = select(0, &rd, nullptr, nullptr, &tv);
    if (n <= 0) return true;              // nothing pending: control socket alive

    char c = 0;
    int k = recv(s.ctrl, &c, 1, MSG_PEEK);
    return k > 0;                         // 0 = peer closed the control too
}

static SOCKET dataConnect(Session& s, std::string& err)
{
    if (s.pasv != INVALID_SOCKET) {
        SOCKET d = acceptWithTimeout(s.pasv, 60);
        if (d == INVALID_SOCKET) { err = "passive accept failed or timed out"; return INVALID_SOCKET; }

        for (int guard = 0; guard < 8; ++guard) {
            if (!isDeadDataConn(d)) break;
            logf("dropped a stale data connection from %s", s.peerIp.c_str());
            closesocket(d);
            d = acceptWithTimeout(s.pasv, 30);
            if (d == INVALID_SOCKET) {
                err = "only stale data connections were queued";
                return INVALID_SOCKET;
            }
        }
        return tuneDataSocket(d);   // NOTE: the listener stays open for the next file
    }
    if (s.hasAct) {
        SOCKET d = socket(AF_INET, SOCK_STREAM, 0);
        if (d == INVALID_SOCKET) { err = "socket() failed"; return INVALID_SOCKET; }

        sockaddr_in la{};
        la.sin_family = AF_INET;
        la.sin_addr.s_addr = INADDR_ANY;
        la.sin_port = 0;
        bind(d, (sockaddr*)&la, sizeof(la));

        if (connect(d, (sockaddr*)&s.act, sizeof(s.act)) != 0) {
            closesocket(d);
            err = "active connect failed";
            return INVALID_SOCKET;
        }
        return tuneDataSocket(d);
    }
    err = "use PASV or PORT first";
    return INVALID_SOCKET;
}

// ---------------------------------------------------------------------------
// command implementations
// ---------------------------------------------------------------------------
static void needLogin(Session& s) { reply(s, "530 Please login with USER and PASS."); }

// "LIST -la /pub" -> "/pub";  "LIST -la" -> ""   (list options are ignored)
static std::string stripListOptions(const std::string& arg)
{
    std::string a = arg;
    while (!a.empty() && (a.front() == ' ' || a.front() == '\t')) a.erase(a.begin());
    while (!a.empty() && (a.back() == ' ' || a.back() == '\t')) a.pop_back();
    if (a.empty() || a[0] != '-') return a;

    size_t i = 0;
    while (i <= a.size()) {
        size_t j = a.find(' ', i);
        std::string w = (j == std::string::npos) ? a.substr(i) : a.substr(i, j - i);
        if (!w.empty() && w[0] != '-') return a.substr(i);
        if (j == std::string::npos) break;
        i = j + 1;
    }
    return std::string();
}

static void cmdList(Session& s, const std::string& argInternal, bool namesOnly)
{
    std::string argUse = stripListOptions(argInternal);
    Resolved r = resolvePath(s, argUse);
    if (!r.ok) { reply(s, "550 %s.", r.err.c_str()); return; }

    std::string content;
    std::string err;

    if (r.virtRoot) {
        std::vector<std::string> dv = listDrives();
        for (size_t i = 0; i < dv.size(); ++i) {
            if (namesOnly) content += dv[i] + "\r\n";
            else content += "drwxrwxrwx   1 ftp      ftp              0 Jan  1 00:00 " + dv[i] + "\r\n";
        }
    } else {
        std::string physName = r.toks.empty() ? std::string("") : r.toks.back();
        if (physName.empty()) physName = "\\";
        if (!buildListing(r.phys, physName, namesOnly, content, err)) {
            reply(s, "550 %s.", err.c_str());
            return;
        }
    }

    reply(s, "150 Opening data connection.");
    std::string derr;
    SOCKET d = dataConnect(s, derr);
    if (d == INVALID_SOCKET) {
        logf("LIST data connection failed: %s (%s)", derr.c_str(), s.peerIp.c_str());
        reply(s, "425 %s.", derr.c_str());
        return;
    }

    bool ok = sendAll(d, content);
    closesocket(d);
    if (ok) {
        reply(s, "226 Transfer complete.");
    } else if (peerClosedDataOnPurpose(s, ok, g_lastSockErr)) {
        // the client read the part of the listing it wanted and closed the data
        // connection itself - not a server-side failure (see FAQ 9)
        reply(s, "226 Transfer complete.");
        logf("LIST: client closed the data connection itself (%u bytes queued) - replied 226 (%s)",
             (unsigned)content.size(), s.peerIp.c_str());
    } else {
        reply(s, "426 Connection closed; transfer aborted.");
        logf("LIST failed after %u bytes (socket error %d) (%s)",
             (unsigned)content.size(), g_lastSockErr, s.peerIp.c_str());
    }
}

// ---------------------------------------------------------------------------
// concurrent upload registry.  A multi-threaded client (MT's file manager, most
// download managers) can split ONE file into segments and push each segment on
// its own connection, every one of them preceded by REST <offset>.  The first
// connection to arrive creates the file; any later connection to the SAME path
// must not truncate it, or it would wipe what its siblings already wrote.
// ---------------------------------------------------------------------------
static std::mutex                g_upMx;
static std::vector<std::wstring> g_upPaths;

static std::wstring upKey(const std::wstring& phys)
{
    std::wstring k = phys;                 // Windows paths are case-insensitive
    for (size_t i = 0; i < k.size(); ++i) k[i] = (wchar_t)towupper(k[i]);
    return k;
}

// true  = this session is the first writer of that file (it may create/truncate)
static bool uploadBegin(const std::wstring& phys)
{
    std::wstring k = upKey(phys);
    std::lock_guard<std::mutex> lk(g_upMx);
    bool first = true;
    for (size_t i = 0; i < g_upPaths.size(); ++i)
        if (g_upPaths[i] == k) { first = false; break; }
    g_upPaths.push_back(k);
    return first;
}

static void uploadEnd(const std::wstring& phys)
{
    std::wstring k = upKey(phys);
    std::lock_guard<std::mutex> lk(g_upMx);
    for (size_t i = 0; i < g_upPaths.size(); ++i)
        if (g_upPaths[i] == k) { g_upPaths.erase(g_upPaths.begin() + (long)i); break; }
}

static bool isReadonlySession(const Session& s)
{
    return g_cfg.readonly || s.anonUser;
}

// A client that reads the head/middle/tail of a file to verify it (MT does that
// after every upload) closes the data connection itself once it has what it
// wants.  That is not a failure, so do not report it with the alarming
// "aborted (socket error 10054)" wording.
#define EARLY_CLOSE_MAX_BYTES (8ULL << 20)

static bool isClientEarlyClose(bool ok, unsigned long long moved, int err)
{
    if (ok) return false;
    if (err != WSAECONNABORTED && err != WSAECONNRESET) return false;
    return moved <= EARLY_CLOSE_MAX_BYTES;
}


// Clients that open a dozen transfers at once starve themselves: the slowest
// ones time out and all the user sees is "transfer failed".  Say so in the log,
// so the next report is obvious at a glance.
static void logIfOverParallel()
{
    int live = xferLiveCount();
    if (live >= 8)
        logf("  note: %d transfers were in flight at that moment - a client that opens "
             "this many at once can starve itself and drop the slowest ones "
             "(try fewer threads or smaller batches)",
             live);
}

static void cmdRetr(Session& s, const std::string& argInternal)
{
    Resolved r = resolvePath(s, argInternal);
    if (!r.ok || r.virtRoot) {
        logf("RETR %s: bad path (%s) (%s)", argInternal.c_str(),
             r.err.empty() ? "not found" : r.err.c_str(), s.peerIp.c_str());
        reply(s, "550 File not found.");
        return;
    }

    HANDLE h = CreateFileW(r.phys.c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD e = GetLastError();
        logf("RETR %s: cannot open (Win32 error %lu: %s)%s (%s)",
             wToU8(r.phys).c_str(), (unsigned long)e, winErrHint(e),
             r.phys.size() > 255 ? " [path is longer than 259 characters]" : "",
             s.peerIp.c_str());
        reply(s, "550 File not found or access denied.");
        return;
    }

    DWORD attr = GetFileAttributesW(r.phys.c_str());
    if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY)) {
        CloseHandle(h);
        reply(s, "550 Not a regular file.");
        return;
    }

    if (s.rest > 0) {
        LARGE_INTEGER li;
        li.QuadPart = s.rest;
        SetFilePointerEx(h, li, nullptr, FILE_BEGIN);
    }

    unsigned long long fileSize = 0;
    LARGE_INTEGER fsz;
    if (GetFileSizeEx(h, &fsz)) fileSize = (unsigned long long)fsz.QuadPart;
    unsigned long long offset = (s.rest > 0) ? (unsigned long long)s.rest : 0;
    s.rest = 0;

    std::string leaf = r.toks.empty() ? std::string("?") : r.toks.back();
    std::string rnote;
    if (offset > 0) {              // the client sent REST <n> first: a resume or a segment
        char rb[64];
        snprintf(rb, sizeof(rb), " resume=%llu", (unsigned long long)offset);
        rnote = rb;
    }
    logf("RETR %s%s (%s)", leaf.c_str(), rnote.c_str(), s.peerIp.c_str());

    reply(s, "150 Opening binary mode data connection.");
    std::string derr;
    SOCKET d = dataConnect(s, derr);
    if (d == INVALID_SOCKET) {
        CloseHandle(h);
        logf("RETR %s data connection failed: %s (%s)", leaf.c_str(), derr.c_str(), s.peerIp.c_str());
        reply(s, "425 %s.", derr.c_str());
        return;
    }

    unsigned long long left = (fileSize > offset) ? (fileSize - offset) : 0;
    int pslot = progBegin(false, "RETR " + leaf, left);
    int xh = xferBegin();          // counted globally, for the frontend totals

    std::vector<char> buf(blockBytes());    // --block KB: fewer syscalls / disk seeks
    bool ok = true;
    unsigned long long moved = 0;
    for (;;) {
        DWORD got = 0;
        if (!ReadFile(h, buf.data(), (DWORD)buf.size(), &got, nullptr)) { ok = false; break; }
        if (got == 0) break;
        if (!sendAll(d, buf.data(), (size_t)got)) { ok = false; break; }
        moved += (unsigned long long)got;
        progTick(pslot, moved);
        xferSet(xh, moved, (double)GetTickCount64());
    }
    CloseHandle(h);
    closesocket(d);
    xferEnd(xh);
    bool early = isClientEarlyClose(ok, moved, g_lastSockErr);
    bool deliberate = peerClosedDataOnPurpose(s, ok, g_lastSockErr);
    progEnd(pslot, moved, ok || early);       // no scary [aborted] for a client-side early close
    if (ok) reply(s, "226 Transfer complete.");
    else if (deliberate) {
        // The client closed its own data connection after the slice it wanted.
        // Nothing is broken (the control connection is still up and waiting),
        // and answering 426 would make multi-threaded clients report a hard
        // failure and retry the same slice forever.
        reply(s, "226 Transfer complete.");
        logf("RETR %s resume=%llu: %.2f MB, client closed the data connection itself "
             "(slice read) - replied 226",
             leaf.c_str(), offset, (double)moved / 1048576.0);
    }
    else {
        reply(s, "426 Connection closed; transfer aborted.");
        if (early)
            logf("RETR %s: %.2f MB, client closed early (verify/thumbnail read - not an error)",
                 leaf.c_str(), (double)moved / 1048576.0);
        else {
            logf("RETR %s aborted after %llu bytes (socket error %d)",
                 leaf.c_str(), (unsigned long long)moved, g_lastSockErr);
            logIfOverParallel();
            ++s.xferFails;
            if (s.xferFails >= 3 && !s.modeHintLogged) {
                s.modeHintLogged = true;
                logf("hint: %d transfers on this connection have failed (mode: %s). Things worth "
                     "checking: (1) switch the client to ACTIVE (PORT) if it uses passive mode, or let "
                     "TCP %d-%d through the firewall / router; (2) a slow target disk - a spinning disk "
                     "with many parallel transfers stalls exactly like this; (3) fewer threads / smaller "
                     "batches in the client",
                     s.xferFails, s.dataMode == 1 ? "active" : "passive",
                     g_cfg.pasvLow, g_cfg.pasvHigh);
            }
        }
    }
}

static void cmdStor(Session& s, const std::string& argInternal, bool append)
{
    if (isReadonlySession(s)) { reply(s, "550 Server is read-only."); return; }

    Resolved r = resolvePath(s, argInternal);
    if (!r.ok || r.virtRoot) { reply(s, "550 Invalid path."); return; }

    DWORD attr = GetFileAttributesW(r.phys.c_str());
    if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY)) {
        reply(s, "550 That is a directory.");
        return;
    }

    // A parallel segment of the same file must not truncate what its siblings
    // already wrote: only the first writer of that path may create/truncate.
    bool firstOpen = uploadBegin(r.phys);
    DWORD disp = append ? OPEN_ALWAYS
                        : ((s.rest > 0 || !firstOpen) ? OPEN_ALWAYS : CREATE_ALWAYS);
    HANDLE h = CreateFileW(r.phys.c_str(), GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, disp, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD e = GetLastError();
        logf("STOR %s: cannot create (Win32 error %lu: %s)%s (%s)",
             wToU8(r.phys).c_str(), (unsigned long)e, winErrHint(e),
             r.phys.size() > 255 ? " [path is longer than 259 characters]" : "",
             s.peerIp.c_str());
        uploadEnd(r.phys);
        reply(s, "550 Cannot create file (access denied?).");
        return;
    }

    LARGE_INTEGER li;
    if (append) {
        li.QuadPart = 0;
        SetFilePointerEx(h, li, nullptr, FILE_END);
    } else if (s.rest > 0) {
        li.QuadPart = s.rest;
        SetFilePointerEx(h, li, nullptr, FILE_BEGIN);
    }

    std::string leaf = r.toks.empty() ? std::string("?") : r.toks.back();
    unsigned long long offset = (s.rest > 0) ? (unsigned long long)s.rest : 0;
    s.rest = 0;

    std::string snotec;
    if (offset > 0) {              // REST <n> before STOR/APPE: a resumed upload
        char rb[64];
        snprintf(rb, sizeof(rb), " resume=%llu", (unsigned long long)offset);
        snotec = rb;
    } else if (!firstOpen) {
        snotec = " (parallel segment, no truncate)";
    }

    logf("%s %s%s (%s)", append ? "APPE" : "STOR", leaf.c_str(), snotec.c_str(), s.peerIp.c_str());

    reply(s, "150 Ok to send data.");
    std::string derr;
    SOCKET d = dataConnect(s, derr);
    if (d == INVALID_SOCKET) {
        CloseHandle(h);
        uploadEnd(r.phys);
        logf("STOR %s data connection failed: %s (%s)", leaf.c_str(), derr.c_str(), s.peerIp.c_str());
        reply(s, "425 %s.", derr.c_str());
        return;
    }

    // uploads usually do not announce a size; use the ALLO hint when the client sent one,
    //  otherwise the live line shows bytes + speed only.
    unsigned long long total = 0;
    if (!append && s.alloTotal > offset) total = s.alloTotal - offset;
    const ULONGLONG t0 = GetTickCount64();   // transfer start, for the abort report
    int pslot = progBegin(true, (append ? "APPE " : "STOR ") + leaf, total);
    int xh = xferBegin();          // counted globally, for the frontend totals

    std::vector<char> buf(blockBytes());    // --block KB: fewer syscalls / disk seeks
    bool ok = true;
    unsigned long long moved = 0;
    for (;;) {
        // Gather one whole block before touching the disk.  A multi-threaded
        // client sends in many small pieces; writing each piece on its own
        // makes a spinning disk seek between files on every single write.
        // Waiting a few milliseconds for the buffer to fill turns that into
        // one larger sequential write.  Nothing is held back longer than the
        // short wait below, so progress and timeouts are unaffected.
        size_t have = 0;
        for (;;) {
            int got = recv(d, buf.data() + have, (int)(buf.size() - have), 0);
            if (got <= 0) {
                if (got < 0) { ok = false; g_lastSockErr = WSAGetLastError(); }
                break;
            }
            have += (size_t)got;
            if (have >= buf.size()) break;        // block full
            if (buf.size() - have < 4096) break;  // no room for a useful read
            if (!waitReadableMs(d, 20)) break;    // socket is dry: write now
        }
        if (!ok) break;
        if (have == 0) break;                     // peer finished, buffer empty
        int got = (int)have;
        DWORD wrote = 0;
        ULONGLONG wt0 = GetTickCount64();
        BOOL wok = WriteFile(h, buf.data(), (DWORD)got, &wrote, nullptr);
        ULONGLONG wms = GetTickCount64() - wt0;
        if (!wok || wrote != (DWORD)got) {
            ok = false;
            break;
        }
        // A 256 KB write should take a millisecond or two.  When several uploads
        // land on a spinning disk at once the head starts seeking between files
        // and each write takes tens of milliseconds - the transfers crawl, the
        // client gives up on the slow ones and the user only sees "failed".
        // Say so once per connection instead of leaving them guessing.
        if (wms >= 100) {
            s.slowMsTotal += wms;
            ++s.slowWrites;
            // one warning per session: eight slow writes in a row (a big file on a
            // busy disk) or a whole second wasted across slow writes (many small
            // files, where no single file ever reaches eight blocks)
            bool warn = (++s.slowBlocks >= 8) || (s.slowMsTotal >= 1000);
            if (warn && !s.slowDiskLogged) {
                s.slowDiskLogged = true;
                logf("slow disk: writing a %d KB block took %llu ms - with %d transfers at once a "
                     "spinning disk behaves like this (an SSD, fewer parallel transfers, or a "
                     "bigger --block helps) (%s)",
                     g_cfg.blockKB, (unsigned long long)wms, xferLiveCount(), s.peerIp.c_str());
            }
        } else if (wms < 20 && s.slowBlocks > 0) {
            s.slowBlocks = 0;            // the disk caught up again
        }
        moved += (unsigned long long)got;
        progTick(pslot, moved);
        xferSet(xh, moved, (double)GetTickCount64());
    }
    xferEnd(xh);
    if (ok && g_cfg.flushOnFinish) FlushFileBuffers(h);   // off by default: a slow disk would make the 226 late
    CloseHandle(h);
    uploadEnd(r.phys);
    closesocket(d);
    progEnd(pslot, moved, ok);
    if (ok) reply(s, "226 Transfer complete.");
    else {
        reply(s, "426 Connection closed; transfer aborted.");
        // Everything needed to tell "the client gave up on a slow transfer" apart
        // from "the client simply stopped sending": how long it took, the average
        // speed, how many transfers were running, and how much of that time this
        // session lost to slow disk writes.
        unsigned long long el = (GetTickCount64() - t0) / 1000;
        logf("%s %s aborted after %llu bytes in %llu s (%.0f B/s average); %d transfers "
             "were in flight; this session lost %llu ms in %d slow disk writes "
             "(socket error %d); partial file kept - a resume with REST %llu + STOR "
             "will continue it",
             append ? "APPE" : "STOR", leaf.c_str(),
             (unsigned long long)moved, el,
             el ? (double)moved / (double)el : (double)moved,
             xferLiveCount(), s.slowMsTotal, s.slowWrites, g_lastSockErr,
             (unsigned long long)(offset + moved));
        logIfOverParallel();
        // A long, slow transfer whose disk writes were all quick means the client
        // stopped sending - nothing this end can fix that, but saying so saves a
        // round of guessing every single time.
        if (s.slowWrites == 0 && moved > 0 && el >= 10)
            logf(" hint: the client reset this transfer after %llu s (%.0f B/s) and no "
                 "disk write on this side was ever slow - the client stalled or stopped "
                 "sending; fewer parallel uploads usually helps (%s)",
                 el, el ? (double)moved / (double)el : (double)moved, s.peerIp.c_str());
        ++s.xferFails;
        if (s.xferFails >= 3 && !s.modeHintLogged) {
            s.modeHintLogged = true;
            logf("hint: %d transfers on this connection have failed (mode: %s). If the client is "
                 "using passive mode, try switching it to ACTIVE (PORT) - or make sure TCP %d-%d "
                 "is allowed through the firewall / router",
                 s.xferFails, s.dataMode == 1 ? "active" : "passive",
                 g_cfg.pasvLow, g_cfg.pasvHigh);
        }
    }
}

// ---------------------------------------------------------------------------
// command dispatch
// ---------------------------------------------------------------------------
static bool dispatch(Session& s, const std::string& line)
{
    std::string cmd, arg;
    size_t sp = line.find(' ');
    if (sp == std::string::npos) cmd = line;
    else { cmd = line.substr(0, sp); arg = line.substr(sp + 1); }

    std::string ucmd = cmd;
    for (size_t i = 0; i < ucmd.size(); ++i) ucmd[i] = (char)toupper((unsigned char)ucmd[i]);

    // remember the last command, so a later disconnect can be explained
    s.lastCmd = cmd;
    if (!arg.empty()) {
        s.lastCmd += " ";
        s.lastCmd += arg.substr(0, 60);
    }

    std::string iarg = toInternal(s, arg);

    // --- login related (allowed before auth) ---
    if (ucmd == "USER") {
        s.user = arg;
        if (g_cfg.anon && (arg.empty() || arg == "anonymous" || arg == "ftp")) {
            s.anonUser = true;
            s.authed = true;
            reply(s, "230 Anonymous login ok, access restrictions apply.");
            logf("login: anon from %s", s.peerIp.c_str());
            return true;
        }
        reply(s, "331 User name ok, need password.");
        return true;
    }
    if (ucmd == "PASS") {
        if (g_cfg.anon && s.anonUser) { reply(s, "230 Already logged in."); return true; }
        if (s.user == g_cfg.user && arg == g_cfg.pass) {
            s.authed = true;
            s.anonUser = false;
            reply(s, "230 Login successful.");
            logf("login: %s@%s ok", s.user.c_str(), s.peerIp.c_str());
        } else {
            reply(s, "530 Login incorrect.");
            logf("login: %s@%s FAILED", s.user.c_str(), s.peerIp.c_str());
        }
        return true;
    }
    if (ucmd == "QUIT") { reply(s, "221 Goodbye."); return false; }
    if (ucmd == "AUTH") { reply(s, "502 AUTH not supported."); return true; }
    if (ucmd == "FEAT") {
        reply(s, "211-Features:");
        replyRaw(s, " SIZE");
        replyRaw(s, " MDTM");
        replyRaw(s, " REST STREAM");
        replyRaw(s, " UTF8");
        replyRaw(s, " EPSV");
        replyRaw(s, " PASV");
        reply(s, "211 End");
        return true;
    }
    if (ucmd == "SYST") { reply(s, "215 Windows_NT"); return true; }
    if (ucmd == "OPTS") {
        std::string u = arg;
        for (size_t i = 0; i < u.size(); ++i) u[i] = (char)toupper((unsigned char)u[i]);
        if (u.find("UTF8") != std::string::npos) {
            if (u.find("OFF") != std::string::npos) { s.utf8 = false; reply(s, "200 UTF8 disabled."); }
            else { s.utf8 = true; reply(s, "200 UTF8 enabled."); }
            return true;
        }
        reply(s, "200 OK.");
        return true;
    }
    if (ucmd == "NOOP") { reply(s, "200 NOOP ok."); return true; }
    if (ucmd == "CLNT") { reply(s, "200 Noted."); return true; }
    if (ucmd == "HELP") { reply(s, "214 Supported: USER PASS QUIT SYST FEAT OPTS NOOP TYPE PWD CWD CDUP LIST NLST RETR STOR APPE DELE MKD RMD RNFR RNTO SIZE MDTM REST PASV EPSV PORT EPRT"); return true; }

    if (!s.authed) { needLogin(s); return true; }

    // --- navigation ---
    if (ucmd == "PWD" || ucmd == "XPWD") {
        replyRaw(s, std::string("257 \"") + s.cwd + "\" is current directory.");
        return true;
    }
    if (ucmd == "CWD" || ucmd == "XCWD") {
        Resolved r = resolvePath(s, iarg);
        if (!r.ok) { reply(s, "550 %s.", r.err.c_str()); return true; }
        if (r.virtRoot) { s.cwd = "/"; reply(s, "250 Directory changed."); return true; }
        DWORD a = GetFileAttributesW(r.phys.c_str());
        if (a == INVALID_FILE_ATTRIBUTES || !(a & FILE_ATTRIBUTE_DIRECTORY)) {
            reply(s, "550 Not a directory.");
            return true;
        }
        s.cwd = virtualOf(r.toks);
        reply(s, "250 Directory changed.");
        return true;
    }
    if (ucmd == "CDUP" || ucmd == "XCUP") {
        Resolved r = resolvePath(s, std::string(".."));
        if (!r.ok) { reply(s, "550 Cannot go up."); return true; }
        if (r.virtRoot) s.cwd = "/";
        else s.cwd = virtualOf(r.toks);
        reply(s, "250 Directory changed.");
        return true;
    }

    // --- type / mode ---
    if (ucmd == "TYPE") {
        char t = arg.empty() ? 'A' : (char)toupper((unsigned char)arg[0]);
        s.binMode = (t == 'I');
        reply(s, "200 Type set to %c.", t);
        return true;
    }
    if (ucmd == "MODE") { reply(s, "200 Mode S ok."); return true; }
    if (ucmd == "STRU") { reply(s, "200 Structure F ok."); return true; }
    if (ucmd == "ALLO") {
        // some clients announce the upload size here - keep it for the progress bar
        s.alloTotal = strtoull(arg.c_str(), nullptr, 10);
        reply(s, "202 ALLO not needed.");
        return true;
    }
    if (ucmd == "REST") {
        // "REST 1234" (RFC 959) or "REST STREAM" (RFC 3659, means a byte offset
        // in the file - which is exactly what we do).  atoll() gives 0 for a
        // non-numeric argument, so "REST STREAM" and "REST 0" behave the same.
        std::string a = arg;
        while (!a.empty() && (a.front() == ' ' || a.front() == '\t')) a.erase(a.begin());
        s.rest = atoll(a.c_str());
        if (s.rest < 0) s.rest = 0;
        reply(s, "350 Restarting at %lld. Send STORE or RETRIEVE to initiate transfer.",
              s.rest);
        return true;
    }

    // --- data channel setup ---
    if (ucmd == "PASV") {
        unsigned short port = 0;
        std::string err;
        if (!ensurePassive(s, port, err)) { reply(s, "425 %s.", err.c_str()); return true; }
        if (s.dataMode != 0) {
            logf("data mode: passive (PASV) - the client dials back to us on TCP %d-%d (%s)",
                 g_cfg.pasvLow, g_cfg.pasvHigh, s.peerIp.c_str());
            s.dataMode = 0;
        }
        std::string ip = localAddrOf(s);
        int a1 = 0, a2 = 0, a3 = 0, a4 = 0;
        sscanf(ip.c_str(), "%d.%d.%d.%d", &a1, &a2, &a3, &a4);
        reply(s, "227 Entering Passive Mode (%d,%d,%d,%d,%d,%d).",
              a1, a2, a3, a4, port >> 8, port & 0xFF);
        return true;
    }
    if (ucmd == "EPSV") {
        unsigned short port = 0;
        std::string err;
        if (!ensurePassive(s, port, err)) { reply(s, "425 %s.", err.c_str()); return true; }
        if (s.dataMode != 0) {
            logf("data mode: passive (EPSV) - the client dials back to us on TCP %d-%d (%s)",
                 g_cfg.pasvLow, g_cfg.pasvHigh, s.peerIp.c_str());
            s.dataMode = 0;
        }
        reply(s, "229 Entering Extended Passive Mode (|||%u|).", (unsigned)port);
        return true;
    }
    if (ucmd == "PORT") {
        int a1 = 0, a2 = 0, a3 = 0, a4 = 0, p1 = 0, p2 = 0;
        if (sscanf(arg.c_str(), "%d,%d,%d,%d,%d,%d", &a1, &a2, &a3, &a4, &p1, &p2) != 6) {
            reply(s, "501 Bad PORT syntax.");
            return true;
        }
        char ipbuf[32];
        snprintf(ipbuf, sizeof(ipbuf), "%d.%d.%d.%d", a1, a2, a3, a4);
        memset(&s.act, 0, sizeof(s.act));
        s.act.sin_family = AF_INET;
        inet_pton(AF_INET, ipbuf, &s.act.sin_addr);
        s.act.sin_port = htons((unsigned short)((p1 << 8) | p2));
        s.hasAct = true;
        if (s.dataMode != 1) {
            logf("data mode: active (PORT) - we dial back to the client (%s)", s.peerIp.c_str());
            s.dataMode = 1;
        }
        if (s.pasv != INVALID_SOCKET) { closesocket(s.pasv); s.pasv = INVALID_SOCKET; }
        reply(s, "200 PORT command successful.");
        return true;
    }
    if (ucmd == "EPRT") {
        // |1|ip|port|
        std::string a = arg;
        size_t p1 = a.find('|');
        if (p1 == std::string::npos) { reply(s, "501 Bad EPRT syntax."); return true; }
        size_t p2 = a.find('|', p1 + 1);
        size_t p3 = (p2 == std::string::npos) ? std::string::npos : a.find('|', p2 + 1);
        size_t p4 = (p3 == std::string::npos) ? std::string::npos : a.find('|', p3 + 1);
        if (p2 == std::string::npos || p3 == std::string::npos || p4 == std::string::npos) {
            reply(s, "501 Bad EPRT syntax.");
            return true;
        }
        std::string ip = a.substr(p2 + 1, p3 - p2 - 1);
        int port = atoi(a.substr(p3 + 1, p4 - p3 - 1).c_str());
        memset(&s.act, 0, sizeof(s.act));
        s.act.sin_family = AF_INET;
        if (inet_pton(AF_INET, ip.c_str(), &s.act.sin_addr) != 1) {
            reply(s, "501 Bad EPRT address.");
            return true;
        }
        s.act.sin_port = htons((unsigned short)port);
        s.hasAct = true;
        if (s.pasv != INVALID_SOCKET) { closesocket(s.pasv); s.pasv = INVALID_SOCKET; }
        reply(s, "200 EPRT command successful.");
        return true;
    }

    // --- transfers ---
    if (ucmd == "LIST") { cmdList(s, iarg, false); return true; }
    if (ucmd == "NLST") { cmdList(s, iarg, true);  return true; }
    if (ucmd == "RETR") { cmdRetr(s, iarg); return true; }
    if (ucmd == "STOR") { cmdStor(s, iarg, false); return true; }
    if (ucmd == "APPE") { cmdStor(s, iarg, true);  return true; }
    if (ucmd == "ABOR") { reply(s, "226 Abort successful."); return true; }

    // --- file management ---
    if (ucmd == "DELE") {
        if (isReadonlySession(s)) { reply(s, "550 Server is read-only."); return true; }
        Resolved r = resolvePath(s, iarg);
        if (!r.ok || r.virtRoot) { reply(s, "550 Invalid path."); return true; }
        if (DeleteFileW(r.phys.c_str())) {
            logf("DELE %s (%s)", wToU8(r.phys).c_str(), s.peerIp.c_str());
            reply(s, "250 File deleted.");
        }
        else {
            DWORD e = GetLastError();
            logf("DELE %s: failed (Win32 error %lu: %s) (%s)", wToU8(r.phys).c_str(),
                 (unsigned long)e, winErrHint(e), s.peerIp.c_str());
            reply(s, "550 Delete failed.");
        }
        return true;
    }
    if (ucmd == "MKD" || ucmd == "XMKD") {
        if (isReadonlySession(s)) { reply(s, "550 Server is read-only."); return true; }
        Resolved r = resolvePath(s, iarg);
        if (!r.ok || r.virtRoot) { reply(s, "550 Invalid path."); return true; }
        if (CreateDirectoryW(r.phys.c_str(), nullptr)) {
            logf("MKD %s (%s)", wToU8(r.phys).c_str(), s.peerIp.c_str());
            std::string v = r.toks.empty() ? std::string("/") : virtualOf(r.toks);
            replyRaw(s, std::string("257 \"") + v + "\" created.");
        } else {
            logf("MKD %s: failed (Win32 error %lu: %s) (%s)", wToU8(r.phys).c_str(),
                 (unsigned long)GetLastError(), winErrHint(GetLastError()), s.peerIp.c_str());
            reply(s, "550 Create directory failed.");
        }
        return true;
    }
    if (ucmd == "RMD" || ucmd == "XRMD") {
        if (isReadonlySession(s)) { reply(s, "550 Server is read-only."); return true; }
        Resolved r = resolvePath(s, iarg);
        if (!r.ok || r.virtRoot) { reply(s, "550 Invalid path."); return true; }
        if (RemoveDirectoryW(r.phys.c_str())) reply(s, "250 Directory removed.");
        else {
            logf("RMD %s: failed (Win32 error %lu: %s) (%s)", wToU8(r.phys).c_str(),
                 (unsigned long)GetLastError(), winErrHint(GetLastError()), s.peerIp.c_str());
            reply(s, "550 Remove failed (not empty or not a directory).");
        }
        return true;
    }
    if (ucmd == "RNFR") {
        if (isReadonlySession(s)) { reply(s, "550 Server is read-only."); return true; }
        Resolved r = resolvePath(s, iarg);
        if (!r.ok || r.virtRoot) { reply(s, "550 Invalid path."); return true; }
        if (GetFileAttributesW(r.phys.c_str()) == INVALID_FILE_ATTRIBUTES) {
            reply(s, "550 Source not found.");
            return true;
        }
        s.rnfr = iarg;
        reply(s, "350 Ready for destination name.");
        return true;
    }
    if (ucmd == "RNTO") {
        if (isReadonlySession(s)) { reply(s, "550 Server is read-only."); return true; }
        if (s.rnfr.empty()) { reply(s, "503 RNFR required first."); return true; }
        Resolved a = resolvePath(s, s.rnfr);
        Resolved b = resolvePath(s, iarg);
        s.rnfr.clear();
        if (!a.ok || !b.ok || a.virtRoot || b.virtRoot) { reply(s, "550 Invalid path."); return true; }
        if (MoveFileW(a.phys.c_str(), b.phys.c_str())) reply(s, "250 Rename successful.");
        else {
            logf("RNTO %s -> %s: failed (Win32 error %lu: %s) (%s)",
                 wToU8(a.phys).c_str(), wToU8(b.phys).c_str(),
                 (unsigned long)GetLastError(), winErrHint(GetLastError()), s.peerIp.c_str());
            reply(s, "550 Rename failed.");
        }
        return true;
    }

    // --- info ---
    if (ucmd == "SIZE") {
        Resolved r = resolvePath(s, iarg);
        if (!r.ok || r.virtRoot) { reply(s, "550 Invalid path."); return true; }
        WIN32_FILE_ATTRIBUTE_DATA fad{};
        if (!GetFileAttributesExW(r.phys.c_str(), GetFileExInfoStandard, &fad)) {
            DWORD e = GetLastError();
            logf("%s %s: not found (Win32 error %lu: %s)%s (%s)", ucmd.c_str(),
                 wToU8(r.phys).c_str(), (unsigned long)e, winErrHint(e),
                 r.phys.size() > 255 ? " [path is longer than 259 characters]" : "",
                 s.peerIp.c_str());
            reply(s, "550 Not found.");
            return true;
        }
        unsigned long long sz = ((unsigned long long)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;
        reply(s, "213 %llu", sz);
        return true;
    }
    if (ucmd == "MDTM") {
        Resolved r = resolvePath(s, iarg);
        if (!r.ok || r.virtRoot) { reply(s, "550 Invalid path."); return true; }
        WIN32_FILE_ATTRIBUTE_DATA fad{};
        if (!GetFileAttributesExW(r.phys.c_str(), GetFileExInfoStandard, &fad)) {
            DWORD e = GetLastError();
            logf("%s %s: not found (Win32 error %lu: %s)%s (%s)", ucmd.c_str(),
                 wToU8(r.phys).c_str(), (unsigned long)e, winErrHint(e),
                 r.phys.size() > 255 ? " [path is longer than 259 characters]" : "",
                 s.peerIp.c_str());
            reply(s, "550 Not found.");
            return true;
        }
        SYSTEMTIME st;
        FileTimeToSystemTime(&fad.ftLastWriteTime, &st);
        reply(s, "213 %04d%02d%02d%02d%02d%02d",
              st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
        return true;
    }
    if (ucmd == "STAT") {
        reply(s, "211-FTPSrv status");
        reply(s, " Connected from %s", s.peerIp.c_str());
        replyRaw(s, " CWD " + toClient(s, s.cwd));
        reply(s, "211 End");
        return true;
    }

    reply(s, "502 Command not implemented.");
    return true;
}

// ---------------------------------------------------------------------------
// session thread
// ---------------------------------------------------------------------------
static bool recvLine(Session& s, std::string& out)
{
    out.clear();
    char c = 0;
    for (;;) {
        int n = recv(s.ctrl, &c, 1, 0);
        if (n <= 0) {
            if (n < 0 && WSAGetLastError() == WSAETIMEDOUT) s.idleTimeout = true;
            return false;
        }
        if (c == '\n') {
            if (!out.empty() && out.back() == '\r') out.pop_back();
            return true;
        }
        out.push_back(c);
        if (out.size() > 4096) return false;
    }
}

static void sessionWorker(SOCKET ctrl, sockaddr_in peer)
{
    Session s;
    s.ctrl = ctrl;
    s.peer = peer;
    s.peerIp = ipv4ToStr(peer.sin_addr);
    registerSession(ctrl);
    applyNoDelay(ctrl);            // replies must go out immediately

    int tv = g_cfg.idleSec * 1000;
    setsockopt(ctrl, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
    setsockopt(ctrl, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tv, sizeof(tv));
    applyKeepAlive(ctrl, g_cfg.keepAliveSec);   // survive long idle transfers

    logf("connect: %s", s.peerIp.c_str());

    reply(s, "220 FTPSrv ready (%s mode).",
          g_cfg.allDrives ? "all-drives" : "single-root");

    std::string line;
    while (g_run && recvLine(s, line)) {
        if (line.empty()) continue;
        if (!dispatch(s, line)) break;
    }

    if (s.pasv != INVALID_SOCKET) { closesocket(s.pasv); s.pasv = INVALID_SOCKET; }

    // say WHY the session ended - "disconnected" alone is useless when debugging
    if (s.idleTimeout) {
        replyRaw(s, "421 Idle timeout, closing control connection.");
        logf("disconnect: %s (idle timeout %d s, last command: %s)",
             s.peerIp.c_str(), g_cfg.idleSec,
             s.lastCmd.empty() ? "-" : s.lastCmd.c_str());
    } else if (s.lastCmd == "QUIT") {
        logf("disconnect: %s (QUIT)", s.peerIp.c_str());
    } else {
        logf("disconnect: %s (connection lost, last command: %s)",
             s.peerIp.c_str(), s.lastCmd.empty() ? "-" : s.lastCmd.c_str());
    }

    closesocket(ctrl);
    unregisterSession(ctrl);
    g_active.fetch_sub(1);
}

static void collectLocalIps(std::vector<std::string>& out)
{
    char host[256] = {0};
    if (gethostname(host, sizeof(host) - 1) != 0) return;

    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    if (getaddrinfo(host, nullptr, &hints, &res) != 0) return;

    for (addrinfo* p = res; p; p = p->ai_next) {
        sockaddr_in* a = (sockaddr_in*)p->ai_addr;
        if (a->sin_addr.s_addr == 0) continue;
        std::string ip = ipv4ToStr(a->sin_addr);
        if (ip == "127.0.0.1") continue;
        if (std::find(out.begin(), out.end(), ip) == out.end()) out.push_back(ip);
    }
    freeaddrinfo(res);
}

// ---------------------------------------------------------------------------
// run a command line without flashing a console window
// ---------------------------------------------------------------------------
static int runQuiet(const std::string& cmd)
{
    STARTUPINFOA si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::string full = "cmd.exe /c " + cmd;
    if (!CreateProcessA(nullptr, (LPSTR)full.c_str(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
        return -1;
    WaitForSingleObject(pi.hProcess, 30000);
    DWORD rc = 1;
    GetExitCodeProcess(pi.hProcess, &rc);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return (int)rc;
}

// ---------------------------------------------------------------------------
// Windows firewall helper (needs an elevated process)
// ---------------------------------------------------------------------------
static int addFirewallRule(int port)
{
    char exe[MAX_PATH] = {0};
    GetModuleFileNameA(nullptr, exe, MAX_PATH);

    char del[512], add[512], prog[1024];
    snprintf(del, sizeof(del),
             "netsh advfirewall firewall delete rule name=\"FTPSrv LAN %d\" >nul 2>&1", port);
    snprintf(add, sizeof(add),
             "netsh advfirewall firewall add rule name=\"FTPSrv LAN %d\" "
             "dir=in action=allow protocol=TCP localport=%d profile=any", port, port);
    snprintf(prog, sizeof(prog),
             "netsh advfirewall firewall add rule name=\"FTPSrv program %d\" "
             "dir=in action=allow program=\"%s\" enable=yes profile=any >nul 2>&1", port, exe);

    runQuiet(del);
    int rc = runQuiet(add);
    runQuiet(prog);

    // passive data ports are ephemeral, so also allow one range for tidy setups
// the passive data range comes from the settings, so the rule always matches
    char rng[512];
    snprintf(rng, sizeof(rng),
             "netsh advfirewall firewall add rule name=\"FTPSrv data %d\" "
             "dir=in action=allow protocol=TCP localport=%d-%d profile=any >nul 2>&1",
             port, g_cfg.pasvLow, g_cfg.pasvHigh);
    runQuiet(rng);
    return rc;
}
// ---------------------------------------------------------------------------
// optional IP allow list.  Entries are plain addresses or CIDR blocks, comma /
// space / semicolon separated:  192.168.1.13 , 192.168.1.0/24 , 10.0.0.5
// An empty list means "everybody may connect" (the default).  Loopback and the
// machine's own addresses are always allowed, so "复制地址" and the local
// self test in 诊断.bat keep working even with a strict list.
// ---------------------------------------------------------------------------
static std::vector<unsigned int> g_allowNet;
static std::vector<int>          g_allowBits;
static std::vector<std::string>  g_localIps;

static bool ip4ToU32(const std::string& s, unsigned int& out)
{
    unsigned int a = 0, b = 0, c = 0, d = 0;
    char tail = 0;
    if (sscanf(s.c_str(), "%u.%u.%u.%u%c", &a, &b, &c, &d, &tail) != 4) return false;
    if (a > 255 || b > 255 || c > 255 || d > 255) return false;
    out = (a << 24) | (b << 16) | (c << 8) | d;
    return true;
}

static unsigned int bitsToMask(int bits)
{
    if (bits <= 0)  return 0u;
    if (bits >= 32) return 0xFFFFFFFFu;
    return 0xFFFFFFFFu << (32 - bits);
}

// pure helper: does a plain address match one of the configured networks?
// (kept free of any Windows API so the CIDR maths can be unit tested)
static bool allowMatchNet(unsigned int a)
{
    for (size_t i = 0; i < g_allowNet.size(); ++i)
        if ((a & bitsToMask(g_allowBits[i])) == g_allowNet[i]) return true;
    return false;
}

static void allowParse()
{
    g_allowNet.clear();
    g_allowBits.clear();

    const std::string& s = g_cfg.allowList;
    size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && (s[i] == ' ' || s[i] == ',' || s[i] == ';' || s[i] == '\t')) ++i;
        size_t j = i;
        while (j < s.size() && s[j] != ' ' && s[j] != ',' && s[j] != ';' && s[j] != '\t') ++j;
        std::string tok = s.substr(i, j - i);
        i = j;
        if (tok.empty()) continue;

        int bits = 32;
        std::string ip = tok;
        size_t sl = tok.find('/');
        if (sl != std::string::npos) {
            ip = tok.substr(0, sl);
            bits = atoi(tok.c_str() + sl + 1);
            if (bits < 0)  bits = 0;
            if (bits > 32) bits = 32;
        }
        unsigned int a = 0;
        if (!ip4ToU32(ip, a)) continue;          // ignore junk entries
        g_allowNet.push_back(a & bitsToMask(bits));
        g_allowBits.push_back(bits);
    }

    g_localIps.clear();
    collectLocalIps(g_localIps);                 // our own addresses stay reachable
}

static bool allowCheck(const std::string& ip)
{
    if (g_allowNet.empty()) return true;        // no list -> open door
    if (ip == "127.0.0.1") return true;         // local self test
    for (size_t i = 0; i < g_localIps.size(); ++i)
        if (g_localIps[i] == ip) return true;

    unsigned int a = 0;
    if (!ip4ToU32(ip, a)) return false;
    return allowMatchNet(a);
}

// ---------------------------------------------------------------------------
// start / stop - shared by both frontends
// ---------------------------------------------------------------------------
static void acceptLoop()
{
    while (g_run) {
        sockaddr_in ca{};
        int cl = sizeof(ca);
        SOCKET cs = accept(g_listen, (sockaddr*)&ca, &cl);
        if (cs == INVALID_SOCKET) {
            if (!g_run) break;
            Sleep(50);
            continue;
        }
        std::string peerIp = ipv4ToStr(ca.sin_addr);

        if (g_active.load() >= g_cfg.maxClients) {
            const char* msg = "421 Too many connections, try again later.\r\n";
            send(cs, msg, (int)strlen(msg), 0);
            closesocket(cs);
            logf("rejected (max clients): %s", peerIp.c_str());
            continue;
        }
        if (!allowCheck(peerIp)) {            // optional IP allow list
            const char* msg = "421 Service not available, closing control connection.\r\n";
            send(cs, msg, (int)strlen(msg), 0);
            closesocket(cs);
            logf("rejected (not in allow list): %s", peerIp.c_str());
            continue;
        }
        g_active.fetch_add(1);
        std::thread(sessionWorker, cs, ca).detach();
    }
}

static std::thread g_acceptThread;

// false + err on failure; on success the server is listening and serving
static bool engineStart(unsigned short port, std::string& err)
{
    if (g_listen != INVALID_SOCKET) { err = "already running"; return false; }

    clampPasvRange();          // data-channel port range must be sane before we serve
    allowParse();              // optional IP allow list (empty = everybody)

    static bool wsaDone = false;
    if (!wsaDone) {
        WSADATA wsa{};
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) { err = "WSAStartup failed"; return false; }
        wsaDone = true;
    }

    SOCKET ls = socket(AF_INET, SOCK_STREAM, 0);
    if (ls == INVALID_SOCKET) { err = "socket() failed"; return false; }

    BOOL yes = TRUE;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, (const char*)&yes, sizeof(yes));

    sockaddr_in la{};
    la.sin_family = AF_INET;
    la.sin_addr.s_addr = INADDR_ANY;
    la.sin_port = htons(port);

    if (bind(ls, (sockaddr*)&la, sizeof(la)) != 0) {
        closesocket(ls);
        char b[160];
        snprintf(b, sizeof(b), "cannot bind port %u (already in use, or blocked)", (unsigned)port);
        err = b;
        return false;
    }
    if (listen(ls, 16) != 0) {
        closesocket(ls);
        err = "listen() failed";
        return false;
    }

    std::vector<std::string> ips;
    collectLocalIps(ips);
    if (!ips.empty()) g_primaryIp = ips[0];

    g_listen = ls;
    g_run = true;

    if (g_acceptThread.joinable()) g_acceptThread.join();
    g_acceptThread = std::thread(acceptLoop);
    return true;
}

static void engineStop()
{
    if (g_listen == INVALID_SOCKET) return;

    g_run = false;
    closesocket(g_listen);
    g_listen = INVALID_SOCKET;

    if (g_acceptThread.joinable()) g_acceptThread.join();

    std::vector<SOCKET> socks;
    {
        std::lock_guard<std::mutex> lk(g_sessMx);
        socks = g_sessSocks;
    }
    for (size_t i = 0; i < socks.size(); ++i) shutdown(socks[i], SD_BOTH);

    for (int i = 0; i < 100 && g_active.load() > 0; ++i) Sleep(30);
}
