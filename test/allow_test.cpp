// ============================================================================
//  IP allow list regression test  (--allow / 设置页「只允许这些 IP 连」)
//
//  The CIDR maths is kept in three Windows-free helpers inside ftpsrv_engine.inc
//  (ip4ToU32 / bitsToMask / allowMatchNet); this test extracts them straight out
//  of the engine and checks the parsing and matching rules.
// ============================================================================

#include <string>
#include <vector>
#include <cstdio>

#include "allow_engine.inc"

static int g_fail = 0, g_pass = 0;

static void check(const char* what, bool cond)
{
    if (cond) { ++g_pass; printf("  ok   %s\n", what); }
    else      { ++g_fail; printf("  FAIL %s\n", what); }
}

static void addNet(const char* spec, int bits)
{
    unsigned int a = 0;
    if (!ip4ToU32(spec, a)) { printf("  (bad test spec %s)\n", spec); return; }
    g_allowNet.push_back(a & bitsToMask(bits));
    g_allowBits.push_back(bits);
}

int main()
{
    printf("== IP allow list ==\n");

    unsigned int a = 0;
    check("parses a plain address", ip4ToU32("192.168.1.13", a) && a == 0xC0A8010Du);
    check("parses 0.0.0.0",         ip4ToU32("0.0.0.0", a) && a == 0u);
    check("rejects 300.1.1.1",     !ip4ToU32("300.1.1.1", a));
    check("rejects 1.2.3",         !ip4ToU32("1.2.3", a));
    check("rejects 1.2.3.4.5",     !ip4ToU32("1.2.3.4.5", a));
    check("rejects junk",          !ip4ToU32("hello", a));

    check("/0 mask covers everything",  bitsToMask(0)  == 0u);
    check("/32 mask is exact",          bitsToMask(32) == 0xFFFFFFFFu);
    check("/24 mask",                   bitsToMask(24) == 0xFFFFFF00u);
    check("/8 mask",                    bitsToMask(8)  == 0xFF000000u);

    addNet("192.168.1.0", 24);
    addNet("10.1.2.3", 32);

    check("192.168.1.13 matches 192.168.1.0/24", ip4ToU32("192.168.1.13", a) && allowMatchNet(a));
    check("192.168.1.255 matches 192.168.1.0/24", ip4ToU32("192.168.1.255", a) && allowMatchNet(a));
    check("192.168.2.13 does NOT match",       ip4ToU32("192.168.2.13", a) && !allowMatchNet(a));
    check("any single address matches itself", ip4ToU32("10.1.2.3", a) && allowMatchNet(a));
    check("neighbour address does NOT match",  ip4ToU32("10.1.2.4", a) && !allowMatchNet(a));

    g_allowNet.clear();
    g_allowBits.clear();
    addNet("0.0.0.0", 0);
    check("/0 matches anything", ip4ToU32("8.8.8.8", a) && allowMatchNet(a));

    printf("== %d passed, %d failed ==\n", g_pass, g_fail);
    if (g_fail == 0) { printf("ALL TESTS PASSED\n"); return 0; }
    return 1;
}