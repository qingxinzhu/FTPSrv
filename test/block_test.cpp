// ============================================================================
//  --block (read/write block size) regression test.
//
//  --block KB decides how much data a single read/write call moves, and uploads
//  now gather a whole block before touching the disk (a spinning disk seeks far
//  less that way).  The value can arrive from three places - the command line,
//  the GUI and ftpsrv.ini - so it is clamped in one place and the buffer size is
//  computed in one place.  This test extracts both helpers straight out of
//  ftpsrv_engine.inc (so they can never go stale).
// ============================================================================
#include <cstdio>
#include <cstddef>

// the engine's real Config has many more fields; only this one matters here
struct Config { int blockKB = 256; };
static Config g_cfg;

#include "block_engine.inc"

static int g_fail = 0, g_pass = 0;
static void check(const char* what, bool cond)
{
    if (cond) { ++g_pass; printf("  ok   %s\n", what); }
    else      { ++g_fail; printf("  FAIL %s\n", what); }
}
static void setKB(int kb) { g_cfg.blockKB = kb; clampBlock(); }

int main()
{
    printf("== read/write block size ==\n");

    setKB(256);
    check("256 KB -> 262144 bytes", blockBytes() == 262144u);
    setKB(1024);
    check("1024 KB -> 1048576 bytes", blockBytes() == 1048576u);
    setKB(64);
    check("64 KB -> 65536 bytes", blockBytes() == 65536u);

    setKB(15);
    check("15 clamps up to 16 KB", g_cfg.blockKB == 16 && blockBytes() == 16u * 1024);
    setKB(16);
    check("16 is allowed as is", g_cfg.blockKB == 16);
    setKB(0);
    check("0 clamps up to 16 KB", g_cfg.blockKB == 16);
    setKB(-5);
    check("negative clamps up to 16 KB", g_cfg.blockKB == 16);

    setKB(4096);
    check("4096 is allowed as is", g_cfg.blockKB == 4096);
    setKB(8192);
    check("8192 clamps down to 4096 KB", g_cfg.blockKB == 4096);
    setKB(100000);
    check("100000 clamps down to 4096 KB", g_cfg.blockKB == 4096);

    // the buffer must never be zero, even if something skips the clamps
    g_cfg.blockKB = 0;
    check("unclamped 0 still yields a 256 KB buffer", blockBytes() == 262144u);

    printf("== %d passed, %d failed ==\n", g_pass, g_fail);
    if (g_fail == 0) printf("ALL TESTS PASSED\n");
    return g_fail ? 1 : 0;
}