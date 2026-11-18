#include <windows.h>

#include <vector>

#include "game.h"
#include "mod.h"

namespace {

struct Region {
    const char *name;
    uint32_t off, words, stride, count;
};
const Region kRegions[] = {
    {"gsf", 0xAC6E4, 48, 0, 1},
    {"quest", 0xAC7A4, 384, 0, 1},
    {"npcdie", 0xACFEC, 29, 0, 1},
    {"stage", 0xADEE8, 82, 0x14C, 65},
};

const uint32_t kSkipOffs[] = {0xAC720};
constexpr DWORD kStableMs = 3000;

std::vector<uint32_t> g_offs;
std::vector<uint32_t> g_local;
std::vector<uint32_t> g_base;
std::vector<uint8_t> g_known;
std::vector<uint32_t> g_cur;
std::vector<DWORD> g_since;
DWORD g_seen_load = 0;
CRITICAL_SECTION g_lock;
std::vector<FlagEv> g_mine;

void build()
{
    for (const Region &r : kRegions)
        for (uint32_t c = 0; c < r.count; ++c)
            for (uint32_t w = 0; w < r.words; ++w) {
                g_offs.push_back(r.off + c * r.stride + w * 4);
                g_local.push_back(r.off == 0xADEE8 ? stage_variant_mask(int(c), int(w)) : 0);
            }
    g_base.assign(g_offs.size(), 0);
    g_known.assign(g_offs.size(), 0);
    g_cur.assign(g_offs.size(), 0);
    g_since.assign(g_offs.size(), 0);
}

bool skipped(uint32_t off)
{
    for (uint32_t s : kSkipOffs)
        if (s == off)
            return true;
    return false;
}

const char *where(uint32_t i, uint32_t &sub)
{
    uint32_t n = 0;
    for (const Region &r : kRegions) {
        uint32_t size = r.words * r.count;
        if (i < n + size) {
            sub = i - n;
            return r.name;
        }
        n += size;
    }
    sub = i;
    return "?";
}

}

bool flags_install()
{
    InitializeCriticalSection(&g_lock);
    build();
    logf("flags: watching %u words of sGameSys (quests, game, NPC deaths, %d areas' flags, items and objects)",
         unsigned(g_offs.size()), int(kRegions[3].count));
    return true;
}

void flags_scan()
{
    uintptr_t gs = game::at<uintptr_t>(game::kGameMain, 0);
    int32_t stage = 0;
    DWORD load = stages_last_load(&stage);
    if (!gs || !current_stage_area() || g_offs.empty())
        return;
    bool rebase = load != g_seen_load;
    if (rebase) {
        if (GetTickCount() - load < 2000)
            return;
        g_seen_load = load;
    }
    int logged = 0;
    DWORD now = GetTickCount() | 1;
    for (size_t i = 0; i < g_offs.size(); ++i) {
        uint32_t v = game::at<uint32_t>(gs, g_offs[i]) & ~g_local[i];
        EnterCriticalSection(&g_lock);
        uint32_t b = g_base[i];
        if (rebase || !g_known[i] || skipped(g_offs[i])) {
            g_base[i] = g_cur[i] = v;
            g_known[i] = 1;
            g_since[i] = 0;
            LeaveCriticalSection(&g_lock);
            continue;
        }
        if (v != g_cur[i]) {
            g_cur[i] = v;
            g_since[i] = v != b ? now : 0;
        }
        if (!g_since[i] || int(now - g_since[i]) < int(kStableMs)) {
            LeaveCriticalSection(&g_lock);
            continue;
        }
        FlagEv e = {uint16_t(i), v & ~b, b & ~v};
        g_base[i] = v;
        g_since[i] = 0;
        g_mine.push_back(e);
        LeaveCriticalSection(&g_lock);
        uint32_t sub = 0;
        const char *t = where(uint32_t(i), sub);
        if (t[0] == 'q')
            quest_sync_local_change();
        if (logged++ < 20)
            logf("flags: %s word %u: %08X -> %08X (set %08X, cleared %08X), sent to the other players", t, sub, b, v,
                 e.set, e.clr);
    }
    if (rebase)
        logf("flags: starting point taken after a load (stage %d)", stage);
}

uint32_t flags_mine_count()
{
    EnterCriticalSection(&g_lock);
    uint32_t n = uint32_t(g_mine.size());
    LeaveCriticalSection(&g_lock);
    return n;
}

void flags_net_mine(std::vector<FlagEv> &out, uint32_t from_seq, int max)
{
    out.clear();
    EnterCriticalSection(&g_lock);
    for (uint32_t s = from_seq; s >= 1 && s <= g_mine.size() && int(out.size()) < max; ++s)
        out.push_back(g_mine[s - 1]);
    LeaveCriticalSection(&g_lock);
}

void flags_net_apply(const FlagEv &e)
{
    uintptr_t gs = game::at<uintptr_t>(game::kGameMain, 0);
    if (!gs || e.index >= g_offs.size() || !current_stage_area())
        return;
    uint32_t set = e.set & ~g_local[e.index], clr = e.clr & ~g_local[e.index];
    if (!set && !clr)
        return;
    auto *w = reinterpret_cast<volatile LONG *>(gs + g_offs[e.index]);
    if (set)
        InterlockedOr(w, LONG(set));
    if (clr)
        InterlockedAnd(w, LONG(~clr));
    EnterCriticalSection(&g_lock);
    g_base[e.index] = (g_base[e.index] | set) & ~clr;
    g_cur[e.index] = game::at<uint32_t>(gs, g_offs[e.index]) & ~g_local[e.index];
    g_since[e.index] = 0;
    LeaveCriticalSection(&g_lock);
    uint32_t sub = 0;
    const char *t = where(e.index, sub);
    logf("flags: from another player: %s word %u set %08X cleared %08X", t, sub, set, clr);
}
