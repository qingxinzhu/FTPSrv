// ==========================================================================
//  longPath regression test.
//
//  Windows file APIs stop at 260 characters unless the path carries the
//  long-path prefix, so every absolute path the server hands to Win32 goes
//  through this helper.  It is pulled straight out of ftpsrv_engine.inc (so it
//  cannot go stale), together with the two constants it builds the prefix from.
//  In the expectations below the at-sign stands for a backslash, which keeps
//  this test clear of escape-sequence confusion.
// ==========================================================================
#include <string>
#include <cstdio>
static std::wstring W(const char* a)
{
    std::wstring r;
    for (; *a; ++a) r += (*a == '@') ? (wchar_t)92 : (wchar_t)*a;
    return r;
}
#include "longpath_engine.inc"
static int g_fail = 0, g_pass = 0;
static void chk(const char* what, const std::wstring& got, const std::wstring& want)
{
    if (got == want) { ++g_pass; printf("  ok   %s\n", what); return; }
    ++g_fail;
    printf("  FAIL %s   (len %zu, want %zu)\n", what, got.size(), want.size());
}
int main()
{
    printf("== long path prefix ==\n");
    chk("drive path gets the prefix",
        longPath(W("C:@a@b@c.txt")), W("@@?@C:@a@b@c.txt"));
    chk("drive root keeps its slash",
        longPath(W("C:@")), W("@@?@C:@"));
    chk("already prefixed is left alone",
        longPath(W("@@?@C:@a@b")), W("@@?@C:@a@b"));
    chk("UNC share becomes a UNC prefix",
        longPath(W("@@srv@share@x")), W("@@?@UNC@srv@share@x"));
    chk("relative path is untouched",
        longPath(W("share@x")), W("share@x"));
    chk("empty stays empty", longPath(W("")), W(""));
    std::wstring deep = W("C:@");
    for (int i = 0; i < 60; ++i) deep += W("folder-name-abcdefghij@");
    std::wstring got = longPath(deep);
    bool ok = got.size() == deep.size() + 4 &&
              got.compare(0, 4, W("@@?@")) == 0 &&
              got.compare(4, std::wstring::npos, deep) == 0;
    if (ok) { ++g_pass; printf("  ok   a 300+ character path gains exactly 4 chars\n"); }
    else    { ++g_fail; printf("  FAIL a 300+ character path\n"); }
    printf("== %d passed, %d failed ==\n", g_pass, g_fail);
    if (!g_fail) printf("ALL TESTS PASSED\n");
    return g_fail ? 1 : 0;
}
