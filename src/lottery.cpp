#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "MinHook.h"
#include "game.h"
#include "mod.h"

namespace {

constexpr uintptr_t kApplyMasks = 0x004A2D30, kSpecialDraw = 0x004A2900, kStageRoll = 0x0052F380;
constexpr uintptr_t kStageTable = 0x01530348;
constexpr int kStages = 65, kNsiCount = 25;
constexpr uint32_t kStageFlags = 0xADEE8, kStageStride = 0x14C;
constexpr DWORD kPeerFresh = 6000;

using Masks_t = void(__fastcall *)(uintptr_t, void *, int32_t, uintptr_t, uintptr_t, uint8_t *);
using Special_t = void(__stdcall *)(uintptr_t, int32_t);
using Roll_t = void(__fastcall *)(uintptr_t, void *);
Masks_t g_masks_orig = nullptr;
Special_t g_special_orig = nullptr;
Roll_t g_roll_orig = nullptr;

CRITICAL_SECTION g_lk;
bool g_off = false;
uint32_t g_loads = 0;
LotteryRecord g_mine = {};
DWORD g_mine_made = 0;
bool g_mine_needs_draw = false;
bool g_kind0_wait = false;
DWORD g_kind0_hold = 0;
DWORD g_kind0_at = 0;
thread_local int32_t t_roll_stage = -1;
thread_local uint32_t t_roll_before[3];
thread_local bool t_roll_entered = false;
LotteryRecord g_peer[4] = {};
DWORD g_peer_rx[4] = {};
int g_from = -1;
LotteryRecord g_from_rec = {};
bool g_saved_logged = false;

uintptr_t nsi_at(uintptr_t mgr, int idx) { return mgr + 0x2290 + uintptr_t(idx) * 0xBE0; }
int nsi_stage(int idx) { return game::at<uint16_t>(kStageTable, uintptr_t(idx + 0x28) * 2); }
int stage_number(int idx) { return game::at<uint16_t>(kStageTable, uintptr_t(idx) * 2); }
int stage_index(int32_t stage)
{
    for (int i = 0; i < kStages; ++i)
        if (stage_number(i) == stage)
            return i;
    return -1;
}

uintptr_t stage_flags(int idx)
{
    uintptr_t gs = game::at<uintptr_t>(game::kGameMain, 0);
    return gs && idx >= 0 && idx < kStages ? gs + kStageFlags + uintptr_t(idx) * kStageStride : 0;
}
bool read_variant(int idx, uint32_t v[3])
{
    uintptr_t f = stage_flags(idx);
    bool any = false;
    for (int k = 0; k < 3; ++k) {
        uint32_t m = stage_variant_mask(idx, k + 1);
        v[k] = f && m ? game::at<uint32_t>(f, uintptr_t(k + 1) * 4) & m : 0;
        any |= m != 0;
    }
    return any && f;
}
void write_variant(int idx, const uint32_t v[3])
{
    uintptr_t f = stage_flags(idx);
    for (int k = 0; f && k < 3; ++k) {
        uint32_t m = stage_variant_mask(idx, k + 1);
        if (!m)
            continue;
        auto *w = reinterpret_cast<volatile LONG *>(f + uintptr_t(k + 1) * 4);
        LONG old = *w, was;
        while ((was = InterlockedCompareExchange(w, LONG((uint32_t(old) & ~m) | (v[k] & m)), old)) != old)
            old = was;
    }
}
void variant_text(int idx, const uint32_t v[3], char *out, size_t cap)
{
    int n = 0;
    out[0] = 0;
    for (int k = 0; k < 3; ++k)
        for (int b = 0; b < 32; ++b)
            if ((stage_variant_mask(idx, k + 1) & v[k] & (0x80000000u >> b)) && n >= 0 && size_t(n) < cap)
                n += _snprintf(out + n, cap - n, n ? " %d" : "%d", (k + 1) * 32 + b);
    if (n <= 0)
        _snprintf(out, cap, "none");
}

void read_draw(uintptr_t nsi, LotteryRecord &r)
{
    r.reset_hour = game::at<uint32_t>(nsi, 4);
    memcpy(r.groups, reinterpret_cast<void *>(nsi + 8), sizeof r.groups);
    for (int i = 0; i < 32; ++i) {
        r.est_hit[i] = game::at<uint32_t>(nsi, 0x48 + i * 12 + 4);
        r.est_group[i] = game::at<uint32_t>(nsi, 0x48 + i * 12 + 8);
    }
}
void write_draw(uintptr_t nsi, const LotteryRecord &r)
{
    game::at<uint32_t>(nsi, 4) = r.reset_hour;
    memcpy(reinterpret_cast<void *>(nsi + 8), r.groups, sizeof r.groups);
    for (int i = 0; i < 32; ++i) {
        game::at<uint32_t>(nsi, 0x48 + i * 12 + 4) = r.est_hit[i];
        game::at<uint32_t>(nsi, 0x48 + i * 12 + 8) = r.est_group[i];
    }
}

void draw_text(const LotteryRecord &r, char *out, size_t cap)
{
    int n = _snprintf(out, cap, "hour %u, buckets", unsigned(r.reset_hour));
    bool any = false;
    for (int b = 0; b < 32 && n > 0 && size_t(n) < cap; ++b)
        if ((r.groups[b] & 0x1FF) != 0x1FF) {
            n += _snprintf(out + n, cap - n, " %d:e%02u", b, unsigned(r.groups[b] & 0x1FF));
            any = true;
        }
    if (!any && n > 0 && size_t(n) < cap)
        n += _snprintf(out + n, cap - n, " none");
    if (n > 0 && size_t(n) < cap)
        n += _snprintf(out + n, cap - n, ", specials");
    any = false;
    for (int i = 0; i < 32 && n > 0 && size_t(n) < cap; ++i)
        if (r.est_hit[i]) {
            n += _snprintf(out + n, cap - n, " %d:e%02u", i, unsigned(r.est_group[i]));
            any = true;
        }
    if (!any && n > 0 && size_t(n) < cap)
        _snprintf(out + n, cap - n, " none");
}
void masks_text(const uint8_t *m, char *out, size_t cap)
{
    uint64_t v[4];
    memcpy(v, m, sizeof v);
    _snprintf(out, cap, "%016llX %016llX %016llX %016llX", (unsigned long long)v[0], (unsigned long long)v[1],
              (unsigned long long)v[2], (unsigned long long)v[3]);
}
bool same_draw(const LotteryRecord &a, const LotteryRecord &b)
{
    return a.reset_hour == b.reset_hour && memcmp(a.groups, b.groups, sizeof a.groups) == 0 &&
           memcmp(a.est_hit, b.est_hit, sizeof a.est_hit) == 0 && memcmp(a.est_group, b.est_group, sizeof a.est_group) == 0;
}

void log_saved(uintptr_t mgr)
{
    for (int idx = 0; idx < kNsiCount; ++idx) {
        LotteryRecord r = {};
        read_draw(nsi_at(mgr, idx), r);
        bool any = false;
        for (int b = 0; b < 32; ++b)
            any |= (r.groups[b] & 0x1FF) != 0x1FF;
        for (int i = 0; i < 32; ++i)
            any |= r.est_hit[i] != 0;
        if (!any)
            continue;
        char t[400];
        draw_text(r, t, sizeof t);
        logf("lottery: saved draw of stage %d: %s", nsi_stage(idx), t);
    }
}

void log_saved_kills(uintptr_t mgr, int32_t stage, uint32_t load, bool groups);
void log_respawn_list(uintptr_t mgr, uintptr_t list, int32_t stage, uint32_t load)
{
    if (list != mgr + 0x19D00)
        return;
    log_saved_kills(mgr, stage, load, true);
    log_saved_kills(mgr, stage, load, false);
}
void log_saved_kills(uintptr_t mgr, int32_t stage, uint32_t load, bool groups)
{
    uintptr_t list = mgr + (groups ? 0x18A90 : 0x19D00), end = groups ? mgr + 0x19D00 : mgr + 0x1B8D0;
    if (!mem_readable(reinterpret_cast<void *>(list), end - list))
        return;
    int idx = stage_index(stage);
    uint32_t n = 0, here = 0, h = 2166136261u;
    char mine[600] = "";
    int len = 0;
    for (uintptr_t e = list + 8; e + 16 <= end; e += 16) {
        uint32_t a = game::at<uint32_t>(e, 0), b = game::at<uint32_t>(e, 4);
        if (!(a & 1))
            continue;
        ++n;
        for (uint32_t v : {a, b})
            for (int k = 0; k < 4; ++k)
                h = (h ^ ((v >> (k * 8)) & 0xFF)) * 16777619u;
        if (int((a >> 1) & 0x7F) == idx) {
            ++here;
            if (len >= 0 && size_t(len) < sizeof mine - 40)
                len += _snprintf(mine + len, sizeof mine - len, " %08X:%08X:%08X:%08X", a, b, game::at<uint32_t>(e, 8),
                                 game::at<uint32_t>(e, 12));
        }
    }
    logf("lottery: stage %d (load %u): saved %s: %u entries, hash %08X; this stage's %u:%s", int(stage), unsigned(load),
         groups ? "group records" : "unit list", unsigned(n), unsigned(h), unsigned(here), here ? mine : " none");
}

void on_masks(uintptr_t mgr, int32_t stage, uint8_t *masks, uintptr_t list2)
{
    DWORD now = GetTickCount();
    char own[80], theirs[80];
    masks_text(masks, own, sizeof own);
    EnterCriticalSection(&g_lk);
    if (!g_saved_logged) {
        g_saved_logged = true;
        log_saved(mgr);
    }
    uint32_t load = ++g_loads;
    int from = -1;
    if (!g_off && cfg_role() != NET_OFF) {
        int me = net_my_slot();
        for (int s = 0; s < 4 && from < 0; ++s)
            if (s != me && g_peer_rx[s] && now - g_peer_rx[s] <= kPeerFresh && g_peer[s].stage == stage)
                from = s;
    }
    g_from = from;
    if (from >= 0) {
        g_from_rec = g_peer[from];
        memcpy(masks, g_from_rec.masks, sizeof g_from_rec.masks);
    }
    g_mine = {};
    g_mine.stage = stage;
    g_mine.load = load;
    g_kind0_wait = false;
    memcpy(g_mine.masks, masks, sizeof g_mine.masks);
    int idx = stage_index(stage);
    uint32_t own_var[3] = {}, var[3] = {};
    bool has_var = idx >= 0 && read_variant(idx, own_var);
    bool take_var = has_var && from >= 0 && g_from_rec.has_variant;
    if (take_var)
        write_variant(idx, g_from_rec.variant);
    if (has_var)
        read_variant(idx, var);
    g_mine.has_variant = has_var;
    memcpy(g_mine.variant, var, sizeof var);
    g_mine_made = now | 1;
    g_mine_needs_draw = idx >= 0x28 && idx <= 0x40;
    uint32_t from_load = from >= 0 ? g_from_rec.load : 0;
    LeaveCriticalSection(&g_lk);
    if (from >= 0) {
        masks_text(masks, theirs, sizeof theirs);
        logf("lottery: stage %d (load %u): respawn masks taken from player %d (its load %u): %s; own draw was %s", int(stage),
             unsigned(load), from, unsigned(from_load), theirs, own);
    } else {
        logf("lottery: stage %d (load %u): respawn masks, own draw: %s", int(stage), unsigned(load), own);
    }
    log_respawn_list(mgr, list2, stage, load);
    bool rolled = t_roll_stage == stage;
    if (rolled)
        t_roll_entered = true;
    if (has_var) {
        char a[200], b[200], c[200] = "";
        variant_text(idx, var, a, sizeof a);
        variant_text(idx, own_var, b, sizeof b);
        if (rolled) {
            char before[200];
            variant_text(idx, t_roll_before, before, sizeof before);
            _snprintf(c, sizeof c, "; its stage script rolled them just before: %s -> %s", before, b);
        }
        if (take_var)
            logf("lottery: stage %d (load %u): variant flags in effect: %s (taken from player %d, its load %u; own were %s, %s)%s",
                 int(stage), unsigned(load), a, from, unsigned(from_load), b,
                 memcmp(var, own_var, sizeof var) == 0 ? "the same" : "different", c);
        else
            logf("lottery: stage %d (load %u): variant flags in effect: %s (own)%s", int(stage), unsigned(load), a, c);
    }
}

void __fastcall hk_roll(uintptr_t self, void *edx)
{
    int32_t st = -1;
    bool skip = true;
    if (mem_readable(reinterpret_cast<void *>(self + 0x724), 4) && mem_readable(reinterpret_cast<void *>(self + 0xA24), 1)) {
        st = game::at<int32_t>(self, 0x724);
        skip = game::at<uint8_t>(self, 0xA24) == 1;
    }
    bool rolls = !skip && (st == 400 || st == 443 || st == 444);
    if (rolls) {
        memset(t_roll_before, 0, sizeof t_roll_before);
        read_variant(stage_index(st), t_roll_before);
        t_roll_stage = st;
        t_roll_entered = false;
    }
    g_roll_orig(self, edx);
    if (!rolls)
        return;
    t_roll_stage = -1;
    char coins[400] = "";
    if (st == 400) {
        int n = 0;
        for (int i = 40; i <= 63 && n >= 0 && size_t(n) < sizeof coins; ++i) {
            uint32_t v[3] = {};
            read_variant(i, v);
            n += _snprintf(coins + n, sizeof coins - n, " %d:%s", stage_number(i),
                           (v[2] & 0x02000000) ? "102" : (v[2] & 0x01000000) ? "103" : "-");
        }
    }
    logf("lottery: stage %d: its stage script's roll done%s%s%s", int(st),
         t_roll_entered ? " (the stage entry ran inside it)" : " (no stage entry inside it)",
         *coins ? "; 102/103 of each stage now:" : "", coins);
}

void __fastcall hk_masks(uintptr_t self, void *edx, int32_t stage, uintptr_t list, uintptr_t list2, uint8_t *masks)
{
    if (list == self + 0x18A90 && masks && mem_readable(masks, 32))
        on_masks(self, stage, masks, list2);
    g_masks_orig(self, edx, stage, list, list2, masks);
}

int carcass_active(uintptr_t nsi)
{
    int n = 0;
    for (int i = 0; i < 32; ++i)
        n += (game::at<uint8_t>(nsi, 0x1D0 + uintptr_t(i) * 0x50 + 4) & 0xF) != 0;
    return n;
}
void write_est(uintptr_t nsi, const LotteryRecord &r)
{
    for (int i = 0; i < 32; ++i) {
        game::at<uint32_t>(nsi, 0x48 + i * 12 + 4) = r.est_hit[i];
        game::at<uint32_t>(nsi, 0x48 + i * 12 + 8) = r.est_group[i];
    }
}

void on_kind0(uintptr_t mgr, const LotteryRecord &before)
{
    int idx = game::at<int32_t>(mgr, 0x14B70);
    if (idx < 0 || idx >= kNsiCount)
        return;
    uintptr_t nsi = nsi_at(mgr, idx);
    int32_t stage = nsi_stage(idx);
    LotteryRecord now = {};
    read_draw(nsi, now);
    EnterCriticalSection(&g_lk);
    bool mine = g_mine.stage == stage;
    if (mine && !g_mine.has_draw) {
        g_mine.has_draw = 1;
        g_mine.reset_hour = now.reset_hour;
        memcpy(g_mine.groups, now.groups, sizeof now.groups);
    }
    int from = mine && g_from >= 0 && g_from_rec.stage == stage ? g_from : -1;
    bool take = false;
    if (from >= 0) {
        const LotteryRecord &p = g_peer[from];
        take = g_peer_rx[from] && p.stage == stage && p.load == g_from_rec.load && p.kind0_done;
        if (take) {
            write_est(nsi, p);
            read_draw(nsi, now);
        }
        g_kind0_wait = !take;
    }
    if (mine) {
        memcpy(g_mine.est_hit, now.est_hit, sizeof now.est_hit);
        memcpy(g_mine.est_group, now.est_group, sizeof now.est_group);
        g_mine.kind0_done = 1;
        g_kind0_at = GetTickCount();
    }
    uint32_t load = g_mine.load;
    LeaveCriticalSection(&g_lk);
    char a[400], b[400], how[64];
    draw_text(before, b, sizeof b);
    draw_text(now, a, sizeof a);
    if (!mine)
        strcpy(how, "not this game's latest load");
    else if (from < 0)
        strcpy(how, "own");
    else
        _snprintf(how, sizeof how, take ? "taken from player %d" : "own for now, waiting for player %d's", from);
    logf("lottery: stage %d (load %u): corpse special draw (kind 0): %s -> %s (%s)", int(stage), unsigned(load), b, a, how);
}

void __stdcall hk_special(uintptr_t mgr, int32_t kind)
{
    LotteryRecord before = {};
    int k0 = kind == 0 ? game::at<int32_t>(mgr, 0x14B70) : -1;
    if (k0 >= 0 && k0 < kNsiCount)
        read_draw(nsi_at(mgr, k0), before);
    g_special_orig(mgr, kind);
    if (kind == 0 && k0 >= 0 && k0 < kNsiCount)
        on_kind0(mgr, before);
    if (kind != 1)
        return;
    int idx = game::at<int32_t>(mgr, 0x14B70);
    if (idx < 0 || idx >= kNsiCount)
        return;
    uintptr_t nsi = nsi_at(mgr, idx);
    LotteryRecord own = {};
    read_draw(nsi, own);
    EnterCriticalSection(&g_lk);
    int32_t stage = g_mine.stage;
    uint32_t load = g_mine.load;
    bool take = g_from >= 0 && g_from_rec.has_draw && g_from_rec.stage == stage;
    LotteryRecord theirs = g_from_rec;
    int from = g_from;
    LeaveCriticalSection(&g_lk);
    if (take)
        write_draw(nsi, theirs);
    const LotteryRecord &used = take ? theirs : own;
    EnterCriticalSection(&g_lk);
    if (g_mine.load == load) {
        g_mine.has_draw = 1;
        g_mine.reset_hour = used.reset_hour;
        memcpy(g_mine.groups, used.groups, sizeof g_mine.groups);
        memcpy(g_mine.est_hit, used.est_hit, sizeof g_mine.est_hit);
        memcpy(g_mine.est_group, used.est_group, sizeof g_mine.est_group);
    }
    LeaveCriticalSection(&g_lk);
    char a[400], b[400];
    draw_text(own, a, sizeof a);
    if (take) {
        draw_text(theirs, b, sizeof b);
        logf("lottery: stage %d (load %u, table %d): draw taken from player %d (its load %u): %s; own draw was %s (%s)",
             int(stage), unsigned(load), nsi_stage(idx), from, unsigned(theirs.load), b, a,
             same_draw(own, theirs) ? "the same" : "different");
    } else {
        logf("lottery: stage %d (load %u, table %d): own draw: %s%s", int(stage), unsigned(load), nsi_stage(idx), a,
             from >= 0 ? " (the player it took the masks from had no draw for it)" : "");
    }
    int corpses = carcass_active(nsi);
    if (corpses)
        logf("lottery: stage %d (load %u): %d active corpse entries (a kind-0 special draw may follow)", int(stage),
             unsigned(load), corpses);
}

}

uint32_t stage_variant_mask(int idx, int word)
{
    uint32_t m = 0;
    if (idx >= 40 && idx <= 63 && word == 3)
        m |= 0x0F000000;
    if ((idx == 58 || idx == 59) && word == 1)
        m |= 0x0000000F;
    if ((idx == 58 || idx == 59) && word == 2)
        m |= 0xFFFF0000;
    return m;
}

bool lottery_install()
{
    InitializeCriticalSection(&g_lk);
    g_off = early_flag("no_lottery_sync");
    MH_STATUS a = MH_CreateHook(reinterpret_cast<void *>(kApplyMasks), reinterpret_cast<void *>(&hk_masks),
                                reinterpret_cast<void **>(&g_masks_orig));
    if (a == MH_OK)
        a = MH_EnableHook(reinterpret_cast<void *>(kApplyMasks));
    MH_STATUS b = MH_CreateHook(reinterpret_cast<void *>(kSpecialDraw), reinterpret_cast<void *>(&hk_special),
                                reinterpret_cast<void **>(&g_special_orig));
    if (b == MH_OK)
        b = MH_EnableHook(reinterpret_cast<void *>(kSpecialDraw));
    MH_STATUS c = MH_CreateHook(reinterpret_cast<void *>(kStageRoll), reinterpret_cast<void *>(&hk_roll),
                                reinterpret_cast<void **>(&g_roll_orig));
    if (c == MH_OK)
        c = MH_EnableHook(reinterpret_cast<void *>(kStageRoll));
    logf("lottery: hook ApplyMasks %08X: %s, SpecialDraw %08X: %s, StageRoll %08X: %s%s", unsigned(kApplyMasks),
         MH_StatusToString(a), unsigned(kSpecialDraw), MH_StatusToString(b), unsigned(kStageRoll), MH_StatusToString(c),
         g_off ? " (no_lottery_sync=1: draws logged only)" : "");
    return a == MH_OK && b == MH_OK && c == MH_OK;
}

bool lottery_mine(LotteryRecord &out)
{
    EnterCriticalSection(&g_lk);
    bool ok = g_mine_made && (!g_mine_needs_draw || g_mine.has_draw || GetTickCount() - g_mine_made > 500);
    if (ok) {
        out = g_mine;
        if (out.kind0_done && g_kind0_hold && int(GetTickCount() - g_kind0_at) < int(g_kind0_hold))
            out.kind0_done = 0;
    }
    LeaveCriticalSection(&g_lk);
    return ok;
}

void lottery_test_kind0_hold(DWORD ms)
{
    InterlockedExchange(reinterpret_cast<volatile LONG *>(&g_kind0_hold), LONG(ms));
    logf("lottery: kind0hold %lu ms: this game's corpse draws are sent as done that long after they run (tests)",
         (unsigned long)ms);
}

void lottery_received(int slot, const LotteryRecord &r)
{
    if (slot < 0 || slot >= 4 || r.stage < 0 || r.stage > 9999)
        return;
    EnterCriticalSection(&g_lk);
    bool is_new = !g_peer_rx[slot] || g_peer[slot].stage != r.stage || g_peer[slot].load != r.load;
    g_peer[slot] = r;
    g_peer_rx[slot] = GetTickCount() | 1;
    bool late = false;
    if (g_kind0_wait && slot == g_from && r.stage == g_mine.stage && r.stage == g_from_rec.stage &&
        r.load == g_from_rec.load && r.kind0_done) {
        uintptr_t mgr = game::at<uintptr_t>(0x018FA504, 0);
        int idx = mgr ? game::at<int32_t>(mgr, 0x14B70) : -1;
        if (idx >= 0 && idx < kNsiCount && nsi_stage(idx) == r.stage) {
            write_est(nsi_at(mgr, idx), r);
            memcpy(g_mine.est_hit, r.est_hit, sizeof r.est_hit);
            memcpy(g_mine.est_group, r.est_group, sizeof r.est_group);
            late = true;
        }
        g_kind0_wait = false;
    }
    LeaveCriticalSection(&g_lk);
    if (late) {
        char t[400];
        draw_text(r, t, sizeof t);
        logf("lottery: stage %d (load %u): corpse special draw (kind 0) taken from player %d now: %s", int(r.stage),
             unsigned(g_mine.load), slot, t);
    }
    if (is_new) {
        char t[400] = "";
        if (r.has_draw)
            draw_text(r, t, sizeof t);
        logf("lottery: player %d's latest load: stage %d (its load %u)%s%s", slot, int(r.stage), unsigned(r.load),
             r.has_draw ? ", draw: " : "", t);
    }
}
