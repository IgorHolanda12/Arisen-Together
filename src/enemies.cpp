#include <windows.h>

#include <cmath>
#include <cstdio>
#include <cstring>

#include "MinHook.h"
#include "game.h"
#include "mod.h"

using Death_t = void(__fastcall *)(uintptr_t, void *, uint32_t, uint32_t);
Death_t g_death_orig = nullptr;
void __fastcall hk_death(uintptr_t ch, void *edx, uint32_t a, uint32_t hit);

extern "C" bool __cdecl enemies_damage_filter(uintptr_t entry, float *dmg) asm("enemies_damage_filter");
extern "C" void *g_damage_orig asm("g_damage_orig");
void *g_damage_orig = nullptr;

extern "C" bool __cdecl enemies_damage_filter(uintptr_t entry, float *dmg)
{
    uintptr_t ch = game::at<uintptr_t>(entry, 0x1B4);
    if (ch && revive_on_damage(ch, dmg))
        return true;
    if (!ch || !enemy_net_block_damage(ch))
        return false;
    if (dmg && enemy_net_feel_hit(ch)) {
        float cap = game::at<float>(entry, 8) - 1.f;
        if (*dmg > cap)
            *dmg = cap > 0.f ? cap : 0.f;
        emact_local_hit(ch);
        return false;
    }
    return true;
}

extern "C" __attribute__((naked)) void enemies_damage_thunk() asm("enemies_damage_thunk");
extern "C" __attribute__((naked)) void enemies_damage_thunk()
{
    asm volatile("pushl %eax\n\t"
                 "leal 8(%esp), %ecx\n\t"
                 "pushl %ecx\n\t"
                 "pushl %eax\n\t"
                 "call enemies_damage_filter\n\t"
                 "addl $8, %esp\n\t"
                 "testb %al, %al\n\t"
                 "popl %eax\n\t"
                 "jnz 1f\n\t"
                 "jmp *g_damage_orig\n"
                 "1:\n\t"
                 "ret $8\n\t");
}

volatile LONG g_queued_damage = 0;
volatile LONG g_queued_kill = 0;

void enemies_queue_arisen_kill() { InterlockedExchange(&g_queued_kill, 1); }
volatile LONG g_queued_pawn_kill = 0;
void enemies_queue_pawn_kill(int pawn) { InterlockedExchange(&g_queued_pawn_kill, 1 + pawn); }

void enemies_queue_arisen_damage(float dmg)
{
    LONG bits;
    memcpy(&bits, &dmg, 4);
    InterlockedExchange(&g_queued_damage, bits);
}

void enemies_run_queued(uintptr_t arisen)
{
    if (InterlockedExchange(&g_queued_kill, 0) && arisen && enemies_hp(arisen)) {
        uintptr_t e = game::at<uintptr_t>(game::at<uintptr_t>(arisen, 0x1D18), 0);
        game::at<float>(e, 8) = 0.f;
        game::at<uint32_t>(e, 0x110) |= 1;
        logf("enemies: test kill of the Arisen (HP entry %08X)", unsigned(e));
    }
    if (LONG pk = InterlockedExchange(&g_queued_pawn_kill, 0)) {
        uintptr_t ch = possess_pawn_ptr(int(pk) - 1);
        if (ch && enemies_hp(ch)) {
            uintptr_t e = game::at<uintptr_t>(game::at<uintptr_t>(ch, 0x1D18), 0);
            game::at<float>(e, 8) = 0.f;
            game::at<uint32_t>(e, 0x110) |= 1;
            logf("enemies: test kill of pawn %ld (HP entry %08X)", pk - 1, unsigned(e));
        }
    }
    LONG bits = InterlockedExchange(&g_queued_damage, 0);
    if (!bits || !arisen || !enemies_hp(arisen))
        return;
    uint32_t b = uint32_t(bits);
    float dmg;
    memcpy(&dmg, &b, 4);
    logf("enemies: test damage %.1f to the Arisen", dmg);
    enemies_apply_damage(arisen, dmg);
}

void enemies_apply_damage(uintptr_t ch, float dmg)
{
    if (!ch || !enemies_hp(ch))
        return;
    uintptr_t entry = game::at<uintptr_t>(game::at<uintptr_t>(ch, 0x1D18), 0);
    uintptr_t fn = game::kApplyDamage;
    uint32_t b;
    memcpy(&b, &dmg, 4);
    asm volatile("pushl $0\n\t"
                 "pushl %%edx\n\t"
                 "call *%%ecx\n\t"
                 : "+a"(entry), "+c"(fn), "+d"(b)
                 :
                 : "memory", "cc", "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7");
}

bool mem_readable(const void *p, size_t n)
{
    if (reinterpret_cast<uintptr_t>(p) + n < reinterpret_cast<uintptr_t>(p))
        return false;
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(p, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
        return false;
    return reinterpret_cast<uintptr_t>(p) + n <= reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
}

bool live_readable(const void *p, size_t n)
{
    struct Region {
        uintptr_t lo, hi;
        DWORD until;
    };
    static thread_local Region kept[8] = {};
    static thread_local int next = 0;
    uintptr_t a = reinterpret_cast<uintptr_t>(p);
    if (a + n < a || a < 0x10000)
        return false;
    DWORD now = GetTickCount();
    for (const Region &r : kept)
        if (a >= r.lo && a + n <= r.hi && int(r.until - now) > 0)
            return true;
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(p, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
        return false;
    uintptr_t lo = reinterpret_cast<uintptr_t>(mbi.BaseAddress), hi = lo + mbi.RegionSize;
    kept[next] = {lo, hi, now + 250};
    next = (next + 1) % 8;
    return a + n <= hi;
}

bool game_origin(float &ox, float &oz);

volatile uintptr_t g_wmap = 0, g_wmap_vt = 0;

uintptr_t field_origin_object()
{
    uintptr_t o = g_wmap, vt = g_wmap_vt;
    if (!o || !vt || !live_readable(reinterpret_cast<void *>(o), game::kFieldOriginPos + 12) || game::at<uintptr_t>(o, 0) != vt)
        return 0;
    return o;
}

void field_origin_pass();

DWORD WINAPI field_origin_thread(void *)
{
    for (;;) {
        Sleep(1000);
        if (game::build() != game::BUILD_STEAM && !field_origin_object() && possess_arisen())
            field_origin_pass();
    }
}

void field_origin_scan()
{
    static volatile LONG started = 0;
    if (game::build() != game::BUILD_STEAM && InterlockedExchange(&started, 1) == 0) {
        HANDLE t = CreateThread(nullptr, 0, field_origin_thread, nullptr, 0, nullptr);
        if (t) {
            SetThreadPriority(t, THREAD_PRIORITY_BELOW_NORMAL);
            CloseHandle(t);
        }
    }
}

void field_origin_pass()
{
    static uintptr_t rejected[64] = {};
    static int n_rejected = 0;
    uintptr_t lo = game::build() == game::BUILD_GOG ? 0x01840000 : 0x01860000;
    for (uintptr_t g = lo; g < lo + 0x30000; g += 4) {
        if ((g & 0xFFF) == 0 && !mem_readable(reinterpret_cast<void *>(g), 0x1000)) {
            g += 0x1000 - 4;
            continue;
        }
        uintptr_t q = game::at<uintptr_t>(g, 0);
        if (q < 0x10000 || q >= 0x7FFF0000 || (q & 3) || !mem_readable(reinterpret_cast<void *>(q), 4))
            continue;
        uintptr_t vt = game::at<uintptr_t>(q, 0);
        if (vt < 0x00401000 || vt >= 0x01A00000)
            continue;
        if (g_wmap_vt) {
            if (vt == g_wmap_vt && live_readable(reinterpret_cast<void *>(q), game::kFieldOriginPos + 12)) {
                g_wmap = q;
                logf("field: cell origin in uWaterReflectionMap %08X (global %08X)", unsigned(q), unsigned(g));
                return;
            }
            continue;
        }
        bool seen = false;
        for (int k = 0; k < n_rejected && !seen; ++k)
            seen = rejected[k] == vt;
        if (seen)
            continue;
        auto in_exe = [](uintptr_t a) { return a >= 0x00401000 && a < 0x01A00000; };
        uintptr_t fnp = mem_readable(reinterpret_cast<void *>(vt + 16), 4) ? game::at<uintptr_t>(vt, 16) : 0;
        if (!in_exe(fnp) || !mem_readable(reinterpret_cast<void *>(fnp), 6))
            goto reject;
        {
            auto fn = reinterpret_cast<const uint8_t *>(fnp);
            uintptr_t dti = 0, name = 0;
            if (fn[0] == 0xB8 && fn[5] == 0xC3)
                memcpy(&dti, fn + 1, 4);
            if (in_exe(dti) && mem_readable(reinterpret_cast<void *>(dti + 4), 4))
                name = game::at<uintptr_t>(dti, 4);
            if (!in_exe(name) || !mem_readable(reinterpret_cast<void *>(name), 20)
                || memcmp(reinterpret_cast<const char *>(name), "uWaterReflectionMap", 20) != 0)
                goto reject;
        }
        if (live_readable(reinterpret_cast<void *>(q), game::kFieldOriginPos + 12)) {
            g_wmap_vt = vt;
            g_wmap = q;
            logf("field: cell origin in uWaterReflectionMap %08X (global %08X, vtable %08X)", unsigned(q), unsigned(g), unsigned(vt));
            return;
        }
    reject:
        if (n_rejected < 64)
            rejected[n_rejected++] = vt;
    }
}

bool field_origin(float &ox, float &oz)
{
    ox = oz = 0.f;
    if (game::build() == game::BUILD_STEAM) {
        float gx, gz;
        if (!game_origin(gx, gz))
            return false;
        ox = -gx + 0.f;
        oz = -gz + 0.f;
        return true;
    }
    uintptr_t fo = field_origin_object();
    if (!fo)
        return false;
    float x = game::at<float>(fo, game::kFieldOriginPos), z = game::at<float>(fo, game::kFieldOriginPos + 8);
    auto cell = [](float v) { return v == v && std::fabs(v) < 1e7f && std::fmod(v, 10000.f) == 0.f; };
    if (!cell(x) || !cell(z))
        return false;
    ox = x;
    oz = z;
    return true;
}

void field_to_world(float *p)
{
    float ox, oz;
    field_origin(ox, oz);
    p[0] -= ox;
    p[2] -= oz;
}

void field_to_local(float *p)
{
    float ox, oz;
    field_origin(ox, oz);
    p[0] += ox;
    p[2] += oz;
}

namespace {

constexpr int kMax = 128;
EnemySlot g_slots[kMax];
CRITICAL_SECTION g_lock;
volatile LONG g_ready = 0;
volatile LONG g_kill_req = 0;
volatile LONG g_del_req = 0;

bool readable(const void *p, size_t n) { return live_readable(p, n); }

const char *class_of(uintptr_t obj)
{
    uintptr_t vt = game::at<uintptr_t>(obj, 0);
    if (!readable(reinterpret_cast<void *>(vt + 16), 4))
        return "?";
    auto fn = reinterpret_cast<const uint8_t *>(game::at<uintptr_t>(vt, 16));
    if (!readable(fn, 6) || fn[0] != 0xB8 || fn[5] != 0xC3)
        return "?";
    uintptr_t dti;
    memcpy(&dti, fn + 1, 4);
    if (!readable(reinterpret_cast<void *>(dti + 4), 4))
        return "?";
    auto name = reinterpret_cast<const char *>(game::at<uintptr_t>(dti, 4));
    return readable(name, 16) ? name : "?";
}

constexpr uintptr_t kGroupParamVtable = 0x01617C68;

volatile LONG g_origin_tiles = 0;
volatile LONG g_origin_tick = 0;
volatile LONG g_origin_changed = 0;
void origin_from(uintptr_t em, int i, const char *name)
{
    if (!readable(reinterpret_cast<void *>(em + 0xE50), 12) || !readable(reinterpret_cast<void *>(em + 0x5ED0), 12))
        return;
    const float *w = reinterpret_cast<const float *>(em + 0xE50), *l = reinterpret_cast<const float *>(em + 0x5ED0);
    float ox = w[0] - l[0], oy = w[1] - l[1], oz = w[2] - l[2];
    float tx = std::round(ox / 10000.f), tz = std::round(oz / 10000.f);
    if (std::fabs(oy) > 1.f || std::fabs(ox - tx * 10000.f) > 1.f || std::fabs(oz - tz * 10000.f) > 1.f ||
        std::fabs(tx) > 30000.f || std::fabs(tz) > 30000.f)
        return;
    float gx, gz;
    if (game_origin(gx, gz)) {
        if (std::fabs(gx - tx * 10000.f) > 1.f || std::fabs(gz - tz * 10000.f) > 1.f) {
            static volatile LONG said = 0;
            DWORD now = GetTickCount();
            if (!said || int(now - DWORD(said)) > 10000) {
                InterlockedExchange(&said, LONG(now | 1));
                logf("world: #%d %s's spawn copies say origin (%.0f, %.0f), the game's is (%.0f, %.0f): kept the game's", i,
                     name, tx * 10000.f, tz * 10000.f, gx, gz);
            }
        }
        return;
    }
    LONG packed = LONG(uint16_t(int16_t(tx))) | LONG(uint32_t(uint16_t(int16_t(tz))) << 16);
    LONG old = InterlockedExchange(&g_origin_tiles, packed);
    DWORD before = DWORD(InterlockedExchange(&g_origin_tick, LONG(GetTickCount() | 1)));
    int32_t stage = -1;
    DWORD loaded = stages_last_load(&stage);
    bool stale = !before || (loaded && int(before - loaded) < 0);
    if (old != packed)
        InterlockedExchange(&g_origin_changed, LONG(GetTickCount() | 1));
    if (old != packed || stale)
        logf("world: origin (%.0f, %.0f) from #%d %s (stage %d)", tx * 10000.f, tz * 10000.f, i, name, int(stage));
}

bool lot_read(uintptr_t em, uint32_t &key, uint32_t &id, uint32_t &split)
{
    if (!readable(reinterpret_cast<void *>(em + 0x20EC), 16))
        return false;
    key = game::at<uint32_t>(em, 0x20EC);
    id = game::at<uint32_t>(em, 0x20F0);
    split = game::at<uint32_t>(em, 0x20F8);
    return true;
}
bool group_ok(uintptr_t em)
{
    if (!readable(reinterpret_cast<void *>(em + 0x20F4), 4))
        return false;
    uintptr_t gp = game::at<uintptr_t>(em, 0x20F4);
    return gp && !(gp & 3) && readable(reinterpret_cast<void *>(gp), 4) && game::at<uintptr_t>(gp, 0) == kGroupParamVtable;
}

struct PartLink {
    const char *prefix;
    uint32_t off;
};
const PartLink kPartLinks[] = {
    {"uEm5500_", 0x7324}, {"uEm5502_", 0x7324}, {"uEm5500C_", 0x7324},
    {"uEm7002", 0x602C},
    {"uEmDieBone", 0x6000},
};
constexpr uint16_t kPartField = 0x0000, kPartJoint = 0x4000, kPartPiece = 0x8000, kPartOther = 0xC000;

bool tentacle_class(const char *name)
{
    return strncmp(name, "uEm5500_", 8) == 0 || strncmp(name, "uEm5502_", 8) == 0 || strncmp(name, "uEm5500C_", 9) == 0;
}

bool monster_at(uintptr_t p)
{
    if (!p || (p & 3) || !readable(reinterpret_cast<void *>(p), 0x2100))
        return false;
    return strncmp(class_of(p), "uEm", 3) == 0;
}

size_t class_size(uintptr_t obj)
{
    uintptr_t vt = game::at<uintptr_t>(obj, 0);
    if (!readable(reinterpret_cast<void *>(vt + 16), 4))
        return 0;
    auto fn = reinterpret_cast<const uint8_t *>(game::at<uintptr_t>(vt, 16));
    if (!readable(fn, 6) || fn[0] != 0xB8 || fn[5] != 0xC3)
        return 0;
    uintptr_t dti;
    memcpy(&dti, fn + 1, 4);
    if (!readable(reinterpret_cast<void *>(dti + 0x18), 4))
        return 0;
    return game::at<uint32_t>(dti, 0x18) & 0x7FFFFF;
}

uint32_t name_hash(const char *s)
{
    uint32_t h = 2166136261u;
    for (; *s; ++s)
        h = (h ^ uint8_t(*s)) * 16777619u;
    return h;
}

bool part_resolve(EnemySlot &s, uintptr_t em, int i)
{
    uintptr_t parent = 0;
    const char *via = "";
    char via_buf[16];
    uintptr_t mp = readable(reinterpret_cast<void *>(em + 0x30), 8) ? game::at<uintptr_t>(em, 0x30) : 0;
    if (monster_at(mp)) {
        parent = mp;
        via = "mpParent";
    } else {
        for (const PartLink &l : kPartLinks)
            if (strncmp(s.name, l.prefix, strlen(l.prefix)) == 0) {
                if (readable(reinterpret_cast<void *>(em + l.off), 4) && monster_at(game::at<uintptr_t>(em, l.off))) {
                    parent = game::at<uintptr_t>(em, l.off);
                    _snprintf(via_buf, sizeof via_buf, "+%X", unsigned(l.off));
                    via = via_buf;
                }
                break;
            }
    }
    if (!parent)
        return false;
    uint32_t pk = kLotNoneKey, pid = kLotNoneId, psplit = 0;
    if (!lot_read(parent, pk, pid, psplit) || !lot_known(pk, pid) || pid > 0xFFFF)
        return false;
    uint16_t tag = 0;
    size_t size = class_size(parent);
    if (size > 0x6000 && size <= 0x10000 && readable(reinterpret_cast<void *>(parent + 0x6000), size - 0x6000))
        for (size_t off = 0x6000; off + 4 <= size; off += 4)
            if (game::at<uintptr_t>(parent, off) == em) {
                tag = uint16_t(kPartField | (off / 4));
                break;
            }
    bool attached = parent == mp;
    int32_t joint = attached ? game::at<int32_t>(em, 0x34) : -1;
    bool tentacle = tentacle_class(s.name) && readable(reinterpret_cast<void *>(em + 0x7324), 0x60);
    int32_t slot = tentacle ? game::at<int32_t>(em, 0x7334) : -1;
    uint8_t piece = tentacle ? game::at<uint8_t>(em, 0x737E) : 0;
    uint8_t kind = game::at<uint8_t>(em, 0x2D);
    if (!tag && attached && joint >= 0 && joint < 0x4000)
        tag = uint16_t(kPartJoint | joint);
    int from = -1;
    if (!tag && tentacle && piece == 1) {
        DWORD now = GetTickCount();
        const float *p = reinterpret_cast<const float *>(em + 0x40);
        float best = 500.f * 500.f;
        for (int k = 0; k < kMax; ++k) {
            const EnemySlot &o = g_slots[k];
            if (k == i || !o.ptr || !o.died || now - o.died > 5000 || o.piece_made || o.part_parent != parent ||
                strcmp(o.name, s.name) != 0 || !o.lot_part || (o.lot_part & 0xC000) == kPartPiece)
                continue;
            const float *at = o.died_at;
            if (readable(reinterpret_cast<void *>(o.ptr), 0xF0) && game::at<uintptr_t>(o.ptr, 0) == o.vt)
                at = reinterpret_cast<const float *>(o.ptr + 0xE0);
            float dx = at[0] - p[0], dy = at[1] - p[1], dz = at[2] - p[2];
            if (dx * dx + dy * dy + dz * dz < best) {
                best = dx * dx + dy * dy + dz * dz;
                from = k;
            }
        }
        if (from < 0 && now - s.first_seen < 2000)
            return false;
        if (from >= 0) {
            g_slots[from].piece_made = true;
            tag = uint16_t(kPartPiece | (g_slots[from].lot_part & 0x3FFF));
        } else {
            logf("enemy: part #%d %s: a cut piece, but no dead tentacle of its parent near it", i, s.name);
        }
    }
    if (!tag)
        tag = uint16_t(kPartOther | (name_hash(s.name) & 0x3FFF));
    s.part_parent = parent;
    s.part_parent_vt = game::at<uintptr_t>(parent, 0);
    s.attached = attached;
    s.lot_key = pk;
    s.lot_id = pid;
    s.lot_split = psplit;
    s.lot_part = tag;
    logf("enemy: part #%d %s %p: parent %s %p (via %s), tag %04X; mpParent %08X joint %d, kind %02X", i, s.name,
         (void *)em, class_of(parent), (void *)parent, via, unsigned(tag), unsigned(mp), int(game::at<int32_t>(em, 0x34)),
         unsigned(kind));
    if (tentacle)
        logf("enemy: part #%d tentacle fields: +7324 %08X slot %d piece %u, +7328 %08X +732C %08X%s", i,
             unsigned(game::at<uintptr_t>(em, 0x7324)), int(slot), unsigned(piece), unsigned(game::at<uintptr_t>(em, 0x7328)),
             unsigned(game::at<uintptr_t>(em, 0x732C)), "");
    if (from >= 0)
        logf("enemy: part #%d is the piece of #%d %s (died %lu ms ago)", i, from, g_slots[from].name,
             GetTickCount() - g_slots[from].died);
    return true;
}

void lot_text(const EnemySlot &s, uintptr_t em, char *out, size_t cap)
{
    if (!lot_known(s.lot_key, s.lot_id)) {
        _snprintf(out, cap, "lot none");
        return;
    }
    bool gp_ok = group_ok(s.lot_part ? s.part_parent : em);
    int n = _snprintf(out, cap, "lot st%u/%02um%02un/e%02u/#%u", s.lot_key & 0x3FF, unsigned(s.lot_split >> 16),
                      unsigned(s.lot_split & 0xFFFF), s.lot_key >> 10, unsigned(s.lot_id));
    if (n < 0 || size_t(n) >= cap)
        return;
    uint16_t t = s.lot_part;
    if (t) {
        switch (t & 0xC000) {
        case kPartField: _snprintf(out + n, cap - n, "/f%04X%s", unsigned(t & 0x3FFF) * 4, gp_ok ? "" : "?"); break;
        case kPartJoint: _snprintf(out + n, cap - n, "/j%u%s", unsigned(t & 0x3FFF), gp_ok ? "" : "?"); break;
        case kPartPiece: _snprintf(out + n, cap - n, "/p%04X%s", unsigned(t & 0x3FFF) * 4, gp_ok ? "" : "?"); break;
        default: _snprintf(out + n, cap - n, "/x%04X%s", unsigned(t & 0x3FFF), gp_ok ? "" : "?"); break;
        }
    } else if (!gp_ok) {
        _snprintf(out + n, cap - n, "?");
    }
}

void id_update(EnemySlot &s, uintptr_t em, int i, DWORD now, bool first)
{
    uint32_t key, id, split;
    if (!lot_read(em, key, id, split))
        return;
    uint32_t old_key = s.lot_key, old_id = s.lot_id, old_split = s.lot_split;
    uint16_t old_part = s.lot_part;
    if (lot_known(key, id)) {
        s.lot_key = key, s.lot_id = id, s.lot_split = split, s.lot_part = 0;
        s.part_parent = 0, s.attached = false;
    } else if (s.part_parent) {
        uint32_t pk, pid, psplit;
        if (readable(reinterpret_cast<void *>(s.part_parent), 4) && game::at<uintptr_t>(s.part_parent, 0) == s.part_parent_vt &&
            lot_read(s.part_parent, pk, pid, psplit) && lot_known(pk, pid) && pid <= 0xFFFF)
            s.lot_key = pk, s.lot_id = pid, s.lot_split = psplit;
        if (readable(reinterpret_cast<void *>(em + 0x30), 4))
            s.attached = game::at<uintptr_t>(em, 0x30) == s.part_parent;
    } else if (now - s.first_seen < 2000 || now - s.part_try >= 1000) {
        s.part_try = now;
        if (!part_resolve(s, em, i))
            s.lot_key = kLotNoneKey, s.lot_id = kLotNoneId, s.lot_split = 0, s.lot_part = 0;
    }
    if (!first && (s.lot_key != old_key || s.lot_id != old_id || s.lot_split != old_split || s.lot_part != old_part)) {
        char lot[96];
        lot_text(s, em, lot, sizeof lot);
        logf("enemy: id #%d %s changed from key %08X id %u split %08X part %04X to %s", i, s.name, unsigned(old_key),
             unsigned(old_id), unsigned(old_split), unsigned(old_part), lot);
    }
}

struct KindOf {
    volatile uintptr_t vt;
    volatile int kind;
};
KindOf g_kinds[32] = {};

}

int enemies_npc_kind(uintptr_t ch)
{
    static int off = -1;
    if (off < 0)
        off = early_flag("no_npc_sync") ? 1 : 0;
    if (off)
        return 0;
    uintptr_t vt = game::at<uintptr_t>(ch, 0);
    if (vt == game::kVtNpc)
        return ENEMY_NPC;
    if (vt == game::kVtPlayer || vt == game::kVtCmc || vt == game::kVtPlayerBase)
        return 0;
    for (const KindOf &k : g_kinds) {
        if (!k.vt)
            break;
        if (k.vt == vt)
            return k.kind > 0 ? k.kind : 0;
    }
    const char *name = class_of(ch);
    int kind = strcmp(name, "uHumanEnemy") == 0 ? ENEMY_HUMAN
               : strcmp(name, "uMultiNpc") == 0 || strcmp(name, "uNpc") == 0 ? ENEMY_NPC
                                                                              : -1;
    for (KindOf &k : g_kinds)
        if (!k.vt || k.vt == vt) {
            k.kind = kind;
            k.vt = vt;
            if (kind > 0)
                logf("enemy: class %s (vtable %08X) synced as %s", name, unsigned(vt), kind == ENEMY_HUMAN ? "a human foe" : "an NPC");
            break;
        }
    return kind > 0 ? kind : 0;
}

void enemies_on_finalize(uintptr_t em)
{
    if (!g_ready)
        return;
    DWORD now = GetTickCount();
    int64_t tw = perf_now();
    EnterCriticalSection(&g_lock);
    perf_add(PERF_WAIT, tw);
    int free_slot = -1, i = 0, kind = 0;
    uintptr_t death = 0, deleted = 0;
    int remove = REMOVE_NONE;
    float at[3];
    memcpy(at, reinterpret_cast<void *>(em + 0x40), sizeof at);
    field_to_world(at);
    for (; i < kMax; ++i) {
        if (g_slots[i].ptr == em && g_slots[i].vt == game::at<uintptr_t>(em, 0)) {
            EnemySlot &o = g_slots[i];
            float d[3] = {at[0] - o.last_pos[0], at[1] - o.last_pos[1], at[2] - o.last_pos[2]};
            if (o.settled && now - o.last_seen > 100 && d[0] * d[0] + d[1] * d[1] + d[2] * d[2] > 1000.f * 1000.f) {
                logf("enemy: #%d %s %p is a new enemy at the same address (%.0f u from its last place after %lu ms)", i, o.name,
                     (void *)em, std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]), now - o.last_seen);
                o.ptr = 0;
                free_slot = i;
                i = kMax;
            }
            break;
        }
        if (free_slot < 0 && (!g_slots[i].ptr || now - g_slots[i].last_seen > 3000))
            free_slot = i;
    }
    if (i == kMax && free_slot >= 0) {
        i = free_slot;
        EnemySlot &s = g_slots[i];
        if (s.ptr)
            logf("enemy: gone #%d %s %p (not updated for %lu ms)", i, s.name, (void *)s.ptr, now - s.last_seen);
        s = {};
        s.ptr = em;
        s.vt = game::at<uintptr_t>(em, 0);
        snprintf(s.name, sizeof s.name, "%s", class_of(em));
        s.kind = uint8_t(enemies_npc_kind(em));
        memcpy(s.spawn, reinterpret_cast<void *>(em + 0x40), sizeof s.spawn);
        s.lot_key = kLotNoneKey;
        s.lot_id = kLotNoneId;
        enemies_world_origin(s.spawn_origin[0], s.spawn_origin[1]);
        memcpy(s.last_pos, at, sizeof at);
        s.first_seen = now;
        id_update(s, em, i, now, true);
        char lot[96];
        lot_text(s, em, lot, sizeof lot);
        logf("enemy: new #%d %s %p at (%.0f, %.0f, %.0f) %s", i, s.name, (void *)em, s.spawn[0], s.spawn[1], s.spawn[2], lot);
        for (int k = 0; k < kMax; ++k)
            if (k != i && g_slots[k].ptr == em) {
                logf("enemy: gone #%d %s %p (address reused)", k, g_slots[k].name, (void *)em);
                g_slots[k] = {};
            }
    }
    if (i < kMax) {
        EnemySlot &s = g_slots[i];
        s.last_seen = now;
        id_update(s, em, i, now, false);
        if (s.kind == ENEMY_MONSTER && lot_placed(s))
            origin_from(em, i, s.name);
        memcpy(s.last_pos, at, sizeof s.last_pos);
        if (!s.settled) {
            const float *p = reinterpret_cast<const float *>(em + 0x40);
            if (now - s.first_seen < 1000) {
                if (s.kind != ENEMY_NPC) {
                    memcpy(s.spawn, p, sizeof s.spawn);
                    enemies_world_origin(s.spawn_origin[0], s.spawn_origin[1]);
                }
            } else {
                s.settled = true;
                char lot[96];
                lot_text(s, em, lot, sizeof lot);
                logf("enemy: settled #%d %s at (%.0f, %.0f, %.0f) %s", i, s.name, s.spawn[0], s.spawn[1], s.spawn[2], lot);
            }
        }
        if (s.kind && !s.died) {
            float *hp = enemies_hp(em);
            if (hp && hp[1] > 0.f && hp[0] <= 0.f) {
                s.died = now | 1;
                memcpy(s.died_at, reinterpret_cast<void *>(em + 0xE0), sizeof s.died_at);
                logf("enemy: died #%d %s at (%.0f, %.0f, %.0f) (HP 0)", i, s.name, game::at<float>(em, 0x40),
                     game::at<float>(em, 0x44), game::at<float>(em, 0x48));
            }
        }
        int64_t tn = perf_now();
        death = enemy_net_after_finalize(g_slots[i], i, &remove);
        perf_add(PERF_NETEN, tn);
        kind = s.kind;
        if (!death && g_kill_req && InterlockedCompareExchange(&g_kill_req, 0, LONG(em)) == LONG(em)) {
            if (float *hp = enemies_hp(em))
                hp[0] = 0.f;
            death = em;
            logf("enemy: #%d %s killed (bridge ekill)", i, s.name);
        }
        if (g_del_req && InterlockedCompareExchange(&g_del_req, 0, LONG(em)) == LONG(em)) {
            deleted = em;
            logf("enemy: #%d %s removed the game's way (bridge edel)", i, s.name);
        }
    }
    uint32_t group = remove == REMOVE_DEATH && i < kMax && lot_placed(g_slots[i]) ? (g_slots[i].lot_key >> 10) & 0x1FF : 0x1FF;
    LeaveCriticalSection(&g_lock);
    uintptr_t sm = group != 0x1FF ? game::at<uintptr_t>(0x018FA504, 0) : 0;
    uint32_t rec[2] = {};
    if (sm) {
        rec[0] = game::at<uint32_t>(sm, 0x18A98 + group * 16);
        rec[1] = game::at<uint32_t>(sm, 0x18A98 + group * 16 + 4);
    }
    if (death && remove != REMOVE_ONLY)
        enemy_net_death(death);
    if (sm)
        logf("enemy: e%02u's group record before / after that death: %08X:%08X / %08X:%08X", unsigned(group), rec[0],
             rec[1], game::at<uint32_t>(sm, 0x18A98 + group * 16), game::at<uint32_t>(sm, 0x18A98 + group * 16 + 4));
    if (death && remove)
        enemies_request_delete(death, kind);
    if (deleted)
        enemies_request_delete(deleted, kind);
}

void enemies_request_delete(uintptr_t em, int kind)
{
    uintptr_t mgr = game::at<uintptr_t>(kind == ENEMY_HUMAN ? 0x018FA4C8 : 0x018FA4B0, 0);
    if (!mgr || !em || !readable(reinterpret_cast<void *>(mgr), 4))
        return;
    using Del_t = void(__thiscall *)(uintptr_t, uintptr_t);
    reinterpret_cast<Del_t>(game::at<uintptr_t>(game::at<uintptr_t>(mgr, 0), 0x30))(mgr, em);
}
void enemies_queue_delete(uintptr_t em) { InterlockedExchange(&g_del_req, LONG(em)); }

float *enemies_hp_raw(uintptr_t ch);
float *enemies_hp(uintptr_t ch)
{
    int64_t t = perf_now();
    float *r = enemies_hp_raw(ch);
    perf_add(PERF_HP, t);
    return r;
}
float *enemies_hp_raw(uintptr_t ch)
{
    if (!ch || !readable(reinterpret_cast<void *>(ch + 0x1D0C), 0x10) || !game::at<uintptr_t>(ch, 0x1D0C))
        return nullptr;
    uintptr_t list = game::at<uintptr_t>(ch, 0x1D18);
    if (!list || !readable(reinterpret_cast<void *>(list), 4))
        return nullptr;
    uintptr_t entry = game::at<uintptr_t>(list, 0);
    if (!entry || !readable(reinterpret_cast<void *>(entry + 8), 8))
        return nullptr;
    return reinterpret_cast<float *>(entry + 8);
}

constexpr uintptr_t kVtLayoutSetEnemy = 0x01593E30;
using EntrySpawn_t = uintptr_t(__fastcall *)(uintptr_t, void *, uintptr_t, int, int, int);
EntrySpawn_t g_entry_spawn_orig = nullptr;
uintptr_t __fastcall hk_entry_spawn(uintptr_t set, void *edx, uintptr_t list, int index, int at_once, int mode)
{
    if (set && list && readable(reinterpret_cast<void *>(list), 4) && readable(reinterpret_cast<void *>(set), 0x50) &&
        game::at<uintptr_t>(set, 0) == kVtLayoutSetEnemy) {
        using Entry_t = uintptr_t(__fastcall *)(uintptr_t, void *, int);
        uintptr_t vt = game::at<uintptr_t>(list, 0);
        uintptr_t e = vt && readable(reinterpret_cast<void *>(vt + 0x3C), 4)
                          ? reinterpret_cast<Entry_t>(game::at<uintptr_t>(vt, 0x3C))(list, nullptr, index) : 0;
        if (e && readable(reinterpret_cast<void *>(e + 4), 4) &&
            net_spawn_gated(game::at<uint32_t>(set, 0x48), game::at<uint32_t>(e, 4), game::at<uint32_t>(set, 0x4C)))
            return 0;
    }
    return g_entry_spawn_orig(set, edx, list, index, at_once, mode);
}

struct LeadGroups {
    uintptr_t g[64];
    int n;
};
LeadGroups g_lead_groups[2] = {};
volatile LONG g_lead_cur = 0;
volatile LONG g_follow_reqs = 0, g_follow_held = 0, g_follow_evict = 0, g_follow_groups = 0, g_follow_masks = 0;
bool lead_group(uintptr_t group)
{
    if (!group)
        return false;
    const LeadGroups &s = g_lead_groups[g_lead_cur & 1];
    for (int k = 0; k < s.n && k < 64; ++k)
        if (s.g[k] == group)
            return true;
    return false;
}
void lead_groups_compute(uintptr_t sm)
{
    LeadGroups s = {};
    static LeadUnit u[128];
    int n = sm ? net_leader_list(u, 128) : 0;
    uint32_t count = n && readable(reinterpret_cast<void *>(sm + 0x15A58), 0x10) ? game::at<uint32_t>(sm, 0x15A58) : 0;
    uintptr_t arr = count ? game::at<uintptr_t>(sm, 0x15A64) : 0;
    for (uint32_t i = 0; arr && i < count && i < 1024 && readable(reinterpret_cast<void *>(arr + i * 4), 4); ++i) {
        uintptr_t set = game::at<uintptr_t>(arr, i * 4);
        if (!set || !readable(reinterpret_cast<void *>(set), 0x80) || game::at<uintptr_t>(set, 0) != kVtLayoutSetEnemy)
            continue;
        uint32_t key = game::at<uint32_t>(set, 0x48), split = game::at<uint32_t>(set, 0x4C);
        uintptr_t g = game::at<uintptr_t>(set, 0x74);
        bool has = false;
        for (int k = 0; k < n && !has; ++k)
            has = u[k].key == key && u[k].split == split;
        bool dup = !g || !has;
        for (int k = 0; k < s.n && !dup; ++k)
            dup = s.g[k] == g;
        if (!dup && s.n < 64)
            s.g[s.n++] = g;
    }
    LONG next = (g_lead_cur + 1) & 1;
    g_lead_groups[next] = s;
    InterlockedExchange(&g_lead_cur, next);
    static LeadGroups last = {};
    if (s.n != last.n || memcmp(s.g, last.g, sizeof s.g) != 0) {
        char t[260] = "";
        int len = 0;
        for (int k = 0; k < s.n && len >= 0 && size_t(len) < sizeof t - 8; ++k)
            len += _snprintf(t + len, sizeof t - len, " e%02u", game::at<uint32_t>(s.g[k], 0x1C) & 0x1FF);
        logf("enemies: follow: groups the leader has units of:%s", s.n ? t : " none");
        last = s;
    }
}
int sets_units(uintptr_t sm, LeadUnit *out, int cap, uintptr_t *ptrs = nullptr)
{
    int n = 0;
    uint32_t count = sm && readable(reinterpret_cast<void *>(sm + 0x15A58), 0x10) ? game::at<uint32_t>(sm, 0x15A58) : 0;
    uintptr_t arr = count ? game::at<uintptr_t>(sm, 0x15A64) : 0;
    for (uint32_t i = 0; arr && i < count && i < 1024 && n < cap && readable(reinterpret_cast<void *>(arr + i * 4), 4); ++i) {
        uintptr_t set = game::at<uintptr_t>(arr, i * 4);
        if (!set || !readable(reinterpret_cast<void *>(set), 0x80) || game::at<uintptr_t>(set, 0) != kVtLayoutSetEnemy)
            continue;
        uint32_t entries = game::at<uint32_t>(set, 0xC);
        uintptr_t units = game::at<uintptr_t>(set, 0x18);
        for (uint32_t k = 0; units && k < entries && k < 256 && n < cap && readable(reinterpret_cast<void *>(units + k * 4), 4); ++k) {
            uintptr_t u = game::at<uintptr_t>(units, k * 4);
            if (!u || (u & 3) || !readable(reinterpret_cast<void *>(u + 0x20EC), 16))
                continue;
            uint32_t key = game::at<uint32_t>(u, 0x20EC), id = game::at<uint32_t>(u, 0x20F0), split = game::at<uint32_t>(u, 0x20F8);
            if (!lot_known(key, id))
                continue;
            if (float *hp = enemies_hp(u))
                if (hp[1] > 0.f && hp[0] <= 0.f)
                    continue;
            if (ptrs)
                ptrs[n] = u;
            out[n++] = {key, id, split};
        }
    }
    return n;
}

void follow_remove_unseen(uintptr_t sm)
{
    struct Lacking {
        uintptr_t u;
        DWORD since;
        bool done;
    };
    static Lacking lack[64];
    static int nl = 0;
    static DWORD next = 0;
    DWORD now = GetTickCount();
    if (next && int(now - next) < 0)
        return;
    next = (now + 500) | 1;
    if (!net_following()) {
        nl = 0;
        return;
    }
    static LeadUnit mine[256];
    static uintptr_t ptr[256];
    int nm = sets_units(sm, mine, 256, ptr);
    static Lacking keep[64];
    int nk = 0;
    for (int k = 0; k < nm && nk < 64; ++k) {
        if (net_leader_has(mine[k].key, mine[k].id, mine[k].split))
            continue;
        bool in_table = false;
        EnterCriticalSection(&g_lock);
        for (int i = 0; i < kMax && !in_table; ++i)
            in_table = g_slots[i].ptr == ptr[k] && now - g_slots[i].last_seen < 3000;
        LeaveCriticalSection(&g_lock);
        if (in_table)
            continue;
        Lacking l = {ptr[k], now, false};
        for (int j = 0; j < nl; ++j)
            if (lack[j].u == ptr[k])
                l = lack[j];
        if (!l.done && int(now - l.since) >= 15000) {
            l.done = true;
            const char *cls = class_of(ptr[k]);
            logf("enemies: follow: e%02u/#%u (%s %p, not in the enemy table): not on the leader for 15 s, removed (deleted; "
                 "its entry follows the leader's list)",
                 unsigned((mine[k].key >> 10) & 0x1FF), unsigned(mine[k].id), cls, (void *)ptr[k]);
            enemies_request_delete(ptr[k], strcmp(cls, "uHumanEnemy") == 0 ? ENEMY_HUMAN : ENEMY_MONSTER);
        }
        keep[nk++] = l;
    }
    memcpy(lack, keep, nk * sizeof(Lacking));
    nl = nk;
}

void enemies_follow_stats(LONG *reqs, LONG *held, LONG *evict, LONG *groups, LONG *masks)
{
    *reqs = InterlockedExchange(&g_follow_reqs, 0);
    *held = InterlockedExchange(&g_follow_held, 0);
    *evict = InterlockedExchange(&g_follow_evict, 0);
    *groups = InterlockedExchange(&g_follow_groups, 0);
    *masks = InterlockedExchange(&g_follow_masks, 0);
}

using SlotReq_t = int(__stdcall *)(uintptr_t, uint32_t *, int, uint32_t, uint32_t *, float *, uint32_t, uint32_t, uint32_t,
                                   uint32_t, uint32_t, uint32_t, uint32_t);
SlotReq_t g_slot_req_orig = nullptr;
volatile LONG g_slot_reqs = 0, g_slot_moved = 0;
int __stdcall hk_slot_req(uintptr_t mgr, uint32_t *key, int id, uint32_t prio, uint32_t *slot, float *pos, uint32_t a7,
                          uint32_t a8, uint32_t a9, uint32_t a10, uint32_t a11, uint32_t a12, uint32_t a13)
{
    float moved[3];
    float *use = pos;
    InterlockedIncrement(&g_slot_reqs);
    uintptr_t holder = game::at<uintptr_t>(0x018FA4EC, 0), ar = 0;
    if (pos && holder && readable(reinterpret_cast<void *>(holder + 0x99C), 4))
        ar = game::at<uintptr_t>(holder, 0x99C);
    float nearest[3];
    int who = -1;
    if (ar && key && pos && readable(reinterpret_cast<void *>(ar + 0xE0), 12) && readable(key, 8) && net_following() &&
        net_leader_has(key[0], uint32_t(id), key[1])) {
        memcpy(moved, reinterpret_cast<const void *>(ar + 0xE0), sizeof moved);
        use = moved;
        InterlockedIncrement(&g_follow_reqs);
        static volatile LONG logged = 0;
        if (InterlockedIncrement(&logged) <= 12)
            logf("enemies: follow: slot request e%02u/#%d (split %08X) at (%.0f, %.0f, %.0f): the leader has it, asked at "
                 "the top score", unsigned((key[0] >> 10) & 0x1FF), id, unsigned(key[1]), pos[0], pos[1], pos[2]);
    } else if (ar && readable(reinterpret_cast<void *>(ar + 0xE0), 12) && readable(pos, 12) &&
        net_group_nearest(pos, reinterpret_cast<const float *>(ar + 0xE0), nearest, &who)) {
        const float *own = reinterpret_cast<const float *>(ar + 0xE0);
        float dx = pos[0] - nearest[0], dy = pos[1] - nearest[1], dz = pos[2] - nearest[2];
        float d = std::sqrt(dx * dx + dy * dy + dz * dz);
        float fwd[3] = {dx, dy, dz};
        uintptr_t cm = game::at<uintptr_t>(0x018D0B6C, 0), cam = 0;
        if (cm && readable(reinterpret_cast<void *>(cm + 0xCE0), 4))
            cam = game::at<uintptr_t>(cm, 0xCE0);
        if (cam && readable(reinterpret_cast<void *>(cam + 0x40), 0x30)) {
            const float *eye = reinterpret_cast<const float *>(cam + 0x40);
            float f[3] = {pos[0] - eye[0], pos[1] - eye[1], pos[2] - eye[2]};
            float len = std::sqrt(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
            if (len > 1e-3f) {
                for (int k = 0; k < 3; ++k)
                    fwd[k] = f[k] / len * d;
            }
        }
        for (int k = 0; k < 3; ++k)
            moved[k] = own[k] + fwd[k];
        use = moved;
        InterlockedIncrement(&g_slot_moved);
        static volatile LONG logged = 0;
        if (InterlockedIncrement(&logged) <= 12)
            logf("enemies: slot request e%02u/#%d at (%.0f, %.0f, %.0f) scored by player %d's distance %.0f (this Arisen's "
                 "%.0f)", unsigned((key ? *key >> 10 : 0) & 0x1FF), id, pos[0], pos[1], pos[2], who, d,
                 std::sqrt((own[0] - pos[0]) * (own[0] - pos[0]) + (own[1] - pos[1]) * (own[1] - pos[1]) +
                           (own[2] - pos[2]) * (own[2] - pos[2])));
    }
    return g_slot_req_orig(mgr, key, id, prio, slot, use, a7, a8, a9, a10, a11, a12, a13);
}
extern "C" void *g_area_orig asm("g_area_orig");
void *g_area_orig = nullptr;
volatile LONG g_area_asks = 0, g_area_others = 0;
extern "C" __attribute__((naked)) bool area_orig_call(uintptr_t group, const float *pt) asm("area_orig_call");
extern "C" __attribute__((naked)) bool area_orig_call(uintptr_t, const float *)
{
    asm volatile("movl 4(%esp), %eax\n\t"
                 "pushl 8(%esp)\n\t"
                 "call *g_area_orig\n\t"
                 "ret\n\t");
}
extern "C" bool __cdecl enemies_area_check(uintptr_t group, const float *pt) asm("enemies_area_check");
extern "C" bool __cdecl enemies_area_check(uintptr_t group, const float *pt)
{
    bool in = area_orig_call(group, pt);
    if (in || !pt)
        return in;
    float others[4][3];
    int n = net_group_positions(others);
    bool lead = lead_group(group);
    uintptr_t holder = game::at<uintptr_t>(0x018FA4EC, 0), ar = 0;
    if ((!n && !lead) || !holder || !readable(reinterpret_cast<void *>(holder + 0x99C), 4) ||
        !(ar = game::at<uintptr_t>(holder, 0x99C)) || !readable(reinterpret_cast<void *>(ar + 0xE0), 12))
        return in;
    float ox = 0.f, oy = 0.f, oz = 0.f;
    uintptr_t org = game::at<uintptr_t>(0x018FA528, 0);
    if (org && readable(reinterpret_cast<void *>(org + 0x20), 0x40) && game::at<uint8_t>(org, 0x20) == 1) {
        ox = game::at<float>(org, 0x50), oy = game::at<float>(org, 0x54), oz = game::at<float>(org, 0x58);
    }
    const float *own = reinterpret_cast<const float *>(ar + 0xE0);
    if (std::fabs(own[0] - ox - pt[0]) > 1.f || std::fabs(own[1] - oy - pt[1]) > 1.f || std::fabs(own[2] - oz - pt[2]) > 1.f)
        return in;
    if (lead) {
        InterlockedIncrement(&g_follow_groups);
        return true;
    }
    InterlockedIncrement(&g_area_asks);
    for (int k = 0; k < n; ++k) {
        float p[4] = {others[k][0] - ox, others[k][1] - oy, others[k][2] - oz, 0.f};
        if (area_orig_call(group, p)) {
            InterlockedIncrement(&g_area_others);
            return true;
        }
    }
    return false;
}
extern "C" __attribute__((naked)) void enemies_area_thunk() asm("enemies_area_thunk");
extern "C" __attribute__((naked)) void enemies_area_thunk()
{
    asm volatile("pushl 4(%esp)\n\t"
                 "pushl %eax\n\t"
                 "call enemies_area_check\n\t"
                 "addl $8, %esp\n\t"
                 "ret $4\n\t");
}

extern "C" void *g_rescore_orig asm("g_rescore_orig");
void *g_rescore_orig = nullptr;
extern "C" const float *__cdecl enemies_rescore_ref(const float *ref, const float *place, uintptr_t unit) asm("enemies_rescore_ref");
extern "C" const float *__cdecl enemies_rescore_ref(const float *ref, const float *place, uintptr_t unit)
{
    static thread_local float near_ref[4];
    int who = -1;
    if (ref && place && unit && readable(reinterpret_cast<void *>(unit + 0x20EC), 16) && net_following()) {
        uint32_t key = game::at<uint32_t>(unit, 0x20EC), id = game::at<uint32_t>(unit, 0x20F0), split = game::at<uint32_t>(unit, 0x20F8);
        if (lot_known(key, id)) {
            if (net_leader_has(key, id, split)) {
                InterlockedIncrement(&g_follow_held);
                return place;
            }
            InterlockedIncrement(&g_follow_evict);
            near_ref[0] = place[0] + 1e6f, near_ref[1] = place[1], near_ref[2] = place[2], near_ref[3] = 0.f;
            return near_ref;
        }
    }
    if (ref && place && net_group_nearest(place, ref, near_ref, &who)) {
        near_ref[3] = 0.f;
        return near_ref;
    }
    return ref;
}
extern "C" __attribute__((naked)) void enemies_rescore_thunk() asm("enemies_rescore_thunk");
extern "C" __attribute__((naked)) void enemies_rescore_thunk()
{
    asm volatile("pushl %ecx\n\t"
                 "pushl %edx\n\t"
                 "pushl %ecx\n\t"
                 "pushl %edi\n\t"
                 "pushl %edx\n\t"
                 "call enemies_rescore_ref\n\t"
                 "addl $12, %esp\n\t"
                 "popl %edx\n\t"
                 "popl %ecx\n\t"
                 "movl %eax, %edx\n\t"
                 "jmp *g_rescore_orig\n\t");
}

struct DomSet {
    uintptr_t g[16];
    int n;
};
DomSet g_dom_sets[2] = {};
volatile LONG g_dom_cur = 0;
using DomPick_t = void(__stdcall *)(uintptr_t, int);
DomPick_t g_dom_pick_orig = nullptr;

extern "C" uintptr_t __cdecl dom_resolve(uintptr_t dominant, uintptr_t group) asm("dom_resolve");
extern "C" uintptr_t __cdecl dom_resolve(uintptr_t dominant, uintptr_t group)
{
    if (!dominant || dominant == group || !group)
        return dominant;
    if (lead_group(group))
        return group;
    const DomSet &s = g_dom_sets[g_dom_cur & 1];
    for (int k = 0; k < s.n && k < 16; ++k)
        if (s.g[k] == group)
            return group;
    return dominant;
}

#define DOM_SAVE_XMM                                                                                                   \
    "subl $128, %esp\n\t"                                                                                              \
    "movdqu %xmm0, 0(%esp)\n\tmovdqu %xmm1, 16(%esp)\n\tmovdqu %xmm2, 32(%esp)\n\tmovdqu %xmm3, 48(%esp)\n\t"          \
    "movdqu %xmm4, 64(%esp)\n\tmovdqu %xmm5, 80(%esp)\n\tmovdqu %xmm6, 96(%esp)\n\tmovdqu %xmm7, 112(%esp)\n\t"
#define DOM_LOAD_XMM                                                                                                   \
    "movdqu 0(%esp), %xmm0\n\tmovdqu 16(%esp), %xmm1\n\tmovdqu 32(%esp), %xmm2\n\tmovdqu 48(%esp), %xmm3\n\t"          \
    "movdqu 64(%esp), %xmm4\n\tmovdqu 80(%esp), %xmm5\n\tmovdqu 96(%esp), %xmm6\n\tmovdqu 112(%esp), %xmm7\n\t"        \
    "addl $128, %esp\n\t"
extern "C" __attribute__((naked)) void dom_stub_spawn() asm("dom_stub_spawn");
extern "C" __attribute__((naked)) void dom_stub_spawn()
{
    asm volatile("movl 0x15a14(%edx,%ecx,4), %eax\n\t"
                 "pushl %ecx\n\tpushl %edx\n\t" DOM_SAVE_XMM "pushl %edi\n\tpushl %eax\n\t"
                 "call dom_resolve\n\t"
                 "addl $8, %esp\n\t" DOM_LOAD_XMM "popl %edx\n\tpopl %ecx\n\t"
                 "ret\n\t");
}
extern "C" __attribute__((naked)) void dom_stub_slot() asm("dom_stub_slot");
extern "C" __attribute__((naked)) void dom_stub_slot()
{
    asm volatile("movl 0x15a14(%ebx,%ecx,4), %edi\n\t"
                 "pushl %eax\n\tpushl %ecx\n\tpushl %edx\n\t" DOM_SAVE_XMM "pushl %eax\n\tpushl %edi\n\t"
                 "call dom_resolve\n\t"
                 "addl $8, %esp\n\t"
                 "movl %eax, %edi\n\t" DOM_LOAD_XMM "popl %edx\n\tpopl %ecx\n\tpopl %eax\n\t"
                 "ret\n\t");
}
extern "C" __attribute__((naked)) void dom_stub_unit() asm("dom_stub_unit");
extern "C" __attribute__((naked)) void dom_stub_unit()
{
    asm volatile("movl 0x15a14(%ecx,%eax,4), %edi\n\t"
                 "pushl %eax\n\tpushl %ecx\n\tpushl %edx\n\t" DOM_SAVE_XMM "pushl %edx\n\tpushl %edi\n\t"
                 "call dom_resolve\n\t"
                 "addl $8, %esp\n\t"
                 "movl %eax, %edi\n\t" DOM_LOAD_XMM "popl %edx\n\tpopl %ecx\n\tpopl %eax\n\t"
                 "ret\n\t");
}

void dom_compute(uintptr_t sm)
{
    DomSet s = {};
    float others[4][3];
    int n = net_group_positions(others);
    uintptr_t holder = game::at<uintptr_t>(0x018FA4EC, 0), ar = 0;
    bool ok = n > 0 && sm && holder && readable(reinterpret_cast<void *>(holder + 0x99C), 4) &&
              (ar = game::at<uintptr_t>(holder, 0x99C)) && readable(reinterpret_cast<void *>(ar + 0xE0), 12);
    static DWORD said = 0;
    if (!ok && n > 0 && GetTickCount() - said > 5000) {
        said = GetTickCount();
        logf("enemies: dominant groups: not worked out (group players %d, set manager %p, Arisen holder %p, Arisen %p)", n,
             (void *)sm, (void *)holder, (void *)ar);
    }
    if (ok) {
        float ox = 0.f, oy = 0.f, oz = 0.f;
        uintptr_t org = game::at<uintptr_t>(0x018FA528, 0);
        if (org && readable(reinterpret_cast<void *>(org + 0x20), 0x40) && game::at<uint8_t>(org, 0x20) == 1)
            ox = game::at<float>(org, 0x50), oy = game::at<float>(org, 0x54), oz = game::at<float>(org, 0x58);
        float P[5][4] = {};
        const float *own = reinterpret_cast<const float *>(ar + 0xE0);
        P[0][0] = own[0] - ox, P[0][1] = own[1] - oy, P[0][2] = own[2] - oz;
        for (int k = 0; k < n && k < 4; ++k)
            P[k + 1][0] = others[k][0] - ox, P[k + 1][1] = others[k][1] - oy, P[k + 1][2] = others[k][2] - oz;
        int np = 1 + (n < 4 ? n : 4);
        uintptr_t best[5][3] = {};
        uint32_t count = game::at<uint32_t>(sm, 0x15AB0);
        uintptr_t arr = game::at<uintptr_t>(sm, 0x15ABC);
        for (uint32_t i = 0; arr && i < count && i < 512; ++i) {
            uintptr_t g = game::at<uintptr_t>(arr, i * 4);
            if (!g || !readable(reinterpret_cast<void *>(g), 0x170) || game::at<uint8_t>(g, 0x160))
                continue;
            uint32_t lvl = game::at<uint8_t>(g, 0x15E), cls = game::at<uint32_t>(g, 0x18);
            if (!lvl || cls > 2)
                continue;
            uint32_t prio = (game::at<uint32_t>(g, 0x1C) >> 9) & 0x3FFFF, t = game::at<uint32_t>(g, 0x164);
            for (int p = 0; p < np; ++p) {
                if (!area_orig_call(g, P[p]))
                    continue;
                uintptr_t b = best[p][cls];
                bool take = !b;
                if (b) {
                    uint32_t bl = game::at<uint8_t>(b, 0x15E), bp = (game::at<uint32_t>(b, 0x1C) >> 9) & 0x3FFFF,
                             bt = game::at<uint32_t>(b, 0x164);
                    take = lvl > bl || (lvl == bl && (prio > bp || (prio == bp && t > bt)));
                }
                if (take)
                    best[p][cls] = g;
            }
        }
        for (int p = 0; p < np; ++p)
            for (int c = 0; c < 3; ++c) {
                uintptr_t g = best[p][c];
                bool dup = !g;
                for (int k = 0; k < s.n && !dup; ++k)
                    dup = s.g[k] == g;
                if (!dup && s.n < 16)
                    s.g[s.n++] = g;
            }
    }
    LONG next = (g_dom_cur + 1) & 1;
    g_dom_sets[next] = s;
    InterlockedExchange(&g_dom_cur, next);
    static DomSet last = {};
    if (s.n != last.n || memcmp(s.g, last.g, sizeof s.g) != 0) {
        char t[200] = "";
        int len = 0;
        for (int k = 0; k < s.n && len >= 0 && size_t(len) < sizeof t - 24; ++k)
            len += _snprintf(t + len, sizeof t - len, " e%02u (class %u)", game::at<uint32_t>(s.g[k], 0x1C) & 0x1FF,
                             game::at<uint32_t>(s.g[k], 0x18));
        logf("enemies: dominant groups for the players of this group: %s", s.n ? t : " none (this game's only)");
        last = s;
    }
}
using WolfUpdate_t = void(__fastcall *)(uintptr_t);
WolfUpdate_t g_wolf_orig = nullptr;
void __fastcall hk_wolf_update(uintptr_t ch)
{
    g_wolf_orig(ch);
    enemies_on_finalize(ch);
}
constexpr uintptr_t kEmptyFn = 0x005F35C0;
void feed_static_bodies()
{
    uintptr_t mgr = game::at<uintptr_t>(0x018FA4B0, 0);
    if (!mgr || !readable(reinterpret_cast<void *>(mgr + 0x24), 0x10))
        return;
    uint32_t n = game::at<uint32_t>(mgr, 0x24);
    uintptr_t arr = game::at<uintptr_t>(mgr, 0x30);
    for (uint32_t i = 0; arr && i < n && i < 1024 && readable(reinterpret_cast<void *>(arr + i * 4), 4); ++i) {
        uintptr_t u = game::at<uintptr_t>(arr, i * 4);
        if (!u || (u & 3) || !readable(reinterpret_cast<void *>(u), 4))
            continue;
        uintptr_t vt = game::at<uintptr_t>(u, 0);
        if (!vt || !readable(reinterpret_cast<void *>(vt + 0x16C), 4) || game::at<uintptr_t>(vt, 0x16C) != kEmptyFn)
            continue;
        if (strncmp(class_of(u), "uEm", 3) == 0)
            enemies_on_finalize(u);
    }
}

void __stdcall hk_dom_pick(uintptr_t sm, int full)
{
    g_dom_pick_orig(sm, full);
    feed_static_bodies();
    dom_compute(sm);
    lead_groups_compute(sm);
    enemies_follow_report(false);
    follow_remove_unseen(sm);
    static DWORD next_pub = 0;
    DWORD now = GetTickCount();
    if (net_leading() && (!next_pub || int(now - next_pub) >= 0)) {
        next_pub = (now + 200) | 1;
        static LeadUnit u[90];
        net_lead_entries_publish(u, sets_units(sm, u, 90));
    }
}

void follow_describe(uintptr_t sm, const LeadUnit &u)
{
    char lot[24];
    _snprintf(lot, sizeof lot, "e%02u/#%u", unsigned((u.key >> 10) & 0x1FF), unsigned(u.id));
    uint32_t count = sm && readable(reinterpret_cast<void *>(sm + 0x15A58), 0x10) ? game::at<uint32_t>(sm, 0x15A58) : 0;
    uintptr_t arr = count ? game::at<uintptr_t>(sm, 0x15A64) : 0, set = 0;
    for (uint32_t i = 0; arr && i < count && i < 1024 && !set && readable(reinterpret_cast<void *>(arr + i * 4), 4); ++i) {
        uintptr_t s = game::at<uintptr_t>(arr, i * 4);
        if (s && readable(reinterpret_cast<void *>(s), 0x98) && game::at<uintptr_t>(s, 0) == kVtLayoutSetEnemy &&
            game::at<uint32_t>(s, 0x48) == u.key && game::at<uint32_t>(s, 0x4C) == u.split)
            set = s;
    }
    if (!set) {
        logf("enemies: follow:   %s (split %08X): no layout set for it here (%u sets)", lot, unsigned(u.split), unsigned(count));
        return;
    }
    uintptr_t gp = game::at<uintptr_t>(set, 0x74);
    bool gok = gp && readable(reinterpret_cast<void *>(gp), 0x170);
    uint32_t entries = game::at<uint32_t>(set, 0xC);
    uintptr_t list = game::at<uintptr_t>(set, 4), units = game::at<uintptr_t>(set, 0x18), states = game::at<uintptr_t>(set, 0x2C);
    uintptr_t lvt = list && readable(reinterpret_cast<void *>(list), 4) ? game::at<uintptr_t>(list, 0) : 0;
    int at = -1, area = -9;
    uint32_t est = 0xFFFFFFFF;
    uintptr_t unit = 0;
    if (lvt && readable(reinterpret_cast<void *>(lvt + 0x3C), 4)) {
        using Entry_t = uintptr_t(__fastcall *)(uintptr_t, void *, int);
        for (uint32_t k = 0; k < entries && k < 256 && at < 0; ++k) {
            uintptr_t e = reinterpret_cast<Entry_t>(game::at<uintptr_t>(lvt, 0x3C))(list, nullptr, int(k));
            if (!e || !readable(reinterpret_cast<void *>(e + 4), 8) || game::at<uint32_t>(e, 4) != u.id)
                continue;
            at = int(k);
            uintptr_t si = game::at<uintptr_t>(e, 8);
            area = si && readable(reinterpret_cast<void *>(si + 0x70), 4) ? game::at<int32_t>(si, 0x70) : -9;
            if (units && readable(reinterpret_cast<void *>(units + k * 4), 4))
                unit = game::at<uintptr_t>(units, k * 4);
            uintptr_t so = states && readable(reinterpret_cast<void *>(states + k * 4), 4) ? game::at<uintptr_t>(states, k * 4) : 0;
            if (so && readable(reinterpret_cast<void *>(so + 4), 4))
                est = game::at<uint32_t>(so, 4);
        }
    }
    int slot = -1;
    uint32_t sstate = 0, skey = ((u.key >> 10) & 0xFFFF) | (u.id << 16);
    for (int k = 0; k < 10 && readable(reinterpret_cast<void *>(sm + 0x844), 320); ++k)
        if (game::at<uint32_t>(sm, 0x844 + k * 32 + 8) && game::at<uint32_t>(sm, 0x844 + k * 32 + 0x14) == skey) {
            slot = k;
            sstate = game::at<uint32_t>(sm, 0x844 + k * 32 + 8);
        }
    logf("enemies: follow:   %s: group on %u state %08X%s, set +31 %u +43 %u +44 %u mask %08X; entry %d of %u, area %d, state "
         "%d, unit %08X; slot %d (state %u)",
         lot, gok ? game::at<uint8_t>(gp, 0x15E) : 0xFF, gok ? game::at<uint32_t>(gp, 0x15C) : 0, lead_group(gp) ? " (a leader group)" : "",
         game::at<uint8_t>(set, 0x31), game::at<uint8_t>(set, 0x43), game::at<uint8_t>(set, 0x44), game::at<uint32_t>(set, 0x78), at,
         unsigned(entries), area, int(est), unsigned(unit), slot, unsigned(sstate));
}
void enemies_follow_report(bool always)
{
    static DWORD next = 0;
    DWORD now = GetTickCount();
    if (!always && (!net_following() || (next && int(now - next) < 0)))
        return;
    next = (now + 10000) | 1;
    static LeadUnit u[128];
    int n = net_leader_list(u, 128);
    static LeadUnit mine[256];
    uintptr_t sm0 = game::at<uintptr_t>(0x018FA504, 0);
    int nm = sm0 ? sets_units(sm0, mine, 256) : 0;
    int missing = 0, extra = 0;
    char ex[200] = "";
    int el = 0;
    for (int k = 0; k < nm; ++k) {
        bool has = false;
        for (int j = 0; j < n && !has; ++j)
            has = u[j].key == mine[k].key && u[j].id == mine[k].id && u[j].split == mine[k].split;
        if (!has && ++extra <= 8 && el >= 0 && size_t(el) < sizeof ex - 16)
            el += _snprintf(ex + el, sizeof ex - el, " e%02u/#%u", unsigned((mine[k].key >> 10) & 0x1FF), unsigned(mine[k].id));
    }
    static bool twin[128], noset[128];
    int nosets = 0;
    uint32_t count = sm0 && readable(reinterpret_cast<void *>(sm0 + 0x15A58), 0x10) ? game::at<uint32_t>(sm0, 0x15A58) : 0;
    uintptr_t arr = count ? game::at<uintptr_t>(sm0, 0x15A64) : 0;
    for (int j = 0; j < n; ++j) {
        twin[j] = false;
        for (int k = 0; k < nm && !twin[j]; ++k)
            twin[j] = u[j].key == mine[k].key && u[j].id == mine[k].id && u[j].split == mine[k].split;
        noset[j] = !twin[j];
        for (uint32_t i = 0; noset[j] && arr && i < count && i < 1024 && readable(reinterpret_cast<void *>(arr + i * 4), 4); ++i) {
            uintptr_t s = game::at<uintptr_t>(arr, i * 4);
            if (s && readable(reinterpret_cast<void *>(s), 0x50) && game::at<uintptr_t>(s, 0) == kVtLayoutSetEnemy &&
                game::at<uint32_t>(s, 0x48) == u[j].key && game::at<uint32_t>(s, 0x4C) == u[j].split)
                noset[j] = false;
        }
        missing += !twin[j] && !noset[j];
        nosets += noset[j];
    }
    if (!always && !missing && !extra)
        return;
    logf("enemies: follow: the leader has %d placed units, %d without a twin here (and %d no enemy layout set here is for); "
         "here %d placed units the leader lacks%s%s",
         n, missing, nosets, extra, extra ? ":" : "", ex);
    uintptr_t sm = sm0;
    for (int j = 0, shown = 0; j < n && shown < 6 && sm; ++j)
        if (!twin[j] && (!noset[j] || always)) {
            follow_describe(sm, u[j]);
            ++shown;
        }
}

bool patch_call(uintptr_t at, const uint8_t (&expect)[7], void *stub)
{
    if (memcmp(reinterpret_cast<void *>(at), expect, 7) != 0)
        return false;
    DWORD old = 0;
    if (!VirtualProtect(reinterpret_cast<void *>(at), 7, PAGE_EXECUTE_READWRITE, &old))
        return false;
    uint8_t code[7] = {0xE8, 0, 0, 0, 0, 0x90, 0x90};
    int32_t rel = int32_t(reinterpret_cast<uintptr_t>(stub) - (at + 5));
    memcpy(code + 1, &rel, 4);
    memcpy(reinterpret_cast<void *>(at), code, 7);
    VirtualProtect(reinterpret_cast<void *>(at), 7, old, &old);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void *>(at), 7);
    return true;
}

using AreaMask_t = uint32_t(__fastcall *)(uintptr_t, uintptr_t);
AreaMask_t g_area_mask_orig = nullptr;
volatile LONG g_mask_added = 0;
using BoxContains_t = bool(__fastcall *)(uintptr_t, void *, const float *, float);
uint32_t __fastcall hk_area_mask(uintptr_t ecx, uintptr_t group)
{
    uint32_t m = g_area_mask_orig(ecx, group);
    if (lead_group(group) && readable(reinterpret_cast<void *>(group + 0x14C), 4)) {
        uint32_t nb = game::at<uint32_t>(group, 0x14C), all = nb >= 32 ? 0xFFFFFFFFu : (1u << nb) - 1;
        if (all & ~m)
            InterlockedIncrement(&g_follow_masks);
        return m | all;
    }
    uint32_t bits = net_group_party_bits();
    float others[4][3];
    int np = net_group_positions(others);
    if ((!bits && !np) || !group || !readable(reinterpret_cast<void *>(group + 0x14C), 0x10))
        return m;
    float ox = 0.f, oy = 0.f, oz = 0.f;
    uintptr_t org = game::at<uintptr_t>(0x018FA528, 0);
    if (org && readable(reinterpret_cast<void *>(org + 0x20), 0x40) && game::at<uint8_t>(org, 0x20) == 1)
        ox = game::at<float>(org, 0x50), oy = game::at<float>(org, 0x54), oz = game::at<float>(org, 0x58);
    uint32_t n = game::at<uint32_t>(group, 0x14C), add = 0;
    uintptr_t arr = game::at<uintptr_t>(group, 0x158);
    for (uint32_t i = 0; arr && i < n && i < 32 && readable(reinterpret_cast<void *>(arr + i * 4), 4); ++i) {
        uintptr_t box = game::at<uintptr_t>(arr, i * 4);
        if (!box)
            break;
        if (!readable(reinterpret_cast<void *>(box), 0x18) || (m & (1u << i)))
            continue;
        if (game::at<uint32_t>(box, 0x10) & bits) {
            add |= 1u << i;
            continue;
        }
        uintptr_t vt = game::at<uintptr_t>(box, 0);
        if (!vt || !readable(reinterpret_cast<void *>(vt + 0x20), 4))
            continue;
        auto contains = reinterpret_cast<BoxContains_t>(game::at<uintptr_t>(vt, 0x20));
        for (int k = 0; k < np; ++k) {
            float p[4] = {others[k][0] - ox, others[k][1] - oy, others[k][2] - oz, 0.f};
            if (contains(box, nullptr, p, 0.f)) {
                add |= 1u << i;
                break;
            }
        }
    }
    if (add & ~m)
        InterlockedIncrement(&g_mask_added);
    return m | add;
}

void enemies_area_stats(LONG *asks, LONG *others, LONG *masks)
{
    *asks = InterlockedExchange(&g_area_asks, 0);
    *others = InterlockedExchange(&g_area_others, 0);
    *masks = InterlockedExchange(&g_mask_added, 0);
}

void enemies_slot_stats(LONG *reqs, LONG *moved)
{
    *reqs = InterlockedExchange(&g_slot_reqs, 0);
    *moved = InterlockedExchange(&g_slot_moved, 0);
}

bool enemies_install()
{
    InitializeCriticalSectionAndSpinCount(&g_lock, 20000);
    InterlockedExchange(&g_ready, 1);
    void *dmg = reinterpret_cast<void *>(game::kApplyDamage);
    MH_STATUS st = MH_CreateHook(dmg, reinterpret_cast<void *>(&enemies_damage_thunk), &g_damage_orig);
    if (st == MH_OK)
        st = MH_EnableHook(dmg);
    void *death = reinterpret_cast<void *>(game::kEnemyDeath);
    MH_STATUS st2 = MH_CreateHook(death, reinterpret_cast<void *>(&hk_death), reinterpret_cast<void **>(&g_death_orig));
    if (st2 == MH_OK)
        st2 = MH_EnableHook(death);
    if (game::build() != game::BUILD_STEAM) {
        logf("enemies: table ready; hook ApplyDamage %p: %s, EnemyDeath %p: %s; spawn hooks off (%s build: Steam addresses)",
             dmg, MH_StatusToString(st), death, MH_StatusToString(st2), game::build_name());
        return st == MH_OK && st2 == MH_OK;
    }
    void *spawn = reinterpret_cast<void *>(0x00760A10);
    MH_STATUS st3 = MH_CreateHook(spawn, reinterpret_cast<void *>(&hk_entry_spawn), reinterpret_cast<void **>(&g_entry_spawn_orig));
    if (st3 == MH_OK)
        st3 = MH_EnableHook(spawn);
    void *req = reinterpret_cast<void *>(0x004A6830);
    MH_STATUS st4 = MH_CreateHook(req, reinterpret_cast<void *>(&hk_slot_req), reinterpret_cast<void **>(&g_slot_req_orig));
    if (st4 == MH_OK)
        st4 = MH_EnableHook(req);
    void *area = reinterpret_cast<void *>(0x00CC5670);
    MH_STATUS st5 = MH_CreateHook(area, reinterpret_cast<void *>(&enemies_area_thunk), &g_area_orig);
    if (st5 == MH_OK)
        st5 = MH_EnableHook(area);
    void *rescore = reinterpret_cast<void *>(0x004A4DA0);
    MH_STATUS st6 = MH_CreateHook(rescore, reinterpret_cast<void *>(&enemies_rescore_thunk), &g_rescore_orig);
    if (st6 == MH_OK)
        st6 = MH_EnableHook(rescore);
    void *amask = reinterpret_cast<void *>(0x00CC55B0);
    MH_STATUS st8 = MH_CreateHook(amask, reinterpret_cast<void *>(&hk_area_mask), reinterpret_cast<void **>(&g_area_mask_orig));
    if (st8 == MH_OK)
        st8 = MH_EnableHook(amask);
    logf("enemies: hook AreaMask %p: %s", amask, MH_StatusToString(st8));
    void *wolf = reinterpret_cast<void *>(0x008F0FF0);
    MH_STATUS st9 = MH_CreateHook(wolf, reinterpret_cast<void *>(&hk_wolf_update), reinterpret_cast<void **>(&g_wolf_orig));
    if (st9 == MH_OK)
        st9 = MH_EnableHook(wolf);
    logf("enemies: hook WolfUpdate %p: %s", wolf, MH_StatusToString(st9));
    void *pick = reinterpret_cast<void *>(0x004A5050);
    MH_STATUS st7 = MH_CreateHook(pick, reinterpret_cast<void *>(&hk_dom_pick), reinterpret_cast<void **>(&g_dom_pick_orig));
    if (st7 == MH_OK)
        st7 = MH_EnableHook(pick);
    static const uint8_t k_spawn[7] = {0x8B, 0x84, 0x8A, 0x14, 0x5A, 0x01, 0x00};
    static const uint8_t k_slot[7] = {0x8B, 0xBC, 0x8B, 0x14, 0x5A, 0x01, 0x00};
    static const uint8_t k_unit[7] = {0x8B, 0xBC, 0x81, 0x14, 0x5A, 0x01, 0x00};
    bool p1 = st7 == MH_OK && patch_call(0x00760B94, k_spawn, reinterpret_cast<void *>(&dom_stub_spawn));
    bool p2 = st7 == MH_OK && patch_call(0x004A4500, k_slot, reinterpret_cast<void *>(&dom_stub_slot));
    bool p3 = st7 == MH_OK && patch_call(0x008871F6, k_unit, reinterpret_cast<void *>(&dom_stub_unit));
    logf("enemies: table ready (fed by the finalize hook); hook ApplyDamage %p: %s, EnemyDeath %p: %s, EntrySpawn %p: %s, "
         "SlotRequest %p: %s, GroupAreas %p: %s, SlotRescore %p: %s, DominantPick %p: %s; dominant checks patched: %s %s %s",
         dmg, MH_StatusToString(st), death, MH_StatusToString(st2), spawn, MH_StatusToString(st3), req,
         MH_StatusToString(st4), area, MH_StatusToString(st5), rescore, MH_StatusToString(st6), pick, MH_StatusToString(st7),
         p1 ? "spawn" : "SPAWN-NO", p2 ? "slot" : "SLOT-NO", p3 ? "unit" : "UNIT-NO");
    return st == MH_OK && st2 == MH_OK && st3 == MH_OK && st4 == MH_OK && st5 == MH_OK && st6 == MH_OK && st7 == MH_OK &&
           st9 == MH_OK && p1 && p2 && p3;
}

void __fastcall hk_death(uintptr_t ch, void *edx, uint32_t a, uint32_t hit)
{
    g_death_orig(ch, edx, a, hit);
    if (!g_ready)
        return;
    EnterCriticalSection(&g_lock);
    for (int i = 0; i < kMax; ++i)
        if (g_slots[i].ptr == ch && !g_slots[i].died && GetTickCount() - g_slots[i].last_seen < 1000) {
            g_slots[i].died = GetTickCount() | 1;
            memcpy(g_slots[i].died_at, reinterpret_cast<void *>(ch + 0xE0), sizeof g_slots[i].died_at);
            logf("enemy: died #%d %s at (%.0f, %.0f, %.0f)%s", i, g_slots[i].name, game::at<float>(ch, 0x40),
                 game::at<float>(ch, 0x44), game::at<float>(ch, 0x48), hit ? "" : " (no hit info: forced)");
        }
    LeaveCriticalSection(&g_lock);
}

int enemies_snapshot(EnemySlot *out, int cap)
{
    if (!g_ready)
        return 0;
    DWORD now = GetTickCount();
    int n = 0;
    EnterCriticalSection(&g_lock);
    for (int i = 0; i < kMax && n < cap; ++i)
        if (g_slots[i].ptr && now - g_slots[i].last_seen < 1000) {
            out[n] = g_slots[i];
            out[n].index = i;
            ++n;
        }
    LeaveCriticalSection(&g_lock);
    return n;
}

const char *enemies_class_of(uintptr_t obj) { return class_of(obj); }

namespace {
bool strict_in_exe(uintptr_t a) { return a >= 0x00401000 && a < 0x01A00000; }
bool safe_read(uintptr_t a, void *out, size_t n)
{
    SIZE_T got = 0;
    return a >= 0x10000 && a + n > a &&
           ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void *>(a), out, n, &got) && got == n;
}
uintptr_t safe_u32(uintptr_t a, bool &ok)
{
    uintptr_t v = 0;
    ok = safe_read(a, &v, 4);
    return v;
}
const char *strict_class_of(uintptr_t q)
{
    bool ok;
    if (q < 0x10000 || q >= 0x7FFF0000 || (q & 3))
        return nullptr;
    uintptr_t vt = safe_u32(q, ok);
    if (!ok || !strict_in_exe(vt))
        return nullptr;
    uintptr_t fnp = safe_u32(vt + 16, ok);
    uint8_t fn[6];
    if (!ok || !strict_in_exe(fnp) || !safe_read(fnp, fn, 6) || fn[0] != 0xB8 || fn[5] != 0xC3)
        return nullptr;
    uintptr_t dti;
    memcpy(&dti, fn + 1, 4);
    uintptr_t nm = strict_in_exe(dti) ? safe_u32(dti + 4, ok) : 0;
    return ok && strict_in_exe(nm) ? reinterpret_cast<const char *>(nm) : nullptr;
}
volatile uintptr_t g_setman = 0, g_setman_vt = 0, g_unit_vt = 0;
volatile uintptr_t g_units = 0;
volatile int g_unit_count = 0;
constexpr size_t kSetManSize = 0x1B950;

int unit_run(uintptr_t a, uintptr_t uvt, size_t max)
{
    bool ok;
    int n = 0;
    while ((n + 1) * 0x20 <= int(max) && safe_u32(a + n * 0x20, ok) == uvt && ok)
        ++n;
    return n;
}
}

void spawn_scan_pass()
{
    bool ok;
    uintptr_t m = g_setman;
    if (m && safe_u32(m, ok) == g_setman_vt && ok && g_units && safe_u32(g_units, ok) == g_unit_vt && ok)
        return;
    g_unit_count = 0;
    g_setman = 0;
    static uint8_t page[0x1000];
    for (uintptr_t g = 0x01600000; g < 0x019C0000; g += 0x1000) {
        if (!safe_read(g, page, sizeof page))
            continue;
        for (int o = 0; o < 0x1000; o += 4) {
            uintptr_t q;
            memcpy(&q, page + o, 4);
            if (q < 0x10000 || q >= 0x7FFF0000 || (q & 3) || strict_in_exe(q))
                continue;
            const char *c = strict_class_of(q);
            if (!c || strcmp(c, "sSetManager") != 0)
                continue;
            uintptr_t first = 0, uvt = 0;
            size_t room = 0;
            for (uintptr_t off = 4; off + 0x20 <= kSetManSize && !first; off += 4) {
                uintptr_t v = safe_u32(q + off, ok);
                if (!ok)
                    break;
                if (strict_in_exe(v)) {
                    const char *uc = strict_class_of(q + off);
                    if (uc && strcmp(uc, "sSetManager::cUnitData") == 0) {
                        first = q + off;
                        uvt = v;
                        room = kSetManSize - off;
                    }
                } else if (v >= 0x10000 && v < 0x7FFF0000 && !(v & 3)) {
                    const char *uc = strict_class_of(v);
                    if (uc && strcmp(uc, "sSetManager::cUnitData") == 0) {
                        first = v;
                        uvt = safe_u32(v, ok);
                        room = 0x100000;
                    }
                }
            }
            if (!first)
                continue;
            int n = unit_run(first, uvt, room);
            g_setman_vt = safe_u32(q, ok);
            g_unit_vt = uvt;
            g_units = first;
            g_unit_count = n;
            g_setman = q;
            logf("spawn: sSetManager %08X (global %08X), %d units at %08X", unsigned(q), unsigned(g + o), n, unsigned(first));
            return;
        }
    }
    static bool told = false;
    if (!told) {
        told = true;
        logf("spawn: no sSetManager found among the exe's globals");
    }
}

bool spawn_units(uintptr_t &units, int &count)
{
    units = g_units;
    count = g_unit_count;
    return g_setman && units && count > 0;
}

bool enemy_spawn_entry(uintptr_t em, uint32_t &block, uint32_t &group_index, uint32_t &kind)
{
    uintptr_t units;
    int count;
    if (!em || !spawn_units(units, count))
        return false;
    uint32_t buf[64 * 8];
    for (int k = 0; k < count; k += 64) {
        int m = count - k < 64 ? count - k : 64;
        if (!safe_read(units + k * 0x20, buf, m * 0x20))
            return false;
        for (int j = 0; j < m; ++j) {
            const uint32_t *u = buf + j * 8;
            if (u[0] == g_unit_vt && u[1] == em) {
                kind = u[3];
                block = u[4];
                group_index = u[5];
                return true;
            }
        }
    }
    return false;
}

namespace {
volatile uintptr_t g_sunit = 0, g_sunit_vt = 0, g_line_vt = 0;
}
bool sunit_find()
{
    bool ok;
    if (g_sunit && safe_u32(g_sunit, ok) == g_sunit_vt && ok)
        return true;
    static uint8_t page[0x1000];
    for (uintptr_t g = 0x01600000; g < 0x019C0000; g += 0x1000) {
        if (!safe_read(g, page, sizeof page))
            continue;
        for (int o = 0; o < 0x1000; o += 4) {
            uintptr_t q;
            memcpy(&q, page + o, 4);
            if (q < 0x10000 || q >= 0x7FFF0000 || (q & 3) || strict_in_exe(q))
                continue;
            const char *c = strict_class_of(q);
            if (!c || (strcmp(c, "sUnit") != 0 && strcmp(c, "sUnitExt") != 0))
                continue;
            const char *lc = strict_class_of(q + 0x20);
            if (!lc || strcmp(lc, "sUnit::MoveLine") != 0)
                continue;
            g_line_vt = safe_u32(q + 0x20, ok);
            g_sunit_vt = safe_u32(q, ok);
            g_sunit = q;
            logf("units: sUnit %08X (global %08X)", unsigned(q), unsigned(g + o));
            return true;
        }
    }
    return false;
}

template <class F> void units_each(F f)
{
    if (!sunit_find())
        return;
    bool ok;
    for (int k = 0; k < 64; ++k) {
        uintptr_t line = g_sunit + 0x20 + k * 0x18;
        if (safe_u32(line, ok) != g_line_vt || !ok)
            break;
        uintptr_t u = safe_u32(line + 0xC, ok);
        for (int n = 0; ok && u && n < 4000; ++n) {
            if (!f(u, k))
                return;
            u = safe_u32(u + 0xC, ok);
        }
    }
}

void units_scan()
{
    struct Count {
        const char *c;
        int line, n;
    };
    static Count counts[256];
    int nc = 0, objs = 0;
    units_each([&](uintptr_t u, int line) {
        const char *c = strict_class_of(u);
        if (!c)
            return true;
        int j = 0;
        while (j < nc && !(counts[j].c == c && counts[j].line == line))
            ++j;
        if (j == nc && nc < 256)
            counts[nc++] = {c, line, 0};
        if (j < nc)
            ++counts[j].n;
        if (strncmp(c, "uOm", 3) == 0 && objs < 120) {
            ++objs;
            float p[3] = {};
            safe_read(u + 0x40, p, sizeof p);
            logf("units: line %d %s %08X at (%.0f, %.0f, %.0f)", line, c, unsigned(u), p[0], p[1], p[2]);
        }
        return true;
    });
    for (int j = 0; j < nc; ++j)
        logf("units: line %d: %d %s", counts[j].line, counts[j].n, counts[j].c);
}

void enemies_setinfo_scan()
{
    spawn_scan_pass();
    {
        uintptr_t units;
        int count;
        static uint32_t buf[0x20 / 4];
        if (spawn_units(units, count))
            for (int k = 0; k < count; ++k) {
                if (!safe_read(units + k * 0x20, buf, 0x20) || !buf[1])
                    continue;
                const char *c = strict_class_of(buf[1]);
                float p[3] = {};
                safe_read(buf[1] + 0x40, p, sizeof p);
                logf("setinfo: unit %d %s kind %X block %X group %X index %u at (%.0f, %.0f, %.0f) +18 %08X", k, c ? c : "?",
                     buf[3], buf[4], buf[5] & 0xFFFF, buf[5] >> 16, p[0], p[1], p[2], buf[6]);
            }
    }
    EnemySlot list[kMax];
    int n = enemies_snapshot(list, kMax);
    for (int k = 0; k < n; ++k) {
        uint32_t b = 0, gi = 0, kd = 0;
        bool ok = enemy_spawn_entry(list[k].ptr, b, gi, kd);
        float p[3];
        memcpy(p, reinterpret_cast<const void *>(list[k].ptr + 0x40), sizeof p);
        field_to_world(p);
        logf("setinfo: #%d %s at (%.0f, %.0f, %.0f) spawn (%.0f, %.0f, %.0f): %s kind %X block %X group %X index %u",
             list[k].index, list[k].name, p[0], p[1], p[2], list[k].spawn[0], list[k].spawn[1], list[k].spawn[2],
             ok ? "entry" : "no entry", kd, b, gi & 0xFFFF, gi >> 16);
    }
}

void enemies_dump(const char *name)
{
    EnemySlot list[kMax];
    int n = enemies_snapshot(list, kMax);
    char path[128];
    _snprintf(path, sizeof path, "dump_%s_enemies.txt", name);
    FILE *idx = fopen(path, "w");
    for (int k = 0; k < n; ++k) {
        const EnemySlot &s = list[k];
        size_t size = 0x8000;
        while (size > 0x1000 && !readable(reinterpret_cast<void *>(s.ptr), size))
            size -= 0x1000;
        _snprintf(path, sizeof path, "dump_%s_e%d.bin", name, s.index);
        if (FILE *f = fopen(path, "wb")) {
            fwrite(reinterpret_cast<const void *>(s.ptr), 1, size, f);
            fclose(f);
        }
        float p[3];
        memcpy(p, reinterpret_cast<const void *>(s.ptr + 0x40), sizeof p);
        field_to_world(p);
        float hp[8] = {};
        uintptr_t list = game::at<uintptr_t>(s.ptr, 0x1D18), entry = 0;
        if (game::at<uintptr_t>(s.ptr, 0x1D0C) && readable(reinterpret_cast<void *>(list), 4))
            entry = game::at<uintptr_t>(list, 0);
        if (entry && readable(reinterpret_cast<void *>(entry), 0x28))
            memcpy(hp, reinterpret_cast<void *>(entry + 8), sizeof hp);
        uint32_t dsize = 0;
        uintptr_t vt = game::at<uintptr_t>(s.ptr, 0);
        if (readable(reinterpret_cast<void *>(vt + 16), 4)) {
            auto fn = reinterpret_cast<const uint8_t *>(game::at<uintptr_t>(vt, 16));
            uintptr_t dti = 0;
            if (readable(fn, 6) && fn[0] == 0xB8 && fn[5] == 0xC3)
                memcpy(&dti, fn + 1, 4);
            if (dti && readable(reinterpret_cast<void *>(dti + 0x18), 4))
                dsize = game::at<uint32_t>(dti, 0x18);
        }
        if (idx)
            fprintf(idx, "%d %s %08X %.1f %.1f %.1f size %X hp@%08X %.1f %.1f %.1f %.1f %.1f %.1f %.1f %.1f follows %d dead %d "
                         "lot %08X %u %08X %04X kind %d dti %08X\n",
                    s.index, s.name, unsigned(s.ptr), p[0], p[1], p[2], unsigned(size), unsigned(entry), hp[0], hp[1],
                    hp[2], hp[3], hp[4], hp[5], hp[6], hp[7], enemy_net_follow(s.index), s.died ? 1 : 0,
                    unsigned(s.lot_key), unsigned(s.lot_id), unsigned(s.lot_split), unsigned(s.lot_part), s.kind,
                    unsigned(dsize));
    }
    if (idx)
        fclose(idx);
    _snprintf(path, sizeof path, "dump_%s_regions.txt", name);
    if (FILE *f = fopen(path, "w")) {
        MEMORY_BASIC_INFORMATION mbi;
        for (uintptr_t a = 0x10000; a < 0xFFFF0000u && VirtualQuery(reinterpret_cast<void *>(a), &mbi, sizeof mbi);) {
            uintptr_t next = uintptr_t(mbi.BaseAddress) + mbi.RegionSize;
            if (mbi.State == MEM_COMMIT)
                fprintf(f, "%08X %08X\n", unsigned(uintptr_t(mbi.BaseAddress)), unsigned(mbi.RegionSize));
            if (next <= a)
                break;
            a = next;
        }
        fclose(f);
    }
    logf("enemies: dumped %d enemies as %s", n, name);
}

void enemies_queue_kill(uintptr_t em) { InterlockedExchange(&g_kill_req, LONG(em)); }

bool enemies_origin_settling()
{
    DWORD t = DWORD(g_origin_changed);
    return t && GetTickCount() - t < 300;
}

void enemies_spawn_now(const EnemySlot &s, float out[3])
{
    float ox = 0.f, oz = 0.f;
    enemies_world_origin(ox, oz);
    if (s.kind == ENEMY_MONSTER && lot_placed(s) && s.ptr &&
        readable(reinterpret_cast<void *>(s.ptr + 0xE50), 12)) {
        const float *w = reinterpret_cast<const float *>(s.ptr + 0xE50);
        out[0] = w[0] - ox, out[1] = w[1], out[2] = w[2] - oz;
        return;
    }
    out[0] = s.spawn[0] + s.spawn_origin[0] - ox;
    out[1] = s.spawn[1];
    out[2] = s.spawn[2] + s.spawn_origin[1] - oz;
}

bool game_origin(float &ox, float &oz)
{
    if (game::build() != game::BUILD_STEAM)
        return false;
    uintptr_t org = game::at<uintptr_t>(0x018FA528, 0);
    if (!org || !readable(reinterpret_cast<void *>(org + 0x20), 0x40))
        return false;
    if (game::at<uint8_t>(org, 0x20) == 1) {
        ox = std::round(-game::at<float>(org, 0x50) / 10000.f) * 10000.f + 0.f;
        oz = std::round(-game::at<float>(org, 0x58) / 10000.f) * 10000.f + 0.f;
    } else {
        ox = oz = 0.f;
    }
    if (std::fabs(ox) > 3e8f || std::fabs(oz) > 3e8f)
        return false;
    LONG packed = LONG(uint16_t(int16_t(ox / 10000.f))) | LONG(uint32_t(uint16_t(int16_t(oz / 10000.f))) << 16);
    LONG old = InterlockedExchange(&g_origin_tiles, packed);
    InterlockedExchange(&g_origin_tick, LONG(GetTickCount() | 1));
    if (old != packed) {
        InterlockedExchange(&g_origin_changed, LONG(GetTickCount() | 1));
        logf("world: origin (%.0f, %.0f), the game's", ox, oz);
    }
    return true;
}

bool enemies_world_origin(float &ox, float &oz)
{
    if (game_origin(ox, oz))
        return true;
    DWORD tick = DWORD(g_origin_tick);
    int32_t stage = -1;
    DWORD loaded = stages_last_load(&stage);
    if (!tick || (loaded && int(tick - loaded) < 0)) {
        ox = oz = 0.f;
        return false;
    }
    LONG p = g_origin_tiles;
    ox = float(int16_t(uint16_t(p & 0xFFFF))) * 10000.f;
    oz = float(int16_t(uint16_t(uint32_t(p) >> 16))) * 10000.f;
    return true;
}

bool mem_safe_read(uintptr_t a, void *out, size_t n) { return safe_read(a, out, n); }
const char *enemies_strict_class_of(uintptr_t q) { return strict_class_of(q); }
void units_each_raw(bool (*f)(uintptr_t unit, void *ctx), void *ctx)
{
    units_each([&](uintptr_t u, int) { return f(u, ctx); });
}

void placename_scan()
{
    auto text_at = [](uintptr_t a, char *out, size_t cap) -> bool {
        uint8_t b[96];
        if (!safe_read(a, b, sizeof b))
            return false;
        size_t n = 0;
        while (n < sizeof b && n + 1 < cap && ((b[n] >= 0x20 && b[n] < 0x7F) || b[n] >= 0xC0))
            out[n] = char(b[n]), ++n;
        out[n] = 0;
        if (n >= 5)
            return true;
        n = 0;
        while (2 * n + 1 < sizeof b && n + 1 < cap && b[2 * n] >= 0x20 && b[2 * n] < 0x7F && b[2 * n + 1] == 0)
            out[n] = char(b[2 * n]), ++n;
        out[n] = 0;
        return n >= 5;
    };
    units_each([&](uintptr_t u, int line) {
        const char *c = strict_class_of(u);
        if (!c || (!strstr(c, "PlaceName") && !strstr(c, "MiniMap") && !strstr(c, "uGUIMap")))
            return true;
        logf("placename: line %d %s %08X", line, c, unsigned(u));
        char s[100];
        for (int off = 0; off < 0x1000; off += 4) {
            if (text_at(u + off, s, sizeof s))
                logf("placename:   +%X inline \"%s\"", off, s);
            bool ok;
            uintptr_t p = safe_u32(u + off, ok);
            if (ok && p >= 0x10000 && p < 0x7FFF0000 && !strict_in_exe(p) && text_at(p, s, sizeof s))
                logf("placename:   +%X -> %08X \"%s\"", off, unsigned(p), s);
        }
        return true;
    });
}
