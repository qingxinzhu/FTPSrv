// ============================================================================
//  FTPSrv - Windows LAN FTP file server  (console frontend)
//
//  the FTP engine lives in ftpsrv_engine.inc and is shared with the GUI build
//  (ftpsrv_gui.cpp).  this file only provides the console look:
//  timestamped log lines + a self-rewriting speed/ETA line.
//
//  build (cross, on Linux):
//    x86_64-w64-mingw32-g++ -O2 -std=c++17 -static -o ftpsrv.exe ftpsrv.cpp -lws2_32
//  build (on Windows, mingw-w64):
//    g++ -O2 -std=c++17 -static -o ftpsrv.exe ftpsrv.cpp -lws2_32
//  build (MSVC):
//    cl /EHsc /O2 /std:c++17 ftpsrv.cpp ws2_32.lib
// ============================================================================

#include "ftpsrv_engine.inc"

// ---------------------------------------------------------------------------
// live progress: one or two lines that rewrite themselves (no scroll-spam)
// ---------------------------------------------------------------------------
#define PROG_SLOTS                 2
#define PROG_BAR_CELLS             12
#define PROG_MIN_INTERVAL_MS       200.0     // console redraw at most 5x per second
#define PROG_FALLBACK_INTERVAL_MS  5000.0    // redirected stdout: one plain line / 5 s
#define PROG_NAME_WIDTH            20

static void logf(const char* fmt, ...);      // defined below

static HANDLE g_hOut        = nullptr;
static bool   g_consoleOut  = false;         // stdout is a real console with ANSI enabled
static bool   g_progEnabled = true;          // --noprogress turns the live line off

static double nowMs()
{
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER u;
    u.LowPart  = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    return (double)(u.QuadPart / 10000ULL);  // milliseconds
}

static void enableAnsiConsole()
{
    g_hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!g_hOut || g_hOut == INVALID_HANDLE_VALUE) { g_consoleOut = false; return; }
    DWORD mode = 0;
    if (!GetConsoleMode(g_hOut, &mode)) { g_consoleOut = false; return; }
#ifndef ENABLE_VIRTUAL_TERMINAL_PROCESSING
#define ENABLE_VIRTUAL_TERMINAL_PROCESSING 0x0004
#endif
    if (!SetConsoleMode(g_hOut, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING)) {
        g_consoleOut = false;
        return;
    }
    g_consoleOut = true;
}

static std::string fmtBytes(double b)
{
    static const char* unit[] = { "B", "KB", "MB", "GB", "TB" };
    int i = 0;
    while (b >= 1024.0 && i < 4) { b /= 1024.0; ++i; }
    char out[64];
    if (i == 0) snprintf(out, sizeof(out), "%.0f %s", b, unit[i]);
    else        snprintf(out, sizeof(out), "%.2f %s", b, unit[i]);
    return std::string(out);
}

static std::string fmtSpeed(double bps)
{
    if (bps <= 0.0) return std::string("--");
    return fmtBytes(bps) + "/s";
}

static std::string fmtHMS(double sec)
{
    if (sec < 0.0) sec = 0.0;
    long t = (long)(sec + 0.5);
    char out[64];
    if (t < 60)        snprintf(out, sizeof(out), "%lds", t);
    else if (t < 3600) snprintf(out, sizeof(out), "%ldm%02lds", t / 60, t % 60);
    else               snprintf(out, sizeof(out), "%ldh%02ldm", t / 3600, (t % 3600) / 60);
    return std::string(out);
}

static std::string clipMid(const std::string& s, size_t maxLen)
{
    if (s.size() <= maxLen || maxLen < 10) return s;
    size_t head = maxLen - 9;
    return s.substr(0, head) + "..." + s.substr(s.size() - 6);
}

struct ProgSlot {
    bool               active = false;
    bool               upload = false;
    std::string        label;
    unsigned long long done  = 0;
    unsigned long long total = 0;        // 0 = unknown
    double             startMs = 0;
    double             lastMs  = 0;
    double             lastPrintMs = 0;
    unsigned long long lastDone = 0;
    double             speed = 0;        // bytes / second (smoothed)
};

static ProgSlot   g_prog[PROG_SLOTS];
static std::mutex g_progMx;
static int        g_progLines  = 0;      // how many lines are drawn right now
static int        g_progCursor = 0;      // which drawn line the cursor sits on

static std::string progLineOf(const ProgSlot& p)
{
    std::string name = clipMid(p.label, PROG_NAME_WIDTH);
    char buf[256];

    if (p.total > 0) {
        double frac = (double)p.done / (double)p.total;
        if (frac < 0.0) frac = 0.0;
        if (frac > 1.0) frac = 1.0;
        int fill = (int)(frac * PROG_BAR_CELLS + 0.5);
        std::string bar;
        for (int i = 0; i < PROG_BAR_CELLS; ++i) bar += (i < fill ? '#' : '-');
        snprintf(buf, sizeof(buf), "%s %-*s [%s] %s/%s %s",
                 p.upload ? "UP  " : "DOWN", PROG_NAME_WIDTH, name.c_str(), bar.c_str(),
                 fmtBytes((double)p.done).c_str(), fmtBytes((double)p.total).c_str(),
                 fmtSpeed(p.speed).c_str());
    } else {
        snprintf(buf, sizeof(buf), "%s %-*s %s %s",
                 p.upload ? "UP  " : "DOWN", PROG_NAME_WIDTH, name.c_str(),
                 fmtBytes((double)p.done).c_str(), fmtSpeed(p.speed).c_str());
    }
    return std::string(buf);
}

static void progDraw_locked()
{
    if (!g_progEnabled || !g_consoleOut) return;

    std::string lines[PROG_SLOTS];
    int n = 0;
    for (int i = 0; i < PROG_SLOTS; ++i)
        if (g_prog[i].active) lines[n++] = progLineOf(g_prog[i]);

    if (n == 0 && g_progLines == 0) return;

    std::string out = "\r";
    if (g_progCursor > 0) out += "\x1b[" + std::to_string(g_progCursor) + "A";

    int total = (n > g_progLines) ? n : g_progLines;
    for (int i = 0; i < total; ++i) {
        out += "\x1b[K";
        if (i < n) out += lines[i];
        if (i + 1 < total) out += "\n";
    }
    int cur = (n > 0) ? (n - 1) : 0;
    int up  = (total - 1) - cur;
    if (up > 0) out += "\x1b[" + std::to_string(up) + "A";
    out += "\r";

    fwrite(out.data(), 1, out.size(), stdout);
    fflush(stdout);
    g_progLines  = n;
    g_progCursor = cur;
}

static void progErase_locked()
{
    if (!g_progEnabled || !g_consoleOut || g_progLines == 0) return;

    std::string out = "\r";
    if (g_progCursor > 0) out += "\x1b[" + std::to_string(g_progCursor) + "A";
    for (int i = 0; i < g_progLines; ++i) {
        out += "\x1b[K";
        if (i + 1 < g_progLines) out += "\n";
    }
    if (g_progLines > 1) out += "\x1b[" + std::to_string(g_progLines - 1) + "A";
    out += "\r";

    fwrite(out.data(), 1, out.size(), stdout);
    fflush(stdout);
    g_progLines  = 0;
    g_progCursor = 0;
}

// returns slot index, or -1 when both lines are already taken / disabled
static int progBegin(bool upload, const std::string& label, unsigned long long total)
{
    std::lock_guard<std::mutex> lk(g_progMx);
    int slot = -1;
    for (int i = 0; i < PROG_SLOTS; ++i) if (!g_prog[i].active) { slot = i; break; }
    if (slot < 0) return -1;

    ProgSlot& p = g_prog[slot];
    p.active   = true;
    p.upload   = upload;
    p.label    = label;
    p.done     = 0;
    p.total    = total;
    p.speed    = 0.0;
    p.lastDone = 0;
    p.startMs  = p.lastMs = p.lastPrintMs = nowMs();
    progDraw_locked();
    return slot;
}

static void progTick(int slot, unsigned long long done)
{
    if (slot < 0) return;

    std::lock_guard<std::mutex> lk(g_progMx);
    ProgSlot& p = g_prog[slot];
    if (!p.active) return;

    double t = nowMs();
    p.done = done;

    double dt = (t - p.lastMs) / 1000.0;
    if (dt >= 0.25) {
        double inst = (double)(done - p.lastDone) / dt;
        p.speed    = (p.speed > 0.0) ? (p.speed * 0.65 + inst * 0.35) : inst;
        p.lastDone = done;
        p.lastMs   = t;
    }

    if (!g_progEnabled) return;

    if (!g_consoleOut) {
        // stdout is redirected (file / pipe): one plain line every few seconds
        if (t - p.lastPrintMs >= PROG_FALLBACK_INTERVAL_MS) {
            p.lastPrintMs = t;
            printf("%s\n", progLineOf(p).c_str());
            fflush(stdout);
        }
        return;
    }

    if (t - p.lastPrintMs >= PROG_MIN_INTERVAL_MS) {
        p.lastPrintMs = t;
        progDraw_locked();
    }
}

static void progEnd(int slot, unsigned long long done, bool ok)
{
    if (slot < 0) return;

    std::string summary;
    {
        std::lock_guard<std::mutex> lk(g_progMx);
        ProgSlot& p = g_prog[slot];
        if (!p.active) return;

        double el = (nowMs() - p.startMs) / 1000.0;
        if (el < 0.001) el = 0.001;
        char b[256];
        snprintf(b, sizeof(b), "%s %s: %s in %s (%s)%s",
                 p.upload ? "UP" : "DOWN", p.label.c_str(),
                 fmtBytes((double)done).c_str(), fmtHMS(el).c_str(),
                 fmtSpeed((double)done / el).c_str(),
                 ok ? "" : "  [aborted]");
        summary  = b;
        p.active = false;
        p.speed  = 0.0;
        progDraw_locked();
    }
    logf("%s", summary.c_str());
}

// ---------------------------------------------------------------------------
// logging
// ---------------------------------------------------------------------------
static std::mutex g_logMx;
static FILE*      g_log = nullptr;

static void logf(const char* fmt, ...)
{
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    SYSTEMTIME st;
    GetLocalTime(&st);

    std::lock_guard<std::mutex> lk(g_logMx);
    {
        std::lock_guard<std::mutex> pk(g_progMx);
        progErase_locked();          // clear the live lines, then print below them
    }
    printf("[%02d:%02d:%02d] %s\n", st.wHour, st.wMinute, st.wSecond, buf);
    fflush(stdout);
    if (g_log) {
        fprintf(g_log, "[%04d-%02d-%02d %02d:%02d:%02d] %s\n",
                st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, buf);
        fflush(g_log);
    }
    {
        std::lock_guard<std::mutex> pk(g_progMx);
        progDraw_locked();           // put the live lines back
    }
}

static void printHelp()
{
    printf(
        "FTPSrv - Windows LAN FTP file server\n"
        "\n"
        "usage: ftpsrv.exe [options]\n"
        "\n"
        "  --port N          listen port (default 2121; use 21 for ftp://ip without port)\n"
        "  --root PATH       share a single directory instead of all drives\n"
        "  --user NAME       login user name (default admin)\n"
        "  --pass WORD       login password  (default admin)\n"
        "  --anon            also allow anonymous login (read-only)\n"
        "  --readonly        disable all write operations\n"
        "  --log FILE        append log to file\n"
        "  --idle SEC        idle/transfer timeout in seconds (default 900)\n"
        "  --maxclients N    concurrent client limit (default 32)\n"
        "  --keepalive SEC   TCP keep-alive idle seconds, 0=off (default 30)\n"
        "  --pasv-range L-H  data channel port range (default 50000-50100)\n"
        "  --flush           flush uploads to disk before answering 226 (slower, safer)\n"
        "  --allow LIST      only these client IPs may connect, e.g. 192.168.1.0/24\n"
        "  --addfw           add Windows Firewall rules (run as administrator)\n"
        "  --noprogress      do not draw the live speed line\n"
        "  -h, --help        this text\n"
        "\n"
        "examples:\n"
        "  ftpsrv.exe\n"
        "  ftpsrv.exe --root D:\\share --user jack --pass 1234\n"
        "  ftpsrv.exe --port 21 --anon --readonly\n"
        "\n"
        "client:  Windows Explorer -> ftp://192.168.x.x:2121\n"
        "         (FileZilla / WinSCP / Total Commander / any FTP client)\n");
}

static BOOL WINAPI ctrlHandler(DWORD type)
{
    if (type == CTRL_C_EVENT || type == CTRL_CLOSE_EVENT || type == CTRL_BREAK_EVENT) {
        {
            std::lock_guard<std::mutex> pk(g_progMx);
            progErase_locked();
            g_progEnabled = false;
        }
        if (g_log) { fflush(g_log); fclose(g_log); g_log = nullptr; }
        printf("\nshutting down...\n");
        ExitProcess(0);
    }
    return FALSE;
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main(int argc, char** argv)
{
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCtrlHandler(ctrlHandler, TRUE);
    enableAnsiConsole();

    bool doAddFw = false;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&](const char* def) -> std::string {
            if (i + 1 < argc) return std::string(argv[++i]);
            return std::string(def);
        };
        if (a == "-h" || a == "--help") { printHelp(); return 0; }
        else if (a == "--port")       g_cfg.port = atoi(next("2121").c_str());
        else if (a == "--root")       g_cfg.rootDir = mbToW(next(""), CP_ACP);
        else if (a == "--user")       g_cfg.user = next("admin");
        else if (a == "--pass")       g_cfg.pass = next("admin");
        else if (a == "--anon")       g_cfg.anon = true;
        else if (a == "--readonly")   g_cfg.readonly = true;
        else if (a == "--flush")      g_cfg.flushOnFinish = true;
        else if (a == "--allow")      g_cfg.allowList = next("");
        else if (a == "--log")        g_cfg.logPath = next("");
        else if (a == "--idle")       g_cfg.idleSec = atoi(next("900").c_str());
        else if (a == "--maxclients") g_cfg.maxClients = atoi(next("64").c_str());
        else if (a == "--keepalive")  g_cfg.keepAliveSec = atoi(next("30").c_str());
        else if (a == "--pasv-range") {
            std::string v = next("50000-50100");
            size_t d = v.find('-');
            if (d != std::string::npos) {
                g_cfg.pasvLow  = atoi(v.substr(0, d).c_str());
                g_cfg.pasvHigh = atoi(v.substr(d + 1).c_str());
            }
        }
        else if (a == "--addfw")      doAddFw = true;
        else if (a == "--noprogress") g_progEnabled = false;
        else {
            printf("unknown option: %s\n", a.c_str());
            printHelp();
            return 1;
        }
    }

    if (g_cfg.port <= 0 || g_cfg.port > 65535) g_cfg.port = 2121;
    if (g_cfg.idleSec <= 0) g_cfg.idleSec = 900;
    if (g_cfg.maxClients <= 0) g_cfg.maxClients = 32;
    if (g_cfg.keepAliveSec < 0) g_cfg.keepAliveSec = 0;
    clampPasvRange();

    if (!g_cfg.rootDir.empty()) {
        DWORD a = GetFileAttributesW(g_cfg.rootDir.c_str());
        if (a == INVALID_FILE_ATTRIBUTES || !(a & FILE_ATTRIBUTE_DIRECTORY)) {
            printf("ERROR: --root directory not found: %s\n", wToU8(g_cfg.rootDir).c_str());
            return 1;
        }
        g_cfg.allDrives = false;
    } else {
        g_cfg.allDrives = true;
    }

    if (!g_cfg.logPath.empty()) fopen_s(&g_log, g_cfg.logPath.c_str(), "a");

    printf("\n");
    printf("  FTPSrv - LAN FTP file server\n");
    printf("  ----------------------------\n");
    printf("  mode     : %s\n", g_cfg.allDrives
           ? "ALL DRIVES (every local drive is visible as C: D: ...)" : "single directory");
    if (!g_cfg.allDrives) printf("  root     : %s\n", wToU8(g_cfg.rootDir).c_str());
    printf("  port     : %d\n", g_cfg.port);
    printf("  login    : %s / %s%s\n", g_cfg.user.c_str(),
           g_cfg.pass.empty() ? "(empty)" : g_cfg.pass.c_str(),
           g_cfg.anon ? "   (+anonymous, read-only)" : "");
    printf("  access   : %s\n", g_cfg.readonly ? "read-only" : "read + write");
    printf("  clients  : max %d, idle timeout %d s\n", g_cfg.maxClients, g_cfg.idleSec);
    printf("  data ports: %d-%d (passive data channel; firewall must allow it too)\n",
           g_cfg.pasvLow, g_cfg.pasvHigh);
    printf("  progress : %s\n", g_progEnabled
           ? (g_consoleOut ? "on  (live speed, rewriting 1-2 lines)"
                           : "on  (stdout redirected: one line every 5 s)")
           : "off");
    printf("\n");

    if (doAddFw) addFirewallRule(g_cfg.port);

    std::string err;
    if (!engineStart((unsigned short)g_cfg.port, err)) {
        printf("ERROR: %s\n", err.c_str());
        return 1;
    }

    std::vector<std::string> ips;
    collectLocalIps(ips);
    printf("  listening. open one of these in Explorer / FileZilla:\n");
    for (size_t i = 0; i < ips.size(); ++i)
        printf("    ftp://%s:%d/\n", ips[i].c_str(), g_cfg.port);
    if (ips.empty()) printf("    ftp://127.0.0.1:%d/\n", g_cfg.port);
    printf("\n  press Ctrl+C to stop.\n\n");

    logf("server started: port=%d mode=%s root=%s auth=%s",
         g_cfg.port, g_cfg.allDrives ? "all-drives" : "single-root",
         g_cfg.allDrives ? "*" : wToU8(g_cfg.rootDir).c_str(), g_cfg.user.c_str());

    while (g_run) Sleep(200);

    engineStop();
    if (g_log) { fclose(g_log); g_log = nullptr; }
    printf("\nstopped.\n");
    return 0;
}
