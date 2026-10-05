// ============================================================================
//  FTPSrv GUI  -  Windows LAN FTP file server, windowed edition
//
//  main window  : status, addresses, start/stop, live speed, log
//  settings page: every knob (share mode, folder, port, account, limits, ...)
//
//  the FTP engine lives in ftpsrv_engine.inc and is shared with the console
//  build (ftpsrv.cpp).
//
//  build (cross, on Linux):
//    x86_64-w64-mingw32-g++ -O2 -std=c++17 -static -mwindows
//      -o ftpsrv-gui.exe ftpsrv_gui.cpp -lws2_32 -lshell32 -lole32 -luuid -lcomdlg32
// ============================================================================

#include "ftpsrv_engine.inc"

#include <shellapi.h>
#include <shlobj.h>
#include <deque>

// ---------------------------------------------------------------------------
// ids / globals
// ---------------------------------------------------------------------------
#define IDC_STATUS        1001
#define IDC_SUMMARY       1002
#define IDC_ADDR          1003
#define IDC_BTN_START     1004
#define IDC_BTN_STOP      1005
#define IDC_BTN_SET       1006
#define IDC_BTN_FW        1007
#define IDC_BTN_COPY      1008
#define IDC_PROG1         1009
#define IDC_PROG2         1010
#define IDC_LOGLIST       1011
#define IDC_BTN_CLEAR     1012
#define IDC_PROG3         1013
#define IDC_PROG4         1014
#define IDC_PROG5         1015
#define IDC_PROG6         1016

#define IDS_MODE_ALL      1101
#define IDS_MODE_DIR      1102
#define IDS_ROOT          1103
#define IDS_BROWSE        1104
#define IDS_USER          1105
#define IDS_PASS          1106
#define IDS_ANON          1107
#define IDS_PORT          1108
#define IDS_MAXCONN       1109
#define IDS_IDLE          1110
#define IDS_IP            1111
#define IDS_READONLY      1112
#define IDS_LOGFILE       1113
#define IDS_AUTOSTART     1114
#define IDS_KEEPALIVE     1115
#define IDS_PASV_LOW      1116
#define IDS_PASV_HIGH     1117
#define IDS_FLUSH         1118
#define IDS_ALLOW         1119
#define IDS_TRAY          1122
#define IDS_BLOCK         1123
#define IDS_OK            1120
#define IDS_CANCEL        1121

static const wchar_t* kMainClass  = L"FTPSrvMainWnd";
static const wchar_t* kSetClass   = L"FTPSrvSettingsWnd";

static HINSTANCE g_hInst = nullptr;
static HWND  g_hMain = nullptr;
static HWND  g_hSet  = nullptr;
static HFONT g_font = nullptr, g_fontBold = nullptr, g_fontMono = nullptr;
static bool  g_running = false;
static bool  g_setOpen = false;
static bool  g_autostartPending = false;
static bool  g_trayIcon = false;      // tray icon currently installed
static bool  g_trayHintShown = false; // balloon tip shown once

#define WM_TRAYMSG (WM_APP + 100)
#define IDM_TRAY_OPEN  1
#define IDM_TRAY_STOP  2
#define IDM_TRAY_QUIT  3

static void trayAdd(HWND h);
static void trayRemove();
static void trayBalloon(const wchar_t* text);

// settings-page control handles
static HWND s_all = nullptr, s_dir = nullptr, s_root = nullptr, s_browse = nullptr;
static HWND s_user = nullptr, s_pass = nullptr, s_anon = nullptr;
static HWND s_port = nullptr, s_maxc = nullptr, s_idle = nullptr, s_ip = nullptr, s_ka = nullptr;
static HWND s_plow = nullptr, s_phigh = nullptr;
static HWND s_ro = nullptr, s_logf = nullptr, s_auto = nullptr, s_flush = nullptr;
static HWND s_allow = nullptr, s_tray = nullptr, s_blk = nullptr;

// main-window control handles
static HWND m_status = nullptr, m_summary = nullptr, m_addr = nullptr;
static HWND m_start = nullptr, m_stop = nullptr, m_set = nullptr, m_fw = nullptr, m_copy = nullptr;
static HWND m_prog1 = nullptr, m_prog2 = nullptr, m_prog3 = nullptr, m_prog4 = nullptr;
static HWND m_prog5 = nullptr, m_prog6 = nullptr;
static HWND m_log = nullptr, m_clear = nullptr;

// ---------------------------------------------------------------------------
// persisted settings
// ---------------------------------------------------------------------------
struct Settings {
    int          mode      = 0;        // 0 = all drives, 1 = single folder
    std::wstring root;
    int          port      = 2121;
    std::string  user      = "admin";
    std::string  pass      = "admin";
    bool         anon      = false;
    bool         readonly  = false;
    bool         logToFile = true;
    bool         autostart = false;
    int          idleSec   = 900;
    int          maxClients = 64;
    int          keepAliveSec = 30;
    int          pasvLow  = 50000;      // data channel port range
    int          pasvHigh = 50100;
    int          blockKB   = 256;       // read/write block (KB); bigger = fewer disk seeks
    bool         flushOnFinish = false; // flush each upload to disk before 226
    bool         trayOnClose = true;    // close button hides to the tray
    std::string  allowList;           // optional client IP allow list
    std::string  preferredIp;          // display/copy hint only
};

static Settings g_set;

static std::wstring exeDir()
{
    wchar_t b[MAX_PATH] = {0};
    GetModuleFileNameW(nullptr, b, MAX_PATH);
    std::wstring s = b;
    size_t p = s.find_last_of(L"\\/");
    return (p == std::wstring::npos) ? std::wstring(L".") : s.substr(0, p);
}

static std::wstring exeFullPath()
{
    wchar_t b[MAX_PATH] = {0};
    GetModuleFileNameW(nullptr, b, MAX_PATH);
    return std::wstring(b);
}

// admin check that needs no extra library: only an elevated process may open
// the raw disk device
static bool isElevated()
{
    HANDLE h = CreateFileW(L"\\\\.\\PHYSICALDRIVE0", 0,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    CloseHandle(h);
    return true;
}

static std::wstring iniPath() { return exeDir() + L"\\ftpsrv.ini"; }

static void loadSettings()
{
    const wchar_t* f = iniPath().c_str();
    g_set.mode       = GetPrivateProfileIntW(L"ftp", L"mode", 0, f);
    g_set.port       = GetPrivateProfileIntW(L"ftp", L"port", 2121, f);
    g_set.anon       = GetPrivateProfileIntW(L"ftp", L"anon", 0, f) != 0;
    g_set.readonly   = GetPrivateProfileIntW(L"ftp", L"readonly", 0, f) != 0;
    g_set.logToFile  = GetPrivateProfileIntW(L"ftp", L"logfile", 1, f) != 0;
    g_set.autostart  = GetPrivateProfileIntW(L"ftp", L"autostart", 0, f) != 0;
    g_set.idleSec    = GetPrivateProfileIntW(L"ftp", L"idle", 900, f);
    g_set.maxClients = GetPrivateProfileIntW(L"ftp", L"maxclients", 64, f);
    g_set.keepAliveSec = GetPrivateProfileIntW(L"ftp", L"keepalive", 30, f);
    g_set.pasvLow  = GetPrivateProfileIntW(L"ftp", L"pasvlow", 50000, f);
    g_set.pasvHigh = GetPrivateProfileIntW(L"ftp", L"pasvhigh", 50100, f);
    g_set.flushOnFinish = GetPrivateProfileIntW(L"ftp", L"flush", 0, f) != 0;
    g_set.blockKB    = GetPrivateProfileIntW(L"ftp", L"block", 256, f);
    g_set.trayOnClose = GetPrivateProfileIntW(L"ftp", L"tray", 1, f) != 0;

    wchar_t buf[1024];
    GetPrivateProfileStringW(L"ftp", L"root", L"", buf, 1024, f);
    g_set.root = buf;
    GetPrivateProfileStringW(L"ftp", L"user", L"admin", buf, 1024, f);
    g_set.user = wToMb(buf, CP_ACP);
    GetPrivateProfileStringW(L"ftp", L"pass", L"admin", buf, 1024, f);
    g_set.pass = wToMb(buf, CP_ACP);
    GetPrivateProfileStringW(L"ftp", L"ip", L"", buf, 1024, f);
    g_set.preferredIp = wToU8(buf);
    GetPrivateProfileStringW(L"ftp", L"allow", L"", buf, 1024, f);
    g_set.allowList = wToMb(buf, CP_ACP);

    if (g_set.port <= 0 || g_set.port > 65535)         g_set.port = 2121;
    if (g_set.idleSec <= 0)                            g_set.idleSec = 900;
    if (g_set.maxClients <= 0)                         g_set.maxClients = 64;
    if (g_set.blockKB < 16)                            g_set.blockKB = 16;
    if (g_set.blockKB > 4096)                          g_set.blockKB = 4096;
    if (g_set.mode != 0 && g_set.mode != 1)            g_set.mode = 0;
}

static void saveSettings()
{
    const wchar_t* f = iniPath().c_str();
    wchar_t b[64];
    swprintf(b, 64, L"%d", g_set.mode);       WritePrivateProfileStringW(L"ftp", L"mode", b, f);
    swprintf(b, 64, L"%d", g_set.port);       WritePrivateProfileStringW(L"ftp", L"port", b, f);
    swprintf(b, 64, L"%d", g_set.anon ? 1 : 0);      WritePrivateProfileStringW(L"ftp", L"anon", b, f);
    swprintf(b, 64, L"%d", g_set.readonly ? 1 : 0);  WritePrivateProfileStringW(L"ftp", L"readonly", b, f);
    swprintf(b, 64, L"%d", g_set.logToFile ? 1 : 0); WritePrivateProfileStringW(L"ftp", L"logfile", b, f);
    swprintf(b, 64, L"%d", g_set.autostart ? 1 : 0); WritePrivateProfileStringW(L"ftp", L"autostart", b, f);
    swprintf(b, 64, L"%d", g_set.idleSec);    WritePrivateProfileStringW(L"ftp", L"idle", b, f);
    swprintf(b, 64, L"%d", g_set.maxClients); WritePrivateProfileStringW(L"ftp", L"maxclients", b, f);
    swprintf(b, 64, L"%d", g_set.keepAliveSec); WritePrivateProfileStringW(L"ftp", L"keepalive", b, f);
    swprintf(b, 64, L"%d", g_set.pasvLow);   WritePrivateProfileStringW(L"ftp", L"pasvlow", b, f);
    swprintf(b, 64, L"%d", g_set.pasvHigh);  WritePrivateProfileStringW(L"ftp", L"pasvhigh", b, f);
    swprintf(b, 64, L"%d", g_set.flushOnFinish ? 1 : 0); WritePrivateProfileStringW(L"ftp", L"flush", b, f);
    swprintf(b, 64, L"%d", g_set.blockKB);   WritePrivateProfileStringW(L"ftp", L"block", b, f);

    WritePrivateProfileStringW(L"ftp", L"root", g_set.root.c_str(), f);
    WritePrivateProfileStringW(L"ftp", L"user", mbToW(g_set.user, CP_ACP).c_str(), f);
    WritePrivateProfileStringW(L"ftp", L"pass", mbToW(g_set.pass, CP_ACP).c_str(), f);
    WritePrivateProfileStringW(L"ftp", L"ip", u8ToW(g_set.preferredIp).c_str(), f);
    swprintf(b, 64, L"%d", g_set.trayOnClose ? 1 : 0); WritePrivateProfileStringW(L"ftp", L"tray", b, f);
    WritePrivateProfileStringW(L"ftp", L"allow", mbToW(g_set.allowList, CP_ACP).c_str(), f);
}

// ---------------------------------------------------------------------------
// log queue (workers -> UI thread)
// ---------------------------------------------------------------------------
static std::mutex           g_logQMx;
static std::deque<std::string> g_logQ;
static FILE*                g_logFile = nullptr;

static void logf(const char* fmt, ...)
{
    char body[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(body, sizeof(body), fmt, ap);
    va_end(ap);

    SYSTEMTIME st;
    GetLocalTime(&st);
    char line[2200];
    snprintf(line, sizeof(line), "[%02d:%02d:%02d] %s", st.wHour, st.wMinute, st.wSecond, body);

    std::lock_guard<std::mutex> lk(g_logQMx);
    g_logQ.push_back(std::string(line));
    if (g_logFile) {
        fprintf(g_logFile, "%04d-%02d-%02d %s\n", st.wYear, st.wMonth, st.wDay, line);
        fflush(g_logFile);
    }
}

// ---------------------------------------------------------------------------
// progress slots (same semantics as the console build)
// ---------------------------------------------------------------------------
#define PROG_SLOTS 5                 // same as the console build

struct ProgSlot {
    bool               active = false;
    bool               upload = false;
    std::string        label;
    unsigned long long done = 0, total = 0;
    double             startMs = 0, lastMs = 0, lastDoneMs = 0;
    unsigned long long lastDone = 0;
    double             speed = 0;
};

static ProgSlot   g_gp[PROG_SLOTS];
static std::mutex g_gpMx;

static double gpNow()
{
    return (double)GetTickCount64();
}

static std::string gFmtBytes(double b)
{
    static const char* u[] = { "B", "KB", "MB", "GB", "TB" };
    int i = 0;
    while (b >= 1024.0 && i < 4) { b /= 1024.0; ++i; }
    char o[64];
    if (i == 0) snprintf(o, sizeof(o), "%.0f %s", b, u[i]);
    else        snprintf(o, sizeof(o), "%.2f %s", b, u[i]);
    return std::string(o);
}

static std::string gFmtSpeed(double bps)
{
    if (bps <= 0.0) return std::string("--");
    return gFmtBytes(bps) + "/s";
}

static std::string gFmtTime(double sec)      // used by the end-of-transfer summary
{
    if (sec < 0) sec = 0;
    long t = (long)(sec + 0.5);
    char o[64];
    if (t < 60)        snprintf(o, sizeof(o), "%lds", t);
    else if (t < 3600) snprintf(o, sizeof(o), "%ldm%02lds", t / 60, t % 60);
    else               snprintf(o, sizeof(o), "%ldh%02ldm", t / 3600, (t % 3600) / 60);
    return std::string(o);
}

static std::string gClip(const std::string& s, size_t n)
{
    if (s.size() <= n || n < 10) return s;
    return s.substr(0, n - 9) + "..." + s.substr(s.size() - 6);
}

static int progBegin(bool upload, const std::string& label, unsigned long long total)
{
    std::lock_guard<std::mutex> lk(g_gpMx);
    int slot = -1;
    for (int i = 0; i < PROG_SLOTS; ++i) if (!g_gp[i].active) { slot = i; break; }
    if (slot < 0) return -1;

    ProgSlot& p = g_gp[slot];
    p.active = true;
    p.upload = upload;
    p.label = label;
    p.done = 0;
    p.total = total;
    p.speed = 0;
    p.lastDone = 0;
    p.startMs = p.lastMs = gpNow();
    return slot;
}

static void progTick(int slot, unsigned long long done)
{
    if (slot < 0) return;
    std::lock_guard<std::mutex> lk(g_gpMx);
    ProgSlot& p = g_gp[slot];
    if (!p.active) return;
    p.done = done;
    double t = gpNow();
    double dt = (t - p.lastMs) / 1000.0;
    if (dt >= 0.25) {
        double inst = (double)(done - p.lastDone) / dt;
        p.speed = (p.speed > 0.0) ? (p.speed * 0.65 + inst * 0.35) : inst;
        p.lastDone = done;
        p.lastMs = t;
    }
}

static void progEnd(int slot, unsigned long long done, bool ok)
{
    if (slot < 0) return;
    std::string summary;
    {
        std::lock_guard<std::mutex> lk(g_gpMx);
        ProgSlot& p = g_gp[slot];
        if (!p.active) return;
        double el = (gpNow() - p.startMs) / 1000.0;
        if (el < 0.001) el = 0.001;
        char b[256];
        snprintf(b, sizeof(b), "%s: %s in %s (%s)%s",
                 p.label.c_str(), gFmtBytes((double)done).c_str(), gFmtTime(el).c_str(),
                 gFmtSpeed((double)done / el).c_str(), ok ? "" : "  [aborted]");
        summary = b;
        p.active = false;
    }
    logf("%s", summary.c_str());
}

// ---------------------------------------------------------------------------
// small UI helpers
// ---------------------------------------------------------------------------
static HWND mk(HWND parent, DWORD ex, const wchar_t* cls, const wchar_t* text,
               DWORD style, int x, int y, int w, int h, int id, HFONT font)
{
    HWND c = CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style,
                             x, y, w, h, parent, (HMENU)(INT_PTR)id, g_hInst, nullptr);
    if (c && font) SendMessageW(c, WM_SETFONT, (WPARAM)font, TRUE);
    return c;
}

static std::wstring getText(HWND e)
{
    int n = GetWindowTextLengthW(e);
    std::wstring s((size_t)n, L'\0');
    if (n > 0) GetWindowTextW(e, &s[0], n + 1);
    return s;
}

static void setText(HWND e, const std::wstring& s) { SetWindowTextW(e, s.c_str()); }

static std::string getTextA(HWND e) { return wToMb(getText(e), CP_ACP); }

static int getInt(HWND e, int def)
{
    std::wstring s = getText(e);
    if (s.empty()) return def;
    int v = _wtoi(s.c_str());
    return v;
}

static HFONT mkFont(int pt, int weight, const wchar_t* face)
{
    HDC dc = GetDC(nullptr);
    int h = -MulDiv(pt, GetDeviceCaps(dc, LOGPIXELSY), 72);
    ReleaseDC(nullptr, dc);
    return CreateFontW(h, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                       OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                       DEFAULT_PITCH | FF_DONTCARE, face);
}

// ---------------------------------------------------------------------------
// clipboard
// ---------------------------------------------------------------------------
static void copyToClipboard(HWND owner, const std::wstring& s)
{
    if (!OpenClipboard(owner)) return;
    EmptyClipboard();
    size_t bytes = (s.size() + 1) * sizeof(wchar_t);
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (h) {
        memcpy(GlobalLock(h), s.c_str(), bytes);
        GlobalUnlock(h);
        SetClipboardData(CF_UNICODETEXT, h);
    }
    CloseClipboard();
}

// ---------------------------------------------------------------------------
// main window: state -> widgets
// ---------------------------------------------------------------------------
static std::wstring shareSummary()
{
    if (g_set.mode == 0) return L"全盘共享（C: D: … 每块盘都能访问）";
    return std::wstring(L"文件夹共享：") + (g_set.root.empty() ? L"（未选择）" : g_set.root);
}

static std::vector<std::string> sortedIps()
{
    std::vector<std::string> all, out;
    collectLocalIps(all);
    if (!g_set.preferredIp.empty()) {
        for (size_t i = 0; i < all.size(); ++i)
            if (all[i] == g_set.preferredIp) out.push_back(all[i]);
    }
    for (size_t i = 0; i < all.size(); ++i)
        if (std::find(out.begin(), out.end(), all[i]) == out.end()) out.push_back(all[i]);
    return out;
}

static void refreshAddresses()
{
    std::vector<std::string> ips = sortedIps();
    std::wstring s;
    for (size_t i = 0; i < ips.size() && i < 3; ++i) {
        if (!s.empty()) s += L"    ";
        s += u8ToW("ftp://" + ips[i] + ":" + std::to_string(g_set.port) + "/");
    }
    if (s.empty()) s = u8ToW("ftp://127.0.0.1:" + std::to_string(g_set.port) + "/");
    setText(m_addr, s);
}

static void refreshUi()
{
    wchar_t b[512];
    if (g_running) {
        swprintf(b, 512, L"状态：运行中 · 监听 %d · 已连接 %d", g_set.port, g_active.load());
        setText(m_status, b);
    } else {
        setText(m_status, L"状态：已停止");
    }
    setText(m_summary, shareSummary() + L" · 账号 " + mbToW(g_set.user, CP_ACP));

    EnableWindow(m_start, !g_running);
    EnableWindow(m_stop, g_running);
    EnableWindow(m_set, !g_running);
    EnableWindow(m_fw, !g_running);
    refreshAddresses();
    InvalidateRect(g_hMain, nullptr, TRUE);
}

static void startServer(HWND owner)
{
    if (g_running) return;

    if (g_set.mode == 1) {
        DWORD a = GetFileAttributesW(g_set.root.c_str());
        if (g_set.root.empty() || a == INVALID_FILE_ATTRIBUTES || !(a & FILE_ATTRIBUTE_DIRECTORY)) {
            MessageBoxW(owner, L"要共享的文件夹不存在，请到「设置」里重新选择。", L"FTPSrv", MB_ICONWARNING);
            return;
        }
    }

    g_cfg.port       = g_set.port;
    g_cfg.user       = g_set.user;
    g_cfg.pass       = g_set.pass;
    g_cfg.anon       = g_set.anon;
    g_cfg.readonly   = g_set.readonly;
    g_cfg.idleSec    = g_set.idleSec;
    g_cfg.maxClients = g_set.maxClients;
    g_cfg.keepAliveSec = g_set.keepAliveSec;
    g_cfg.pasvLow    = g_set.pasvLow;
    g_cfg.pasvHigh   = g_set.pasvHigh;
    g_cfg.flushOnFinish = g_set.flushOnFinish;
    g_cfg.blockKB    = g_set.blockKB;
    clampBlock();
    g_cfg.allowList  = g_set.allowList;
    g_cfg.rootDir    = (g_set.mode == 1) ? g_set.root : std::wstring();
    g_cfg.allDrives  = (g_set.mode == 0);

    if (g_set.logToFile && !g_logFile) {
        std::wstring p = exeDir() + L"\\ftpsrv.log";
        g_logFile = _wfopen(p.c_str(), L"a");
    }

    std::string err;
    if (!engineStart((unsigned short)g_set.port, err)) {
        MessageBoxW(owner, (L"启动失败：" + u8ToW(err)).c_str(), L"FTPSrv", MB_ICONERROR);
        logf("start failed: %s", err.c_str());
        return;
    }

    g_running = true;
    logf("server started: port=%d data=%d-%d mode=%s root=%s user=%s",
         g_set.port, g_set.pasvLow, g_set.pasvHigh,
         g_set.mode == 0 ? "all-drives" : "single-root",
         g_set.mode == 0 ? "*" : wToU8(g_set.root).c_str(), g_set.user.c_str());
    refreshUi();
}

static void stopServer()
{
    if (!g_running) return;
    engineStop();
    g_running = false;
    logf("server stopped");
    if (g_logFile) { fclose(g_logFile); g_logFile = nullptr; }
    refreshUi();
}

// ---------------------------------------------------------------------------
// settings window
// ---------------------------------------------------------------------------
static void fillIpCombo()
{
    SendMessageW(s_ip, CB_RESETCONTENT, 0, 0);
    SendMessageW(s_ip, CB_ADDSTRING, 0, (LPARAM)L"自动（显示全部地址）");
    std::vector<std::string> ips = sortedIps();
    int sel = 0;
    for (size_t i = 0; i < ips.size(); ++i) {
        SendMessageW(s_ip, CB_ADDSTRING, 0, (LPARAM)u8ToW(ips[i]).c_str());
        if (!g_set.preferredIp.empty() && ips[i] == g_set.preferredIp) sel = (int)i + 1;
    }
    SendMessageW(s_ip, CB_SETCURSEL, sel, 0);
}

static void setEditInt(HWND e, int v)
{
    wchar_t b[32];
    swprintf(b, 32, L"%d", v);
    setText(e, b);
}

static void browseFolder(HWND owner)
{
    BROWSEINFOW bi{};
    bi.hwndOwner = owner;
    bi.lpszTitle = L"选择要共享的文件夹";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE | BIF_USENEWUI;
    LPITEMIDLIST pidl = SHBrowseForFolderW(&bi);
    if (!pidl) return;
    wchar_t path[MAX_PATH] = {0};
    if (SHGetPathFromIDListW(pidl, path)) setText(s_root, path);
    CoTaskMemFree(pidl);
}

static bool readSettingsFromUi(HWND owner)
{
    int port = getInt(s_port, 2121);
    if (port < 1 || port > 65535) {
        MessageBoxW(owner, L"端口要在 1 到 65535 之间。", L"设置", MB_ICONWARNING);
        SetFocus(s_port);
        return false;
    }
    int mc = getInt(s_maxc, 64);
    if (mc < 1 || mc > 512) {
        MessageBoxW(owner, L"最大连接数要在 1 到 512 之间。", L"设置", MB_ICONWARNING);
        SetFocus(s_maxc);
        return false;
    }
    int idle = getInt(s_idle, 900);
    if (idle < 10 || idle > 86400) {
        MessageBoxW(owner, L"空闲超时要在 10 到 86400 秒之间。", L"设置", MB_ICONWARNING);
        SetFocus(s_idle);
        return false;
    }
    int ka = getInt(s_ka, 30);
    if (ka < 0 || ka > 3600) {
        MessageBoxW(owner, L"TCP保活要在 0 到 3600 秒之间（填 0 表示关闭）。", L"设置", MB_ICONWARNING);
        SetFocus(s_ka);
        return false;
    }
    int blk = getInt(s_blk, 256);
    if (blk < 16 || blk > 4096) {
        MessageBoxW(owner, L"写入块大小要在 16 到 4096 之间（单位 KB，默认 256）。", L"设置", MB_ICONWARNING);
        SetFocus(s_blk);
        return false;
    }
    int plow = getInt(s_plow, 50000);
    int phigh = getInt(s_phigh, 50100);
    if (plow < 1024 || phigh > 65535 || plow >= phigh || phigh - plow > 4096) {
        MessageBoxW(owner,
                    L"数据端口范围要满足：1024 ≤ 起始 < 结束 ≤ 65535，且跨度不超过 4096。\n"
                    L"（默认 50000-50100）。改完记得重跑「放行防火墙」。",
                    L"设置", MB_ICONWARNING);
        SetFocus(s_plow);
        return false;
    }

    int mode = (SendMessageW(s_dir, BM_GETCHECK, 0, 0) == BST_CHECKED) ? 1 : 0;
    std::wstring root = getText(s_root);
    if (mode == 1) {
        DWORD a = GetFileAttributesW(root.c_str());
        if (root.empty() || a == INVALID_FILE_ATTRIBUTES || !(a & FILE_ATTRIBUTE_DIRECTORY)) {
            MessageBoxW(owner, L"请选择一个存在的共享文件夹。", L"设置", MB_ICONWARNING);
            SetFocus(s_root);
            return false;
        }
    }

    std::string user = getTextA(s_user);
    if (user.empty()) {
        MessageBoxW(owner, L"账号不能为空（建议用英文）。", L"设置", MB_ICONWARNING);
        SetFocus(s_user);
        return false;
    }

    g_set.mode = mode;
    g_set.root = root;
    g_set.port = port;
    g_set.user = user;
    g_set.pass = getTextA(s_pass);
    g_set.anon = (SendMessageW(s_anon, BM_GETCHECK, 0, 0) == BST_CHECKED);
    g_set.readonly = (SendMessageW(s_ro, BM_GETCHECK, 0, 0) == BST_CHECKED);
    g_set.logToFile = (SendMessageW(s_logf, BM_GETCHECK, 0, 0) == BST_CHECKED);
    g_set.autostart = (SendMessageW(s_auto, BM_GETCHECK, 0, 0) == BST_CHECKED);
    g_set.idleSec = idle;
    g_set.maxClients = mc;
    g_set.keepAliveSec = ka;
    g_set.flushOnFinish = (SendMessageW(s_flush, BM_GETCHECK, 0, 0) == BST_CHECKED);
    g_set.blockKB = blk;
    g_set.trayOnClose = (SendMessageW(s_tray, BM_GETCHECK, 0, 0) == BST_CHECKED);
    g_set.allowList = getTextA(s_allow);
    g_set.pasvLow = plow;
    g_set.pasvHigh = phigh;

    int ipSel = (int)SendMessageW(s_ip, CB_GETCURSEL, 0, 0);
    g_set.preferredIp.clear();
    if (ipSel > 0) {
        std::vector<std::string> ips = sortedIps();
        if (ipSel - 1 < (int)ips.size()) g_set.preferredIp = ips[ipSel - 1];
        else {
            std::wstring t = getText(s_ip);
            g_set.preferredIp = wToU8(t);
        }
    }
    saveSettings();
    return true;
}

static LRESULT CALLBACK setProc(HWND h, UINT m, WPARAM wp, LPARAM lp)
{
    switch (m) {
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDS_BROWSE:  browseFolder(h); return 0;
        case IDS_OK:
            if (readSettingsFromUi(h)) { g_setOpen = false; DestroyWindow(h); }
            return 0;
        case IDS_CANCEL:
        case IDCANCEL:
            g_setOpen = false;
            DestroyWindow(h);
            return 0;
        default: break;
        }
        break;
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) {
            g_setOpen = false;
            DestroyWindow(h);
            return 0;
        }
        break;
    case WM_CLOSE:
        g_setOpen = false;
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        g_hSet = nullptr;
        return 0;
    default: break;
    }
    return DefWindowProcW(h, m, wp, lp);
}

static void openSettings(HWND owner)
{
    if (g_setOpen) { SetForegroundWindow(g_hSet); return; }

    int cw = 400, ch = 632;
    DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU;
    RECT r = { 0, 0, cw, ch };
    AdjustWindowRectEx(&r, style, FALSE, WS_EX_DLGMODALFRAME);

    RECT o{};
    GetWindowRect(owner, &o);
    int w = r.right - r.left, hgt = r.bottom - r.top;
    int x = o.left + ((o.right - o.left) - w) / 2;
    int y = o.top + ((o.bottom - o.top) - hgt) / 2;
    if (x < 0) x = 0;
    if (y < 0) y = 0;

    g_hSet = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT, kSetClass, L"服务器设置",
                             style, x, y, w, hgt, owner, nullptr, g_hInst, nullptr);
    if (!g_hSet) return;

    int X = 10, Y = 10, W = 380;
    mk(g_hSet, 0, L"BUTTON", L"共享内容", BS_GROUPBOX, X, Y, W, 86, -1, g_font);
    s_all  = mk(g_hSet, 0, L"BUTTON", L"全盘共享", BS_AUTORADIOBUTTON | WS_GROUP, X + 14, Y + 22, 110, 20, IDS_MODE_ALL, g_font);
    s_dir  = mk(g_hSet, 0, L"BUTTON", L"指定文件夹", BS_AUTORADIOBUTTON, X + 14, Y + 48, 100, 20, IDS_MODE_DIR, g_font);
    s_root = mk(g_hSet, WS_EX_CLIENTEDGE, L"EDIT", L"", ES_AUTOHSCROLL, X + 112, Y + 46, 186, 22, IDS_ROOT, g_font);
    s_browse = mk(g_hSet, 0, L"BUTTON", L"浏览…", BS_PUSHBUTTON, X + 304, Y + 45, 64, 24, IDS_BROWSE, g_font);
    SendMessageW(s_all, BM_SETCHECK, (g_set.mode == 0) ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(s_dir, BM_SETCHECK, (g_set.mode == 1) ? BST_CHECKED : BST_UNCHECKED, 0);
    setText(s_root, g_set.root);

    Y = 104;
    mk(g_hSet, 0, L"BUTTON", L"登录账号", BS_GROUPBOX, X, Y, W, 96, -1, g_font);
    mk(g_hSet, 0, L"STATIC", L"账号", SS_LEFT, X + 14, Y + 26, 40, 20, -1, g_font);
    s_user = mk(g_hSet, WS_EX_CLIENTEDGE, L"EDIT", L"", ES_AUTOHSCROLL, X + 56, Y + 24, 120, 22, IDS_USER, g_font);
    mk(g_hSet, 0, L"STATIC", L"密码", SS_LEFT, X + 190, Y + 26, 40, 20, -1, g_font);
    s_pass = mk(g_hSet, WS_EX_CLIENTEDGE, L"EDIT", L"", ES_AUTOHSCROLL | ES_PASSWORD, X + 232, Y + 24, 130, 22, IDS_PASS, g_font);
    s_anon = mk(g_hSet, 0, L"BUTTON", L"允许匿名登录（匿名只能读）", BS_AUTOCHECKBOX, X + 14, Y + 58, 240, 22, IDS_ANON, g_font);
    setText(s_user, mbToW(g_set.user, CP_ACP));
    setText(s_pass, mbToW(g_set.pass, CP_ACP));
    SendMessageW(s_anon, BM_SETCHECK, g_set.anon ? BST_CHECKED : BST_UNCHECKED, 0);

    Y = 208;
    mk(g_hSet, 0, L"BUTTON", L"网络与限制", BS_GROUPBOX, X, Y, W, 190, -1, g_font);
    mk(g_hSet, 0, L"STATIC", L"端口", SS_LEFT, X + 14, Y + 28, 40, 20, -1, g_font);
    s_port = mk(g_hSet, WS_EX_CLIENTEDGE, L"EDIT", L"", ES_AUTOHSCROLL | ES_NUMBER, X + 54, Y + 26, 70, 22, IDS_PORT, g_font);
    mk(g_hSet, 0, L"STATIC", L"最大连接数", SS_LEFT, X + 146, Y + 28, 76, 20, -1, g_font);
    s_maxc = mk(g_hSet, WS_EX_CLIENTEDGE, L"EDIT", L"", ES_AUTOHSCROLL | ES_NUMBER, X + 224, Y + 26, 60, 22, IDS_MAXCONN, g_font);
    mk(g_hSet, 0, L"STATIC", L"空闲超时(秒)", SS_LEFT, X + 14, Y + 62, 90, 20, -1, g_font);
    s_idle = mk(g_hSet, WS_EX_CLIENTEDGE, L"EDIT", L"", ES_AUTOHSCROLL | ES_NUMBER, X + 106, Y + 60, 70, 22, IDS_IDLE, g_font);
    mk(g_hSet, 0, L"STATIC", L"首选地址", SS_LEFT, X + 196, Y + 62, 60, 20, -1, g_font);
    s_ip = mk(g_hSet, 0, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL, X + 258, Y + 60, 104, 200, IDS_IP, g_font);
    mk(g_hSet, 0, L"STATIC", L"TCP保活(秒)", SS_LEFT, X + 14, Y + 96, 90, 20, -1, g_font);
    s_ka = mk(g_hSet, WS_EX_CLIENTEDGE, L"EDIT", L"", ES_AUTOHSCROLL | ES_NUMBER, X + 106, Y + 94, 70, 22, IDS_KEEPALIVE, g_font);
    mk(g_hSet, 0, L"STATIC", L"0 = 关闭；大文件长传时防掉线", SS_LEFT, X + 186, Y + 98, 180, 20, -1, g_font);
    mk(g_hSet, 0, L"STATIC", L"数据端口", SS_LEFT, X + 14, Y + 132, 90, 20, -1, g_font);
    s_plow  = mk(g_hSet, WS_EX_CLIENTEDGE, L"EDIT", L"", ES_AUTOHSCROLL | ES_NUMBER, X + 106, Y + 130, 70, 22, IDS_PASV_LOW, g_font);
    mk(g_hSet, 0, L"STATIC", L"~", SS_LEFT, X + 180, Y + 134, 12, 20, -1, g_font);
    s_phigh = mk(g_hSet, WS_EX_CLIENTEDGE, L"EDIT", L"", ES_AUTOHSCROLL | ES_NUMBER, X + 194, Y + 130, 70, 22, IDS_PASV_HIGH, g_font);
    mk(g_hSet, 0, L"STATIC", L"文件走这条通道，改完重跑「放行防火墙」", SS_LEFT, X + 14, Y + 158, 360, 20, -1, g_font);
    setEditInt(s_port, g_set.port);
    setEditInt(s_maxc, g_set.maxClients);
    setEditInt(s_idle, g_set.idleSec);
    setEditInt(s_ka, g_set.keepAliveSec);
    setEditInt(s_plow, g_set.pasvLow);
    setEditInt(s_phigh, g_set.pasvHigh);
    fillIpCombo();

    Y = 408;
    mk(g_hSet, 0, L"BUTTON", L"其它", BS_GROUPBOX, X, Y, W, 184, -1, g_font);
    s_tray = mk(g_hSet, 0, L"BUTTON", L"关闭窗口时缩到托盘", BS_AUTOCHECKBOX, X + 214, Y + 24, 160, 22, IDS_TRAY, g_font);
    s_ro   = mk(g_hSet, 0, L"BUTTON", L"只读模式（禁止上传/删除）", BS_AUTOCHECKBOX, X + 14, Y + 24, 200, 22, IDS_READONLY, g_font);
    s_logf = mk(g_hSet, 0, L"BUTTON", L"记录日志到 ftpsrv.log", BS_AUTOCHECKBOX, X + 14, Y + 50, 200, 22, IDS_LOGFILE, g_font);
    s_auto = mk(g_hSet, 0, L"BUTTON", L"启动程序时自动开始服务", BS_AUTOCHECKBOX, X + 214, Y + 50, 160, 22, IDS_AUTOSTART, g_font);
    s_flush = mk(g_hSet, 0, L"BUTTON", L"传输完成后强制写入磁盘（慢盘会拖慢大文件，一般不用勾）",
                 BS_AUTOCHECKBOX, X + 14, Y + 80, 360, 22, IDS_FLUSH, g_font);
    SendMessageW(s_ro, BM_SETCHECK, g_set.readonly ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(s_logf, BM_SETCHECK, g_set.logToFile ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(s_auto, BM_SETCHECK, g_set.autostart ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(s_flush, BM_SETCHECK, g_set.flushOnFinish ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(s_tray, BM_SETCHECK, g_set.trayOnClose ? BST_CHECKED : BST_UNCHECKED, 0);
    mk(g_hSet, 0, L"STATIC", L"只允许这些 IP 连（留空 = 不限制；可写 192.168.1.0/24，逗号分隔多个）",
       SS_LEFT, X + 14, Y + 108, 360, 18, -1, g_font);
    s_allow = mk(g_hSet, WS_EX_CLIENTEDGE, L"EDIT", L"", ES_AUTOHSCROLL, X + 14, Y + 126, 360, 22, IDS_ALLOW, g_font);
    setText(s_allow, mbToW(g_set.allowList, CP_ACP));
    mk(g_hSet, 0, L"STATIC", L"写入块大小(KB)", SS_LEFT, X + 14, Y + 158, 100, 20, -1, g_font);
    s_blk = mk(g_hSet, WS_EX_CLIENTEDGE, L"EDIT", L"", ES_AUTOHSCROLL | ES_NUMBER,
               X + 118, Y + 156, 66, 22, IDS_BLOCK, g_font);
    mk(g_hSet, 0, L"STATIC", L"16~4096，默认 256；慢盘上可调大（每次读写的数据量）",
       SS_LEFT, X + 190, Y + 160, 180, 20, -1, g_font);
    setEditInt(s_blk, g_set.blockKB);

    HWND ok = mk(g_hSet, 0, L"BUTTON", L"确定", BS_DEFPUSHBUTTON, 200, 602, 90, 26, IDS_OK, g_font);
    mk(g_hSet, 0, L"BUTTON", L"取消", BS_PUSHBUTTON, 300, 602, 90, 26, IDS_CANCEL, g_font);

    g_setOpen = true;
    EnableWindow(owner, FALSE);
    ShowWindow(g_hSet, SW_SHOW);
    SetFocus(s_user);

    MSG msg;
    while (g_setOpen && GetMessageW(&msg, nullptr, 0, 0)) {
        if (g_hSet && IsDialogMessageW(g_hSet, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    EnableWindow(owner, TRUE);
    if (g_set.trayOnClose) trayAdd(g_hMain); else trayRemove();
    SetForegroundWindow(owner);
    refreshUi();
    (void)ok;
}

// ---------------------------------------------------------------------------
// main window
// ---------------------------------------------------------------------------
static void drainLog()
{
    std::vector<std::string> lines;
    {
        std::lock_guard<std::mutex> lk(g_logQMx);
        while (!g_logQ.empty() && lines.size() < 50) {
            lines.push_back(g_logQ.front());
            g_logQ.pop_front();
        }
    }
    if (lines.empty()) return;

    int n = (int)SendMessageW(m_log, LB_GETCOUNT, 0, 0);
    for (size_t i = 0; i < lines.size(); ++i) {
        std::wstring w = u8ToW(lines[i]);
        SendMessageW(m_log, LB_ADDSTRING, 0, (LPARAM)w.c_str());
        ++n;
    }
    while (n > 800) {                       // keep the box from growing forever
        SendMessageW(m_log, LB_DELETESTRING, 0, 0);
        --n;
    }
    SendMessageW(m_log, LB_SETTOPINDEX, (WPARAM)(n - 1), 0);
}

static void refreshProgress()
{
    std::string out[PROG_SLOTS + 1];   // +1 = the "... +N more" summary line
    {
        std::lock_guard<std::mutex> lk(g_gpMx);
        int n = 0;
        for (int i = 0; i < PROG_SLOTS; ++i) {
            ProgSlot& p = g_gp[i];
            if (!p.active) continue;
            char buf[256];
            if (p.total > 0) {
                double frac = (double)p.done / (double)p.total;
                if (frac > 1.0) frac = 1.0;
                int fill = (int)(frac * 10 + 0.5);
                std::string bar;
                for (int k = 0; k < 10; ++k) bar += (k < fill ? '#' : '-');
                snprintf(buf, sizeof(buf), "%s %-16s [%s] %3.0f%% %s/%s %s",
                         p.upload ? "UP " : "DOWN", gClip(p.label, 16).c_str(), bar.c_str(),
                         frac * 100.0, gFmtBytes((double)p.done).c_str(),
                         gFmtBytes((double)p.total).c_str(), gFmtSpeed(p.speed).c_str());
            } else {
                snprintf(buf, sizeof(buf), "%s %-16s %s %s",
                         p.upload ? "UP " : "DOWN", gClip(p.label, 16).c_str(),
                         gFmtBytes((double)p.done).c_str(), gFmtSpeed(p.speed).c_str());
            }
            out[n++] = buf;
        }

        // more transfers than lines: say how many and what they add up to
        int live = xferLiveCount();
        if (live > n) {
            char b[192];
            snprintf(b, sizeof(b), "... +%d more running, total %s",
                     live - n, gFmtSpeed(xferSpeedNow()).c_str());
            out[n++] = b;
        }
    }
    setText(m_prog1, u8ToW(out[0]));
    setText(m_prog2, u8ToW(out[1]));
    setText(m_prog3, u8ToW(out[2]));
    setText(m_prog4, u8ToW(out[3]));
    setText(m_prog5, u8ToW(out[4]));
    setText(m_prog6, u8ToW(out[5]));
}

// ---------------------------------------------------------------------------
// the program icon lives in icon.res (built from icon.ico by windres), resource
// id 1.  Falls back to the generic Windows icon if it was not linked in.
// ---------------------------------------------------------------------------
static HICON appIconBig()
{
    HICON ic = (HICON)LoadImageW(g_hInst, (LPCWSTR)(ULONG_PTR)1, IMAGE_ICON, 0, 0,
                                 LR_DEFAULTSIZE | LR_SHARED);
    if (!ic) ic = LoadIconW(nullptr, (LPCWSTR)IDI_APPLICATION);
    return ic;
}

static HICON appIconSmall()
{
    HICON ic = (HICON)LoadImageW(g_hInst, (LPCWSTR)(ULONG_PTR)1, IMAGE_ICON,
                                 GetSystemMetrics(SM_CXSMICON),
                                 GetSystemMetrics(SM_CYSMICON), LR_SHARED);
    if (!ic) ic = LoadIconW(nullptr, (LPCWSTR)IDI_APPLICATION);
    return ic;
}

// ---------------------------------------------------------------------------
// tray icon: closing the window keeps the server running in the background
// ---------------------------------------------------------------------------
static NOTIFYICONDATAW g_nid{};

static void nidCopy(wchar_t* dst, size_t cch, const wchar_t* src)
{
    size_t n = wcslen(src);
    if (n > cch - 1) n = cch - 1;
    memcpy(dst, src, n * sizeof(wchar_t));
    dst[n] = 0;
}

static void trayAdd(HWND h)
{
    if (g_trayIcon) return;
    ZeroMemory(&g_nid, sizeof(g_nid));
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd   = h;
    g_nid.uID    = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAYMSG;
    g_nid.hIcon  = appIconSmall();
    nidCopy(g_nid.szTip, 128, L"FTPSrv - 局域网 FTP 文件共享（在后台运行）");
    if (Shell_NotifyIconW(NIM_ADD, &g_nid)) g_trayIcon = true;
}

static void trayRemove()
{
    if (!g_trayIcon) return;
    Shell_NotifyIconW(NIM_DELETE, &g_nid);
    g_trayIcon = false;
}

static void trayBalloon(const wchar_t* text)
{
    if (!g_trayIcon) return;
    g_nid.uFlags      = NIF_INFO;
    g_nid.dwInfoFlags = NIIF_INFO;
    nidCopy(g_nid.szInfoTitle, 64, L"FTPSrv 还在后台运行");
    nidCopy(g_nid.szInfo, 256, text);
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
}

static void showFromTray(HWND h)
{
    ShowWindow(h, SW_SHOW);
    ShowWindow(h, SW_RESTORE);
    SetForegroundWindow(h);
}

static LRESULT CALLBACK mainProc(HWND h, UINT m, WPARAM wp, LPARAM lp)
{
    switch (m) {
    case WM_CREATE:
        m_status = mk(h, 0, L"STATIC", L"状态：已停止", SS_LEFT, 12, 12, 480, 22, IDC_STATUS, g_fontBold);
        m_summary = mk(h, 0, L"STATIC", L"", SS_LEFT, 12, 36, 480, 20, IDC_SUMMARY, g_font);
        m_addr = mk(h, WS_EX_CLIENTEDGE, L"EDIT", L"", ES_READONLY | ES_AUTOHSCROLL, 12, 60, 372, 24, IDC_ADDR, g_font);
        m_copy = mk(h, 0, L"BUTTON", L"复制地址", BS_PUSHBUTTON, 392, 59, 100, 26, IDC_BTN_COPY, g_font);

        m_start = mk(h, 0, L"BUTTON", L"启动服务", BS_DEFPUSHBUTTON, 12, 94, 115, 30, IDC_BTN_START, g_font);
        m_stop  = mk(h, 0, L"BUTTON", L"停止服务", BS_PUSHBUTTON, 137, 94, 115, 30, IDC_BTN_STOP, g_font);
        m_set   = mk(h, 0, L"BUTTON", L"设置…", BS_PUSHBUTTON, 262, 94, 115, 30, IDC_BTN_SET, g_font);
        m_fw    = mk(h, 0, L"BUTTON", L"放行防火墙", BS_PUSHBUTTON, 387, 94, 115, 30, IDC_BTN_FW, g_font);

        mk(h, 0, L"STATIC", L"传输进度（实时速度）：", SS_LEFT, 12, 132, 300, 18, -1, g_font);
        m_prog1 = mk(h, 0, L"STATIC", L"", SS_LEFTNOWORDWRAP, 12, 152, 480, 20, IDC_PROG1, g_fontMono);
        m_prog2 = mk(h, 0, L"STATIC", L"", SS_LEFTNOWORDWRAP, 12, 172, 480, 20, IDC_PROG2, g_fontMono);
        m_prog3 = mk(h, 0, L"STATIC", L"", SS_LEFTNOWORDWRAP, 12, 192, 480, 20, IDC_PROG3, g_fontMono);
        m_prog4 = mk(h, 0, L"STATIC", L"", SS_LEFTNOWORDWRAP, 12, 212, 480, 20, IDC_PROG4, g_fontMono);
        m_prog5 = mk(h, 0, L"STATIC", L"", SS_LEFTNOWORDWRAP, 12, 232, 480, 20, IDC_PROG5, g_fontMono);
        m_prog6 = mk(h, 0, L"STATIC", L"", SS_LEFTNOWORDWRAP, 12, 252, 480, 20, IDC_PROG6, g_fontMono);

        mk(h, 0, L"BUTTON", L"日志", BS_GROUPBOX, 12, 280, 480, 200, -1, g_font);
        m_log = mk(h, WS_EX_CLIENTEDGE, L"LISTBOX", L"",
                   WS_VSCROLL | WS_HSCROLL | LBS_NOINTEGRALHEIGHT | WS_TABSTOP, 24, 304, 456, 140, IDC_LOGLIST, g_fontMono);
        SendMessageW(m_log, LB_SETHORIZONTALEXTENT, (WPARAM)1600, 0);   // long lines can be scrolled
        m_clear = mk(h, 0, L"BUTTON", L"清空日志", BS_PUSHBUTTON, 388, 448, 92, 24, IDC_BTN_CLEAR, g_font);

        SetTimer(h, 1, 300, nullptr);
        EnableWindow(m_stop, FALSE);
        if (g_set.trayOnClose) trayAdd(h);   // closing the window will hide here
        return 0;

    case WM_TIMER:
        drainLog();
        refreshProgress();
        if (g_running) {
            static int lastActive = -1;
            int a = g_active.load();
            if (a != lastActive) { lastActive = a; refreshUi(); }
        }
        if (g_autostartPending) {
            g_autostartPending = false;
            startServer(h);
        }
        return 0;

    case WM_TRAYMSG: {
        UINT ev = (UINT)LOWORD(lp);
        if (ev == WM_LBUTTONUP || ev == WM_LBUTTONDBLCLK) { showFromTray(h); return 0; }
        if (ev != WM_RBUTTONUP && ev != WM_CONTEXTMENU) return 0;
        POINT p{};
        GetCursorPos(&p);
        SetForegroundWindow(h);
        HMENU menu = CreatePopupMenu();
        AppendMenuW(menu, MF_STRING, IDM_TRAY_OPEN, L"打开主界面");
        AppendMenuW(menu, (g_running ? MF_STRING : MF_STRING | MF_GRAYED),
                    IDM_TRAY_STOP, L"停止服务");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, IDM_TRAY_QUIT, L"退出");
        int cmd = (int)TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
                                      p.x, p.y, 0, h, nullptr);
        DestroyMenu(menu);
        PostMessage(h, WM_NULL, 0, 0);
        if (cmd == IDM_TRAY_OPEN)      showFromTray(h);
        else if (cmd == IDM_TRAY_STOP) stopServer();
        else if (cmd == IDM_TRAY_QUIT) { stopServer(); DestroyWindow(h); }
        return 0;
    }

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_BTN_START: startServer(h); return 0;
        case IDC_BTN_STOP:  if (MessageBoxW(h, L"确定停止 FTP 服务吗？", L"FTPSrv",
                                           MB_ICONQUESTION | MB_YESNO) == IDYES) stopServer();
                            return 0;
        case IDC_BTN_SET:   openSettings(h); return 0;
        case IDC_BTN_FW: {
            if (isElevated()) {
                g_cfg.pasvLow  = g_set.pasvLow;      // rule must match the configured range
                g_cfg.pasvHigh = g_set.pasvHigh;
                clampPasvRange();
                int rc = addFirewallRule(g_set.port);
                wchar_t m2[320];
                swprintf(m2, 320,
                         L"防火墙放行规则已处理。\n控制端口 TCP %d\n数据端口 TCP %d-%d\n退出码 = %d",
                         g_set.port, g_cfg.pasvLow, g_cfg.pasvHigh, rc);
                MessageBoxW(h, m2, L"FTPSrv", MB_ICONINFORMATION);
                return 0;
            }
            wchar_t args[96];
            swprintf(args, 96, L"--addfw --port%d --pasv-range %d-%d",
                     g_set.port, g_set.pasvLow, g_set.pasvHigh);
            std::wstring exePath = exeFullPath();
            HINSTANCE r = ShellExecuteW(h, L"runas", exePath.c_str(), args, nullptr, SW_SHOWNORMAL);
            if ((INT_PTR)r <= 32) {
                MessageBoxW(h, L"未能提权（可能被取消了 UAC）。\n请右键以管理员身份运行本程序，再点这个按钮。",
                            L"FTPSrv", MB_ICONWARNING);
            } else {
                MessageBoxW(h, L"已尝试添加防火墙放行规则（TCP 端口 + 程序 + 数据端口段）。\n"
                               L"如果对方还是连不上，请检查第三方杀软/防火墙。", L"FTPSrv", MB_ICONINFORMATION);
            }
            return 0;
        }
        case IDC_BTN_COPY: {
            std::vector<std::string> ips = sortedIps();
            std::string ip = ips.empty() ? "127.0.0.1" : ips[0];
            copyToClipboard(h, u8ToW("ftp://" + ip + ":" + std::to_string(g_set.port) + "/"));
            return 0;
        }
        case IDC_BTN_CLEAR:
            SendMessageW(m_log, LB_RESETCONTENT, 0, 0);
            return 0;
        default: break;
        }
        break;

    case WM_CLOSE:
        if (g_set.trayOnClose && g_running) {      // hide, keep serving
            ShowWindow(h, SW_HIDE);
            trayAdd(h);
            if (!g_trayHintShown) {
                g_trayHintShown = true;
                trayBalloon(L"服务还在跑。双击托盘图标可以重新打开窗口，右键可以停止或退出。");
            }
            return 0;
        }
        if (g_running) {
            if (MessageBoxW(h, L"服务正在运行，停止并退出吗？", L"FTPSrv",
                            MB_ICONQUESTION | MB_YESNO) != IDYES) return 0;
            stopServer();
        }
        DestroyWindow(h);
        return 0;

    case WM_DESTROY:
        KillTimer(h, 1);
        trayRemove();
        if (g_logFile) { fclose(g_logFile); g_logFile = nullptr; }
        PostQuitMessage(0);
        return 0;

    default: break;
    }
    return DefWindowProcW(h, m, wp, lp);
}

// ---------------------------------------------------------------------------
// entry point
// ---------------------------------------------------------------------------
static int elevatedAddFw(const std::wstring& cmdline)
{
    int port = 2121;
    size_t p = cmdline.find(L"--port");
    if (p != std::wstring::npos) {
        int v = _wtoi(cmdline.c_str() + p + 6);
        if (v > 0 && v <= 65535) port = v;
    }

    // the GUI passes its configured data range along, so the firewall rule
    // always matches what the server will actually use
    size_t q = cmdline.find(L"--pasv-range");
    if (q != std::wstring::npos) {
        // hand-rolled parse: one swscanf() would drag ~80 KB of wide scanf tables
        // into the exe, which is silly for reading "50000-50100"
        const wchar_t* s = cmdline.c_str() + q + 12;
        while (*s == L' ') ++s;
        long lo = 0, hi = 0;
        while (*s >= L'0' && *s <= L'9') { lo = lo * 10 + (*s - L'0'); ++s; }
        if (*s == L'-') {
            ++s;
            while (*s >= L'0' && *s <= L'9') { hi = hi * 10 + (*s - L'0'); ++s; }
        }
        if (lo > 0 && hi > 0) {
            g_cfg.pasvLow  = (int)lo;
            g_cfg.pasvHigh = (int)hi;
            clampPasvRange();
        }
    }

    int rc = addFirewallRule(port);
    wchar_t msg[512];
    swprintf(msg, 512,
             L"防火墙放行规则处理完成。\n控制端口 TCP %d\n数据端口 TCP %d-%d\nexit code = %d\n\n"
             L"如果仍然连不上，请检查杀软或路由器隔离。",
             port, g_cfg.pasvLow, g_cfg.pasvHigh, rc);
    MessageBoxW(nullptr, msg, L"FTPSrv 防火墙", MB_ICONINFORMATION);
    return 0;
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int)
{
    g_hInst = hInst;
    SetProcessDPIAware();
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    std::wstring cmd = GetCommandLineW();
    if (cmd.find(L"--addfw") != std::wstring::npos) {
        int rc = elevatedAddFw(cmd);
        CoUninitialize();
        return rc;
    }

    g_font     = mkFont(9, FW_NORMAL, L"Microsoft YaHei UI");
    g_fontBold = mkFont(10, FW_SEMIBOLD, L"Microsoft YaHei UI");
    g_fontMono = mkFont(9, FW_NORMAL, L"Consolas");

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = mainProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hIcon = appIconBig();
    wc.hIconSm = appIconSmall();
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = kMainClass;
    RegisterClassExW(&wc);

    wc.lpfnWndProc = setProc;
    wc.lpszClassName = kSetClass;
    RegisterClassExW(&wc);

    loadSettings();
    if (g_set.autostart) g_autostartPending = true;

    RECT r = { 0, 0, 504, 492 };
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    AdjustWindowRectEx(&r, style, FALSE, 0);

    g_hMain = CreateWindowExW(0, kMainClass, L"FTPSrv - 局域网 FTP 文件共享",
                              style, CW_USEDEFAULT, CW_USEDEFAULT,
                              r.right - r.left, r.bottom - r.top,
                              nullptr, nullptr, hInst, nullptr);
    if (!g_hMain) return 1;

    refreshUi();
    ShowWindow(g_hMain, SW_SHOW);
    UpdateWindow(g_hMain);
    logf("ready. mode=%s port=%d user=%s",
         g_set.mode == 0 ? "all-drives" : "single-root", g_set.port, g_set.user.c_str());

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    CoUninitialize();
    return 0;
}
