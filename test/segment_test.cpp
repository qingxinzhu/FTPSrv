// ============================================================================
//  Multi-thread / segmented-upload regression test.
//
//  A client with "multi-threaded transfer" on (MT file manager, most download
//  managers) splits ONE file into segments and pushes each segment on its own
//  connection, every one of them with a REST <offset> in front.  The engine
//  keeps a registry of uploads in flight so that only the FIRST writer of a
//  path may create/truncate it - otherwise a late segment would wipe what its
//  siblings already wrote.
//
//  This test extracts that registry straight out of ftpsrv_engine.inc (so it
//  can never go stale) and checks the bookkeeping rules.
// ============================================================================

#include <string>
#include <vector>
#include <mutex>
#include <cctype>
#include <cwctype>
#include <cstdio>

#include "segment_engine.inc"

static int g_fail = 0, g_pass = 0;

static void check(const char* what, bool cond)
{
    if (cond) { ++g_pass; printf("  ok   %s\n", what); }
    else      { ++g_fail; printf("  FAIL %s\n", what); }
}

int main()
{
    printf("== concurrent upload registry ==\n");

    const std::wstring A = L"C:\\movies\\clip.mp4";
    const std::wstring B = L"D:\\other\\clip.mp4";

    // 1. the first writer of a path is the creator
    check("first writer of a path is the creator", uploadBegin(A) == true);

    // 2. a second connection to the same file must NOT truncate it
    check("2nd parallel segment is not the creator", uploadBegin(A) == false);
    check("3rd parallel segment is not the creator", uploadBegin(A) == false);

    // 3. a different file is independent
    check("a different path is its own creator", uploadBegin(B) == true);

    // 4. Windows paths are case-insensitive
    check("case variant counts as the same file", uploadBegin(L"c:\\MOVIES\\CLIP.MP4") == false);

    // 5. while one of several writers is still active, a newcomer is not the creator
    //    (at this point A has 4 writers in flight: steps 1,2,3 and the case variant)
    uploadEnd(A);                       // one of them finished
    check("still busy after one finishes", uploadBegin(A) == false);
    uploadEnd(A); uploadEnd(A); uploadEnd(A); uploadEnd(A);   // drain the rest

    // 6. nobody left -> the next writer may create/truncate again
    check("creator again once all writers are gone", uploadBegin(A) == true);
    uploadEnd(A);

    // 7. finishing an unknown path is harmless, and B is still in flight
    uploadEnd(L"Z:\\never\\opened.bin");
    check("uploadEnd on an unknown path is a no-op", uploadBegin(B) == false);
    uploadEnd(B); uploadEnd(B);         // both B writers from steps 4 and 7

    check("registry drains back to empty", uploadBegin(B) == true);
    uploadEnd(B);

    printf("== %d passed, %d failed ==\n", g_pass, g_fail);
    if (g_fail == 0) { printf("ALL TESTS PASSED\n"); return 0; }
    return 1;
}
