#include <windows.h>

#include <deque>
#include <vector>

#include "MinHook.h"
#include "game.h"
#include "mod.h"

namespace {

using AddItem_t = int(__attribute__((thiscall)) *)(void *mgr, unsigned item, unsigned count, int who, int a5,
                                                    unsigned flags, unsigned a7);
const uintptr_t kAddItem = game::va(0x0045E680), kItemManager = game::va(0x018FA4D4), kPickupCall = game::va(0x00C29471);
const uintptr_t kMoveCall = game::va(0x0045FB56), kShopCall = game::va(0x006E676E);
constexpr unsigned kGift = 0x80000000u, kItemFlags = 0x00FFFFFFu;
struct Queued { LootEv e; int from; };
AddItem_t g_orig = nullptr;
thread_local bool g_applying = false;
CRITICAL_SECTION g_lock;
std::vector<LootEv> g_mine;
std::deque<Queued> g_queue;

int __attribute__((thiscall)) hk_add_item(void *mgr, unsigned item, unsigned count, int who, int a5, unsigned flags, unsigned a7)
{
    int r = g_orig(mgr, item, count, who, a5, flags, a7);
    uintptr_t from = uintptr_t(__builtin_return_address(0));
    if (g_applying)
        return r;
    if (cfg_debug_log())
        logf("loot: add item %u x%u to %d (a5 %d, flags %X, a7 %X) from %08X -> %d", item, count, who, a5, flags, a7,
             unsigned(from), r);
    if (net_shared() && from == kMoveCall && r && who >= 1 && who <= 3 && net_puppet(who) && item <= 0xFFFF && count) {
        int to = net_party_slot(who);
        if (to >= 0 && to <= 3 && to != net_my_slot()) {
            LootEv g = {uint16_t(item), uint16_t(count > 999 ? 999 : count), (flags & kItemFlags) | kGift | (unsigned(to) << 24)};
            EnterCriticalSection(&g_lock);
            g_mine.push_back(g);
            LeaveCriticalSection(&g_lock);
            logf("loot: item %u x%u given to the pawn showing player %d: sent to that player", item, count, to);
            if (net_role() == NET_GUEST)
                progress_shared_gain(0, 0, int(item), -int(g.count), flags & kItemFlags);
        }
        return r;
    }
    if (net_shared() && from == kShopCall && r && who == 0 && net_role() == NET_GUEST && item <= 0xFFFF && count) {
        logf("loot: bought item %u x%u: kept for this player's own save", item, count);
        progress_shared_gain(0, 0, int(item), int(count > 999 ? 999 : count), flags);
        return r;
    }
    bool ours = who == 0 || (who >= 1 && who <= 3 && !net_puppet(who));
    if (!net_shared() || !ours || from != kPickupCall || !r || item > 0xFFFF || !count)
        return r;
    LootEv e = {uint16_t(item), uint16_t(count > 999 ? 999 : count), flags};
    EnterCriticalSection(&g_lock);
    g_mine.push_back(e);
    size_t n = g_mine.size();
    LeaveCriticalSection(&g_lock);
    logf("loot: picked up item %u x%u (pickup %u), a copy goes to the other players", item, count, unsigned(n));
    if (net_role() == NET_GUEST)
        progress_shared_gain(0, 0, int(item), int(e.count), flags);
    return r;
}

using AddGold_t = void(__attribute__((thiscall)) *)(void *self, int amount);
const uintptr_t kAddGold = game::va(0x006E08D0);
AddGold_t g_orig_gold = nullptr;
volatile LONG g_sale_gold = 0;

void __attribute__((thiscall)) hk_add_gold(void *self, int amount)
{
    uintptr_t from = uintptr_t(__builtin_return_address(0));
    g_orig_gold(self, amount);
    bool sale = from >= game::va(0x006E3300) && from < game::va(0x006E3400);
    if (cfg_debug_log())
        logf("loot: gold +%d from %08X%s", amount, unsigned(from), sale ? " (a sale)" : "");
    if (sale && amount > 0 && net_role() == NET_GUEST)
        InterlockedExchangeAdd(&g_sale_gold, amount);
}

}

long loot_take_sale_gold() { return InterlockedExchange(&g_sale_gold, 0); }

bool loot_install()
{
    InitializeCriticalSection(&g_lock);
    {
        MH_STATUS gs = MH_CreateHook(reinterpret_cast<void *>(kAddGold), reinterpret_cast<void *>(&hk_add_gold),
                                     reinterpret_cast<void **>(&g_orig_gold));
        if (gs == MH_OK)
            gs = MH_EnableHook(reinterpret_cast<void *>(kAddGold));
        logf("loot: hook AddGold %08X: %s", unsigned(kAddGold), MH_StatusToString(gs));
    }
    MH_STATUS st = MH_CreateHook(reinterpret_cast<void *>(kAddItem), reinterpret_cast<void *>(&hk_add_item),
                                 reinterpret_cast<void **>(&g_orig));
    if (st == MH_OK)
        st = MH_EnableHook(reinterpret_cast<void *>(kAddItem));
    logf("loot: hook AddItem %08X: %s", unsigned(kAddItem), MH_StatusToString(st));
    return st == MH_OK;
}

uint32_t loot_mine_count()
{
    EnterCriticalSection(&g_lock);
    uint32_t n = uint32_t(g_mine.size());
    LeaveCriticalSection(&g_lock);
    return n;
}

void loot_net_mine(std::vector<LootEv> &out, uint32_t from_seq, int max)
{
    out.clear();
    EnterCriticalSection(&g_lock);
    for (uint32_t s = from_seq; s >= 1 && s <= g_mine.size() && int(out.size()) < max; ++s)
        out.push_back(g_mine[s - 1]);
    LeaveCriticalSection(&g_lock);
}

void loot_net_apply(const LootEv &e, int from_slot)
{
    EnterCriticalSection(&g_lock);
    g_queue.push_back(Queued{e, from_slot});
    LeaveCriticalSection(&g_lock);
}

void loot_apply_queued()
{
    if (!g_orig)
        return;
    for (;;) {
        EnterCriticalSection(&g_lock);
        if (g_queue.empty()) {
            LeaveCriticalSection(&g_lock);
            return;
        }
        Queued q = g_queue.front();
        g_queue.pop_front();
        LeaveCriticalSection(&g_lock);
        LootEv e = q.e;
        void *mgr = game::at<void *>(kItemManager, 0);
        int r = 0;
        if (e.flags & kGift) {
            int to = int((e.flags >> 24) & 3);
            if (to != net_my_slot())
                continue;
            if (mgr) {
                g_applying = true;
                r = g_orig(mgr, e.item, e.count, 0, 0, e.flags & kItemFlags, 1);
                g_applying = false;
            }
            logf("loot: player %d gave this player item %u x%u: put in its bag -> %d", q.from, e.item, e.count, r);
            if (net_role() == NET_GUEST && q.from != 0 && r)
                progress_shared_gain(0, 0, e.item, e.count, e.flags & kItemFlags);
            continue;
        }
        if (mgr) {
            g_applying = true;
            r = g_orig(mgr, e.item, e.count, 4, 0, e.flags, 1);
            g_applying = false;
        }
        logf("loot: a copy of another player's item %u x%u put in the storage -> %d", e.item, e.count, r);
        if (net_role() == NET_GUEST)
            progress_shared_gain(0, 0, e.item, e.count, e.flags);
    }
}
