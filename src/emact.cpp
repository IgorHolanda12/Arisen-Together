#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#include "MinHook.h"
#include "game.h"
#include "mod.h"

namespace {
int g_log = 0;

const char *dti_name(uintptr_t dti)
{
    if (!dti || !mem_readable(reinterpret_cast<void *>(dti + 4), 4))
        return "?";
    auto name = game::at<const char *>(dti, 4);
    return name && mem_readable(name, 2) ? name : "?";
}

const char *obj_class(uintptr_t obj)
{
    if (!obj || !mem_readable(reinterpret_cast<void *>(obj), 4))
        return "-";
    uintptr_t vt = game::at<uintptr_t>(obj, 0);
    if (!mem_readable(reinterpret_cast<void *>(vt + 16), 4))
        return "?";
    auto fn = reinterpret_cast<const uint8_t *>(game::at<uintptr_t>(vt, 16));
    if (!mem_readable(fn, 6) || fn[0] != 0xB8 || fn[5] != 0xC3)
        return "?";
    uintptr_t dti;
    memcpy(&dti, fn + 1, 4);
    return dti_name(dti);
}

const char *act_class(uintptr_t mgr, uint32_t n)
{
    if (n == 0xFFFFFFFF)
        return "none";
    if (n == 0xFFFFFFFE)
        return obj_class(game::at<uintptr_t>(mgr, 0x14));
    uintptr_t banks = game::at<uintptr_t>(mgr, 8);
    uint32_t count = game::at<uint32_t>(mgr, 0xC), b = (n >> 24) & 0xFF, i = n & 0xFFFF;
    if (!banks || b >= count || !mem_readable(reinterpret_cast<void *>(banks + b * 0x24), 0x24))
        return "?";
    uintptr_t tbl = game::at<uintptr_t>(banks + b * 0x24, 0x1C);
    if (!tbl || !mem_readable(reinterpret_cast<void *>(tbl + i * 4), 4))
        return "?";
    uintptr_t proto = game::at<uintptr_t>(tbl, i * 4);
    if (!proto || !mem_readable(reinterpret_cast<void *>(proto), 4))
        return "?";
    return dti_name(game::at<uintptr_t>(proto, 0));
}

bool is_enemy(uintptr_t ch) { return ch && strncmp(enemies_class_of(ch), "uEm", 3) == 0; }
bool is_human(uintptr_t ch) { return ch && strcmp(enemies_class_of(ch), "uHumanEnemy") == 0; }
bool is_actor(uintptr_t ch) { return is_enemy(ch) || is_human(ch); }

void id_text(uintptr_t ch, char *out, size_t cap)
{
    uint32_t key = game::at<uint32_t>(ch, 0x20EC), id = game::at<uint32_t>(ch, 0x20F0), split = game::at<uint32_t>(ch, 0x20F8);
    if (!lot_known(key, id))
        _snprintf(out, cap, "none");
    else if (split)
        _snprintf(out, cap, "st%u/%02um%02un/e%02u#%u", key & 0x3FF, unsigned(split >> 16), unsigned(split & 0xFFFF), key >> 10,
                  unsigned(id));
    else
        _snprintf(out, cap, "st%u/e%02u#%u", key & 0x3FF, key >> 10, unsigned(id));
    out[cap - 1] = 0;
}

volatile LONG g_lines = 0;
bool may_log() { return g_log && InterlockedIncrement(&g_lines) <= 60000; }

volatile LONG g_same = 0, g_same_said = 0;
bool same_again(uintptr_t ch, uint32_t n)
{
    if (n != game::at<uint32_t>(ch, 0x2DB8 + 0x1C))
        return false;
    InterlockedIncrement(&g_same);
    DWORD now = GetTickCount(), said = DWORD(g_same_said);
    if (int(now - said) > 5000 && InterlockedCompareExchange(&g_same_said, LONG(now), LONG(said)) == LONG(said))
        logf("emact: %ld requests for the action already running so far (not logged)", g_same);
    return true;
}

int g_host_ai = 0;
volatile LONG g_human_drops = 0;
struct Unit {
    uintptr_t ch;
    uint32_t starts;
    DWORD last_start;
    bool ours;
    bool driven;
    int follow;
    bool ex;
    uint32_t p[16];
    float dest[3];
    bool dest_new;
    uintptr_t grab_victim;
    uint32_t grab_dti;
    DWORD grab_at;
    uintptr_t gv;
    uint32_t gd;
    DWORD think_seen;
    DWORD driven_since;
    bool human;
    bool ours_pending;
    DWORD hit_at;
    DWORD local_at;
};
constexpr int kUnits = 256;
Unit g_units[kUnits] = {};
volatile LONG g_units_busy = 0;
struct UnitsLock {
    UnitsLock()
    {
        while (InterlockedExchange(&g_units_busy, 1))
            YieldProcessor();
    }
    ~UnitsLock() { InterlockedExchange(&g_units_busy, 0); }
};
Unit *unit_of(uintptr_t ch, bool add)
{
    unsigned h = (unsigned(ch >> 4) * 2654435761u) >> 24;
    Unit *oldest = nullptr;
    for (int k = 0; k < 16; ++k) {
        Unit &u = g_units[(h + k) & (kUnits - 1)];
        if (u.ch == ch)
            return &u;
        if (!oldest || (oldest->ch && (!u.ch || int(u.last_start - oldest->last_start) < 0)))
            oldest = &u;
    }
    if (!add)
        return nullptr;
    *oldest = Unit{};
    oldest->ch = ch;
    oldest->last_start = GetTickCount();
    return oldest;
}
}
extern "C" void *g_emact_upd_orig asm("g_emact_upd_orig");
void *g_emact_upd_orig = nullptr;

uint32_t emact_starts(uintptr_t ch)
{
    UnitsLock lock;
    Unit *u = unit_of(ch, false);
    return u ? u->starts : 0;
}

int g_think_mode = 1;
volatile LONG g_think_hooked = 0;
static bool think_routed(const Unit *u)
{
    return g_think_mode && g_think_hooked && u && u->think_seen && int(GetTickCount() - u->think_seen) < 1000;
}
void emact_drive(uintptr_t ch, bool on, int slot)
{
    if (is_human(ch)) {
        UnitsLock lock;
        if (Unit *u = unit_of(ch, on)) {
            if (on && !u->driven)
                u->driven_since = GetTickCount();
            u->driven = on;
            u->human = on;
            u->follow = on ? slot : -1;
        }
        return;
    }
    uint32_t &flags = game::at<uint32_t>(ch, 0x2748);
    UnitsLock lock;
    Unit *u = unit_of(ch, on);
    bool use_flag = !think_routed(u);
    if (u && on && !u->driven)
        u->driven_since = GetTickCount();
    if (on && use_flag && u && g_think_mode && g_think_hooked && int(GetTickCount() - u->driven_since) > 3000) {
        static uintptr_t said[32] = {};
        uintptr_t vt = game::at<uintptr_t>(ch, 0);
        bool known = false;
        int k = 0;
        for (; k < 32 && said[k] && !known; ++k)
            known = said[k] == vt;
        if (!known && k < 32) {
            said[k] = vt;
            logf("emact: %s %p: its think is not seen at 00AA97D2: the no-think flag for it (its class's update is its own)",
                 enemies_class_of(ch), (void *)ch);
        }
    }
    if (u) {
        u->driven = on;
        u->follow = on ? slot : -1;
    }
    if (on && use_flag && !(flags & 4)) {
        flags |= 4;
        u->ours = true;
    } else if ((!on || !use_flag) && u && u->ours) {
        flags &= ~4u;
        u->ours = false;
    }
}

void emact_request(uintptr_t ch, uint32_t n)
{
    {
        UnitsLock lock;
        if (Unit *u = unit_of(ch, false))
            u->ours_pending = true;
    }
    if (is_human(ch)) {
        using Set_t = void(__thiscall *)(uintptr_t, uint32_t);
        reinterpret_cast<Set_t>(game::at<uintptr_t>(game::at<uintptr_t>(ch, 0), 0x210))(ch, n);
        return;
    }
    game::at<uint32_t>(ch, 0x2DD8) = n;
    game::at<uint16_t>(ch, 0x2DD0) = 1;
    if (uintptr_t obj = game::at<uintptr_t>(ch, 0x2DCC)) {
        using Del_t = void(__thiscall *)(uintptr_t, int);
        reinterpret_cast<Del_t>(game::at<uintptr_t>(game::at<uintptr_t>(obj, 0), 0))(obj, 1);
        game::at<uintptr_t>(ch, 0x2DCC) = 0;
    }
    game::at<uint16_t>(ch, 0x5BD1) = 0;
    game::at<uint32_t>(ch, 0x2740) = game::at<uint32_t>(ch, 0x273C);
}

int emact_extras(uintptr_t ch, uint32_t p[16], float dest[3], uintptr_t *victim, uint32_t *dti)
{
    UnitsLock lock;
    Unit *u = unit_of(ch, false);
    if (!u)
        return 0;
    if (u->ex)
        memcpy(p, u->p, sizeof u->p);
    if (u->dest_new)
        memcpy(dest, u->dest, sizeof u->dest);
    if (u->gv && victim && dti)
        *victim = u->gv, *dti = u->gd;
    return (u->ex ? 1 : 0) | (u->dest_new ? 2 : 0) | (u->gv && victim ? 8 : 0);
}

bool emact_apply_grab(uintptr_t em, uintptr_t victim, uint32_t dti)
{
    if (!em || !victim || dti < 0x00400000 || dti >= 0x02000000 || !mem_readable(reinterpret_cast<void *>(dti), 0x20) ||
        !mem_readable(reinterpret_cast<void *>(victim), 0x2108))
        return false;
    using Req218_t = void(__thiscall *)(uintptr_t, uint32_t);
    reinterpret_cast<Req218_t>(game::at<uintptr_t>(game::at<uintptr_t>(victim, 0), 0x218))(victim, dti);
    game::at<uintptr_t>(victim, 0x2104) = em;
    using Set98_t = void(__thiscall *)(uintptr_t, uintptr_t);
    reinterpret_cast<Set98_t>(game::at<uintptr_t>(game::at<uintptr_t>(em, 0), 0x98))(em, victim);
    return true;
}

void emact_set_dest(uintptr_t ch, const float dest[3])
{
    uintptr_t path = game::at<uintptr_t>(ch, 0x2E64);
    if (path && mem_readable(reinterpret_cast<void *>(path + 0x290), 12))
        memcpy(reinterpret_cast<void *>(path + 0x290), dest, 12);
}

void emact_request_ex(uintptr_t ch, uint32_t n, const uint32_t p[16])
{
    {
        UnitsLock lock;
        if (Unit *u = unit_of(ch, false))
            u->ours_pending = true;
    }
    if (is_human(ch)) {
        using SetEx_t = void(__thiscall *)(uintptr_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t,
                                           uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
        reinterpret_cast<SetEx_t>(game::at<uintptr_t>(game::at<uintptr_t>(ch, 0), 0x214))(
            ch, n, p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7], p[8], p[9], p[10], p[11], p[12], p[13], p[14], p[15]);
        return;
    }
    game::at<uint32_t>(ch, 0x2DD8) = n;
    game::at<uint16_t>(ch, 0x2DD0) = 0x101;
    if (uintptr_t obj = game::at<uintptr_t>(ch, 0x2DCC)) {
        using Del_t = void(__thiscall *)(uintptr_t, int);
        reinterpret_cast<Del_t>(game::at<uintptr_t>(game::at<uintptr_t>(obj, 0), 0))(obj, 1);
        game::at<uintptr_t>(ch, 0x2DCC) = 0;
    }
    memcpy(reinterpret_cast<void *>(ch + 0x2DB8 + 0x34), p, 64);
    game::at<uint32_t>(ch, 0x2740) = game::at<uint32_t>(ch, 0x273C);
}

bool emact_request_if(uintptr_t ch, uint32_t n, const char *cls)
{
    if (strcmp(act_class(ch + 0x2DB8, n), cls) != 0)
        return false;
    emact_request(ch, n);
    return true;
}

const char *emact_running_class(uintptr_t ch) { return obj_class(game::at<uintptr_t>(ch, 0x2DC8)); }

void emact_forget(uintptr_t ch)
{
    UnitsLock lock;
    if (Unit *u = unit_of(ch, false))
        *u = Unit{};
}

bool emact_host_ai() { return g_host_ai != 0; }
bool emact_logging() { return g_log != 0; }
bool emact_host_ai_humans()
{
    static int on = -1;
    if (on < 0)
        on = g_host_ai && early_flag("host_ai_humans") ? 1 : 0;
    return on != 0;
}
const char *emact_act_class(uintptr_t ch, uint32_t n) { return act_class(ch + 0x2DB8, n); }
bool emact_context_action(uintptr_t ch, uint32_t n)
{
    static const char *const kNeeds[] = {"Door", "Ladder", "Grab", "Climb", "Lift", "Catch", "Hold", "Item", "Talk",
                                         "Push", "Throw", "Carry", "Lever", "Chest", "Gather", "Mount", "Cling", "Hang"};
    const char *c = act_class(ch + 0x2DB8, n);
    if (!c || c[0] == '?' || c[0] == '-')
        return true;
    for (const char *k : kNeeds)
        if (strstr(c, k))
            return true;
    return false;
}

extern "C" int __cdecl emact_is_driven(uintptr_t ch) asm("emact_is_driven");
extern "C" int __cdecl emact_is_driven(uintptr_t ch)
{
    if (!ch)
        return 0;
    UnitsLock lock;
    Unit *u = unit_of(ch, false);
    return u && u->driven ? 1 : 0;
}
extern "C" int __cdecl emact_skip_decisions(uintptr_t ch) asm("emact_skip_decisions");
extern "C" int __cdecl emact_skip_decisions(uintptr_t ch)
{
    if (!ch)
        return 0;
    UnitsLock lock;
    Unit *u = unit_of(ch, false);
    return u && u->driven && !think_routed(u) ? 1 : 0;
}
extern "C" void *g_emact_tent_orig asm("g_emact_tent_orig");
void *g_emact_tent_orig = nullptr;
extern "C" __attribute__((naked)) void emact_tent_thunk() asm("emact_tent_thunk");
extern "C" __attribute__((naked)) void emact_tent_thunk()
{
    asm volatile("pushal\n\t"
                 "pushl %edi\n\t"
                 "call emact_skip_decisions\n\t"
                 "addl $4, %esp\n\t"
                 "testl %eax, %eax\n\t"
                 "popal\n\t"
                 "jnz 1f\n\t"
                 "jmp *g_emact_tent_orig\n"
                 "1:\n\t"
                 "ret\n\t");
}
namespace {
using Body_t = bool(__stdcall *)(uintptr_t);
Body_t g_body_orig = nullptr;
bool __stdcall hk_body_periodic(uintptr_t ch)
{
    if (emact_skip_decisions(ch))
        return false;
    return g_body_orig(ch);
}
}

namespace {
char g_treq_cls[32] = "";
volatile LONG g_treq_n = 0, g_treq_on = 0;
}
void emact_test_request(const char *cls, uint32_t n)
{
    strncpy(g_treq_cls, cls, sizeof g_treq_cls - 1);
    InterlockedExchange(&g_treq_n, LONG(n));
    InterlockedExchange(&g_treq_on, 1);
}
namespace {
char g_tcatch_cls[32] = "";
volatile LONG g_tcatch_ch = 0;
}
void emact_test_catch(const char *cls, uintptr_t victim)
{
    strncpy(g_tcatch_cls, cls, sizeof g_tcatch_cls - 1);
    InterlockedExchange(&g_tcatch_ch, LONG(victim));
}

extern "C" void __cdecl emact_before(uintptr_t mgr) asm("emact_before");
extern "C" void __cdecl emact_before(uintptr_t mgr)
{
    uintptr_t ch = game::at<uintptr_t>(mgr, 4);
    if (g_treq_on && ch && is_enemy(ch) && strcmp(enemies_class_of(ch), g_treq_cls) == 0 && !emact_is_driven(ch) &&
        InterlockedExchange(&g_treq_on, 0)) {
        uint32_t n = uint32_t(g_treq_n);
        char id[40];
        id_text(ch, id, sizeof id);
        logf("emact: test request 0x%X (%s) on %s %s %p through its vtable +0x210", n, act_class(mgr, n), g_treq_cls, id,
             (void *)ch);
        using Req_t = void(__thiscall *)(uintptr_t, uint32_t);
        reinterpret_cast<Req_t>(game::at<uintptr_t>(game::at<uintptr_t>(ch, 0), 0x210))(ch, n);
    }
    if (g_tcatch_ch && ch && is_enemy(ch) && strncmp(enemies_class_of(ch), "uEm010", 6) == 0 &&
        strcmp(enemies_class_of(ch), g_tcatch_cls) == 0 && !emact_is_driven(ch)) {
        uintptr_t v = uintptr_t(InterlockedExchange(&g_tcatch_ch, 0));
        if (v) {
            char id[40];
            id_text(ch, id, sizeof id);
            logf("emact: test catch: %s %s %p gets %s %p in +0x60A8 (its next think tramples it)", g_tcatch_cls, id, (void *)ch,
                 enemies_class_of(v), (void *)v);
            game::at<uintptr_t>(ch, 0x60A8) = v;
        }
    }
    if (g_host_ai && ch) {
        int follow = -1;
        bool drop = false;
        {
            UnitsLock lock;
            Unit *u = unit_of(ch, false);
            if (u && u->driven)
                follow = u->follow;
            if (u && u->driven && u->human) {
                drop = !u->ours_pending;
                u->ours_pending = false;
            }
        }
        if (drop && game::at<uint8_t>(mgr, 0x18)) {
            if (uintptr_t obj = game::at<uintptr_t>(mgr, 0x14)) {
                using Del_t = void(__thiscall *)(uintptr_t, int);
                reinterpret_cast<Del_t>(game::at<uintptr_t>(game::at<uintptr_t>(obj, 0), 0))(obj, 1);
                game::at<uintptr_t>(mgr, 0x14) = 0;
            }
            game::at<uint8_t>(mgr, 0x18) = 0;
            game::at<uint8_t>(mgr, 0x19) = 0;
            LONG n = InterlockedIncrement(&g_human_drops);
            if (g_log && (n <= 5 || n % 200 == 0))
                logf("emact: %s %p: a request of its own AI dropped (driven human foe; %ld so far)", enemies_class_of(ch),
                     (void *)ch, n);
        }
        if (follow >= 0)
            enemy_net_before_action(follow, ch);
    }
    if (game::at<uint8_t>(mgr, 0x18) != 1)
        return;
    uint32_t cur = game::at<uint32_t>(mgr, 0x1C), req = game::at<uint32_t>(mgr, 0x20);
    if (!is_actor(ch))
        return;
    if (g_host_ai) {
        UnitsLock lock;
        Unit *u = unit_of(ch, true);
        DWORD now = GetTickCount();
        bool fresh = req != cur || int(now - u->last_start) >= 250;
        u->last_start = now;
        if (fresh) {
            ++u->starts;
            u->gv = u->grab_victim && int(now - u->grab_at) < 200 ? u->grab_victim : 0;
            u->gd = u->gv ? u->grab_dti : 0;
            u->grab_victim = 0;
            u->ex = game::at<uint8_t>(mgr, 0x19) != 0;
            if (u->ex)
                memcpy(u->p, reinterpret_cast<void *>(mgr + 0x34), sizeof u->p);
            uintptr_t path = is_enemy(ch) ? game::at<uintptr_t>(ch, 0x2E64) : 0;
            u->dest_new = false;
            if (path && mem_readable(reinterpret_cast<void *>(path + 0x290), 12)) {
                u->dest_new = memcmp(u->dest, reinterpret_cast<void *>(path + 0x290), 12) != 0;
                memcpy(u->dest, reinterpret_cast<void *>(path + 0x290), 12);
            }
        }
    }
    if (!g_log || cur == req || !may_log())
        return;
    char id[40];
    id_text(ch, id, sizeof id);
    const float *pos = reinterpret_cast<const float *>(ch + 0x40);
    if (game::at<uint8_t>(mgr, 0x19)) {
        const uint32_t *u = reinterpret_cast<const uint32_t *>(mgr + 0x34);
        const int32_t *s = reinterpret_cast<const int32_t *>(mgr + 0x44);
        const float *f = reinterpret_cast<const float *>(mgr + 0x54);
        logf("emact: %s %s %p change 0x%X %s -> 0x%X %s at (%.0f, %.0f, %.0f) ex u[%X %X %X %X] s[%d %d %d %d] "
             "f[%.2f %.2f %.2f %.2f %.2f %.2f %.2f %.2f]",
             enemies_class_of(ch), id, (void *)ch, cur, obj_class(game::at<uintptr_t>(mgr, 0x10)), req, act_class(mgr, req),
             pos[0], pos[1], pos[2], u[0], u[1], u[2], u[3], s[0], s[1], s[2], s[3], f[0], f[1], f[2], f[3], f[4], f[5],
             f[6], f[7]);
    } else {
        logf("emact: %s %s %p change 0x%X %s -> 0x%X %s at (%.0f, %.0f, %.0f)", enemies_class_of(ch), id, (void *)ch, cur,
             obj_class(game::at<uintptr_t>(mgr, 0x10)), req, act_class(mgr, req), pos[0], pos[1], pos[2]);
    }
}

extern "C" __attribute__((naked)) void emact_upd_thunk() asm("emact_upd_thunk");
extern "C" __attribute__((naked)) void emact_upd_thunk()
{
    asm volatile("pushal\n\t"
                 "pushfl\n\t"
                 "pushl %edi\n\t"
                 "call emact_before\n\t"
                 "addl $4, %esp\n\t"
                 "popfl\n\t"
                 "popal\n\t"
                 "jmp *g_emact_upd_orig\n\t");
}

namespace {
int g_tw = 0;
struct TwClass {
    uintptr_t vt;
    const char *name;
    uint32_t calls, size;
    uint32_t *count, *last;
};
constexpr int kTwClasses = 64;
TwClass g_tw_cls[kTwClasses] = {};
volatile LONG g_tw_busy = 0;
thread_local uint8_t g_tw_before[0x8000];
thread_local uintptr_t g_tw_unit = 0;
thread_local uint32_t g_tw_size = 0;
DWORD g_tw_said = 0;

uint32_t class_size(uintptr_t ch)
{
    uintptr_t vt = game::at<uintptr_t>(ch, 0);
    auto fn = reinterpret_cast<const uint8_t *>(game::at<uintptr_t>(vt, 16));
    if (!mem_readable(fn, 6) || fn[0] != 0xB8 || fn[5] != 0xC3)
        return 0;
    uintptr_t dti;
    memcpy(&dti, fn + 1, 4);
    if (!mem_readable(reinterpret_cast<void *>(dti + 0x18), 4))
        return 0;
    return game::at<uint32_t>(dti, 0x18) & 0x7FFFFF;
}

void tw_report()
{
    for (TwClass &c : g_tw_cls) {
        if (!c.vt || !c.calls)
            continue;
        char line[1800];
        int n = _snprintf(line, sizeof line, "emact: think writes, %s (%u calls, size 0x%X):", c.name, c.calls, c.size);
        static int order[0x2000];
        int m = 0;
        for (uint32_t k = 0; k < c.size / 4; ++k)
            if (c.count[k])
                order[m++] = int(k);
        for (int a = 1; a < m; ++a)
            for (int b = a; b > 0 && c.count[order[b]] > c.count[order[b - 1]]; --b)
                std::swap(order[b], order[b - 1]);
        int shown = 0;
        for (int a = 0; a < m && n > 0 && n < int(sizeof line) - 60; ++a, ++shown) {
            uint32_t k = uint32_t(order[a]), v = c.last[k];
            float f;
            memcpy(&f, &v, 4);
            bool ptr = v >= 0x10000 && v % 4 == 0 && mem_readable(reinterpret_cast<void *>(v), 4);
            if (ptr)
                n += _snprintf(line + n, sizeof line - n, " +%X x%u p%08X", k * 4, c.count[k], v);
            else if (v && std::fabs(f) > 1e-4f && std::fabs(f) < 1e7f)
                n += _snprintf(line + n, sizeof line - n, " +%X x%u %.3g", k * 4, c.count[k], f);
            else
                n += _snprintf(line + n, sizeof line - n, " +%X x%u %X", k * 4, c.count[k], v);
        }
        if (n > 0 && shown < m)
            _snprintf(line + n, sizeof line - n, " ... (%d fields in all)", m);
        line[sizeof line - 1] = 0;
        logf("%s", line);
    }
}
}

namespace {
struct ReqSave {
    uintptr_t ch;
    uint8_t mgr[0x60];
    uint32_t r2740, r5bd0;
    uintptr_t r2100;
    uintptr_t victim;
};
thread_local ReqSave g_rs = {};
thread_local uintptr_t g_think_cur = 0;
volatile LONG g_dropped = 0, g_dropped_said = 0;
int g_react = 0;
volatile LONG g_react_kept = 0, g_react_matched = 0, g_react_stale = 0;
}

void emact_local_hit(uintptr_t ch)
{
    if (!g_host_ai)
        return;
    bool driven = false;
    {
        UnitsLock lock;
        if (Unit *u = unit_of(ch, false))
            if ((driven = u->driven) && g_react)
                u->hit_at = GetTickCount() | 1;
    }
    if (driven && may_log()) {
        char id[40];
        id_text(ch, id, sizeof id);
        logf("emact: %s %s %p: this player's blow felt here", enemies_class_of(ch), id, (void *)ch);
    }
}

bool emact_local_match(uintptr_t ch, uint32_t act)
{
    if (!g_react || strncmp(act_class(ch + 0x2DB8, act), "cEmActDmg", 9) != 0)
        return false;
    UnitsLock lock;
    Unit *u = unit_of(ch, false);
    if (!u || !u->local_at || int(GetTickCount() - u->local_at) > 800)
        return false;
    u->local_at = 0;
    InterlockedIncrement(&g_react_matched);
    return true;
}

bool emact_local_stale(uintptr_t ch)
{
    if (!g_react)
        return false;
    UnitsLock lock;
    Unit *u = unit_of(ch, false);
    if (!u || !u->local_at || int(GetTickCount() - u->local_at) <= 800)
        return false;
    u->local_at = 0;
    InterlockedIncrement(&g_react_stale);
    return true;
}

extern "C" void __cdecl emact_think_before(uintptr_t ch) asm("emact_think_before");
extern "C" void __cdecl emact_think_before(uintptr_t ch)
{
    g_think_cur = ch;
    g_rs.ch = 0;
    bool driven = false;
    if (ch && g_think_mode && g_host_ai) {
        UnitsLock lock;
        if (Unit *u = unit_of(ch, false)) {
            u->think_seen = GetTickCount() | 1;
            driven = u->driven && !(game::at<uint32_t>(ch, 0x2748) & 4);
        }
    }
    if (driven) {
        memcpy(g_rs.mgr, reinterpret_cast<void *>(ch + 0x2DCC), sizeof g_rs.mgr);
        g_rs.r2740 = game::at<uint32_t>(ch, 0x2740);
        g_rs.r5bd0 = game::at<uint32_t>(ch, 0x5BD0);
        g_rs.r2100 = game::at<uintptr_t>(ch, 0x2100);
        g_rs.victim = 0;
        g_rs.ch = ch;
        return;
    }
    g_tw_unit = 0;
    if (!g_tw || !ch || (game::at<uint32_t>(ch, 0x2748) & 4) || !is_enemy(ch))
        return;
    uint32_t size = class_size(ch);
    if (size < 0x1000 || size > sizeof g_tw_before || !mem_readable(reinterpret_cast<void *>(ch), size))
        return;
    memcpy(g_tw_before, reinterpret_cast<void *>(ch), size);
    g_tw_unit = ch;
    g_tw_size = size;
}

extern "C" void __cdecl emact_think_after(uintptr_t ch) asm("emact_think_after");
extern "C" void __cdecl emact_think_after(uintptr_t ch)
{
    g_think_cur = 0;
    if (ch && ch == g_rs.ch) {
        g_rs.ch = 0;
        if (uintptr_t v = g_rs.victim) {
            g_rs.victim = 0;
            if (mem_readable(reinterpret_cast<void *>(v), 0x2108) && game::at<uintptr_t>(v, 0x2104) == ch)
                game::at<uintptr_t>(v, 0x2104) = 0;
            game::at<uintptr_t>(ch, 0x2100) = g_rs.r2100;
            if (g_log)
                logf("emact: %s %p: its own think's grab of %s %p undone (driven: its leader's grabs come instead)",
                     enemies_class_of(ch), (void *)ch, enemies_class_of(v), (void *)v);
        }
        uint8_t *mgr = reinterpret_cast<uint8_t *>(ch + 0x2DCC);
        if (memcmp(mgr, g_rs.mgr, sizeof g_rs.mgr) == 0)
            return;
        if (g_react && mgr[4] == 1 && !mgr[5]) {
            uint32_t req;
            memcpy(&req, mgr + 0xC, 4);
            const char *cls = act_class(ch + 0x2DB8, req);
            DWORD now = GetTickCount();
            bool keep = false;
            const char *run = act_class(ch + 0x2DB8, game::at<uint32_t>(ch, 0x2DD4));
            bool busy = strncmp(run, "cEmActDmg", 9) == 0 || strncmp(run, "cEmActDown", 10) == 0 || strncmp(run, "cEmActDie", 9) == 0;
            if (!busy && strncmp(cls, "cEmActDmg", 9) == 0) {
                UnitsLock lock;
                Unit *u = unit_of(ch, false);
                keep = u && u->hit_at && int(now - u->hit_at) < 300;
                if (keep) {
                    u->hit_at = 0;
                    u->local_at = now | 1;
                }
            }
            if (keep) {
                InterlockedIncrement(&g_react_kept);
                if (may_log()) {
                    char id[40];
                    id_text(ch, id, sizeof id);
                    logf("emact: %s %s %p: its own reaction 0x%X %s to this player's blow kept (driven)", enemies_class_of(ch), id,
                         (void *)ch, req, cls);
                }
                return;
            }
        }
        uintptr_t was_obj, now_obj;
        memcpy(&was_obj, g_rs.mgr, 4);
        memcpy(&now_obj, mgr, 4);
        if (now_obj && now_obj != was_obj) {
            using Del_t = void(__thiscall *)(uintptr_t, int);
            reinterpret_cast<Del_t>(game::at<uintptr_t>(game::at<uintptr_t>(now_obj, 0), 0))(now_obj, 1);
        }
        memcpy(mgr, g_rs.mgr, sizeof g_rs.mgr);
        if (was_obj && now_obj != was_obj) {
            game::at<uintptr_t>(ch, 0x2DCC) = 0;
            game::at<uint8_t>(ch, 0x2DD0) = 0;
        }
        game::at<uint32_t>(ch, 0x2740) = g_rs.r2740;
        game::at<uint32_t>(ch, 0x5BD0) = g_rs.r5bd0;
        LONG n = InterlockedIncrement(&g_dropped);
        DWORD t = GetTickCount(), said = DWORD(g_dropped_said);
        if (g_log && int(t - said) > 10000 && InterlockedCompareExchange(&g_dropped_said, LONG(t), LONG(said)) == LONG(said))
            logf("emact: %ld requests made by driven units' own thinks undone so far", n);
        return;
    }
    if (!ch || ch != g_tw_unit)
        return;
    g_tw_unit = 0;
    uintptr_t vt = game::at<uintptr_t>(ch, 0);
    while (InterlockedExchange(&g_tw_busy, 1))
        YieldProcessor();
    TwClass *c = nullptr;
    for (TwClass &k : g_tw_cls)
        if (k.vt == vt || !k.vt) {
            c = &k;
            break;
        }
    if (c && !c->vt) {
        c->vt = vt;
        c->name = enemies_class_of(ch);
        c->size = g_tw_size;
        c->count = new uint32_t[g_tw_size / 4]();
        c->last = new uint32_t[g_tw_size / 4]();
    }
    if (c) {
        ++c->calls;
        const uint32_t *was = reinterpret_cast<const uint32_t *>(g_tw_before), *now = reinterpret_cast<const uint32_t *>(ch);
        for (uint32_t k = 0, e = std::min(c->size, g_tw_size) / 4; k < e; ++k)
            if (was[k] != now[k]) {
                ++c->count[k];
                c->last[k] = now[k];
            }
    }
    DWORD t = GetTickCount();
    bool say = int(t - g_tw_said) > 10000;
    if (say)
        g_tw_said = t;
    InterlockedExchange(&g_tw_busy, 0);
    if (say) {
        while (InterlockedExchange(&g_tw_busy, 1))
            YieldProcessor();
        tw_report();
        InterlockedExchange(&g_tw_busy, 0);
    }
}

extern "C" void *g_tw_resume asm("g_tw_resume");
void *g_tw_resume = reinterpret_cast<void *>(0x00AA97DE);
extern "C" __attribute__((naked)) void emact_think_thunk() asm("emact_think_thunk");
extern "C" __attribute__((naked)) void emact_think_thunk()
{
    asm volatile("pushal\n\t"
                 "pushl %esi\n\t"
                 "call emact_think_before\n\t"
                 "addl $4, %esp\n\t"
                 "popal\n\t"
                 "movl (%esi), %eax\n\t"
                 "movl 0x220(%eax), %edx\n\t"
                 "movl %esi, %ecx\n\t"
                 "call *%edx\n\t"
                 "pushal\n\t"
                 "pushl %esi\n\t"
                 "call emact_think_after\n\t"
                 "addl $4, %esp\n\t"
                 "popal\n\t"
                 "jmp *g_tw_resume\n\t");
}

namespace {
void req_seen(uintptr_t ch, uint32_t n, uintptr_t ret, const char *how)
{
    if (g_rs.ch && g_rs.ch == ch)
        return;
    if (!g_log || n == 0xFFFFFFFF || !is_enemy(ch) || same_again(ch, n) || !may_log())
        return;
    char id[40];
    id_text(ch, id, sizeof id);
    logf("emact: %s %s %p asks 0x%X %s (%s) from %08X", enemies_class_of(ch), id, (void *)ch, n,
         act_class(ch + 0x2DB8, n), how, unsigned(ret));
}

using Req_t = void(__fastcall *)(uintptr_t, void *, int);
Req_t g_req_orig[4] = {};
template <int K> void __fastcall hk_req(uintptr_t ch, void *edx, int n)
{
    req_seen(ch, uint32_t(n), uintptr_t(__builtin_return_address(0)), "210");
    g_req_orig[K](ch, edx, n);
}

using ReqEx_t = void(__fastcall *)(uintptr_t, void *, int, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t,
                                   uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
ReqEx_t g_req_ex_orig = nullptr;
void __fastcall hk_req_ex(uintptr_t ch, void *edx, int n, uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e, uint32_t f,
                          uint32_t g, uint32_t h, uint32_t i, uint32_t j, uint32_t k, uint32_t l, uint32_t m, uint32_t o, uint32_t p,
                          uint32_t q)
{
    req_seen(ch, uint32_t(n), uintptr_t(__builtin_return_address(0)), "214");
    g_req_ex_orig(ch, edx, n, a, b, c, d, e, f, g, h, i, j, k, l, m, o, p, q);
}

using ReqObj_t = void(__fastcall *)(uintptr_t, void *, uintptr_t);
ReqObj_t g_req_obj_orig = nullptr;
void __fastcall hk_req_obj(uintptr_t ch, void *edx, uintptr_t proto)
{
    uintptr_t th = g_think_cur;
    if (g_host_ai && th && th != ch) {
        if (g_rs.ch && g_rs.ch == th) {
            g_rs.victim = ch;
            return;
        }
        if (is_enemy(th)) {
            UnitsLock lock;
            if (Unit *u = unit_of(th, true)) {
                u->grab_victim = ch;
                u->grab_dti = uint32_t(proto);
                u->grab_at = GetTickCount();
            }
        }
    }
    if (g_log && !(g_rs.ch && g_rs.ch == ch) && may_log()) {
        char id[40];
        id_text(ch, id, sizeof id);
        logf("emact: %s %s %p asks by object %s (218) from %08X%s", enemies_class_of(ch), id, (void *)ch, dti_name(proto),
             unsigned(uintptr_t(__builtin_return_address(0))), th && th != ch ? " (a monster's think: a grab)" : "");
    }
    g_req_obj_orig(ch, edx, proto);
}

void hook(uintptr_t addr, void *fn, void **orig, const char *what)
{
    void *t = reinterpret_cast<void *>(addr);
    MH_STATUS st = MH_CreateHook(t, fn, orig);
    if (st == MH_OK)
        st = MH_EnableHook(t);
    logf("emact: hook %s %08X: %s", what, unsigned(addr), MH_StatusToString(st));
}

DWORD WINAPI late_install(void *)
{
    Sleep(25000);
    hook(0x007F1EA0, reinterpret_cast<void *>(&emact_upd_thunk), &g_emact_upd_orig, "ActionUpdate");
    if (g_tw || (g_host_ai && g_think_mode)) {
        static const uint8_t want[12] = {0x8B, 0x06, 0x8B, 0x90, 0x20, 0x02, 0x00, 0x00, 0x8B, 0xCE, 0xFF, 0xD2};
        void *tramp = nullptr;
        void *at = reinterpret_cast<void *>(0x00AA97D2);
        if (memcmp(at, want, sizeof want) == 0 && MH_CreateHook(at, reinterpret_cast<void *>(&emact_think_thunk), &tramp) == MH_OK &&
            MH_EnableHook(at) == MH_OK) {
            InterlockedExchange(&g_think_hooked, 1);
            logf("emact: hook ThinkCall 00AA97D2: MH_OK (%s%s)", g_host_ai && g_think_mode ? "driven units' thinks run, their requests "
                 "undone" : "", g_tw ? (g_host_ai && g_think_mode ? "; test_think_writes" : "test_think_writes") : "");
        } else {
            logf("emact: the think call at 00AA97D2 is not as expected or not hooked: the no-think flag instead");
        }
    }
    if (g_host_ai) {
        hook(0x00A26F70, reinterpret_cast<void *>(&emact_tent_thunk), &g_emact_tent_orig, "TentacleThink");
        hook(0x00A095A0, reinterpret_cast<void *>(&hk_body_periodic), reinterpret_cast<void **>(&g_body_orig), "BodyPeriodic");
    }
    hook(0x004C7350, reinterpret_cast<void *>(&hk_req_obj), reinterpret_cast<void **>(&g_req_obj_orig), "Request218");
    if (!g_log)
        return 0;
    hook(0x009CC4B0, reinterpret_cast<void *>(&hk_req<0>), reinterpret_cast<void **>(&g_req_orig[0]), "Request210 (most)");
    hook(0x008D11E0, reinterpret_cast<void *>(&hk_req<1>), reinterpret_cast<void **>(&g_req_orig[1]), "Request210 (uEm010x)");
    hook(0x00AAD070, reinterpret_cast<void *>(&hk_req<2>), reinterpret_cast<void **>(&g_req_orig[2]), "Request210 (uEnemy)");
    hook(0x009690D0, reinterpret_cast<void *>(&hk_req<3>), reinterpret_cast<void **>(&g_req_orig[3]), "Request210 (bones)");
    hook(0x004C72A0, reinterpret_cast<void *>(&hk_req_ex), reinterpret_cast<void **>(&g_req_ex_orig), "Request214");
    return 0;
}
}

bool emact_install()
{
    g_log = early_flag("test_log_emact") ? 1 : 0;
    char ha[8] = "";
    g_host_ai = cfg_str("host_ai", ha, sizeof ha) && ha[0] ? (atoi(ha) != 0 ? 1 : 0) : 1;
    g_tw = early_flag("test_think_writes") ? 1 : 0;
    char v[8];
    if (cfg_str("host_ai_think", v, sizeof v) && atoi(v) == 0)
        g_think_mode = 0;
    g_react = cfg_str("host_ai_react", v, sizeof v) && atoi(v) == 1 ? 1 : 0;
    if (g_host_ai)
        logf("emact: host_ai=1: a monster's actions are its leader's (on the followers %s), step 12a",
             g_think_mode ? "its think runs and its own requests are undone" : "its think is off, the no-think flag");
    if (!g_log && !g_host_ai && !g_tw)
        return true;
    HANDLE t = CreateThread(nullptr, 0, late_install, nullptr, 0, nullptr);
    if (t)
        CloseHandle(t);
    return true;
}
