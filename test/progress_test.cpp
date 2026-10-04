// harness: run the REAL progress engine from ftpsrv.cpp against a fake ANSI terminal
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <cstdarg>
#include <ctime>
#include <string>
#include <vector>
#include <mutex>
#include <unistd.h>

// ------------------------- tiny windows shims -------------------------
typedef unsigned long DWORD;
typedef void*         HANDLE;
#define INVALID_HANDLE_VALUE ((HANDLE)(long)-1)
#define STD_OUTPUT_HANDLE    ((DWORD)-11)

struct FILETIME { uint32_t dwLowDateTime; uint32_t dwHighDateTime; };
union ULARGE_INTEGER {
    struct { uint32_t LowPart; uint32_t HighPart; };
    unsigned long long QuadPart;
};

static void GetSystemTimeAsFileTime(FILETIME* ft)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    unsigned long long t = (unsigned long long)ts.tv_sec * 10000000ULL
                         + 116444736000000000ULL
                         + (unsigned long long)ts.tv_nsec / 100ULL;
    ft->dwLowDateTime  = (uint32_t)(t & 0xffffffffULL);
    ft->dwHighDateTime = (uint32_t)(t >> 32);
}
static HANDLE GetStdHandle(DWORD) { return (HANDLE)1; }
static int GetConsoleMode(HANDLE, DWORD* m) { if (m) *m = 0; return 1; }
static int SetConsoleMode(HANDLE, DWORD) { return 1; }

// ------------------------- fake terminal -------------------------
struct Term {
    int W = 200;
    std::vector<std::string> rows;
    int r = 0, c = 0;

    Term() { rows.push_back(std::string(W, ' ')); }

    void ensure(int rr) { while ((int)rows.size() <= rr) rows.push_back(std::string(W, ' ')); }

    void feed(const char* p, size_t n)
    {
        size_t i = 0;
        while (i < n) {
            char ch = p[i];
            if (ch == 0x1b && i + 1 < n && p[i + 1] == '[') {
                size_t j = i + 2;
                int num = 0; bool has = false;
                while (j < n && p[j] >= '0' && p[j] <= '9') { num = num * 10 + (p[j] - '0'); has = true; ++j; }
                if (j < n) {
                    char f = p[j];
                    int v = has ? num : 1;
                    if (f == 'A') { r -= v; if (r < 0) r = 0; }
                    else if (f == 'K') { ensure(r); for (int k = c; k < W; ++k) rows[r][k] = ' '; }
                    i = j + 1;
                    continue;
                }
            }
            if (ch == '\r') { c = 0; ++i; continue; }
            if (ch == '\n') { ++r; ensure(r); c = 0; ++i; continue; }
            ensure(r);
            if (c < W) rows[r][c] = ch;
            ++c; ++i;
        }
    }

    std::string rowStr(int i) const
    {
        if (i < 0 || i >= (int)rows.size()) return std::string();
        std::string s = rows[i];
        while (!s.empty() && s.back() == ' ') s.pop_back();
        return s;
    }
    int lastNonBlank() const
    {
        for (int i = (int)rows.size() - 1; i >= 0; --i)
            if (!rowStr(i).empty()) return i;
        return -1;
    }
    int countNonBlank() const
    {
        int n = 0;
        for (int i = 0; i < (int)rows.size(); ++i) if (!rowStr(i).empty()) ++n;
        return n;
    }
};

static Term  g_term;
static FILE* g_out = nullptr;

static ssize_t cookieWrite(void*, const char* buf, size_t n)
{
    g_term.feed(buf, n);
    return (ssize_t)n;
}
static FILE* makeTermOut()
{
    cookie_io_functions_t fns;
    memset(&fns, 0, sizeof(fns));
    fns.write = cookieWrite;
    return fopencookie(nullptr, "w", fns);
}

#undef stdout
#define stdout g_out
#define printf(...) fprintf(g_out, __VA_ARGS__)

// ------------------------- the real code under test -------------------------
#include "prog_engine.inc"

// faithful copy of the logging path from ftpsrv.cpp
static std::mutex g_logMx;

static void logf(const char* fmt, ...)
{
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    std::lock_guard<std::mutex> lk(g_logMx);
    {
        std::lock_guard<std::mutex> pk(g_progMx);
        progErase_locked();
    }
    printf("[log] %s\n", buf);
    fflush(stdout);
    {
        std::lock_guard<std::mutex> pk(g_progMx);
        progDraw_locked();
    }
}

// ------------------------- tests -------------------------
static int g_fail = 0;

static void check(const char* what, bool ok, const std::string& detail = std::string())
{
    fprintf(stderr, "  %-58s %s%s\n", what, ok ? "PASS" : "FAIL",
            detail.empty() ? "" : ("   [" + detail + "]").c_str());
    if (!ok) ++g_fail;
}

int main()
{
    g_out = makeTermOut();
    setvbuf(g_out, nullptr, _IONBF, 0);      // no buffering: every write hits the fake screen
    enableAnsiConsole();

    printf("FTPSrv - LAN FTP file server\n");
    printf("  listening. open one of these in Explorer\n");
    fflush(g_out);

    logf("server started: port=2121");
    fflush(g_out);

    int base = g_term.lastNonBlank();
    fprintf(stderr, "\n== baseline: last content line = %d ==\n", base);
    fprintf(stderr, "   row%d: %s\n", base, g_term.rowStr(base).c_str());

    // --- A: single slot, many redraws, must never add lines ---
    int s1 = progBegin(false, "RETR ubuntu-24.04.iso", 300ULL * 1024 * 1024);
    check("A1 progBegin draws exactly one line below the log",
          g_term.lastNonBlank() == base + 1, g_term.rowStr(base + 1));

    for (int i = 0; i < 40; ++i) {
        progTick(s1, (unsigned long long)(i + 1) * 5ULL * 1024 * 1024);
        usleep(60000);
    }
    check("A2 after 40 redraws no new line was consumed (no scroll-spam)",
          g_term.lastNonBlank() == base + 1, g_term.rowStr(base + 1));
    check("A3 the progress line has a bar + speed, and no ETA prediction",
          g_term.rowStr(base + 1).find("[") != std::string::npos &&
          g_term.rowStr(base + 1).find("/s") != std::string::npos &&
          g_term.rowStr(base + 1).find("ETA") == std::string::npos,
          g_term.rowStr(base + 1));
    check("A4 earlier log lines are untouched",
          g_term.rowStr(base).find("server started") != std::string::npos,
          g_term.rowStr(base));

    // --- B: second concurrent transfer -> second line ---
    int s2 = progBegin(true, "STOR photo-collection.zip", 0);
    check("B1 second transfer joins on the next line",
          g_term.lastNonBlank() == base + 2, g_term.rowStr(base + 2));
    for (int i = 0; i < 12; ++i) {
        progTick(s2, (unsigned long long)(i + 1) * 700ULL * 1024);
        usleep(70000);
    }
    check("B2 upload line shows bytes + speed (no ETA when size unknown)",
          g_term.rowStr(base + 2).find("UP") != std::string::npos &&
          g_term.rowStr(base + 2).find("ETA") == std::string::npos,
          g_term.rowStr(base + 2));
    check("B3 still only two progress lines in total",
          g_term.lastNonBlank() == base + 2);

    // --- C: second transfer ends -> line must be cleared again ---
    progEnd(s2, 8ULL * 1024 * 1024, true);
    fflush(g_out);
    bool upGone = g_term.rowStr(base + 2).find("UP") == std::string::npos;
    check("C1 finished upload line is gone", upGone, g_term.rowStr(base + 2));
    std::string tail = g_term.rowStr(g_term.lastNonBlank());
    check("C2 block compacted: the live download line is now the last line",
          tail.find("DOWN RETR") != std::string::npos && tail.find("[") != std::string::npos,
          tail);

    // --- D: a log during transfer must not eat the progress area ---
    int before = g_term.lastNonBlank();
    logf("connect: 192.168.1.31");
    fflush(g_out);
    check("D1 log line printed, progress redrawn below it",
          g_term.lastNonBlank() == before + 1 &&
          g_term.rowStr(before).find("connect") != std::string::npos &&
          g_term.rowStr(before + 1).find("DOWN") != std::string::npos,
          g_term.rowStr(before) + " | " + g_term.rowStr(before + 1));

    // --- E: all transfers done -> progress lines vanish ---
    progEnd(s1, 200ULL * 1024 * 1024, true);
    fflush(g_out);
    std::string lastLine = g_term.rowStr(g_term.lastNonBlank());
    bool anyBar = false;
    for (int i = 0; i <= g_term.lastNonBlank(); ++i)
        if (g_term.rowStr(i).find("ETA") != std::string::npos) anyBar = true;
    check("E1 no live progress line is left behind (no bar / ETA anywhere)",
          !anyBar, lastLine);
    check("E2 summary carries size + duration + average speed",
          lastLine.find("DOWN RETR") != std::string::npos &&
          lastLine.find("in ") != std::string::npos &&
          lastLine.find("/s)") != std::string::npos,
          lastLine);

    // --- F: redirected stdout fallback (no console) ---
    g_consoleOut = false;
    int rowsBefore = g_term.countNonBlank();
    int s3 = progBegin(false, "RETR plain.txt", 0);
    for (int i = 0; i < 5; ++i) { progTick(s3, (i + 1) * 1024); }
    check("F1 redirected stdout: no ANSI was written for a fresh transfer",
          g_term.countNonBlank() == rowsBefore, std::to_string(g_term.countNonBlank()));
    progEnd(s3, 4096, true);
    g_consoleOut = true;

    fprintf(stderr, "\n== final screen (what the console would show) ==\n");
    for (int i = 0; i <= g_term.lastNonBlank(); ++i)
        fprintf(stderr, "  %2d | %s\n", i, g_term.rowStr(i).c_str());

    fprintf(stderr, "\n%s (%d failure%s)\n", g_fail == 0 ? "ALL TESTS PASSED" : "TESTS FAILED",
            g_fail, g_fail == 1 ? "" : "s");
    return g_fail == 0 ? 0 : 1;
}