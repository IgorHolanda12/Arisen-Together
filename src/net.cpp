#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstddef>
#include <cstring>
#include <string>

#include "game.h"
#include "mod.h"

void guest_talk_mirror(DWORD now);

namespace {

enum MsgType : uint8_t { MSG_INPUT = 1, MSG_SNAP = 2, MSG_ENEMIES = 3, MSG_SAVE_CHUNK = 4, MSG_SAVE_NEED = 5, MSG_CHAR_CHUNK = 6,
                       MSG_PROGRESS = 7, MSG_PROGRESS_ACK = 8, MSG_PSTATE = 9, MSG_LOOT = 10, MSG_FLAGS = 11, MSG_REVIVE = 12, MSG_QUEST = 13,
                       MSG_AREA_ENEMIES = 14, MSG_PING = 15, MSG_PONG = 16, MSG_EVENT = 17, MSG_LOTTERY = 18,
                       MSG_LEAD_ENTRIES = 19, MSG_ACT_PARAMS = 20,
                       MSG_OBJECTS = 21,
                       MSG_HUMAN_CMDS = 22 };

constexpr uintptr_t kYawA = 0x2578, kYawB = 0x257C, kYawC = 0x2654;
constexpr uintptr_t kAimPitch = 0x32D4;
constexpr uintptr_t kPosCopies[] = {0xE70, 0xE80, 0x1990};
constexpr uintptr_t kCmdCopyFrom = 0x0C, kCmdCopyLen = 0x54;
constexpr uintptr_t kCmdExtraFrom = 0x88, kCmdExtraLen = 0x08;

#pragma pack(push, 1)
struct Header {
    uint32_t magic;
    uint8_t type;
    uint32_t seq;
};
struct InputMsg {
    Header h;
    XINPUT_GAMEPAD pad;
    uint8_t has_camera;
    float eye[3], target[3];
    uint8_t away;
};
struct PingMsg {
    Header h;
    uint32_t t;
    uint32_t wall;
    uint32_t build;
};
constexpr uint32_t build_hash(const char *s, uint32_t h = 2166136261u) { return *s ? build_hash(s + 1, (h ^ uint8_t(*s)) * 16777619u) : h; }
constexpr uint32_t kBuild = build_hash(__DATE__ " " __TIME__);

uint32_t wall_ms()
{
    SYSTEMTIME st;
    GetLocalTime(&st);
    return ((st.wHour * 60u + st.wMinute) * 60u + st.wSecond) * 1000u + st.wMilliseconds;
}
struct CharState {
    uint8_t valid;
    float pos[4];
    float rot[4];
    float yaw;
    uint8_t cmd[kCmdCopyLen];
    uint8_t cmd_extra[kCmdExtraLen];
    float hp, hp_max;
    int32_t action;
    int8_t hold_slot;
    int16_t hold_enemy;
    int8_t hold_kind;
    float hold_off[3];
    uint8_t lantern;
    int32_t look[16];
    uint8_t monster_held;
    float aim_pitch;
};
struct StageState {
    int32_t stage, point;
    int32_t jump_stage, jump_point;
    uint8_t jumping;
    uint32_t jump_seq;
    uint32_t reload_seq;
    int32_t day, day_ms;
    int32_t time_speed;
    int32_t event;
    uint32_t party_sig;
    uint16_t pad_buttons;
    int16_t pad_lx, pad_ly;
    float talk_npc[3];
};
struct SnapMsg {
    Header h;
    CharState ch[kParty];
    StageState stage;
    int8_t my_pawn;
    uint32_t char_crc;
    int32_t stage_of[kParty];
    int64_t exp_total[4];
    int8_t my_slot;
    uint32_t loot_got;
    uint32_t flags_got;
    uint8_t down[4];
    int8_t pawn_of[4];
    uint64_t steam_of[4];
    uint16_t ping_of[4];
    uint8_t world;
};
struct QuestChunkMsg {
    Header h;
    uint8_t origin;
    uint32_t crc, total;
    uint16_t index, len;
    uint8_t data[1024];
};
struct LotteryMsg {
    Header h;
    uint8_t from;
    LotteryRecord r;
};
struct EventMsg {
    Header h;
    uint8_t from;
    uint8_t pad;
    uint16_t id;
    int32_t event;
    int32_t stage;
};
struct ReviveMsg {
    Header h;
    uint8_t target;
};
struct PStateMsg {
    Header h;
    CharState me;
    int32_t stage;
    int64_t exp_total;
    uint32_t loot_session;
    uint32_t loot_applied[4];
    uint32_t flags_applied[4];
    uint8_t downed;
    uint8_t world;
};
constexpr int kFlagsPage = 80;
struct FlagsMsg {
    Header h;
    uint8_t slot;
    uint32_t session, first;
    uint8_t n;
    FlagEv ev[kFlagsPage];
};
constexpr int kLootPage = 24;
struct LootMsg {
    Header h;
    uint8_t slot;
    uint32_t session, first;
    uint8_t n;
    LootEv ev[kLootPage];
};
struct EnemyState {
    uint8_t slot;
    uint32_t cls;
    float spawn[3];
    float pos[3];
    float rot[4];
    float yaw;
    float hp, hp_max;
    uint8_t died;
    uint32_t lot_key, lot_id;
    uint8_t world;
    uint32_t lot_split;
    uint16_t lot_part;
    uint8_t attached;
    uint32_t act;
    uint8_t act_starts;
};
constexpr int kEnemiesPerPage = 15;
struct EnemiesMsg {
    Header h;
    uint8_t count;
    EnemyState e[kEnemiesPerPage];
};
constexpr int enemies_msg_size(int count) { return int(offsetof(EnemiesMsg, e) + count * sizeof(EnemyState)); }
constexpr int kAreaPerPage = 15;
struct AreaEnemiesMsg {
    Header h;
    int32_t stage;
    uint8_t from;
    uint8_t count;
    EnemyState e[kAreaPerPage];
};
constexpr int area_msg_size(int count) { return int(offsetof(AreaEnemiesMsg, e) + count * sizeof(EnemyState)); }
constexpr int kLeadEntriesMax = 90;
struct LeadEntriesMsg {
    Header h;
    int32_t stage;
    uint8_t from;
    uint8_t count;
    LeadUnit e[kLeadEntriesMax];
};
constexpr int lead_entries_size(int count) { return int(offsetof(LeadEntriesMsg, e) + count * sizeof(LeadUnit)); }
static_assert(lead_entries_size(kLeadEntriesMax) <= 1200, "LEAD_ENTRIES over 1200 bytes");
struct ActParams {
    uint8_t slot;
    uint8_t starts;
    uint32_t act;
    uint8_t has;
    float dest[3];
    uint32_t p[16];
    uint8_t grab;
    uint32_t grab_dti;
};
constexpr int kActParamsMax = 13;
struct ActParamsMsg {
    Header h;
    int32_t stage;
    uint8_t from;
    uint8_t count;
    ActParams e[kActParamsMax];
};
constexpr int act_params_size(int count) { return int(offsetof(ActParamsMsg, e) + count * sizeof(ActParams)); }
static_assert(act_params_size(kActParamsMax) <= 1200, "ACT_PARAMS over 1200 bytes");
constexpr uintptr_t kHumanCmdFrom = 0x0C, kHumanCmdLen = 0x30;
struct HumanCmd {
    uint8_t slot;
    uint8_t cmd[kHumanCmdLen];
    float aim;
    uint32_t press_act;
    uint16_t press_ms;
};
constexpr int kHumanCmdsMax = 20;
struct HumanCmdsMsg {
    Header h;
    int32_t stage;
    uint8_t from;
    uint8_t count;
    HumanCmd e[kHumanCmdsMax];
};
constexpr int human_cmds_size(int count) { return int(offsetof(HumanCmdsMsg, e) + count * sizeof(HumanCmd)); }
static_assert(human_cmds_size(kHumanCmdsMax) <= 1200, "HUMAN_CMDS over 1200 bytes");
static_assert(sizeof(Header) + 1 + kEnemiesPerPage * sizeof(EnemyState) <= 1200, "ENEMIES page over 1200 bytes");
static_assert(area_msg_size(kAreaPerPage) <= 1200, "AREA_ENEMIES page over 1200 bytes");
static_assert(sizeof(SnapMsg) <= 1200, "SNAP over 1200 bytes");
static_assert(sizeof(LotteryMsg) <= 1200, "LOTTERY over 1200 bytes");
constexpr int kObjPerMsg = 16;
struct ObjectsMsg {
    Header h;
    int32_t stage;
    uint8_t from;
    uint8_t count;
    ObjState o[kObjPerMsg];
};
constexpr int objects_msg_size(int count) { return int(offsetof(ObjectsMsg, o) + count * sizeof(ObjState)); }
static_assert(objects_msg_size(kObjPerMsg) <= 1200, "OBJECTS over 1200 bytes");
constexpr int kSaveChunk = 1024;
struct SaveChunkMsg {
    Header h;
    uint32_t version, total, crc;
    uint32_t sig;
    uint16_t index, len;
    uint8_t data[kSaveChunk];
};
struct CharChunkMsg {
    Header h;
    uint32_t total, crc;
    uint16_t index, len;
    uint8_t data[1024];
};
constexpr int kSaveNeedMax = 200;
struct ProgressAckMsg {
    Header h;
    uint32_t crc;
};
struct SaveNeedMsg {
    Header h;
    uint32_t have;
    uint32_t receiving;
    uint16_t count;
    uint16_t index[kSaveNeedMax];
};
#pragma pack(pop)

constexpr uint32_t kMagic = 0x33475441;
constexpr uint32_t kFamily = 0x00475441, kFamilyUpstream = 0x00434444;
bool other_version(uint32_t magic) { return ((magic & 0x00FFFFFF) == kFamily || (magic & 0x00FFFFFF) == kFamilyUpstream) && magic != kMagic; }
volatile LONG g_old_peer = 0;
NetRole g_role = NET_OFF;
SOCKET g_sock = INVALID_SOCKET;
bool g_steam = false;
constexpr DWORD kPeerTimeoutMs = 10000;
uint32_t g_seq = 0;
volatile LONG g_enemy_ready = 0;
long g_rx = 0, g_tx = 0;

constexpr int kMaxPeers = 3;
struct Peer {
    bool used;
    sockaddr_in addr;
    uint64_t steam;
    uint32_t last_seq;
    DWORD last_rx;
    uint16_t rtt;
    DWORD rtt_at;
    int pawn;
    uint8_t holder[0x40], camera[0x80];
    volatile LONG cam_valid;
    DWORD next_cam_log;
    uint32_t save_have;
    std::vector<uint8_t> save_queued;
    DWORD save_last_full;
    std::string char_data;
    std::vector<uint8_t> char_got;
    uint32_t char_crc_want, char_crc;
    uint32_t prog_acked;
    DWORD prog_next;
    CharState pstate;
    int32_t pstage;
    DWORD pstate_tick;
    int64_t exp_total;
    uint32_t loot_applied[4], loot_joined[4];
    DWORD loot_next;
    uint32_t flags_applied[4], flags_joined[4];
    uint8_t downed;
};
Peer g_peers[kMaxPeers] = {};
int g_pawn_order[kPawns] = {1, 2, 0};

bool g_shared = false;
CharState g_rp[kParty] = {};
float g_rp_org[kParty][2] = {};
uint32_t g_rp_press[kParty] = {}, g_rp_release[kParty] = {};
DWORD g_rp_tick[kParty] = {};
bool g_rp_away[kParty] = {};
bool g_rp_hidden[kParty] = {};
float g_rp_vel[kParty][3] = {};
DWORD g_rp_at[kParty] = {};
struct RpSample {
    DWORD t;
    float pos[3];
    float org[2];
};
constexpr int kRpSamples = 16;
RpSample g_rp_hist[kParty][kRpSamples] = {};
int g_rp_hist_n[kParty] = {};

std::vector<LootEv> g_loot_log[4];
uint32_t g_loot_session[4] = {};
uint32_t g_loot_applied[4] = {}, g_loot_known[4] = {};
uint32_t g_loot_my_session = 0;
std::vector<FlagEv> g_flags_log[4];
uint32_t g_flags_session[4] = {};
uint32_t g_flags_applied[4] = {}, g_flags_known[4] = {};
struct QuestBlob {
    std::vector<uint8_t> data;
    uint32_t crc = 0;
    DWORD since = 0, next = 0;
    std::vector<uint8_t> got;
    std::vector<uint8_t> in;
    uint32_t in_crc = 0;
} g_qb[4];
CRITICAL_SECTION g_qb_lock;
constexpr float kParkHeight = 3000.f;

volatile LONG g_acc_press[kParty] = {}, g_acc_release[kParty] = {};

CRITICAL_SECTION g_snap_lock;
SnapMsg g_snap = {};
bool g_have_snap = false;
uint32_t g_pending_press[kParty] = {}, g_pending_release[kParty] = {};

const char *peer_name(const Peer &p, char *buf, size_t cap)
{
    if (g_steam) {
        _snprintf(buf, cap, "friend ...%04u", link_steam_short(p.steam));
    } else {
        char ip[64] = "?";
        inet_ntop(AF_INET, const_cast<in_addr *>(&p.addr.sin_addr), ip, sizeof ip);
        _snprintf(buf, cap, "%s:%u", ip, ntohs(p.addr.sin_port));
    }
    return buf;
}

CRITICAL_SECTION g_cap_lock;
CharState g_cap[kParty] = {};
DWORD g_cap_tick[kParty] = {};

void shared_set_hidden(int i, bool hidden)
{
    if (i < 1 || i >= kParty)
        return;
    static DWORD unused_since[kParty] = {};
    DWORD now = GetTickCount();
    if (!hidden)
        unused_since[i] = 0;
    else if (!unused_since[i])
        unused_since[i] = now | 1;
    if (hidden && !g_rp_hidden[i] && int(now - unused_since[i]) < 3000)
        return;
    EnterCriticalSection(&g_cap_lock);
    if (g_rp_hidden[i] != hidden)
        logf("net: shared: party %d %s", i, hidden ? "is a pawn no player uses: kept out of sight" : "is used again");
    g_rp_hidden[i] = hidden;
    if (hidden)
        g_rp_tick[i] = GetTickCount() | 1;
    LeaveCriticalSection(&g_cap_lock);
}

}
void guest_world(uintptr_t gm, const StageState &h);
namespace {

int32_t my_stage()
{
    uintptr_t gm = game::at<uintptr_t>(game::kGameMain, 0);
    return gm && current_stage_area() ? game::at<int32_t>(gm, 0x34) : -1;
}

void rp_rebase(int i, float ox, float oz)
{
    if (g_rp_org[i][0] != ox || g_rp_org[i][1] != oz) {
        g_rp[i].pos[0] += g_rp_org[i][0] - ox;
        g_rp[i].pos[2] += g_rp_org[i][1] - oz;
        g_rp_org[i][0] = ox, g_rp_org[i][1] = oz;
    }
    for (int k = 0; k < g_rp_hist_n[i]; ++k) {
        RpSample &h = g_rp_hist[i][k];
        if (h.org[0] != ox || h.org[1] != oz) {
            h.pos[0] += h.org[0] - ox;
            h.pos[2] += h.org[1] - oz;
            h.org[0] = ox, h.org[1] = oz;
        }
    }
}

void shared_take(int i, const CharState &c, bool here, float ox, float oz)
{
    if (i < 1 || i >= kParty)
        return;
    uint32_t p, r;
    memcpy(&p, c.cmd + (0x2C - kCmdCopyFrom), 4);
    memcpy(&r, c.cmd + (0x34 - kCmdCopyFrom), 4);
    EnterCriticalSection(&g_cap_lock);
    DWORD t = GetTickCount();
    if (c.valid)
        rp_rebase(i, ox, oz);
    if (c.valid && here) {
        RpSample *h = g_rp_hist[i];
        int &n = g_rp_hist_n[i];
        if (n) {
            const RpSample &last = h[n - 1];
            float jx = c.pos[0] - last.pos[0], jz = c.pos[2] - last.pos[2];
            if (t - last.t > 500 || jx * jx + jz * jz > 400.f * 400.f)
                n = 0;
        }
        if (n == kRpSamples) {
            memmove(h, h + 1, sizeof(RpSample) * (kRpSamples - 1));
            --n;
        }
        h[n].t = t;
        memcpy(h[n].pos, c.pos, sizeof h[n].pos);
        h[n].org[0] = ox, h[n].org[1] = oz;
        ++n;
        int from = -1;
        for (int k = 0; k < n - 1 && from < 0; ++k)
            if (t - h[k].t <= 600 && t - h[k].t >= 150)
                from = k;
        if (from >= 0) {
            float dt = (t - h[from].t) / 1000.f;
            for (int a = 0; a < 3; ++a)
                g_rp_vel[i][a] = (c.pos[a] - h[from].pos[a]) / dt;
        } else if (n == 1) {
            memset(g_rp_vel[i], 0, sizeof g_rp_vel[i]);
        }
        g_rp_at[i] = t | 1;
    }
    if (!c.valid) {
        LeaveCriticalSection(&g_cap_lock);
        return;
    }
    g_rp[i] = c;
    g_rp[i].valid = c.valid && here;
    g_rp_away[i] = c.valid && !here;
    if (c.valid) {
        if (here) {
            g_rp_press[i] |= p;
            g_rp_release[i] |= r;
        }
        g_rp_tick[i] = GetTickCount() | 1;
    }
    LeaveCriticalSection(&g_cap_lock);
}

void shared_from_snap(const SnapMsg &m, float ox, float oz)
{
    int me = m.my_pawn;
    int32_t here = my_stage();
    if (me < 0 || me >= kPawns)
        return;
    bool used[kParty] = {};
    for (int i = 0; i < kParty; ++i) {
        if (i == 1 + me)
            continue;
        int local = i == 0 ? 1 + me : i;
        used[local] = true;
        if (m.ch[i].valid)
            shared_take(local, m.ch[i], here >= 0 && m.stage_of[i] == here, ox, oz);
        else if (i >= 1)
            used[local] = false;
    }
    for (int s = 1; s < 4; ++s)
        if (m.pawn_of[s] >= 0 && m.pawn_of[s] < kPawns && m.pawn_of[s] != me)
            used[1 + m.pawn_of[s]] = true;
    used[1 + me] = true;
    for (int i = 1; i < kParty; ++i)
        shared_set_hidden(i, !used[i]);
}

}
bool net_shared_in_host_area();
namespace {
bool shared_in_host_area() { return net_shared_in_host_area(); }
}
bool net_shared_in_host_area()
{
    if (!g_have_snap)
        return false;
    EnterCriticalSection(&g_snap_lock);
    int32_t hs = g_snap.stage.stage;
    LeaveCriticalSection(&g_snap_lock);
    return hs >= 0 && hs == my_stage();
}
uintptr_t holder_of(uintptr_t ch)
{
    uintptr_t p = game::at<uintptr_t>(ch, 0x30);
    if (!p || p == ch)
        return 0;
    if (p == possess_arisen())
        return p;
    for (int k = 0; k < kPawns; ++k)
        if (p == possess_pawn_ptr(k))
            return p;
    return 0;
}
bool log_actions_flag()
{
    static int v = -1;
    if (v < 0)
        v = early_flag("test_log_actions") ? 1 : 0;
    return v == 1;
}
bool monster_caught(uintptr_t ch)
{
    return ch && strncmp(action_name(ch), "cEm", 3) == 0;
}

void world_pos(uintptr_t ch, float out[3])
{
    memcpy(out, reinterpret_cast<void *>(ch + 0x40), 12);
    if (uintptr_t h = holder_of(ch))
        if (std::fabs(out[0]) + std::fabs(out[1]) + std::fabs(out[2]) < 600.f && !holder_of(h))
            for (int k = 0; k < 3; ++k)
                out[k] += game::at<float>(h, 0x40 + 4 * k);
}

namespace {

void shared_apply_commands(uintptr_t ch, int i)
{
    if (!net_puppet(i))
        return;
    EnterCriticalSection(&g_cap_lock);
    CharState s = g_rp[i];
    bool away = g_rp_away[i] || g_rp_hidden[i];
    uint32_t press = g_rp_press[i], release = g_rp_release[i];
    g_rp_press[i] = g_rp_release[i] = 0;
    LeaveCriticalSection(&g_cap_lock);
    uintptr_t cmd = ch + game::kCmd;
    if (away) {
        memset(reinterpret_cast<void *>(cmd + kCmdCopyFrom), 0, kCmdCopyLen);
        memset(reinterpret_cast<void *>(cmd + kCmdExtraFrom), 0, kCmdExtraLen);
        return;
    }
    memcpy(reinterpret_cast<void *>(cmd + kCmdCopyFrom), s.cmd, kCmdCopyLen);
    memcpy(reinterpret_cast<void *>(cmd + kCmdExtraFrom), s.cmd_extra, kCmdExtraLen);
    game::at<uint32_t>(cmd, 0x2C) = press;
    game::at<uint32_t>(cmd, 0x34) = release;
    if ((uint32_t(s.action) >> 16) != 0 && s.aim_pitch == s.aim_pitch && std::fabs(s.aim_pitch) < 1.6f) {
        static DWORD next_log[kParty] = {};
        float was = game::at<float>(ch, kAimPitch);
        if ((std::fabs(was - s.aim_pitch) > 0.05f || log_actions_flag()) && (!next_log[i] || int(GetTickCount() - next_log[i]) >= 0)) {
            next_log[i] = (GetTickCount() + 1000) | 1;
            logf("net: shared: party %d aims at pitch %.3f here, its player at %.3f (0x%X): set", i, was, s.aim_pitch,
                 unsigned(s.action));
        }
        game::at<float>(ch, kAimPitch) = s.aim_pitch;
    }
}

int party_to_slot(int i)
{
    if (i < 1 || i >= kParty)
        return -1;
    if (g_role == NET_HOST) {
        for (int k = 0; k < kMaxPeers; ++k)
            if (g_peers[k].used && g_peers[k].pawn == i - 1)
                return 1 + k;
        return -1;
    }
    if (!g_have_snap)
        return -1;
    EnterCriticalSection(&g_snap_lock);
    int slot = -1, me = g_snap.my_pawn;
    if (i == 1 + me)
        slot = 0;
    else
        for (int s = 1; s < 4; ++s)
            if (g_snap.pawn_of[s] == i - 1)
                slot = s;
    LeaveCriticalSection(&g_snap_lock);
    return slot;
}

uintptr_t slot_character(int slot)
{
    if (slot < 0)
        return 0;
    if (slot == net_my_slot())
        return possess_arisen();
    for (int i = 1; i < kParty; ++i)
        if (party_to_slot(i) == slot)
            return possess_pawn_ptr(i - 1);
    return 0;
}

bool shared_party_down(int i);

bool hold_action(int a)
{
    return a == 0x39 || a == 0xA9 || a == 0xE3 || (a >= 0x3B && a <= 0x6F) || a == 0xDB || a == 0xE5;
}
int enemy_share_id(uintptr_t em);
uintptr_t enemy_from_share_id(int id);

void facing(uintptr_t ch, float &fx, float &fz)
{
    float qx = game::at<float>(ch, 0x50), qy = game::at<float>(ch, 0x54), qz = game::at<float>(ch, 0x58), qw = game::at<float>(ch, 0x5C);
    fx = 2.f * (qx * qz + qw * qy);
    fz = 1.f - 2.f * (qx * qx + qy * qy);
    float n = std::sqrt(fx * fx + fz * fz);
    if (!(n > 0.01f)) {
        fx = 0.f, fz = 1.f;
        return;
    }
    fx /= n, fz /= n;
}

bool carry_pose_on()
{
    static int on = -1;
    if (on < 0)
        on = early_flag("test_carry_pose") ? 1 : 0;
    return on != 0;
}

uintptr_t g_carried[kParty] = {};
bool carried_here(uintptr_t em)
{
    for (uintptr_t c : g_carried)
        if (c && c == em)
            return true;
    return false;
}
void carry(uintptr_t ch, uintptr_t em, const float off[3])
{
    float fx, fz;
    facing(ch, fx, fz);
    float to[3] = {game::at<float>(ch, 0x40) + fx * off[0] + fz * off[2], game::at<float>(ch, 0x44) + off[1],
                   game::at<float>(ch, 0x48) + fz * off[0] - fx * off[2]};
    if (!(std::isfinite(to[0]) && std::isfinite(to[1]) && std::isfinite(to[2])) || !(std::fabs(off[0]) + std::fabs(off[1]) + std::fabs(off[2]) < 400.f))
        return;
    memcpy(reinterpret_cast<void *>(em + 0x40), to, 12);
    for (uintptr_t o : kPosCopies)
        memcpy(reinterpret_cast<void *>(em + o), to, 12);
}

void carry_pose(uintptr_t ch, uintptr_t em, const CharState &s)
{
    static DWORD next[4] = {};
    if (!carry_pose_on() || !strstr(action_class(s.action), "Lift"))
        return;
    int k = 0;
    for (int p = 0; p < kPawns; ++p)
        if (possess_pawn_ptr(p) == ch)
            k = 1 + p;
    DWORD t = GetTickCount();
    if (next[k] && int(t - next[k]) < 0)
        return;
    next[k] = (t + 1000) | 1;
    const char *now = action_name(ch);
    if (strstr(now, "Lift") || strstr(now, "Dead") || strncmp(now, "cPlActDmg", 9) == 0)
        return;
    game::at<uintptr_t>(ch, 0x38F0) = em;
    game::at<int32_t>(ch, 0x3938) = s.hold_kind;
    action_set(ch, s.action);
    logf("net: shared: carrying pawn %p: %s -> %s (0x%X)", (void *)ch, now, action_name(ch), unsigned(s.action));
}

void place_in_front(uintptr_t ch, uintptr_t other, float d)
{
    float qx = game::at<float>(ch, 0x50), qy = game::at<float>(ch, 0x54), qz = game::at<float>(ch, 0x58), qw = game::at<float>(ch, 0x5C);
    float fx = 2.f * (qx * qz + qw * qy), fz = 1.f - 2.f * (qx * qx + qy * qy);
    float n = std::sqrt(fx * fx + fz * fz);
    if (!(n > 0.01f))
        return;
    static const float sign = early_flag("grab_front_flip") ? -1.f : 1.f;
    float to[3] = {game::at<float>(ch, 0x40) + sign * fx / n * d, game::at<float>(other, 0x44), game::at<float>(ch, 0x48) + sign * fz / n * d};
    memcpy(reinterpret_cast<void *>(other + 0x40), to, 12);
    for (uintptr_t off : kPosCopies)
        memcpy(reinterpret_cast<void *>(other + off), to, 12);
}

void shared_skill_look(uintptr_t ch, int i, const CharState &s)
{
    static int32_t seen[kParty] = {};
    static DWORD due[kParty] = {};
    DWORD now = GetTickCount();
    unsigned table = unsigned(s.action) >> 24;
    bool weapon = (table >= 1 && table <= 0x0F) || s.action == 0x99 || s.action == 0x9A;
    if (s.action != seen[i]) {
        seen[i] = s.action;
        due[i] = weapon ? (now + 200) | 1 : 0;
        return;
    }
    int32_t mine = game::at<int32_t>(ch, 0x2DD4);
    static int32_t tried_act[kParty] = {};
    static int tries[kParty] = {};
    if (mine == s.action)
        tries[i] = 0;
    if (!due[i] && table >= 1 && table <= 0x0F && mine != s.action)
        due[i] = (now + 400) | 1;
    if (!due[i] || int(now - due[i]) < 0)
        return;
    due[i] = 0;
    if (mine == s.action)
        return;
    const char *was = action_name(ch);
    if (strncmp(was, "cPlActDmg", 9) == 0 || strstr(was, "Dead") || strstr(was, "Lift")) {
        static DWORD next_skip_log[kParty] = {};
        if (!next_skip_log[i] || int(now - next_skip_log[i]) >= 0) {
            next_skip_log[i] = (now + 5000) | 1;
            logf("net: shared: party %d: its player's 0x%X not played over %s", i, unsigned(s.action), was);
        }
        return;
    }
    if (tried_act[i] != s.action) {
        tried_act[i] = s.action;
        tries[i] = 0;
    }
    if (++tries[i] > 3) {
        if (tries[i] == 4)
            logf("net: shared: party %d: its player's 0x%X played 3 times and left again: not played any more", i,
                 unsigned(s.action));
        return;
    }
    logf("net: shared: party %d plays its player's 0x%X (it was in %s 0x%X)", i, unsigned(s.action), was,
         unsigned(mine));
    action_set(ch, s.action);
}

constexpr int16_t kLanternItems[] = {64, 65, 76, 0x72B, 0x72E, 0x72F};

int party_member(uintptr_t ch)
{
    uint8_t f = game::at<uint8_t>(ch, 0x206D);
    return (f & 2) ? 0 : (f & 4) ? 1 + game::at<int32_t>(ch, 0x5854) : -1;
}

uintptr_t lantern_item(uintptr_t ch)
{
    if (uintptr_t it = game::at<uintptr_t>(ch, 0x3D70))
        return it;
    int m = party_member(ch);
    uintptr_t mgr = game::at<uintptr_t>(game::va(0x018FA4D4), 0);
    if (m < 0 || m > 3 || !mgr)
        return 0;
    uintptr_t list = mgr + 0x178 + uintptr_t(m) * 0x11E8;
    uint32_t n = game::at<uint32_t>(list, 8);
    uintptr_t *items = game::at<uintptr_t *>(list, 0x14);
    if (n > 512 || !items || !mem_readable(items, n * sizeof *items))
        return 0;
    for (int16_t want : kLanternItems)
        for (uint32_t k = 0; k < n; ++k)
            if (items[k] && game::at<int16_t>(items[k], 0x10) == want)
                return items[k];
    return 0;
}

bool lantern_on(uintptr_t ch)
{
    uintptr_t it = lantern_item(ch);
    return it && (game::at<uint32_t>(it, 0x20) & 2);
}

void shared_lantern_look(uintptr_t ch, int i, const CharState &s)
{
    static uint8_t item[kParty][0x100];
    static DWORD again[kParty] = {};
    static int tries[kParty] = {};
    static bool lit[kParty] = {};
    uintptr_t mine = uintptr_t(item[i]);
    uintptr_t it = game::at<uintptr_t>(ch, 0x3D70);
    if (it && it != mine)
        return;
    if (s.lantern) {
        if (!lit[i]) {
            lit[i] = true;
            tries[i] = 0;
            logf("net: shared: party %d: lantern lit, as its player's", i);
        }
        if (!it) {
            memset(item[i], 0, sizeof item[i]);
            *reinterpret_cast<int16_t *>(item[i] + 0x10) = kLanternItems[0];
            *reinterpret_cast<uint32_t *>(item[i] + 0x20) = 2;
            game::at<uintptr_t>(ch, 0x3D70) = mine;
        }
        int32_t act = game::at<int32_t>(ch, 0x2DD4);
        DWORD t = GetTickCount();
        if (!game::at<uintptr_t>(ch, 0x3D6C) && act != 0x99 && act != 0x9A && (!again[i] || int(t - again[i]) >= 0) && tries[i] < 3) {
            again[i] = (t + 3000) | 1;
            ++tries[i];
            action_set(ch, 0x99);
            logf("net: shared: party %d: no lantern in hand: lantern action played", i);
        }
        return;
    }
    if (!lit[i])
        return;
    lit[i] = false;
    if (it == mine) {
        game::at<uintptr_t>(ch, 0x3D70) = 0;
        if (uintptr_t obj = game::at<uintptr_t>(ch, 0x3D6C)) {
            game::at<uint8_t>(obj, 0x29B1) = 1;
            game::at<uintptr_t>(ch, 0x3D6C) = 0;
        }
    }
    logf("net: shared: party %d: lantern put away, as its player's", i);
}

bool look_of(uintptr_t ch, int32_t *out)
{
    uintptr_t prm = game::at<uintptr_t>(ch, 0x3DEC);
    if (!prm || !mem_readable(reinterpret_cast<void *>(prm + 0x10), 0x40))
        return false;
    memcpy(out, reinterpret_cast<void *>(prm + 0x10), 0x40);
    return true;
}

void shared_gear_look(uintptr_t ch, int i, const CharState &s)
{
    static const bool on = !early_flag("no_look_sync");
    static DWORD next[kParty] = {}, same_since[kParty] = {};
    if (!on || !s.look[1])
        return;
    int32_t mine[16];
    if (!look_of(ch, mine))
        return;
    DWORD t = GetTickCount();
    if (mine[0] != s.look[0]) {
        same_since[i] = 0;
        return;
    }
    if (!same_since[i])
        same_since[i] = t | 1;
    if (int(t - same_since[i]) < 10000 || !memcmp(mine + 1, s.look + 1, sizeof mine - 4))
        return;
    if (next[i] && int(t - next[i]) < 0)
        return;
    next[i] = (t + 1000) | 1;
    int m = party_member(ch);
    uintptr_t gs = game::at<uintptr_t>(game::kGameMain, 0);
    if (m < 0 || m > 3 || !gs)
        return;
    uintptr_t prm = game::at<uintptr_t>(ch, 0x3DEC);
    uintptr_t mprm = m == 0 ? gs + 0xA76D0 : gs + 0xA6860 + uintptr_t(m) * 0x1660;
    memcpy(reinterpret_cast<void *>(prm + 0x14), s.look + 1, sizeof mine - 4);
    memcpy(reinterpret_cast<void *>(mprm + 0x14), s.look + 1, sizeof mine - 4);
    int32_t &state = game::at<int32_t>(gs, 0xAC3C0 + 4 * m);
    logf("net: shared: party %d (member %d): gear changed, as its player's: look refresh asked (state was %d)", i, m, state);
    if (state == 0)
        state = 1;
}

DWORD g_me_held_at = 0, g_hold_fall = 0;

void shared_grab_look(uintptr_t ch, int i, const CharState &s)
{
    static int applied[kParty] = {-1, -1, -1, -1};
    static DWORD again[kParty] = {}, released[kParty] = {};
    static uintptr_t held_by[kParty] = {}, old_2104[kParty] = {}, old_30[kParty] = {};
    static constexpr uintptr_t kEmLinks[3] = {0x2B98, 0x5CA4, 0x60E4};
    static uintptr_t old_em[kParty][3] = {};
    static bool held_player[kParty] = {};
    static float rel_y[kParty] = {};
    static int applied_was[kParty] = {};
    static int applied_act[kParty] = {};
    static bool gave_up[kParty] = {};
    static float pre_at[kParty][3] = {};
    DWORD t = GetTickCount();
    int want = !hold_action(s.action) ? -1 : s.hold_slot >= 0 ? s.hold_slot : s.hold_enemy >= 0 ? 0x100 + s.hold_enemy : -1;
    if (want >= 0 && want == net_my_slot())
        g_me_held_at = t | 1;
    if (g_hold_fall) {
        uintptr_t a = possess_arisen();
        int act = a ? game::at<int32_t>(a, 0x2DD4) : -1;
        if (a && (act == 0 || act == 1 || act == 2) && !holder_of(a) && int(t - g_hold_fall) > 1500)
            g_hold_fall = 0;
    }
    if (want < 0) {
        gave_up[i] = false;
        if (applied[i] >= 0 && held_by[i] && !held_player[i]) {
            uintptr_t h = held_by[i];
            for (int k = 0; k < 3; ++k)
                if (game::at<uintptr_t>(h, kEmLinks[k]) == ch)
                    game::at<uintptr_t>(h, kEmLinks[k]) = old_em[i][k];
        }
        if (applied[i] >= 0 && held_by[i]) {
            uintptr_t h = held_by[i];
            if (held_player[i] && game::at<uintptr_t>(h, 0x30) == ch) {
                float w[3];
                world_pos(h, w);
                memcpy(reinterpret_cast<void *>(h + 0x40), w, 12);
                for (uintptr_t off : kPosCopies)
                    memcpy(reinterpret_cast<void *>(h + off), w, 12);
                logf("net: shared: party %d let go of player %d here: put at (%.0f, %.0f, %.0f)", i, applied[i], w[0], w[1], w[2]);
                rel_y[i] = w[1];
                applied_was[i] = applied[i];
            } else if (held_player[i]) {
                rel_y[i] = game::at<float>(h, 0x44);
                applied_was[i] = applied[i];
            }
            if (held_player[i] && h == possess_arisen())
                g_hold_fall = t | 1;
            if (game::at<uintptr_t>(h, 0x2104) == ch)
                game::at<uintptr_t>(h, 0x2104) = old_2104[i];
            if (game::at<uintptr_t>(h, 0x30) == ch)
                game::at<uintptr_t>(h, 0x30) = old_30[i];
            if (game::at<uintptr_t>(ch, 0x38F0) == h)
                game::at<uintptr_t>(ch, 0x38F0) = 0;
            released[i] = t | 1;
        }
        applied[i] = -1;
        if (g_carried[i]) {
            logf("net: shared: party %d let go of the monster it carried here", i);
            g_carried[i] = 0;
        }
        if (released[i] && held_player[i] && held_by[i] && int(t - released[i]) < 2500) {
            uintptr_t h = held_by[i];
            float *p = reinterpret_cast<float *>(h + 0x40);
            if (!holder_of(h) && p[1] < rel_y[i] - 40.f) {
                float to[3] = {p[0], rel_y[i], p[2]};
                memcpy(p, to, 12);
                for (uintptr_t off : kPosCopies)
                    memcpy(reinterpret_cast<void *>(h + off), to, 12);
                const char *was = action_name(h);
                action_set(h, 0);
                logf("net: shared: player %d let go here sank under where it was let go (%s): put back at y %.0f", applied_was[i], was, rel_y[i]);
            }
        }
        if (released[i] && int(t - released[i]) > 2500) {
            released[i] = 0;
            held_by[i] = 0;
        } else if (released[i] && int(t - released[i]) > 1200) {
            uintptr_t h = held_by[i];
            int32_t a = h && held_player[i] ? game::at<int32_t>(h, 0x2DD4) : 0;
            if (h && (a == 0xAA || a == 0xAB))
                action_set(h, 0);
        }
        return;
    }
    bool player = want < 0x100;
    uintptr_t held = player ? slot_character(want) : enemy_from_share_id(want - 0x100);
    float reach = -1.f;
    float far_at[3] = {};
    if (held && held != ch) {
        world_pos(held, far_at);
        float rx = far_at[0] - game::at<float>(ch, 0x40), ry = far_at[1] - game::at<float>(ch, 0x44),
              rz = far_at[2] - game::at<float>(ch, 0x48);
        reach = std::sqrt(rx * rx + ry * ry + rz * rz);
        if (!(reach < 300.f) && player)
            held = 0;
    }
    static DWORD next_why[kParty] = {};
    bool tell = !next_why[i] || int(t - next_why[i]) >= 0;
    if (player && !held && reach >= 300.f && applied[i] == want && held_by[i] && int(t - again[i]) < 1500 &&
        game::at<uintptr_t>(held_by[i], 0x30) == ch) {
        uintptr_t h = held_by[i];
        memcpy(reinterpret_cast<void *>(h + 0x40), pre_at[i], 12);
        for (uintptr_t off : kPosCopies)
            memcpy(reinterpret_cast<void *>(h + off), pre_at[i], 12);
        game::at<uintptr_t>(h, 0x30) = old_30[i];
        if (game::at<uintptr_t>(h, 0x2104) == ch)
            game::at<uintptr_t>(h, 0x2104) = old_2104[i];
        if (game::at<uintptr_t>(ch, 0x38F0) == h)
            game::at<uintptr_t>(ch, 0x38F0) = 0;
        action_set(h, 0);
        logf("net: shared: party %d's hold of player %d threw it %.0f u away here: put back at (%.0f, %.0f, %.0f), not rebuilt again",
             i, want, reach, pre_at[i][0], pre_at[i][1], pre_at[i][2]);
        applied[i] = -1;
        held_by[i] = 0;
        gave_up[i] = true;
        return;
    }
    if (!held || held == ch) {
        if (tell) {
            next_why[i] = (t + 2000) | 1;
            logf("net: shared: party %d holds %s %d in its world: not found here (%s)", i, player ? "player" : "enemy",
                 player ? want : want - 0x100, reach < 0.f ? "no such one" : "too far from the pawn");
            if (reach >= 0.f)
                logf("net: shared:   it is %.0f u from the pawn: it at (%.0f, %.0f, %.0f), the pawn at (%.0f, %.0f, %.0f)", reach,
                     far_at[0], far_at[1], far_at[2], game::at<float>(ch, 0x40), game::at<float>(ch, 0x44), game::at<float>(ch, 0x48));
        }
        return;
    }
    const char *now_act = action_name(ch);
    if (shared_party_down(i) || strstr(now_act, "Dead") || strstr(now_act, "Neardeath") || strncmp(now_act, "cPlActDmg", 9) == 0) {
        if (tell) {
            next_why[i] = (t + 2000) | 1;
            logf("net: shared: party %d holds something in its world: not here, the pawn is in %s", i, now_act);
        }
        return;
    }
    if (applied[i] == want) {
        bool holding = game::at<uintptr_t>(ch, 0x38F0) == held &&
                       (player ? game::at<uintptr_t>(held, 0x2104) == ch && (s.action != 0xA9 || game::at<int32_t>(held, 0x2DD4) == 0xAA)
                               : g_carried[i] == held);
        static DWORD next_log[kParty] = {};
        if (!next_log[i] || int(t - next_log[i]) >= 0) {
            next_log[i] = (t + 1000) | 1;
            float dx = game::at<float>(held, 0x40) - game::at<float>(ch, 0x40), dy = game::at<float>(held, 0x44) - game::at<float>(ch, 0x44),
                  dz = game::at<float>(held, 0x48) - game::at<float>(ch, 0x48);
            logf("net: shared: party %d holding %s %d: %s, %.0f u from the pawn (height %+.0f), pawn in %s", i,
                 player ? "player" : "enemy", player ? want : want - 0x100, holding ? "held" : "lost",
                 std::sqrt(dx * dx + dz * dz), dy, action_name(ch));
        }
        if (!player) {
            carry(ch, held, s.hold_off);
            carry_pose(ch, held, s);
            return;
        }
        if (holding || (int(t - again[i]) < 1000 && applied_act[i] == s.action))
            return;
    }
    if (gave_up[i])
        return;
    again[i] = t | 1;
    if (player)
        world_pos(held, pre_at[i]);
    if (held_by[i] != held) {
        old_2104[i] = game::at<uintptr_t>(held, 0x2104) == ch ? 0 : game::at<uintptr_t>(held, 0x2104);
        old_30[i] = game::at<uintptr_t>(held, 0x30) == ch ? 0 : game::at<uintptr_t>(held, 0x30);
        if (!player)
            for (int k = 0; k < 3; ++k)
                old_em[i][k] = game::at<uintptr_t>(held, kEmLinks[k]) == ch ? 0 : game::at<uintptr_t>(held, kEmLinks[k]);
    }
    applied[i] = want;
    applied_act[i] = s.action;
    held_by[i] = held;
    held_player[i] = player;
    released[i] = 0;
    game::at<uintptr_t>(ch, 0x38F0) = held;
    game::at<int32_t>(ch, 0x3938) = s.hold_kind;
    if (!player) {
        game::at<uintptr_t>(ch, 0x38F0) = 0;
        g_carried[i] = held;
        carry(ch, held, s.hold_off);
        logf("net: shared: party %d (player %d) holds enemy %d: carried here where its player has it (%.0f, %+.0f, %.0f)", i,
             party_to_slot(i), want - 0x100, s.hold_off[0], s.hold_off[1], s.hold_off[2]);
        return;
    }
    action_set(ch, s.action);
    if (s.action == 0xA9)
        action_set(held, 0xAA);
    game::at<uintptr_t>(held, 0x2104) = ch;
    game::at<uintptr_t>(held, 0x30) = ch;
    logf("net: shared: party %d (player %d) holds %s %d (action 0x%X, kind %d): hold rebuilt here", i, party_to_slot(i),
         player ? "player" : "enemy", player ? want : want - 0x100, unsigned(s.action), int(s.hold_kind));
}

bool shared_party_down(int i)
{
    if (i < 1 || i >= kParty)
        return false;
    if (g_role == NET_HOST) {
        for (const Peer &p : g_peers)
            if (p.used && p.pawn == i - 1)
                return p.downed != 0;
        return false;
    }
    if (!g_have_snap)
        return false;
    EnterCriticalSection(&g_snap_lock);
    bool down = false;
    int me = g_snap.my_pawn;
    if (i == 1 + me)
        down = g_snap.down[0] != 0;
    else
        for (int s = 1; s < 4; ++s)
            if (g_snap.pawn_of[s] == i - 1)
                down = g_snap.down[s] != 0;
    LeaveCriticalSection(&g_snap_lock);
    return down;
}

void shared_down_look(uintptr_t ch, int i)
{
    static DWORD next[kParty] = {};
    static bool forced[kParty] = {};
    DWORD now = GetTickCount();
    bool down = shared_party_down(i);
    bool lying = strcmp(action_name(ch), "cPlActDmgSleep") == 0;
    if (down && !lying && (!next[i] || int(now - next[i]) >= 0)) {
        next[i] = (now + 500) | 1;
        forced[i] = true;
        action_set(ch, 0xB7);
    } else if (!down && forced[i]) {
        forced[i] = false;
        if (lying)
            action_set(ch, 0xB8);
    } else if (!down && strstr(action_name(ch), "Dead") && (!next[i] || int(now - next[i]) >= 0)) {
        next[i] = (now + 1000) | 1;
        uintptr_t e = game::at<uintptr_t>(ch, 0x1D18) ? game::at<uintptr_t>(game::at<uintptr_t>(ch, 0x1D18), 0) : 0;
        if (e)
            game::at<uint32_t>(e, 0x110) &= ~1u;
        logf("net: shared: party %d shows a player who is up but was in %s here: standing again", i, action_name(ch));
        action_set(ch, 0);
    }
}

uintptr_t g_warp_watch = 0;
DWORD WINAPI warp_watch_thread(LPVOID)
{
    watch_start(g_warp_watch, 16, 0);
    return 0;
}

void shared_apply_transform(uintptr_t ch, int i)
{
    if (i == 0) {
        static DWORD next_where = 0;
        if (!next_where || int(GetTickCount() - next_where) >= 0) {
            next_where = (GetTickCount() + 5000) | 1;
            logf("net: shared: me at (%.0f, %.0f), stage %d", game::at<float>(ch, 0x40), game::at<float>(ch, 0x48), my_stage());
        }
    }
    if (i == 0 && g_role == NET_GUEST && g_have_snap) {
        uintptr_t gm = game::at<uintptr_t>(game::kGameMain, 0);
        EnterCriticalSection(&g_snap_lock);
        StageState h = g_snap.stage;
        LeaveCriticalSection(&g_snap_lock);
        if (gm && current_stage_area())
            guest_world(gm, h);
        return;
    }
    if (!net_puppet(i))
        return;
    shared_down_look(ch, i);
    EnterCriticalSection(&g_cap_lock);
    CharState s = g_rp[i];
    bool away = g_rp_away[i] || g_rp_hidden[i];
    bool elsewhere = g_rp_away[i] && !g_rp_hidden[i];
    LeaveCriticalSection(&g_cap_lock);
    if (!s.valid && !away)
        return;
    if (!away) {
        shared_grab_look(ch, i, s);
        shared_skill_look(ch, i, s);
        shared_lantern_look(ch, i, s);
        shared_gear_look(ch, i, s);
    }
    EnterCriticalSection(&g_cap_lock);
    float vel[3];
    memcpy(vel, g_rp_vel[i], sizeof vel);
    float age = g_rp_at[i] ? (GetTickCount() - g_rp_at[i]) / 1000.f : 0.f;
    LeaveCriticalSection(&g_cap_lock);
    float *pos = reinterpret_cast<float *>(ch + 0x40);
    static float last_here[kParty][3] = {};
    static int32_t last_stage[kParty] = {-1, -1, -1, -1};
    if (!away) {
        memcpy(last_here[i], s.pos, sizeof last_here[i]);
        last_stage[i] = my_stage();
    }
    static bool was_parked[kParty] = {};
    if (away) {
        static bool watched = false;
        if (!watched && !g_rp_hidden[i] && early_flag("test_watch_dead")) {
            uintptr_t e = game::at<uintptr_t>(ch, 0x1D18) ? game::at<uintptr_t>(game::at<uintptr_t>(ch, 0x1D18), 0) : 0;
            if (e) {
                watched = true;
                logf("net: shared: watching party %d's HP entry %08X (+0x110 dead flag, +8 HP)", i, unsigned(e));
                watch_start(e + 0x110, 6, 0);
            }
        }
        was_parked[i] = true;
        uintptr_t me = possess_arisen();
        bool at_door = elsewhere && last_stage[i] >= 0 && last_stage[i] == my_stage();
        if (me) {
            float park[3] = {game::at<float>(me, 0x40), game::at<float>(me, 0x44) - kParkHeight, game::at<float>(me, 0x48)};
            if (at_door) {
                park[0] = last_here[i][0];
                park[1] = last_here[i][1] - kParkHeight;
                park[2] = last_here[i][2];
            }
            memcpy(pos, park, sizeof park);
            for (uintptr_t off : kPosCopies)
                memcpy(reinterpret_cast<void *>(ch + off), park, 12);
        }
        static DWORD next_park_log[kParty] = {};
        if (!next_park_log[i] || int(GetTickCount() - next_park_log[i]) >= 0) {
            next_park_log[i] = GetTickCount() + 5000;
            logf("net: shared: party %d shows a player in another area: kept out of sight at y %.0f%s", i, pos[1],
                 at_door ? ", under the last place it was seen here" : "");
        }
        return;
    }
    if (was_parked[i]) {
        was_parked[i] = false;
        memcpy(pos, s.pos, 12);
        for (uintptr_t off : kPosCopies)
            memcpy(reinterpret_cast<void *>(ch + off), s.pos, 12);
        uintptr_t e = game::at<uintptr_t>(ch, 0x1D18) ? game::at<uintptr_t>(game::at<uintptr_t>(ch, 0x1D18), 0) : 0;
        if (e)
            game::at<uint32_t>(e, 0x110) &= ~1u;
        logf("net: shared: party %d back in this area (was %s): its fall cancelled", i, action_name(ch));
        action_set(ch, 0);
    }
    if (holder_of(ch)) {
        static DWORD next_held_log[kParty] = {};
        if (!next_held_log[i] || int(GetTickCount() - next_held_log[i]) >= 0) {
            next_held_log[i] = (GetTickCount() + 1000) | 1;
            logf("net: shared: party %d is held here (%s): its place left to the hold", i, action_name(ch));
        }
        return;
    }
    static DWORD caught_since[kParty] = {}, next_end[kParty] = {}, next_caught_log[kParty] = {};
    if (monster_caught(ch)) {
        DWORD t = GetTickCount();
        if (!caught_since[i])
            caught_since[i] = t | 1;
        if (!s.monster_held && t - caught_since[i] > 1000 && (!next_end[i] || int(t - next_end[i]) >= 0)) {
            next_end[i] = (t + 3000) | 1;
            logf("net: shared: party %d is caught by a monster here (%s) and its player is not: catch ended", i,
                 action_name(ch));
            action_set(ch, 0);
        } else if (!next_caught_log[i] || int(t - next_caught_log[i]) >= 0) {
            next_caught_log[i] = (t + 1000) | 1;
            logf("net: shared: party %d is caught by a monster here (%s; its player %s): its place left to the catch", i,
                 action_name(ch), s.monster_held ? "too" : "not yet");
        }
        return;
    }
    caught_since[i] = 0;
    float ahead = age + 0.06f > 0.3f ? 0.3f : age + 0.06f;
    float target[3] = {s.pos[0] + vel[0] * ahead, s.pos[1], s.pos[2] + vel[2] * ahead};
    float dx = target[0] - pos[0], dy = target[1] - pos[1], dz = target[2] - pos[2];
    float err = std::sqrt(dx * dx + dy * dy + dz * dz);
    static DWORD off_since[kParty] = {};
    bool stuck = false;
    if (err > 100.f) {
        if (!off_since[i])
            off_since[i] = GetTickCount() | 1;
        else if (GetTickCount() - off_since[i] > 1500) {
            stuck = true;
            off_since[i] = 0;
            logf("net: shared: party %d %.0f u off its player's place for 1.5 s (%s): put there, from (%.0f, %.0f, %.0f) to (%.0f, %.0f, %.0f)",
                 i, err, action_name(ch), pos[0], pos[1], pos[2], target[0], target[1], target[2]);
            static bool watched_warp = false;
            if (!watched_warp && early_flag("test_watch_warp")) {
                watched_warp = true;
                g_warp_watch = ch + 0x40;
                CreateThread(nullptr, 0, warp_watch_thread, nullptr, 0, nullptr);
            }
        }
    } else {
        off_since[i] = 0;
    }
    static DWORD fall_since[kParty] = {};
    bool fall_here = strstr(action_name(ch), "FallHeight") != nullptr && s.action != 0x25;
    bool cancel_fall = false;
    if (!fall_here)
        fall_since[i] = 0;
    else if (!fall_since[i])
        fall_since[i] = GetTickCount() | 1;
    else if (GetTickCount() - fall_since[i] > 1000) {
        fall_since[i] = 0;
        cancel_fall = true;
        logf("net: shared: party %d falls here (%s) and its player does not (0x%X): fall cancelled, put at its place",
             i, action_name(ch), unsigned(s.action));
    }
    if (err > 400.f || stuck || cancel_fall) {
        memcpy(pos, target, sizeof target);
        for (uintptr_t off : kPosCopies)
            memcpy(reinterpret_cast<void *>(ch + off), target, 12);
        if (cancel_fall) {
            uintptr_t e = game::at<uintptr_t>(ch, 0x1D18) ? game::at<uintptr_t>(game::at<uintptr_t>(ch, 0x1D18), 0) : 0;
            if (e)
                game::at<uint32_t>(e, 0x110) &= ~1u;
            action_set(ch, 0);
        }
    } else {
        float k = err > 120.f ? 0.3f : 0.12f;
        pos[0] += dx * k;
        pos[1] += dy * k;
        pos[2] += dz * k;
    }
    game::at<float>(ch, kYawA) = s.yaw;
    game::at<float>(ch, kYawB) = s.yaw;
    game::at<float>(ch, kYawC) = s.yaw;
    memcpy(reinterpret_cast<void *>(ch + 0x50), s.rot, sizeof s.rot);
    if (s.hp_max > 0.f)
        if (float *hp = enemies_hp(ch))
            hp[0] = s.hp;
    static DWORD next_log[kParty] = {};
    if (!next_log[i] || int(GetTickCount() - next_log[i]) >= 0) {
        next_log[i] = GetTickCount() + 1000;
        logf("net: shared: party %d shows another player, error %.1f u before correction, HP %.1f (state %d ms old, %.0f u/s)",
             i, err, s.hp, int(age * 1000.f), std::sqrt(vel[0] * vel[0] + vel[2] * vel[2]));
    }
}

uint32_t crc32(const uint8_t *p, size_t n)
{
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; ++i) {
        c ^= p[i];
        for (int k = 0; k < 8; ++k)
            c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return ~c;
}
struct HostSave {
    std::vector<uint8_t> data;
    uint32_t version = 0, crc = 0, sig = 0;
} g_hsave;
struct GuestSave {
    std::vector<uint8_t> data, got;
    uint32_t version = 0, total = 0, crc = 0, have = 0, sig = 0;
    DWORD next_need = 0;
} g_gsave;

constexpr int kEnemySlots = 128;
CRITICAL_SECTION g_enemy_lock;
EnemyState g_enemy_state[kEnemySlots] = {};
DWORD g_enemy_tick[kEnemySlots] = {};
int g_send_cursor = 0;
struct Follow {
    DWORD first_seen;
    int host_slot;
    DWORD next_try;
    uintptr_t ptr;
    DWORD dead_since;
    bool forced;
    DWORD unpaired_since;
    DWORD last;
    int paired_to;
    float host_spawn[3];
    bool by_id;
    uint32_t pair_key, pair_id;
    uint32_t pair_split;
    uint16_t pair_part;
    int force_tries;
    DWORD force_at;
    int act_starts;
    uint32_t act_seen;
    int tgt_seen;
    DWORD act_wait;
    uint32_t my_key, my_id;
    bool my_died;
    bool drove;
    DWORD held;
    bool loose;
};
ActParams g_act_params[kEnemySlots] = {};
DWORD g_act_params_tick[kEnemySlots] = {};
volatile LONG g_act_params_now = 0;
uint8_t g_enemy_urgent[kEnemySlots] = {};
HANDLE g_net_wake = nullptr;
struct HumanCmdState {
    uint8_t cmd[kHumanCmdLen];
    float aim;
    uint32_t press, release;
    DWORD tick;
    uint32_t press_act;
    uint16_t press_ms;
    DWORD press_rx;
};
HumanCmdState g_hc_out[kEnemySlots] = {}, g_hc_in[kEnemySlots] = {};
volatile LONG g_hc_sent = 0, g_hc_rx = 0, g_hc_applied = 0, g_hc_presses = 0, g_hc_fixed = 0;
volatile LONG g_hc_now = 0;
template <class A, class B> bool same_spawn_id(const A &a, const B &b)
{
    return a.lot_key == b.lot_key && a.lot_id == b.lot_id && a.lot_split == b.lot_split && a.lot_part == b.lot_part;
}
template <class A> bool same_as_pair(const A &a, const Follow &f)
{
    return a.lot_key == f.pair_key && a.lot_id == f.pair_id && a.lot_split == f.pair_split && a.lot_part == f.pair_part;
}
Follow g_follow[kEnemySlots] = {};
struct EnemyStats {
    int applied, matched_new, unmatched;
    float worst_err;
    int blocked, allowed, forced, removed, puppet_blocked, felt;
    int acted;
} g_enemy_stats = {};
DWORD g_last_enemies_rx = 0;

SRWLOCK g_entries_lock = SRWLOCK_INIT;
LeadUnit g_my_entries[kLeadEntriesMax];
int g_my_entries_n = 0;
int32_t g_my_entries_stage = -1;
DWORD g_my_entries_tick = 0;
LeadUnit g_rx_entries[kLeadEntriesMax];
int g_rx_entries_n = 0, g_rx_entries_from = -1;
int32_t g_rx_entries_stage = -1;
DWORD g_rx_entries_tick = 0;

constexpr float kLeadRange = 5000.f, kRemoveRange = 2500.f;
volatile LONG g_lead = -1;
volatile LONG g_lead_others = 0;
float g_lead_pos[3] = {};
int g_area_cursor = 0;

void state_to_world(EnemyState &s)
{
    float ox, oz;
    if (s.attached & 1)
        return;
    if (!enemies_world_origin(ox, oz))
        return;
    s.spawn[0] += ox, s.spawn[2] += oz;
    s.pos[0] += ox, s.pos[2] += oz;
    s.world = 1;
}
void state_to_local(EnemyState &s)
{
    if (!s.world)
        return;
    float ox, oz;
    if (!enemies_world_origin(ox, oz)) {
        static DWORD said = 0;
        if (!said || GetTickCount() - said > 10000) {
            said = GetTickCount() | 1;
            logf("world: an enemy in world coordinates arrived, this game's origin not known yet: taken as (0, 0)");
        }
    }
    s.spawn[0] -= ox, s.spawn[2] -= oz;
    s.pos[0] -= ox, s.pos[2] -= oz;
    s.world = 0;
}

void char_shift(CharState &c, float ox, float oz)
{
    if (c.valid)
        c.pos[0] += ox, c.pos[2] += oz;
}
void snap_to_world(SnapMsg &m)
{
    float ox, oz;
    if (!enemies_world_origin(ox, oz))
        return;
    for (CharState &c : m.ch)
        char_shift(c, ox, oz);
    if (m.stage.talk_npc[0] != 0.f || m.stage.talk_npc[2] != 0.f)
        m.stage.talk_npc[0] += ox, m.stage.talk_npc[2] += oz;
    m.world = 1;
}
void snap_to_local(SnapMsg &m, float &ox, float &oz)
{
    enemies_world_origin(ox, oz);
    if (!m.world)
        return;
    for (CharState &c : m.ch)
        char_shift(c, -ox, -oz);
    if (m.stage.talk_npc[0] != 0.f || m.stage.talk_npc[2] != 0.f)
        m.stage.talk_npc[0] -= ox, m.stage.talk_npc[2] -= oz;
    m.world = 0;
}

uint32_t class_hash(const char *name)
{
    uint32_t h = 2166136261u;
    for (; *name; ++name)
        h = (h ^ uint8_t(*name)) * 16777619u;
    return h;
}

void capture_state(uintptr_t ch, CharState &s)
{
    s.valid = ch != 0;
    if (!ch)
        return;
    world_pos(ch, s.pos);
    struct Kept {
        uintptr_t ch;
        DWORD used;
        bool has;
        float pos[3];
    };
    static Kept kept[8] = {};
    DWORD now = GetTickCount();
    int slot = -1;
    for (int k = 0; k < 8 && slot < 0; ++k)
        if (kept[k].ch == ch)
            slot = k;
    if (slot < 0) {
        slot = 0;
        for (int k = 0; k < 8; ++k)
            if (!kept[k].ch || int(kept[k].used - kept[slot].used) < 0) {
                slot = k;
                if (!kept[k].ch)
                    break;
            }
        kept[slot] = {ch, now, false, {}};
    }
    kept[slot].used = now;
    s.monster_held = monster_caught(ch);
    if (s.monster_held && std::fabs(s.pos[0]) + std::fabs(s.pos[1]) + std::fabs(s.pos[2]) < 600.f) {
        if (kept[slot].has)
            memcpy(s.pos, kept[slot].pos, sizeof kept[slot].pos);
    } else {
        memcpy(kept[slot].pos, s.pos, sizeof kept[slot].pos);
        kept[slot].has = true;
    }
    memcpy(s.rot, reinterpret_cast<void *>(ch + 0x50), sizeof s.rot);
    s.yaw = game::at<float>(ch, kYawA);
    s.aim_pitch = game::at<float>(ch, kAimPitch);
    memcpy(s.cmd, reinterpret_cast<void *>(ch + game::kCmd + kCmdCopyFrom), kCmdCopyLen);
    memcpy(s.cmd_extra, reinterpret_cast<void *>(ch + game::kCmd + kCmdExtraFrom), kCmdExtraLen);
    if (const float *hp = enemies_hp(ch)) {
        s.hp = hp[0];
        s.hp_max = hp[1];
    }
}

void read_state(int i, CharState &s)
{
    EnterCriticalSection(&g_cap_lock);
    s = g_cap[i];
    s.valid = s.valid && g_cap_tick[i] && int(GetTickCount() - g_cap_tick[i]) < 500;
    LeaveCriticalSection(&g_cap_lock);
    if (!s.valid) {
        s = CharState{};
        return;
    }
    uint32_t press = uint32_t(InterlockedExchange(&g_acc_press[i], 0));
    uint32_t release = uint32_t(InterlockedExchange(&g_acc_release[i], 0));
    memcpy(s.cmd + (0x2C - kCmdCopyFrom), &press, 4);
    memcpy(s.cmd + (0x34 - kCmdCopyFrom), &release, 4);
}

void log_scene_flags(const char *who, uintptr_t gm)
{
    static const uint32_t kOffs[] = {0xAC720, 0xB883C, 0xB8840, 0xB8844};
    static uint32_t logged[2][4] = {};
    uint32_t *last = logged[who[0] == 'g'];
    uint32_t v[4];
    bool changed = false;
    for (int i = 0; i < 4; ++i) {
        v[i] = game::at<uint32_t>(gm, kOffs[i]);
        changed = changed || v[i] != last[i];
    }
    if (!changed)
        return;
    logf("world: %s scene flags %08X %08X %08X %08X", who, v[0], v[1], v[2], v[3]);
    memcpy(last, v, sizeof v);
}

void read_stage(StageState &s)
{
    static bool was_jumping = false;
    static uint32_t seq = 0;
    uintptr_t gm = game::at<uintptr_t>(game::kGameMain, 0), st = current_stage_area();
    s.stage = st ? game::at<int32_t>(st, 0x724) : -1;
    s.point = gm && st ? game::at<int32_t>(gm, 0x44) : -1;
    s.jumping = st && game::at<int32_t>(st, 0x808) == 2;
    if (s.jumping) {
        s.jump_stage = game::at<int32_t>(st, 0x80C);
        s.jump_point = game::at<int32_t>(st, 0x810);
        if (!was_jumping)
            logf("net: stage: host jumps from %d to %d (point %d), jump %u", s.stage, s.jump_stage, s.jump_point, ++seq);
    }
    was_jumping = s.jumping;
    s.jump_seq = seq;
    s.reload_seq = stages_other_loads();
    s.party_sig = rejoin_loaded_sig();
    talk_last(s.talk_npc);
    XINPUT_STATE pad = {};
    if (pads_read_raw(0, &pad) == ERROR_SUCCESS) {
        s.pad_buttons = pad.Gamepad.wButtons;
        s.pad_lx = pad.Gamepad.sThumbLX;
        s.pad_ly = pad.Gamepad.sThumbLY;
    }
    s.day = gm && st ? game::at<int32_t>(gm, game::kGsDay) : -1;
    s.day_ms = gm ? game::at<int32_t>(gm, game::kGsDayMs) : 0;
    s.time_speed = gm ? game::at<int32_t>(gm, game::kGsTimeSpeed) : 500;
    s.event = gm ? game::at<int32_t>(gm, game::kGsEvent) : 0;
    static int32_t ev_logged = -1;
    if (gm && st && s.event != ev_logged) {
        logf("world: host event state %d -> %d (time day %d %02d:%02d)", ev_logged, s.event, s.day, s.day_ms / 3600000,
             s.day_ms / 60000 % 60);
        ev_logged = s.event;
    }
    if (gm && st)
        log_scene_flags("host", gm);
    static int32_t logged = -2;
    if (s.stage != logged) {
        logged = s.stage;
        logf("net: stage: host now in %d (point %d)", s.stage, s.point);
    }
}

void send_to(const Peer &peer, const void *p, int n)
{
    if (!peer.used || (g_role == NET_GUEST && storage_guest_refused()))
        return;
    if (g_steam) {
        if (g_role == NET_GUEST ? link_steam_send(p, n) : link_steam_send_to(peer.steam, p, n))
            ++g_tx;
        return;
    }
    if (sendto(g_sock, static_cast<const char *>(p), n, 0, reinterpret_cast<const sockaddr *>(&peer.addr),
               sizeof peer.addr) == n) {
        ++g_tx;
    } else {
        static DWORD last_err_log = 0;
        if (GetTickCount() - last_err_log > 5000) {
            last_err_log = GetTickCount();
            logf("net: sendto failed, WSA error %d", WSAGetLastError());
        }
    }
}

void send_raw(const void *p, int n)
{
    for (const Peer &peer : g_peers)
        send_to(peer, p, n);
}

int free_pawn()
{
    for (int pass = 0; pass < 2; ++pass)
        for (int p : g_pawn_order) {
            bool taken = false;
            for (const Peer &q : g_peers)
                taken |= q.used && q.pawn == p;
            if (!taken && (pass == 1 || possess_pawn_ptr(p)))
                return p;
        }
    return -1;
}

void drop_peer(int k, const char *why)
{
    Peer &p = g_peers[k];
    char name[64];
    logf("net: guest %d (%s) %s, dropped%s", k + 1, peer_name(p, name, sizeof name), why,
         p.pawn >= 0 ? "; its pawn goes back to its AI" : "");
    if (p.pawn >= 0) {
        possess_pawn(p.pawn, false, 0, -1);
        storage_set_party_character(p.pawn, std::string());
    }
    InterlockedExchange(&p.cam_valid, 0);
    p = Peer{};
    p.pawn = -1;
}

void receive_all()
{
    char buf[1500];
    for (;;) {
        sockaddr_in from = {};
        uint64_t from_steam = 0;
        int n;
        if (g_steam) {
            n = link_steam_recv(buf, sizeof buf, &from_steam);
        } else {
            int fl = sizeof from;
            n = recvfrom(g_sock, buf, sizeof buf, 0, reinterpret_cast<sockaddr *>(&from), &fl);
        }
        if (n < 0)
            return;
        if (n < int(sizeof(Header)))
            continue;
        auto h = reinterpret_cast<const Header *>(buf);
        bool hello = h->magic == 0x4F434444 && h->type == 0 && n == 9;
        if (!hello && other_version(h->magic) && !InterlockedExchange(&g_old_peer, 1))
            logf("net: a player has an older version of the mod (its messages are not read): every player needs this one"
                 " (format %08X, %d bytes, type %u, from ...%04u)",
                 unsigned(h->magic), n, unsigned(h->type), unsigned(from_steam % 10000));
        if (h->magic != kMagic)
            continue;
        int k = -1;
        for (int i = 0; i < kMaxPeers && k < 0; ++i) {
            const Peer &p = g_peers[i];
            if (!p.used)
                continue;
            if (g_role == NET_GUEST || (g_steam ? p.steam == from_steam
                                                : p.addr.sin_addr.s_addr == from.sin_addr.s_addr &&
                                                      p.addr.sin_port == from.sin_port))
                k = i;
        }
        if (k < 0 && g_role == NET_HOST && h->type == MSG_INPUT && n == sizeof(InputMsg) && !storage_host_ready()) {
            static DWORD next_wait_log = 0;
            if (!next_wait_log || int(GetTickCount() - next_wait_log) >= 0) {
                next_wait_log = (GetTickCount() + 10000) | 1;
                logf("net: a guest waits: this game is before the end of the tutorial (no main pawn yet), co-op starts after it");
            }
            continue;
        }
        if (k < 0 && g_role == NET_HOST && h->type == MSG_INPUT && n == sizeof(InputMsg)) {
            for (int i = 0; i < kMaxPeers && k < 0; ++i)
                if (!g_peers[i].used)
                    k = i;
            int pawn = k >= 0 ? free_pawn() : -1;
            if (k < 0 || pawn < 0) {
                static DWORD next_full_log = 0;
                if (!next_full_log || int(GetTickCount() - next_full_log) >= 0) {
                    next_full_log = GetTickCount() + 10000;
                    logf("net: a guest knocked, but all %d places are taken", kMaxPeers);
                }
                continue;
            }
            Peer &p = g_peers[k];
            p = Peer{};
            p.used = true;
            p.addr = from;
            p.steam = from_steam;
            p.pawn = pawn;
            for (int s = 0; s < 4; ++s) {
                p.loot_joined[s] = s == 0 ? loot_mine_count() : uint32_t(g_loot_log[s].size());
                p.flags_joined[s] = s == 0 ? flags_mine_count() : uint32_t(g_flags_log[s].size());
            }
            char name[64];
            logf("net: guest connected from %s (guest %d, pad slot %d, pawn %d%s)", peer_name(p, name, sizeof name), k + 1,
                 k + 1, pawn, pawn == 0 ? ", the main pawn" : "");
            if (!g_shared)
                possess_pawn(pawn, true, k + 1, k);
        }
        if (k < 0)
            continue;
        Peer &peer = g_peers[k];
        if (peer.last_seq != 0 && int32_t(h->seq - peer.last_seq) <= 0 && int32_t(peer.last_seq - h->seq) < 1000)
            continue;
        peer.last_seq = h->seq;
        ++g_rx;
        peer.last_rx = GetTickCount() | 1;
        if (g_role == NET_HOST && h->type == MSG_INPUT && n == sizeof(InputMsg)) {
            if (g_shared)
                continue;
            auto m = reinterpret_cast<const InputMsg *>(buf);
            if (m->away != (possess_pawn_on(peer.pawn) ? 0 : 1) && peer.pawn >= 0) {
                possess_pawn(peer.pawn, !m->away, k + 1, k);
                logf("net: guest %d %s", k + 1, m->away ? "explores on its own: its pawn goes back to its AI" : "is back: it drives its pawn again");
            }
            if (m->away)
                continue;
            XINPUT_GAMEPAD pad = m->pad;
            pad.sThumbRX = pad.sThumbRY = 0;
            pad.wButtons &= WORD(~(XINPUT_GAMEPAD_RIGHT_THUMB | XINPUT_GAMEPAD_BACK | XINPUT_GAMEPAD_START));
            pads_override(unsigned(k + 1), pad, GetTickCount() + 300);
            if (m->has_camera) {
                memcpy(peer.camera + 0x40, m->eye, sizeof m->eye);
                memcpy(peer.camera + 0x60, m->target, sizeof m->target);
                InterlockedExchange(&peer.cam_valid, 1);
                if (!peer.next_cam_log || int(GetTickCount() - peer.next_cam_log) >= 0) {
                    peer.next_cam_log = (GetTickCount() + 2000) | 1;
                    float fx = m->target[0] - m->eye[0], fz = m->target[2] - m->eye[2], n = std::sqrt(fx * fx + fz * fz);
                    if (n > 1e-3f)
                        logf("net: guest %d camera forward (%.3f, %.3f)", k + 1, fx / n, fz / n);
                }
            }
        } else if (g_role == NET_HOST && h->type == MSG_CHAR_CHUNK && n >= int(sizeof(Header) + 12)) {
            auto m = reinterpret_cast<const CharChunkMsg *>(buf);
            if (m->total == 0 || m->total > 256u * 1024 || m->len > sizeof m->data || peer.char_crc == m->crc)
                continue;
            if (peer.char_crc_want != m->crc || peer.char_data.size() != m->total) {
                peer.char_crc_want = m->crc;
                peer.char_data.assign(m->total, '\0');
                peer.char_got.assign((m->total + sizeof m->data - 1) / sizeof m->data, 0);
            }
            size_t off = size_t(m->index) * sizeof m->data;
            if (m->index < peer.char_got.size() && off + m->len <= m->total) {
                memcpy(&peer.char_data[off], m->data, m->len);
                peer.char_got[m->index] = 1;
            }
            bool complete = true;
            for (uint8_t b : peer.char_got)
                complete &= b != 0;
            if (complete && crc32(reinterpret_cast<const uint8_t *>(peer.char_data.data()), peer.char_data.size()) == m->crc) {
                peer.char_crc = m->crc;
                logf("net: guest %d's character arrived: %s (%u bytes), it replaces pawn %d", k + 1,
                     save_character_name(peer.char_data).c_str(), unsigned(peer.char_data.size()), peer.pawn);
                if (peer.pawn >= 0)
                    storage_set_party_character(peer.pawn, peer.char_data);
            }
        } else if (g_role == NET_HOST && h->type == MSG_PSTATE && n == int(sizeof(PStateMsg))) {
            auto m = reinterpret_cast<const PStateMsg *>(buf);
            peer.pstate = m->me;
            float pox, poz;
            enemies_world_origin(pox, poz);
            if (m->world)
                char_shift(peer.pstate, -pox, -poz);
            peer.pstage = m->stage;
            peer.pstate_tick = GetTickCount() | 1;
            peer.exp_total = m->exp_total;
            memcpy(peer.loot_applied, m->loot_applied, sizeof peer.loot_applied);
            memcpy(peer.flags_applied, m->flags_applied, sizeof peer.flags_applied);
            if (peer.downed != m->downed)
                logf("net: guest %d is %s", k + 1, m->downed ? "down" : "up again");
            peer.downed = m->downed;
            if (g_shared)
                exp_sync_seen(1 + k, m->exp_total);
            if (g_shared && peer.pawn >= 0)
                shared_take(1 + peer.pawn, peer.pstate, m->stage == my_stage(), pox, poz);
        } else if (h->type == MSG_QUEST && n >= int(sizeof(Header) + 15)) {
            auto m = reinterpret_cast<const QuestChunkMsg *>(buf);
            int o = m->origin;
            if (o >= 4 || m->total == 0 || m->total > (4u << 20) || m->len > sizeof m->data)
                continue;
            if (g_role == NET_HOST)
                o = 1 + k;
            QuestBlob &q = g_qb[o];
            EnterCriticalSection(&g_qb_lock);
            bool done_already = q.crc == m->crc;
            if (!done_already && (q.in_crc != m->crc || q.in.size() != m->total)) {
                q.in_crc = m->crc;
                q.in.assign(m->total, 0);
                q.got.assign((m->total + sizeof m->data - 1) / sizeof m->data, 0);
            }
            bool complete = false;
            if (!done_already) {
                size_t off = size_t(m->index) * sizeof m->data;
                if (m->index < q.got.size() && off + m->len <= m->total) {
                    memcpy(q.in.data() + off, m->data, m->len);
                    q.got[m->index] = 1;
                }
                complete = true;
                for (uint8_t b : q.got)
                    complete &= b != 0;
            }
            std::vector<uint8_t> blob;
            if (complete) {
                blob.swap(q.in);
                q.got.clear();
            }
            LeaveCriticalSection(&g_qb_lock);
            std::string state;
            if (complete && save_inflate(blob, state) && (save_crc(state.data(), state.size()) | 1) == m->crc) {
                EnterCriticalSection(&g_qb_lock);
                q.crc = m->crc;
                if (g_role == NET_HOST) {
                    q.data = blob;
                    q.since = GetTickCount() | 1;
                    q.next = 0;
                }
                LeaveCriticalSection(&g_qb_lock);
                quest_sync_received(state, m->crc);
            }
        } else if ((h->type == MSG_PING || h->type == MSG_PONG) && n == int(sizeof(PingMsg))) {
            PingMsg m = *reinterpret_cast<const PingMsg *>(buf);
            static bool build_said[kMaxPeers];
            if (m.build != kBuild && !build_said[k]) {
                build_said[k] = true;
                logf("net: BUILD MISMATCH: player %d runs another build of the mod (%08X, this one %08X): same message format, but"
                     " both players should use the same package", g_role == NET_HOST ? k + 1 : 0, unsigned(m.build), unsigned(kBuild));
            }
            if (h->type == MSG_PING) {
                m.h = {kMagic, MSG_PONG, ++g_seq};
                m.wall = wall_ms();
                m.build = kBuild;
                send_to(peer, &m, sizeof m);
            } else {
                DWORD rtt = GetTickCount() - m.t;
                peer.rtt = uint16_t(rtt > 9999 ? 9999 : rtt < 1 ? 1 : rtt);
                peer.rtt_at = GetTickCount() | 1;
                static DWORD best_rtt[kMaxPeers], next_log[kMaxPeers];
                static int32_t best_off[kMaxPeers];
                int32_t off = int32_t(m.wall) - int32_t(wall_ms() - rtt / 2);
                if (off > 43200000) off -= 86400000;
                if (off < -43200000) off += 86400000;
                if (!best_rtt[k] || rtt < best_rtt[k]) {
                    best_rtt[k] = rtt ? rtt : 1;
                    best_off[k] = off;
                }
                DWORD tick = GetTickCount();
                if (!next_log[k] || int(tick - next_log[k]) >= 0) {
                    next_log[k] = (tick + 10000) | 1;
                    logf("net: clock: player %d's clock is %+d ms from this one (round trip %lu ms)", g_role == NET_HOST ? k + 1 : 0,
                         int(best_off[k]), best_rtt[k]);
                    best_rtt[k] = 0;
                }
            }
        } else if (g_shared && h->type == MSG_EVENT && n == int(sizeof(EventMsg))) {
            auto m = reinterpret_cast<const EventMsg *>(buf);
            int from = g_role == NET_HOST ? 1 + k : m->from;
            static uint32_t seen[16] = {};
            static int seen_at = 0;
            uint32_t key = (uint32_t(from) << 16) | m->id;
            bool dup = false;
            for (uint32_t s : seen)
                dup |= s == key;
            if (dup)
                continue;
            seen[seen_at++ & 15] = key;
            if (g_role == NET_HOST) {
                EventMsg f = *m;
                f.from = uint8_t(from);
                for (int q = 0; q < kMaxPeers; ++q)
                    if (q != k && g_peers[q].used)
                        for (int r = 0; r < 3; ++r)
                            send_to(g_peers[q], &f, sizeof f);
            }
            cutscene_remote(m->event, m->stage, from);
        } else if (h->type == MSG_LOTTERY && n == int(sizeof(LotteryMsg))) {
            LotteryMsg m = *reinterpret_cast<const LotteryMsg *>(buf);
            if (g_role == NET_HOST) {
                m.from = uint8_t(1 + k);
                for (int q = 0; q < kMaxPeers; ++q)
                    if (q != k && g_peers[q].used)
                        send_to(g_peers[q], &m, sizeof m);
            }
            lottery_received(m.from, m.r);
        } else if (h->type == MSG_REVIVE && n == int(sizeof(ReviveMsg))) {
            int t = reinterpret_cast<const ReviveMsg *>(buf)->target;
            if (g_role == NET_GUEST || t == 0)
                revive_helped();
            else if (t >= 1 && t <= kMaxPeers && g_peers[t - 1].used)
                send_to(g_peers[t - 1], buf, n);
        } else if (h->type == MSG_FLAGS && n >= int(sizeof(Header) + 10)) {
            auto m = reinterpret_cast<const FlagsMsg *>(buf);
            if (m->n > kFlagsPage || m->slot >= 4 || n < int(sizeof(Header) + 10 + m->n * sizeof(FlagEv)))
                continue;
            if (g_role == NET_HOST) {
                int s = 1 + k;
                if (g_flags_session[s] != m->session) {
                    g_flags_session[s] = m->session;
                    g_flags_log[s].clear();
                }
                for (int e = 0; e < m->n; ++e)
                    if (m->first + e == g_flags_log[s].size() + 1) {
                        g_flags_log[s].push_back(m->ev[e]);
                        flags_net_apply(m->ev[e]);
                    }
            } else {
                int s = m->slot;
                if (g_flags_known[s] != m->session) {
                    g_flags_known[s] = m->session;
                    g_flags_applied[s] = m->first - 1;
                }
                for (int e = 0; e < m->n; ++e)
                    if (m->first + e == g_flags_applied[s] + 1) {
                        flags_net_apply(m->ev[e]);
                        ++g_flags_applied[s];
                    }
            }
        } else if (h->type == MSG_LOOT && n >= int(sizeof(Header) + 10)) {
            auto m = reinterpret_cast<const LootMsg *>(buf);
            if (m->n > kLootPage || m->slot >= 4 || n < int(sizeof(Header) + 10 + m->n * sizeof(LootEv)))
                continue;
            if (g_role == NET_HOST) {
                int s = 1 + k;
                if (g_loot_session[s] != m->session) {
                    g_loot_session[s] = m->session;
                    g_loot_log[s].clear();
                }
                for (int e = 0; e < m->n; ++e)
                    if (m->first + e == g_loot_log[s].size() + 1) {
                        g_loot_log[s].push_back(m->ev[e]);
                        loot_net_apply(m->ev[e], s);
                    }
            } else {
                int s = m->slot;
                if (g_loot_known[s] != m->session) {
                    g_loot_known[s] = m->session;
                    g_loot_applied[s] = m->first - 1;
                }
                for (int e = 0; e < m->n; ++e)
                    if (m->first + e == g_loot_applied[s] + 1) {
                        loot_net_apply(m->ev[e], s);
                        ++g_loot_applied[s];
                    }
            }
        } else if (g_role == NET_HOST && h->type == MSG_PROGRESS_ACK && n == int(sizeof(ProgressAckMsg))) {
            peer.prog_acked = reinterpret_cast<const ProgressAckMsg *>(buf)->crc;
        } else if (g_role == NET_GUEST && h->type == MSG_PROGRESS && n >= int(sizeof(Header) + 12)) {
            auto m = reinterpret_cast<const CharChunkMsg *>(buf);
            static std::string data;
            static std::vector<uint8_t> got;
            static uint32_t want = 0, done = 0;
            if (m->total == 0 || m->total > 256u * 1024 || m->len > sizeof m->data)
                continue;
            if (m->crc != done) {
                if (want != m->crc || data.size() != m->total) {
                    want = m->crc;
                    data.assign(m->total, '\0');
                    got.assign((m->total + sizeof m->data - 1) / sizeof m->data, 0);
                }
                size_t off = size_t(m->index) * sizeof m->data;
                if (m->index < got.size() && off + m->len <= m->total) {
                    memcpy(&data[off], m->data, m->len);
                    got[m->index] = 1;
                }
                bool complete = true;
                for (uint8_t b : got)
                    complete &= b != 0;
                if (!complete || crc32(reinterpret_cast<const uint8_t *>(data.data()), data.size()) != m->crc)
                    continue;
                done = m->crc;
                progress_guest_received(data);
            }
            ProgressAckMsg a = {{kMagic, MSG_PROGRESS_ACK, ++g_seq}, done};
            send_raw(&a, sizeof a);
        } else if (g_role == NET_HOST && h->type == MSG_SAVE_NEED && n >= int(sizeof(Header) + 10)) {
            auto m = reinterpret_cast<const SaveNeedMsg *>(buf);
            peer.save_have = m->have;
            if (m->receiving == g_hsave.version && m->have != g_hsave.version)
                for (int i = 0; i < m->count && i < kSaveNeedMax; ++i)
                    if (m->index[i] < peer.save_queued.size())
                        peer.save_queued[m->index[i]] = 1;
        } else if (g_role == NET_GUEST && h->type == MSG_SAVE_CHUNK && n >= int(sizeof(Header) + 20)) {
            auto m = reinterpret_cast<const SaveChunkMsg *>(buf);
            GuestSave &g = g_gsave;
            if (m->version == g.have || m->total == 0 || m->total > 4u << 20 || m->len > kSaveChunk)
                continue;
            if (m->version != g.version) {
                g.version = m->version;
                g.total = m->total;
                g.crc = m->crc;
                g.sig = m->sig;
                g.data.assign(m->total, 0);
                g.got.assign((m->total + kSaveChunk - 1) / kSaveChunk, 0);
                logf("net: save: receiving the host's save, version %u, %u bytes", g.version, g.total);
            }
            size_t off = size_t(m->index) * kSaveChunk;
            if (m->index < g.got.size() && off + m->len <= g.total) {
                memcpy(g.data.data() + off, m->data, m->len);
                g.got[m->index] = 1;
            }
            size_t missing = 0;
            for (uint8_t b : g.got)
                missing += !b;
            if (!missing) {
                uint32_t c = crc32(g.data.data(), g.data.size());
                if (c == g.crc) {
                    storage_set_peer_save(g.data.data(), g.data.size(), g.version, g.sig);
                    g.have = g.version;
                    logf("net: save: the host's save version %u arrived (%u bytes, crc %08X)", g.version, g.total, c);
                } else {
                    logf("net: save: version %u crc %08X, expected %08X: asking again", g.version, c, g.crc);
                    g.got.assign(g.got.size(), 0);
                }
            }
        } else if (h->type == MSG_OBJECTS && n >= objects_msg_size(0)) {
            auto m = reinterpret_cast<ObjectsMsg *>(buf);
            int count = m->count;
            if (!g_shared || count > kObjPerMsg || n != objects_msg_size(count))
                continue;
            if (g_role == NET_HOST) {
                m->from = uint8_t(1 + k);
                for (int q = 0; q < kMaxPeers; ++q)
                    if (q != k && g_peers[q].used && g_peers[q].pstate_tick && g_peers[q].pstage == m->stage)
                        send_to(g_peers[q], m, n);
            }
            if (m->stage == my_stage())
                objects_receive(m->o, count);
        } else if (h->type == MSG_AREA_ENEMIES && n >= area_msg_size(0)) {
            auto m = reinterpret_cast<AreaEnemiesMsg *>(buf);
            int count = m->count;
            if (count > kAreaPerPage || n != area_msg_size(count))
                continue;
            if (g_role == NET_HOST) {
                m->from = uint8_t(1 + k);
                for (int q = 0; q < kMaxPeers; ++q)
                    if (q != k && g_peers[q].used && g_peers[q].pstate_tick && g_peers[q].pstage == m->stage)
                        send_to(g_peers[q], m, n);
                continue;
            }
            if (!g_shared || m->from != g_lead || int(g_lead) == net_my_slot() || m->stage != my_stage())
                continue;
            DWORD now = GetTickCount();
            EnterCriticalSection(&g_enemy_lock);
            for (int e = 0; e < count; ++e)
                if (m->e[e].slot < kEnemySlots) {
                    g_enemy_state[m->e[e].slot] = m->e[e];
                    state_to_local(g_enemy_state[m->e[e].slot]);
                    g_enemy_tick[m->e[e].slot] = now;
                }
            g_last_enemies_rx = now;
            LeaveCriticalSection(&g_enemy_lock);
        } else if (h->type == MSG_LEAD_ENTRIES && n >= lead_entries_size(0)) {
            auto m = reinterpret_cast<LeadEntriesMsg *>(buf);
            int count = m->count;
            if (count > kLeadEntriesMax || n != lead_entries_size(count))
                continue;
            if (g_role == NET_HOST) {
                m->from = uint8_t(1 + k);
                for (int q = 0; q < kMaxPeers; ++q)
                    if (q != k && g_peers[q].used && g_peers[q].pstate_tick && g_peers[q].pstage == m->stage)
                        send_to(g_peers[q], m, n);
                continue;
            }
            AcquireSRWLockExclusive(&g_entries_lock);
            memcpy(g_rx_entries, m->e, count * sizeof(LeadUnit));
            g_rx_entries_n = count;
            g_rx_entries_from = m->from;
            g_rx_entries_stage = m->stage;
            g_rx_entries_tick = GetTickCount() | 1;
            ReleaseSRWLockExclusive(&g_entries_lock);
        } else if (h->type == MSG_ACT_PARAMS && n >= act_params_size(0)) {
            auto m = reinterpret_cast<ActParamsMsg *>(buf);
            int count = m->count;
            if (count > kActParamsMax || n != act_params_size(count))
                continue;
            if (g_role == NET_HOST) {
                m->from = uint8_t(1 + k);
                for (int q = 0; q < kMaxPeers; ++q)
                    if (q != k && g_peers[q].used && g_peers[q].pstate_tick && g_peers[q].pstage == m->stage)
                        send_to(g_peers[q], m, n);
                continue;
            } else if (int(m->from) != (g_shared ? int(g_lead) : 0) || (g_shared && m->stage != my_stage())) {
                continue;
            }
            DWORD now = GetTickCount();
            EnterCriticalSection(&g_enemy_lock);
            for (int e = 0; e < count; ++e)
                if (m->e[e].slot < kEnemySlots) {
                    g_act_params[m->e[e].slot] = m->e[e];
                    g_act_params_tick[m->e[e].slot] = now | 1;
                }
            LeaveCriticalSection(&g_enemy_lock);
        } else if (h->type == MSG_HUMAN_CMDS && n >= human_cmds_size(0)) {
            auto m = reinterpret_cast<HumanCmdsMsg *>(buf);
            int count = m->count;
            if (count > kHumanCmdsMax || n != human_cmds_size(count))
                continue;
            if (g_role == NET_HOST) {
                m->from = uint8_t(1 + k);
                for (int q = 0; q < kMaxPeers; ++q)
                    if (q != k && g_peers[q].used && g_peers[q].pstate_tick && g_peers[q].pstage == m->stage)
                        send_to(g_peers[q], m, n);
                continue;
            } else if (int(m->from) != (g_shared ? int(g_lead) : 0) || (g_shared && m->stage != my_stage())) {
                continue;
            }
            DWORD now = GetTickCount();
            EnterCriticalSection(&g_enemy_lock);
            for (int e = 0; e < count; ++e) {
                const HumanCmd &c = m->e[e];
                if (c.slot >= kEnemySlots)
                    continue;
                HumanCmdState &d = g_hc_in[c.slot];
                uint32_t p, r;
                memcpy(&p, c.cmd + (0x2C - kHumanCmdFrom), 4);
                memcpy(&r, c.cmd + (0x34 - kHumanCmdFrom), 4);
                memcpy(d.cmd, c.cmd, kHumanCmdLen);
                d.aim = c.aim;
                d.press |= p;
                d.release |= r;
                d.tick = now | 1;
                if (p) {
                    d.press_act = c.press_act;
                    d.press_ms = c.press_ms;
                    d.press_rx = now | 1;
                }
            }
            LeaveCriticalSection(&g_enemy_lock);
            InterlockedExchangeAdd(&g_hc_rx, count);
        } else if (g_role == NET_GUEST && h->type == MSG_ENEMIES && n >= enemies_msg_size(0)) {
            auto m = reinterpret_cast<const EnemiesMsg *>(buf);
            int count = m->count;
            if (count > kEnemiesPerPage || n != enemies_msg_size(count))
                continue;
            if (g_shared && g_lead != 0)
                continue;
            DWORD now = GetTickCount();
            EnterCriticalSection(&g_enemy_lock);
            for (int k = 0; k < count; ++k)
                if (m->e[k].slot < kEnemySlots) {
                    g_enemy_state[m->e[k].slot] = m->e[k];
                    state_to_local(g_enemy_state[m->e[k].slot]);
                    g_enemy_tick[m->e[k].slot] = now;
                }
            g_last_enemies_rx = now;
            LeaveCriticalSection(&g_enemy_lock);
        } else if (g_role == NET_GUEST && h->type == MSG_SNAP && n == sizeof(SnapMsg)) {
            SnapMsg local = *reinterpret_cast<const SnapMsg *>(buf);
            float sox, soz;
            snap_to_local(local, sox, soz);
            const SnapMsg *m = &local;
            if (g_shared) {
                shared_from_snap(*m, sox, soz);
                for (int s = 0; s < 4; ++s)
                    if (s != m->my_slot)
                        exp_sync_seen(s, m->exp_total[s]);
            }
            EnterCriticalSection(&g_snap_lock);
            g_snap = *m;
            g_have_snap = true;
            for (int i = 0; i < kParty; ++i) {
                uint32_t p, r;
                memcpy(&p, m->ch[i].cmd + (0x2C - kCmdCopyFrom), 4);
                memcpy(&r, m->ch[i].cmd + (0x34 - kCmdCopyFrom), 4);
                g_pending_press[i] |= p;
                g_pending_release[i] |= r;
            }
            LeaveCriticalSection(&g_snap_lock);
        }
    }
}

void log_enemy_stats(DWORD now)
{
    EnterCriticalSection(&g_enemy_lock);
    int fresh = 0;
    for (int i = 0; i < kEnemySlots; ++i)
        fresh += g_enemy_tick[i] && now - g_enemy_tick[i] < 5000;
    EnemyStats st = g_enemy_stats;
    g_enemy_stats = {};
    LeaveCriticalSection(&g_enemy_lock);
    if (g_role == NET_HOST)
        logf("net: enemies: %d live on the host", fresh);
    else
        logf("net: enemies: %d from the host, %d local updates applied, %d new pairs, %d unpaired updates, "
             "worst error %.1f u; local hits %d blocked, %d let through (host: dead), %d deaths forced, %d "
             "guest-only removed, %d hits on the puppets blocked; %d blocked hits played here without the kill; %d of "
             "the leader's actions started here",
             fresh, st.applied, st.matched_new, st.unmatched, st.worst_err, st.blocked, st.allowed, st.forced, st.removed,
             st.puppet_blocked, st.felt, st.acted);
    {
        LONG sent = InterlockedExchange(&g_hc_sent, 0), rx = InterlockedExchange(&g_hc_rx, 0);
        LONG applied = InterlockedExchange(&g_hc_applied, 0), presses = InterlockedExchange(&g_hc_presses, 0);
        LONG fixed = InterlockedExchange(&g_hc_fixed, 0);
        if (sent || rx || applied)
            logf("net: human foes' commands: %ld sent, %ld received, written on twins in %ld frames (%ld with presses); %ld "
                 "blows asked for", sent, rx, applied, presses, fixed);
    }
    LONG refused = 0, passed = 0, reqs = 0, held = 0, evict = 0, groups = 0, masks = 0;
    net_gate_stats(&refused, &passed);
    enemies_follow_stats(&reqs, &held, &evict, &groups, &masks);
    if (refused || passed || reqs || held || evict || groups || masks)
        logf("net: follow: spawns %ld refused, %ld let through; slot requests at the top score %ld; held slots kept %ld, "
             "lowered %ld; group area checks passed by the leader's units %ld; area masks opened %ld",
             long(refused), long(passed), long(reqs), long(held), long(evict), long(groups), long(masks));
}

void send_enemies(DWORD now)
{
    EnemiesMsg m = {};
    m.h = {kMagic, MSG_ENEMIES, ++g_seq};
    EnterCriticalSection(&g_enemy_lock);
    int next = 0;
    for (int n = 0; n < kEnemySlots && m.count < kEnemiesPerPage; ++n) {
        int i = (g_send_cursor + n) % kEnemySlots;
        if (g_enemy_tick[i] && now - g_enemy_tick[i] < 5000) {
            m.e[m.count] = g_enemy_state[i];
            m.count++;
        }
        next = (i + 1) % kEnemySlots;
    }
    g_send_cursor = m.count == kEnemiesPerPage ? next : 0;
    LeaveCriticalSection(&g_enemy_lock);
    if (m.count) {
        send_raw(&m, enemies_msg_size(m.count));
    }
}

bool lead_self() { return g_role == NET_HOST || (g_shared && int(g_lead) == net_my_slot()); }

int enemy_share_id(uintptr_t em)
{
    if (!g_enemy_ready || !em)
        return -1;
    if (lead_self()) {
        EnemySlot list[128];
        int n = enemies_snapshot(list, 128);
        for (int k = 0; k < n; ++k)
            if (list[k].ptr == em)
                return list[k].index;
        return -1;
    }
    EnterCriticalSection(&g_enemy_lock);
    int id = -1;
    for (int k = 0; k < kEnemySlots && id < 0; ++k)
        if (g_follow[k].ptr == em && g_follow[k].host_slot >= 0)
            id = g_follow[k].host_slot;
    LeaveCriticalSection(&g_enemy_lock);
    return id;
}

uintptr_t enemy_from_share_id(int id)
{
    if (!g_enemy_ready || id < 0 || id >= kEnemySlots)
        return 0;
    if (lead_self()) {
        EnemySlot list[128];
        int n = enemies_snapshot(list, 128);
        for (int k = 0; k < n; ++k)
            if (list[k].index == id)
                return list[k].ptr;
        return 0;
    }
    EnterCriticalSection(&g_enemy_lock);
    uintptr_t em = 0;
    DWORD now = GetTickCount();
    for (int k = 0; k < kEnemySlots && !em; ++k)
        if (g_follow[k].host_slot == id && now - g_follow[k].last < 1000)
            em = g_follow[k].ptr;
    LeaveCriticalSection(&g_enemy_lock);
    return em;
}

void area_lead_update()
{
    int me = net_my_slot();
    PlayerInfo mine;
    int lead = -1, others = 0;
    float lp[3] = {};
    if (g_shared && me >= 0 && net_player_info(me, mine) && mine.stage >= 0) {
        for (int s = 0; s < 4; ++s) {
            PlayerInfo o;
            if (!net_player_info(s, o) || !o.present || o.stage != mine.stage)
                continue;
            float dx = o.pos[0] - mine.pos[0], dz = o.pos[2] - mine.pos[2];
            if (s != me && dx * dx + dz * dz > kLeadRange * kLeadRange)
                continue;
            if (s != me)
                ++others;
            if (lead < 0) {
                lead = s;
                memcpy(lp, o.pos, sizeof lp);
            }
        }
        if (!others)
            lead = -1;
    }
    static int pending = -2;
    static DWORD pending_since = 0;
    DWORD now = GetTickCount();
    if (lead != int(g_lead)) {
        if (lead != pending) {
            pending = lead;
            pending_since = now;
        }
        if (now - pending_since < 1500)
            return;
    }
    pending = -2;
    InterlockedExchange(&g_lead_others, lead == me ? others : 0);
    EnterCriticalSection(&g_enemy_lock);
    memcpy(g_lead_pos, lp, sizeof lp);
    if (lead != int(g_lead)) {
        memset(g_enemy_tick, 0, sizeof g_enemy_tick);
        for (Follow &f : g_follow) {
            f.host_slot = -1;
            f.unpaired_since = 0;
            f.dead_since = 0;
        }
        g_last_enemies_rx = 0;
        InterlockedExchange(&g_lead, lead);
        logf("net: enemies around this player: %s", lead < 0 ? "its own (no other player near)"
                                                    : lead == me ? "this game runs them (it leads its group)"
                                                    : lead == 0  ? "the host's"
                                                                 : "another guest's");
        if (lead > 0 && lead != me)
            logf("net: enemies: player %d leads this group (stage %d)", lead, mine.stage);
    }
    LeaveCriticalSection(&g_enemy_lock);
}

SRWLOCK g_group_lock = SRWLOCK_INIT;
float g_group_pos[4][3] = {};
int g_group_slot[4] = {};
int g_group_n = 0;
DWORD g_group_tick = 0;
volatile LONG g_group_slots = 1;
void leadlist_update(DWORD now);
void group_update(DWORD now)
{
    static DWORD next = 0;
    if (next && int(now - next) < 0)
        return;
    next = (now + 100) | 1;
    int me = net_my_slot(), n = 0;
    float p[4][3] = {};
    int who[4] = {};
    PlayerInfo mine;
    char why[160] = "";
    int wl = 0;
    bool lead = lead_self(), have_me = me >= 0 && net_player_info(me, mine);
    if (g_shared && me >= 0 && lead && have_me && mine.stage >= 0)
        for (int s = 0; s < 4; ++s) {
            PlayerInfo o;
            if (s == me)
                continue;
            bool info = net_player_info(s, o);
            if ((g_role != NET_HOST && s < me) || !info || !o.present || o.stage != mine.stage) {
                if (info && wl >= 0 && size_t(wl) < sizeof why - 40)
                    wl += _snprintf(why + wl, sizeof why - wl, " %d:%s", s, !o.present ? "absent" : o.stage != mine.stage ? "stage" : "order");
                continue;
            }
            float dx = o.pos[0] - mine.pos[0], dz = o.pos[2] - mine.pos[2];
            if (dx * dx + dz * dz > kLeadRange * kLeadRange) {
                if (wl >= 0 && size_t(wl) < sizeof why - 40)
                    wl += _snprintf(why + wl, sizeof why - wl, " %d:far %.0f", s, std::sqrt(dx * dx + dz * dz));
                continue;
            }
            memcpy(p[n], o.pos, sizeof p[n]);
            who[n++] = s;
        }
    static int last_n = -1;
    if (n != last_n) {
        logf("net: group: %d other player(s) in the group this game leads%s (shared %d, slot %d, leads %d, own state %d)%s%s", n,
             n ? "" : " (none)", int(g_shared), me, int(lead), int(have_me), *why ? "; left out:" : "", why);
        last_n = n;
    }
    AcquireSRWLockExclusive(&g_group_lock);
    memcpy(g_group_pos, p, sizeof p);
    memcpy(g_group_slot, who, sizeof who);
    g_group_n = n;
    g_group_tick = now | 1;
    ReleaseSRWLockExclusive(&g_group_lock);
    leadlist_update(now);
}

bool group_nearest(const float pos[3], const float own[3], float out[3], int *slot)
{
    if (!g_group_slots || !g_group_n)
        return false;
    auto d2 = [](const float *a, const float *b) {
        float x = a[0] - b[0], y = a[1] - b[1], z = a[2] - b[2];
        return x * x + y * y + z * z;
    };
    bool found = false;
    AcquireSRWLockShared(&g_group_lock);
    if (g_group_tick && int(GetTickCount() - g_group_tick) < 2000) {
        float best = d2(own, pos);
        for (int k = 0; k < g_group_n; ++k)
            if (d2(g_group_pos[k], pos) < best) {
                best = d2(g_group_pos[k], pos);
                memcpy(out, g_group_pos[k], 3 * sizeof(float));
                *slot = g_group_slot[k];
                found = true;
            }
    }
    ReleaseSRWLockShared(&g_group_lock);
    return found;
}
int group_positions(float out[4][3])
{
    if (!g_group_slots)
        return 0;
    AcquireSRWLockShared(&g_group_lock);
    int n = g_group_tick && int(GetTickCount() - g_group_tick) < 2000 ? g_group_n : 0;
    if (n)
        memcpy(out, g_group_pos, sizeof g_group_pos);
    ReleaseSRWLockShared(&g_group_lock);
    return n;
}
uint32_t group_party_bits()
{
    if (!g_group_slots)
        return 0;
    int who[4] = {};
    AcquireSRWLockShared(&g_group_lock);
    int n = g_group_tick && int(GetTickCount() - g_group_tick) < 2000 ? g_group_n : 0;
    memcpy(who, g_group_slot, sizeof who);
    ReleaseSRWLockShared(&g_group_lock);
    uint32_t bits = 0;
    for (int i = 1; i <= 3 && n; ++i) {
        int s = net_party_slot(i);
        for (int k = 0; k < n && s >= 0; ++k)
            if (who[k] == s)
                bits |= 1u << i;
    }
    return bits;
}
void group_slots_set(bool on)
{
    InterlockedExchange(&g_group_slots, on ? 1 : 0);
    logf("net: group slots %s: the spawn manager's slots go by %s (tests)", on ? "on" : "off",
         on ? "the nearest player of this game's group" : "this game's Arisen only, as in the unmodded game");
}

SRWLOCK g_leadlist_lock = SRWLOCK_INIT;
LeadUnit g_leadlist[kEnemySlots];
int g_leadlist_n = 0;
bool g_leadlist_on = false;
DWORD g_leadlist_tick = 0;
volatile LONG g_follow_list = 1;
volatile LONG g_gate_refused = 0, g_gate_passed = 0;
void leadlist_update(DWORD now)
{
    static LeadUnit u[kEnemySlots];
    int n = 0;
    bool on = false;
    const char *why = "";
    int lead = g_shared ? int(g_lead) : 0;
    if (!g_follow_list)
        why = "off (tests)";
    else if (!g_enemy_ready || g_role == NET_OFF || lead_self() || lead < 0)
        why = "this game leads its monsters";
    else if (net_guest_roaming())
        why = "roaming";
    else {
        PlayerInfo me, ld;
        bool same = !g_shared || (net_player_info(net_my_slot(), me) && net_player_info(lead, ld) && ld.present &&
                                  me.stage >= 0 && ld.stage == me.stage);
        EnterCriticalSection(&g_enemy_lock);
        on = same && g_last_enemies_rx && int(now - g_last_enemies_rx) < 5000;
        for (int k = 0; k < kEnemySlots && on; ++k) {
            const EnemyState &s = g_enemy_state[k];
            if (g_enemy_tick[k] && int(now - g_enemy_tick[k]) < 3000 && !s.died && !s.lot_part && lot_known(s.lot_key, s.lot_id))
                u[n++] = {s.lot_key, s.lot_id, s.lot_split};
        }
        LeaveCriticalSection(&g_enemy_lock);
        why = !same ? "the leader is in another stage" : !on ? "no pages from the leader" : "";
        int32_t stage = g_shared ? me.stage : my_stage();
        AcquireSRWLockShared(&g_entries_lock);
        bool fresh = on && g_rx_entries_tick && int(now - g_rx_entries_tick) < 1500 && g_rx_entries_from == lead &&
                     g_rx_entries_stage == stage;
        for (int e = 0; fresh && e < g_rx_entries_n && n < kEnemySlots; ++e) {
            bool dup = false;
            for (int k = 0; k < n && !dup; ++k)
                dup = u[k].key == g_rx_entries[e].key && u[k].id == g_rx_entries[e].id && u[k].split == g_rx_entries[e].split;
            if (!dup)
                u[n++] = g_rx_entries[e];
        }
        ReleaseSRWLockShared(&g_entries_lock);
    }
    AcquireSRWLockExclusive(&g_leadlist_lock);
    memcpy(g_leadlist, u, n * sizeof(LeadUnit));
    g_leadlist_n = n;
    g_leadlist_on = on;
    g_leadlist_tick = now | 1;
    ReleaseSRWLockExclusive(&g_leadlist_lock);
    static int last = -1;
    if (int(on) != last) {
        if (on)
            logf("net: follow: this game spawns the leader's units only (leader: player %d, %d placed units now)", lead, n);
        else
            logf("net: follow: the leader's list is not used (%s)", why);
        last = on;
    }
}
template <class F> bool leadlist_any(F match)
{
    bool hit = false;
    AcquireSRWLockShared(&g_leadlist_lock);
    if (g_leadlist_on && g_leadlist_tick && int(GetTickCount() - g_leadlist_tick) < 1000)
        for (int k = 0; k < g_leadlist_n && !hit; ++k)
            hit = match(g_leadlist[k]);
    ReleaseSRWLockShared(&g_leadlist_lock);
    return hit;
}
int lead_entries_has(uint32_t key, uint32_t id, uint32_t split, int *n = nullptr, int *age = nullptr)
{
    int lead = g_shared ? int(g_lead) : 0;
    DWORD now = GetTickCount();
    int32_t stage = my_stage();
    PlayerInfo me;
    if (g_shared && net_player_info(net_my_slot(), me))
        stage = me.stage;
    int r = -1;
    AcquireSRWLockShared(&g_entries_lock);
    if (g_rx_entries_tick && int(now - g_rx_entries_tick) < 1500 && g_rx_entries_from == lead && g_rx_entries_stage == stage) {
        r = 0;
        for (int e = 0; e < g_rx_entries_n && !r; ++e)
            r = g_rx_entries[e].key == key && g_rx_entries[e].id == id && g_rx_entries[e].split == split ? 1 : 0;
    }
    if (n)
        *n = g_rx_entries_n;
    if (age)
        *age = g_rx_entries_tick ? int(now - g_rx_entries_tick) : -1;
    ReleaseSRWLockShared(&g_entries_lock);
    return r;
}
bool leadlist_on()
{
    AcquireSRWLockShared(&g_leadlist_lock);
    bool on = g_leadlist_on && g_leadlist_tick && int(GetTickCount() - g_leadlist_tick) < 1000;
    ReleaseSRWLockShared(&g_leadlist_lock);
    return on;
}

void send_area_enemies(DWORD now)
{
    AreaEnemiesMsg m = {};
    m.h = {kMagic, MSG_AREA_ENEMIES, ++g_seq};
    m.stage = my_stage();
    m.from = uint8_t(net_my_slot());
    EnterCriticalSection(&g_enemy_lock);
    int next = 0;
    for (int c = 0; c < kEnemySlots && m.count < kAreaPerPage; ++c) {
        int i = (g_area_cursor + c) % kEnemySlots;
        if (g_enemy_tick[i] && now - g_enemy_tick[i] < 5000) {
            m.e[m.count] = g_enemy_state[i];
            m.count++;
        }
        next = (i + 1) % kEnemySlots;
    }
    g_area_cursor = m.count == kAreaPerPage ? next : 0;
    LeaveCriticalSection(&g_enemy_lock);
    if (m.count) {
        send_raw(&m, area_msg_size(m.count));
    }
}

void send_urgent_enemies(DWORD now)
{
    if (!emact_host_ai() || !lead_self() || (g_role != NET_HOST && !g_lead_others))
        return;
    EnemyState e[kEnemiesPerPage];
    int count = 0;
    EnterCriticalSection(&g_enemy_lock);
    for (int i = 0; i < kEnemySlots && count < kEnemiesPerPage; ++i)
        if (g_enemy_urgent[i]) {
            g_enemy_urgent[i] = 0;
            if (g_enemy_tick[i] && now - g_enemy_tick[i] < 5000)
                e[count++] = g_enemy_state[i];
        }
    LeaveCriticalSection(&g_enemy_lock);
    if (!count)
        return;
    if (g_role == NET_HOST) {
        EnemiesMsg m = {};
        m.h = {kMagic, MSG_ENEMIES, ++g_seq};
        m.count = uint8_t(count);
        memcpy(m.e, e, count * sizeof(EnemyState));
        send_raw(&m, int(sizeof(Header) + 1 + count * sizeof(EnemyState)));
    } else {
        AreaEnemiesMsg m = {};
        m.h = {kMagic, MSG_AREA_ENEMIES, ++g_seq};
        m.stage = my_stage();
        m.from = uint8_t(net_my_slot());
        m.count = uint8_t(count);
        memcpy(m.e, e, count * sizeof(EnemyState));
        send_raw(&m, area_msg_size(count));
    }
}

void salon_profile(int k, uint64_t id, std::string &persona)
{
    static uint64_t written[4] = {};
    static std::string names[4];
    static DWORD next_try[4] = {};
    char path[32];
    _snprintf(path, sizeof path, "coop_avatar_%d.bmp", k);
    if (!id) {
        if (written[k])
            DeleteFileA(path);
        written[k] = 0;
        names[k].clear();
        persona.clear();
        return;
    }
    DWORD now = GetTickCount();
    if (written[k] != id && (!next_try[k] || int(now - next_try[k]) >= 0)) {
        next_try[k] = (now + 2000) | 1;
        std::vector<uint8_t> rgba;
        uint32_t w = 0, h = 0;
        std::string name;
        if (link_steam_profile(id, name, rgba, w, h)) {
            names[k] = name;
            if (!rgba.empty()) {
                BITMAPFILEHEADER fh = {};
                BITMAPINFOHEADER ih = {};
                ih.biSize = sizeof ih;
                ih.biWidth = LONG(w);
                ih.biHeight = -LONG(h);
                ih.biPlanes = 1;
                ih.biBitCount = 32;
                ih.biCompression = BI_RGB;
                fh.bfType = 0x4D42;
                fh.bfOffBits = sizeof fh + sizeof ih;
                fh.bfSize = DWORD(fh.bfOffBits + rgba.size());
                for (size_t i = 0; i + 3 < rgba.size(); i += 4)
                    std::swap(rgba[i], rgba[i + 2]);
                char tmp[40];
                _snprintf(tmp, sizeof tmp, "%s.tmp", path);
                if (FILE *f = fopen(tmp, "wb")) {
                    fwrite(&fh, sizeof fh, 1, f);
                    fwrite(&ih, sizeof ih, 1, f);
                    fwrite(rgba.data(), 1, rgba.size(), f);
                    fclose(f);
                    MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING);
                    written[k] = id;
                    logf("net: salon: player %d's Steam picture saved (%ux%u)", k, unsigned(w), unsigned(h));
                }
            }
        }
    }
    persona = names[k];
}

int host_level = 0;

void write_status(DWORD now)
{
    std::string out = "version 1\n";
    out += g_role == NET_HOST ? "role host\n" : "role guest\n";
    int me = net_my_slot();
    int32_t here = my_stage();
    char line[256];
    _snprintf(line, sizeof line, "me %d %d\n", me, int(here));
    out += line;
    for (int s = 0; s < 4; ++s) {
        bool connected = false;
        long ago = -1;
        std::string name;
        if (s == me) {
            connected = true;
            ago = 0;
            std::string own;
            if (g_role == NET_GUEST && storage_guest_character(own))
                name = save_character_name(own);
            static uint32_t host_version = 0;
            static std::string host_name;
            std::vector<uint8_t> file;
            uint32_t v = 0;
            if (g_role == NET_HOST && storage_own_save(file, v) && v != host_version) {
                std::string xml, ch;
                uint32_t head[8];
                if (save_unpack(file, xml, head) && save_character(xml, -1, ch)) {
                    host_name = save_character_name(ch);
                    host_level = save_character_level(ch);
                }
                host_version = v;
            }
            if (g_role == NET_HOST)
                name = host_name;
        } else if (g_role == NET_HOST) {
            const Peer &p = g_peers[s - 1];
            connected = s >= 1 && p.used;
            if (connected && p.last_rx)
                ago = long(now - p.last_rx);
            if (connected && !p.char_data.empty())
                name = save_character_name(p.char_data);
        } else if (g_have_snap) {
            EnterCriticalSection(&g_snap_lock);
            connected = s == 0 || g_snap.pawn_of[s] >= 0;
            int pawn = s == 0 ? net_guest_my_pawn() : g_snap.pawn_of[s];
            LeaveCriticalSection(&g_snap_lock);
            if (connected)
                name = roam_pawn_name(pawn);
            if (s == 0 && g_peers[0].last_rx)
                ago = long(now - g_peers[0].last_rx);
        }
        PlayerInfo info;
        bool known = connected && net_player_info(s, info);
        for (char &c : name)
            if (c == '\n' || c == '\r')
                c = ' ';
        _snprintf(line, sizeof line, "player %d %d %d %d %d %ld %d %s\n", s, connected ? 1 : 0, known && info.present ? 1 : 0,
                  known && info.downed ? 1 : 0, known ? int(info.stage) : -1, ago, known && here >= 0 && info.stage == here ? 1 : 0,
                  name.c_str());
        out += line;
    }
    std::string pings;
    for (int k = 0; k < 4; ++k) {
        int level = 0, ping = 0;
        if (k == me) {
            std::string own;
            if (g_role == NET_GUEST && storage_guest_character(own))
                level = save_character_level(own);
            if (g_role == NET_HOST)
                level = host_level;
        } else if (g_role == NET_HOST) {
            const Peer &q = g_peers[k - 1];
            if (k >= 1 && q.used) {
                level = q.char_data.empty() ? 0 : save_character_level(q.char_data);
                ping = q.rtt_at && int(now - q.rtt_at) < 5000 ? q.rtt : 0;
            }
        } else if (g_have_snap) {
            EnterCriticalSection(&g_snap_lock);
            int pawn = k == 0 ? net_guest_my_pawn() : g_snap.pawn_of[k];
            uint16_t relayed = g_snap.ping_of[k];
            LeaveCriticalSection(&g_snap_lock);
            level = pawn >= 0 ? roam_pawn_level(pawn) : 0;
            ping = k == 0 ? (g_peers[0].rtt_at && now - g_peers[0].rtt_at < 5000 ? g_peers[0].rtt : 0) : relayed;
        }
        if (level > 0) {
            _snprintf(line, sizeof line, "level %d %d\n", k, level);
            out += line;
        }
        if (ping > 0) {
            _snprintf(line, sizeof line, "ping %d %d\n", k, ping);
            out += line;
            _snprintf(line, sizeof line, " %d: %d ms", k, ping);
            pings += line;
        }
    }
    static DWORD next_ping_log = 0;
    if (!pings.empty() && (!next_ping_log || int(now - next_ping_log) >= 0)) {
        next_ping_log = (now + 10000) | 1;
        logf("net: pings (player: round trip):%s", pings.c_str());
    }
    if (g_steam && link_steam_self()) {
        _snprintf(line, sizeof line, "self %llu\n", (unsigned long long)link_steam_self());
        out += line;
    }
    uintptr_t gm = game::at<uintptr_t>(game::kGameMain, 0);
    _snprintf(line, sizeof line, "game %s %s %d %d\n", g_shared ? "shared" : "classic", g_steam ? "steam" : "lan",
              gm ? game::at<int32_t>(gm, game::kGsDay) : -1, gm ? game::at<int32_t>(gm, game::kGsDayMs) : -1);
    out += line;
    for (int k = 0; k < 4; ++k) {
        uint64_t id = 0;
        if (g_steam && g_role == NET_HOST)
            id = k == 0 ? link_steam_self() : g_peers[k - 1].used ? g_peers[k - 1].steam : 0;
        else if (g_steam && k == me)
            id = link_steam_self();
        else if (g_steam && g_have_snap) {
            EnterCriticalSection(&g_snap_lock);
            id = g_snap.steam_of[k];
            LeaveCriticalSection(&g_snap_lock);
        }
        std::string persona;
        salon_profile(k, id, persona);
        for (char &c : persona)
            if (c == '\n' || c == '\r')
                c = ' ';
        if (id) {
            _snprintf(line, sizeof line, "steam %d %s\n", k, persona.c_str());
            out += line;
        }
    }
    if (FILE *f = fopen("coop_status.tmp", "wb")) {
        fwrite(out.data(), 1, out.size(), f);
        fclose(f);
        MoveFileExA("coop_status.tmp", "coop_status.txt", MOVEFILE_REPLACE_EXISTING);
    }
}

void host_save_pieces(Peer &peer);

void host_save_send(DWORD now)
{
    HostSave &s = g_hsave;
    std::vector<uint8_t> data;
    uint32_t version = 0, sig = 0;
    if (storage_own_save(data, version, &sig) && version != s.version && storage_host_ready()) {
        s.data.swap(data);
        s.version = version;
        s.sig = sig;
        s.crc = crc32(s.data.data(), s.data.size());
        for (Peer &p : g_peers)
            p.save_queued.clear();
        logf("net: save: sending save version %u (%u bytes, crc %08X)", s.version, unsigned(s.data.size()), s.crc);
    }
    if (s.data.empty())
        return;
    size_t pieces = (s.data.size() + kSaveChunk - 1) / kSaveChunk;
    for (Peer &peer : g_peers) {
        if (!peer.used || peer.save_have == s.version)
            continue;
        if (peer.save_queued.size() != pieces) {
            peer.save_queued.assign(pieces, 1);
            peer.save_last_full = now | 1;
        }
        bool any = false;
        for (uint8_t q : peer.save_queued)
            any |= q != 0;
        if (!any && now - peer.save_last_full > 3000) {
            peer.save_queued.assign(pieces, 1);
            peer.save_last_full = now | 1;
        }
        host_save_pieces(peer);
    }
}

void host_save_pieces(Peer &peer)
{
    HostSave &s = g_hsave;
    int sent = 0;
    for (size_t i = 0; i < peer.save_queued.size() && sent < 24; ++i) {
        if (!peer.save_queued[i])
            continue;
        peer.save_queued[i] = 0;
        SaveChunkMsg m = {};
        m.h = {kMagic, MSG_SAVE_CHUNK, ++g_seq};
        m.version = s.version;
        m.total = uint32_t(s.data.size());
        m.crc = s.crc;
        m.sig = s.sig;
        m.index = uint16_t(i);
        size_t off = i * kSaveChunk;
        m.len = uint16_t(std::min<size_t>(kSaveChunk, s.data.size() - off));
        memcpy(m.data, s.data.data() + off, m.len);
        send_to(peer, &m, int(sizeof(Header) + 20 + m.len));
        ++sent;
    }
}

void quest_send(DWORD now)
{
    int me = g_role == NET_HOST ? 0 : (g_have_snap ? g_snap.my_slot : -1);
    for (int o = 0; o < 4; ++o) {
        if (g_role == NET_GUEST && o != me)
            continue;
        EnterCriticalSection(&g_qb_lock);
        QuestBlob &q = g_qb[o];
        bool due = !q.data.empty() && q.since && int(now - q.since) < 30000 && (!q.next || int(now - q.next) >= 0);
        std::vector<uint8_t> data = due ? q.data : std::vector<uint8_t>();
        uint32_t crc = q.crc;
        if (due)
            q.next = (now + 3000) | 1;
        LeaveCriticalSection(&g_qb_lock);
        if (data.empty())
            continue;
        for (size_t off = 0, i = 0; off < data.size(); off += sizeof(QuestChunkMsg::data), ++i) {
            QuestChunkMsg m = {};
            m.h = {kMagic, MSG_QUEST, ++g_seq};
            m.origin = uint8_t(o);
            m.crc = crc;
            m.total = uint32_t(data.size());
            m.index = uint16_t(i);
            m.len = uint16_t(std::min<size_t>(sizeof m.data, data.size() - off));
            memcpy(m.data, data.data() + off, m.len);
            if (g_role == NET_GUEST) {
                send_raw(&m, int(sizeof(Header) + 15 + m.len));
            } else {
                for (int k = 0; k < kMaxPeers; ++k)
                    if (g_peers[k].used && 1 + k != o)
                        send_to(g_peers[k], &m, int(sizeof(Header) + 15 + m.len));
            }
        }
    }
}

void host_loot_send(DWORD now)
{
    g_loot_session[0] = g_loot_my_session;
    for (int k = 0; k < kMaxPeers; ++k) {
        Peer &peer = g_peers[k];
        if (!peer.used || (peer.loot_next && int(now - peer.loot_next) < 0))
            continue;
        peer.loot_next = (now + 250) | 1;
        for (int s = 0; s < 4; ++s) {
            if (s == 1 + k)
                continue;
            uint32_t size = s == 0 ? loot_mine_count() : uint32_t(g_loot_log[s].size());
            uint32_t from = std::max(peer.loot_applied[s], peer.loot_joined[s]) + 1;
            if (from > size)
                continue;
            std::vector<LootEv> evs;
            if (s == 0) {
                loot_net_mine(evs, from, kLootPage);
            } else {
                for (uint32_t q = from; q <= size && int(evs.size()) < kLootPage; ++q)
                    evs.push_back(g_loot_log[s][q - 1]);
            }
            LootMsg lm = {};
            lm.h = {kMagic, MSG_LOOT, ++g_seq};
            lm.slot = uint8_t(s);
            lm.session = g_loot_session[s];
            lm.first = from;
            lm.n = uint8_t(evs.size());
            memcpy(lm.ev, evs.data(), evs.size() * sizeof(LootEv));
            send_to(peer, &lm, int(sizeof(Header) + 10 + evs.size() * sizeof(LootEv)));
        }
        for (int s = 0; s < 4; ++s) {
            if (s == 1 + k)
                continue;
            uint32_t size = s == 0 ? flags_mine_count() : uint32_t(g_flags_log[s].size());
            uint32_t from = std::max(peer.flags_applied[s], peer.flags_joined[s]) + 1;
            if (from > size)
                continue;
            std::vector<FlagEv> evs;
            if (s == 0) {
                flags_net_mine(evs, from, kFlagsPage);
            } else {
                for (uint32_t q = from; q <= size && int(evs.size()) < kFlagsPage; ++q)
                    evs.push_back(g_flags_log[s][q - 1]);
            }
            FlagsMsg fm = {};
            fm.h = {kMagic, MSG_FLAGS, ++g_seq};
            fm.slot = uint8_t(s);
            fm.session = s == 0 ? g_loot_my_session : g_flags_session[s];
            fm.first = from;
            fm.n = uint8_t(evs.size());
            memcpy(fm.ev, evs.data(), evs.size() * sizeof(FlagEv));
            send_to(peer, &fm, int(sizeof(Header) + 10 + evs.size() * sizeof(FlagEv)));
        }
    }
}

void host_progress_send(DWORD now)
{
    for (Peer &peer : g_peers) {
        if (!peer.used || peer.pawn < 0 || (peer.prog_next && int(now - peer.prog_next) < 0))
            continue;
        peer.prog_next = (now + 500) | 1;
        std::string rec;
        if (!progress_host_record(peer.pawn, rec))
            continue;
        uint32_t crc = crc32(reinterpret_cast<const uint8_t *>(rec.data()), rec.size());
        if (crc == peer.prog_acked)
            continue;
        for (size_t off = 0, i = 0; off < rec.size(); off += sizeof(CharChunkMsg::data), ++i) {
            CharChunkMsg m = {};
            m.h = {kMagic, MSG_PROGRESS, ++g_seq};
            m.total = uint32_t(rec.size());
            m.crc = crc;
            m.index = uint16_t(i);
            m.len = uint16_t(std::min<size_t>(sizeof m.data, rec.size() - off));
            memcpy(m.data, rec.data() + off, m.len);
            send_to(peer, &m, int(sizeof(Header) + 12 + m.len));
        }
    }
}

void guest_char_send(DWORD now)
{
    static DWORD next = 0;
    static std::string ch;
    static uint32_t crc = 0;
    if (next && int(now - next) < 0)
        return;
    next = (now + 500) | 1;
    if (ch.empty()) {
        char opt[8] = "1";
        if (cfg_str("send_character", opt, sizeof opt) && opt[0] == '0') {
            static bool logged = false;
            if (!logged)
                logf("net: send_character=0: this guest plays the host's pawn, its own character stays home");
            logged = true;
            return;
        }
        if (!storage_guest_character(ch))
            return;
        crc = crc32(reinterpret_cast<const uint8_t *>(ch.data()), ch.size());
    }
    EnterCriticalSection(&g_snap_lock);
    uint32_t host_has = g_have_snap ? g_snap.char_crc : 0;
    LeaveCriticalSection(&g_snap_lock);
    if (host_has == crc)
        return;
    for (size_t off = 0, i = 0; off < ch.size(); off += sizeof(CharChunkMsg::data), ++i) {
        CharChunkMsg m = {};
        m.h = {kMagic, MSG_CHAR_CHUNK, ++g_seq};
        m.total = uint32_t(ch.size());
        m.crc = crc;
        m.index = uint16_t(i);
        m.len = uint16_t(std::min<size_t>(sizeof m.data, ch.size() - off));
        memcpy(m.data, ch.data() + off, m.len);
        send_raw(&m, int(sizeof(Header) + 12 + m.len));
    }
}

void guest_save_need(DWORD now)
{
    GuestSave &g = g_gsave;
    if (g.next_need && int(now - g.next_need) < 0)
        return;
    g.next_need = (now + 250) | 1;
    SaveNeedMsg m = {};
    m.h = {kMagic, MSG_SAVE_NEED, ++g_seq};
    m.have = g.have;
    m.receiving = g.version != g.have ? g.version : 0;
    for (size_t i = 0; m.receiving && i < g.got.size() && m.count < kSaveNeedMax; ++i)
        if (!g.got[i])
            m.index[m.count++] = uint16_t(i);
    send_raw(&m, int(sizeof(Header) + 10 + 2 * m.count));
}

DWORD WINAPI net_thread(LPVOID)
{
    DWORD next_send = GetTickCount(), next_log = GetTickCount() + 5000;
    DWORD next_tick = GetTickCount();
    bool steam_up = false;
    if (emact_host_ai() && !g_net_wake)
        g_net_wake = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    for (;;) {
        DWORD now = GetTickCount();
        if (g_steam && !steam_up) {
            steam_up = link_steam_start(g_role);
            Sleep(50);
            continue;
        }
        if (g_steam && int(now - next_tick) >= 0) {
            link_steam_tick();
            next_tick = now + 2000;
        }
        receive_all();
        now = GetTickCount();
        send_urgent_enemies(now);
        group_update(now);
        static DWORD next_lottery = 0;
        static uint32_t lottery_sent_load = 0;
        {
            LotteryMsg m = {{kMagic, MSG_LOTTERY, 0}, 0, {}};
            int me = net_my_slot();
            bool have = me >= 0 && lottery_mine(m.r);
            uint32_t ver = m.r.load * 2 + m.r.kind0_done;
            if (have && (ver != lottery_sent_load || !next_lottery || int(now - next_lottery) >= 0)) {
                next_lottery = (now + 2000) | 1;
                lottery_sent_load = ver;
                m.from = uint8_t(me);
                m.h.seq = ++g_seq;
                if (g_role == NET_GUEST)
                    send_raw(&m, sizeof m);
                else
                    for (int q = 0; q < kMaxPeers; ++q)
                        if (g_peers[q].used)
                            send_to(g_peers[q], &m, sizeof m);
            }
        }
        static DWORD next_entries = 0;
        if (g_role != NET_OFF && lead_self() && (g_role == NET_HOST || g_lead_others) &&
            (!next_entries || int(now - next_entries) >= 0)) {
            next_entries = (now + 250) | 1;
            LeadEntriesMsg m = {};
            m.h = {kMagic, MSG_LEAD_ENTRIES, ++g_seq};
            AcquireSRWLockShared(&g_entries_lock);
            bool ok = g_my_entries_tick && int(now - g_my_entries_tick) < 1000 && g_my_entries_stage >= 0;
            if (ok) {
                m.count = uint8_t(g_my_entries_n);
                memcpy(m.e, g_my_entries, m.count * sizeof(LeadUnit));
                m.stage = g_my_entries_stage;
            }
            ReleaseSRWLockShared(&g_entries_lock);
            m.from = uint8_t(g_role == NET_HOST ? 0 : net_my_slot());
            if (ok && g_role == NET_GUEST)
                send_raw(&m, lead_entries_size(m.count));
            else if (ok)
                for (int q = 0; q < kMaxPeers; ++q)
                    if (g_peers[q].used)
                        send_to(g_peers[q], &m, lead_entries_size(m.count));
        }
        static DWORD next_params = 0, next_params_slow = 0;
        if (emact_host_ai() && g_role != NET_OFF && lead_self() && (g_role == NET_HOST || g_lead_others) &&
            (InterlockedExchange(&g_act_params_now, 0) || !next_params || int(now - next_params) >= 0)) {
            next_params = (now + 100) | 1;
            bool slow = !next_params_slow || int(now - next_params_slow) >= 0;
            if (slow)
                next_params_slow = (now + 1000) | 1;
            ActParamsMsg m = {};
            m.h = {kMagic, MSG_ACT_PARAMS, ++g_seq};
            m.stage = my_stage();
            m.from = uint8_t(g_role == NET_HOST ? 0 : net_my_slot());
            EnterCriticalSection(&g_enemy_lock);
            for (int k = 0; k < kEnemySlots && m.count < kActParamsMax; ++k) {
                if (!g_act_params_tick[k] || !g_enemy_tick[k] || now - g_enemy_tick[k] >= 1000)
                    continue;
                const EnemyState &cur = g_enemy_state[k];
                bool running = (cur.attached & 2) && cur.act == g_act_params[k].act && cur.act_starts == g_act_params[k].starts;
                if (int(now - g_act_params_tick[k]) < 3000 || (slow && running))
                    m.e[m.count++] = g_act_params[k];
            }
            LeaveCriticalSection(&g_enemy_lock);
            if (m.count && g_role == NET_GUEST)
                send_raw(&m, act_params_size(m.count));
            else if (m.count)
                for (int q = 0; q < kMaxPeers; ++q)
                    if (g_peers[q].used)
                        send_to(g_peers[q], &m, act_params_size(m.count));
        }
        static DWORD next_hc = 0;
        if (net_human_cmds_on() && g_role != NET_OFF && lead_self() && (g_role == NET_HOST || g_lead_others) &&
            (InterlockedExchange(&g_hc_now, 0) || !next_hc || int(now - next_hc) >= 0)) {
            next_hc = (now + 33) | 1;
            HumanCmdsMsg m = {};
            m.h = {kMagic, MSG_HUMAN_CMDS, ++g_seq};
            m.stage = my_stage();
            m.from = uint8_t(g_role == NET_HOST ? 0 : net_my_slot());
            EnterCriticalSection(&g_enemy_lock);
            for (int k = 0; k < kEnemySlots && m.count < kHumanCmdsMax; ++k) {
                HumanCmdState &s = g_hc_out[k];
                if (!s.tick || int(now - s.tick) >= 500)
                    continue;
                HumanCmd &c = m.e[m.count++];
                c.slot = uint8_t(k);
                memcpy(c.cmd, s.cmd, kHumanCmdLen);
                memcpy(c.cmd + (0x2C - kHumanCmdFrom), &s.press, 4);
                memcpy(c.cmd + (0x34 - kHumanCmdFrom), &s.release, 4);
                c.aim = s.aim;
                c.press_act = s.press ? s.press_act : 0;
                c.press_ms = s.press ? s.press_ms : 0xFFFF;
                s.press = s.release = 0;
            }
            LeaveCriticalSection(&g_enemy_lock);
            if (m.count) {
                InterlockedExchangeAdd(&g_hc_sent, m.count);
                if (g_role == NET_GUEST)
                    send_raw(&m, human_cmds_size(m.count));
                else
                    for (int q = 0; q < kMaxPeers; ++q)
                        if (g_peers[q].used)
                            send_to(g_peers[q], &m, human_cmds_size(m.count));
            }
        }
        static DWORD next_status = 0;
        if (g_shared && (!next_status || int(now - next_status) >= 0)) {
            next_status = (now + 1000) | 1;
            write_status(now);
            for (Peer &p : g_peers)
                if (p.used && p.last_rx) {
                    PingMsg m = {{kMagic, MSG_PING, ++g_seq}, GetTickCount(), 0, kBuild};
                    send_to(p, &m, sizeof m);
                }
        }
        for (int k = 0; k < kMaxPeers; ++k) {
            Peer &p = g_peers[k];
            if (!p.used || !p.last_rx || int(now - p.last_rx) <= int(kPeerTimeoutMs))
                continue;
            if (g_role == NET_HOST) {
                char why[48];
                _snprintf(why, sizeof why, "silent for %lu ms", now - p.last_rx);
                drop_peer(k, why);
            } else {
                logf("net: peer silent for %lu ms, dropped", now - p.last_rx);
                p.last_rx = 0;
                p.last_seq = 0;
            }
        }
        field_origin_scan();
        static DWORD obj_next = 0;
        if (g_shared && (!obj_next || int(now - obj_next) >= 0)) {
            obj_next = (now + 100) | 1;
            bool anyone = false;
            for (const Peer &p : g_peers)
                anyone |= p.used;
            if (anyone && possess_arisen()) {
                ObjectsMsg om = {};
                om.h = {kMagic, MSG_OBJECTS, ++g_seq};
                om.stage = my_stage();
                om.from = uint8_t(net_my_slot());
                om.count = uint8_t(objects_collect(om.o, kObjPerMsg));
                if (om.count)
                    send_raw(&om, objects_msg_size(om.count));
            }
        }
        if (int(now - next_send) >= 0) {
            if (g_role == NET_GUEST) {
                InputMsg m = {};
                m.h = {kMagic, MSG_INPUT, ++g_seq};
                XINPUT_STATE s = {};
                m.away = net_guest_roaming() ? 1 : 0;
                if (!m.away && possess_arisen() && pads_read_raw(0, &s) == ERROR_SUCCESS)
                    m.pad = s.Gamepad;
                uintptr_t holder = game::at<uintptr_t>(0, game::va(0x018D0B6C));
                uintptr_t cam = holder ? game::at<uintptr_t>(holder, 0x34) : 0;
                if (cam && !m.away) {
                    memcpy(m.eye, reinterpret_cast<void *>(cam + 0x40), sizeof m.eye);
                    memcpy(m.target, reinterpret_cast<void *>(cam + 0x60), sizeof m.target);
                    m.has_camera = 1;
                }
                send_raw(&m, sizeof m);
                next_send = now + 16;
                guest_save_need(now);
                guest_char_send(now);
                if (g_shared) {
                    revive_tick();
                    quest_sync_tick();
                    quest_send(now);
                }
                static DWORD flags_next = 0;
                if (g_shared && (!flags_next || int(now - flags_next) >= 0)) {
                    flags_next = (now + 250) | 1;
                    flags_scan();
                }
                if (g_shared) {
                    PStateMsg ps = {};
                    ps.h = {kMagic, MSG_PSTATE, ++g_seq};
                    read_state(0, ps.me);
                    float wox, woz;
                    if (enemies_world_origin(wox, woz)) {
                        char_shift(ps.me, wox, woz);
                        ps.world = 1;
                    }
                    ps.stage = my_stage();
                    ps.exp_total = exp_sync_total();
                    ps.loot_session = g_loot_my_session;
                    memcpy(ps.loot_applied, g_loot_applied, sizeof ps.loot_applied);
                    memcpy(ps.flags_applied, g_flags_applied, sizeof ps.flags_applied);
                    ps.downed = revive_down() ? 1 : 0;
                    send_raw(&ps, sizeof ps);
                    area_lead_update();
                    if (g_lead_others && g_lead == net_my_slot())
                        send_area_enemies(now);
                    static DWORD loot_next = 0;
                    if (g_have_snap && (!loot_next || int(now - loot_next) >= 0)) {
                        loot_next = (now + 250) | 1;
                        EnterCriticalSection(&g_snap_lock);
                        uint32_t got = g_snap.loot_got, fgot = g_snap.flags_got;
                        int8_t slot = g_snap.my_slot;
                        LeaveCriticalSection(&g_snap_lock);
                        std::vector<FlagEv> fevs;
                        flags_net_mine(fevs, fgot + 1, kFlagsPage);
                        if (!fevs.empty()) {
                            FlagsMsg fm = {};
                            fm.h = {kMagic, MSG_FLAGS, ++g_seq};
                            fm.slot = uint8_t(slot);
                            fm.session = g_loot_my_session;
                            fm.first = fgot + 1;
                            fm.n = uint8_t(fevs.size());
                            memcpy(fm.ev, fevs.data(), fevs.size() * sizeof(FlagEv));
                            send_raw(&fm, int(sizeof(Header) + 10 + fevs.size() * sizeof(FlagEv)));
                        }
                        std::vector<LootEv> evs;
                        loot_net_mine(evs, got + 1, kLootPage);
                        if (!evs.empty()) {
                            LootMsg lm = {};
                            lm.h = {kMagic, MSG_LOOT, ++g_seq};
                            lm.slot = uint8_t(slot);
                            lm.session = g_loot_my_session;
                            lm.first = got + 1;
                            lm.n = uint8_t(evs.size());
                            memcpy(lm.ev, evs.data(), evs.size() * sizeof(LootEv));
                            send_raw(&lm, int(sizeof(Header) + 10 + evs.size() * sizeof(LootEv)));
                        }
                    }
                } else {
                    roam_tick();
                }
                bool roam = net_guest_roaming() || roam_busy() || g_shared;
                if (!roam)
                    guest_talk_mirror(now);
                if (g_have_snap && (!roam || g_shared)) {
                    EnterCriticalSection(&g_snap_lock);
                    uint32_t host_sig = g_snap.stage.party_sig;
                    LeaveCriticalSection(&g_snap_lock);
                    rejoin_tick(NET_GUEST, host_sig);
                }
            } else {
                SnapMsg m = {};
                m.h = {kMagic, MSG_SNAP, ++g_seq};
                for (int i = 0; i < kParty; ++i)
                    read_state(i, m.ch[i]);
                read_stage(m.stage);
                for (int i = 0; i < kParty; ++i)
                    m.stage_of[i] = m.stage.stage;
                if (g_shared) {
                    bool taken[kParty] = {};
                    for (const Peer &p : g_peers)
                        if (p.used && p.pawn >= 0 && p.pawn < kPawns)
                            taken[1 + p.pawn] = true;
                    for (int i = 2; i < kParty; ++i) {
                        m.ch[i].valid = 0;
                        shared_set_hidden(i, !taken[i]);
                    }
                    if (taken[1])
                        m.ch[1].valid = 0;
                    for (Peer &p : g_peers)
                        if (p.used && p.pawn >= 0 && p.pstate_tick && int(now - p.pstate_tick) < 2000) {
                            m.ch[1 + p.pawn] = p.pstate;
                            m.stage_of[1 + p.pawn] = p.pstage;
                            memset(p.pstate.cmd + (0x2C - kCmdCopyFrom), 0, 4);
                            memset(p.pstate.cmd + (0x34 - kCmdCopyFrom), 0, 4);
                        }
                }
                snap_to_world(m);
                m.exp_total[0] = exp_sync_total();
                for (int k = 0; k < kMaxPeers; ++k)
                    m.exp_total[1 + k] = g_peers[k].used ? g_peers[k].exp_total : 0;
                for (const Peer &peer : g_peers) {
                    m.my_slot = int8_t(1 + (&peer - g_peers));
                    m.loot_got = uint32_t(g_loot_log[m.my_slot].size());
                    m.flags_got = uint32_t(g_flags_log[m.my_slot].size());
                    m.down[0] = revive_down() ? 1 : 0;
                    m.pawn_of[0] = -1;
                    for (int q = 0; q < kMaxPeers; ++q) {
                        m.down[1 + q] = g_peers[q].used ? g_peers[q].downed : 0;
                        m.pawn_of[1 + q] = int8_t(g_peers[q].used ? g_peers[q].pawn : -1);
                    }
                    m.steam_of[0] = g_steam ? link_steam_self() : 0;
                    for (int q = 0; q < kMaxPeers; ++q)
                        m.steam_of[1 + q] = g_steam && g_peers[q].used ? g_peers[q].steam : 0;
                    for (int q = 0; q < kMaxPeers; ++q)
                        m.ping_of[1 + q] = g_peers[q].used && g_peers[q].rtt_at && int(now - g_peers[q].rtt_at) < 5000 ? g_peers[q].rtt : 0;
                    m.my_pawn = int8_t(peer.pawn);
                    m.char_crc = peer.char_crc;
                    send_to(peer, &m, sizeof m);
                }
                next_send = now + 30;
                send_enemies(now);
                storage_merge_tick();
                rejoin_tick(NET_HOST, 0);
                host_save_send(now);
                host_progress_send(now);
                if (g_shared) {
                    revive_tick();
                    quest_sync_tick();
                    quest_send(now);
                }
                static DWORD flags_next = 0;
                if (g_shared && (!flags_next || int(now - flags_next) >= 0)) {
                    flags_next = (now + 250) | 1;
                    flags_scan();
                }
                if (g_shared)
                    host_loot_send(now);
            }
        }
        if (int(now - next_log) >= 0) {
            DWORD last = 0;
            for (const Peer &p : g_peers)
                if (p.used && p.last_rx && (!last || int(p.last_rx - last) > 0))
                    last = p.last_rx;
            logf("net: tx %ld rx %ld, last rx %ld ms ago", g_tx, g_rx, last ? long(now - last) : -1L);
            if (long n = catchup_skipped_take())
                logf("net: shared: %ld catch-ups of the pawns showing players skipped (the game would put them next to this Arisen)", n);
            if (g_role == NET_HOST) {
                CharState c[kParty] = {};
                EnterCriticalSection(&g_cap_lock);
                memcpy(c, g_cap, sizeof c);
                LeaveCriticalSection(&g_cap_lock);
                char line[160];
                int len = 0;
                for (int i = 1; i < kParty; ++i)
                    if (c[0].valid && c[i].valid)
                        len += _snprintf(line + len, sizeof line - len, " pawn %d %.0f u;", i - 1,
                                         std::sqrt((c[i].pos[0] - c[0].pos[0]) * (c[i].pos[0] - c[0].pos[0]) +
                                                   (c[i].pos[2] - c[0].pos[2]) * (c[i].pos[2] - c[0].pos[2])));
                if (len)
                    logf("net: spread from the Arisen at (%.0f, %.0f):%s", c[0].pos[0], c[0].pos[2], line);
            }
            log_enemy_stats(now);
            next_log = now + 5000;
        }
        if (g_net_wake)
            WaitForSingleObject(g_net_wake, 2);
        else
            Sleep(4);
    }
}

}

volatile LONG g_fake_hold_slot = -1;
volatile LONG g_fake_hold_until = 0;
void net_test_fake_hold(int slot, int ms)
{
    g_fake_hold_slot = slot;
    g_fake_hold_until = LONG(GetTickCount() + DWORD(ms));
}

void net_capture(uintptr_t ch, int i)
{
    if (!(g_role == NET_HOST || (g_shared && i == 0)) || i < 0 || i >= kParty)
        return;
    if (i == 0) {
        static float last_ox = 0.f, last_oz = 0.f;
        float ox, oz;
        enemies_world_origin(ox, oz);
        EnterCriticalSection(&g_cap_lock);
        for (int k = 0; k < kParty; ++k)
            rp_rebase(k, ox, oz);
        LeaveCriticalSection(&g_cap_lock);
        if (ox != last_ox || oz != last_oz) {
            logf("net: field cell origin (%.0f, %.0f) -> (%.0f, %.0f)", -last_ox + 0.f, -last_oz + 0.f, -ox + 0.f, -oz + 0.f);
            last_ox = ox;
            last_oz = oz;
        }
    }
    CharState s = {};
    capture_state(ch, s);
    s.hold_slot = -1;
    s.hold_enemy = -1;
    if (g_shared && i == 0) {
        s.action = game::at<int32_t>(ch, 0x2DD4);
        s.hold_kind = int8_t(game::at<int32_t>(ch, 0x3938));
        s.lantern = lantern_on(ch);
        look_of(ch, s.look);
        uintptr_t held = hold_action(s.action) ? game::at<uintptr_t>(ch, 0x38F0) : 0;
        for (int p = 0; held && p < kPawns; ++p)
            if (possess_pawn_ptr(p) == held)
                s.hold_slot = int8_t(party_to_slot(1 + p));
        if (held && s.hold_slot < 0)
            s.hold_enemy = int16_t(enemy_share_id(held));
        if (g_fake_hold_slot >= 0 && int(GetTickCount() - DWORD(g_fake_hold_until)) < 0) {
            s.action = 0xA9;
            s.hold_slot = int8_t(g_fake_hold_slot);
            held = 0;
        }
        if (held && s.hold_slot < 0) {
            float fx, fz;
            facing(ch, fx, fz);
            float dx = game::at<float>(held, 0x40) - game::at<float>(ch, 0x40), dz = game::at<float>(held, 0x48) - game::at<float>(ch, 0x48);
            s.hold_off[0] = dx * fx + dz * fz;
            s.hold_off[1] = game::at<float>(held, 0x44) - game::at<float>(ch, 0x44);
            s.hold_off[2] = dx * fz - dz * fx;
            if (strstr(action_name(ch), "Lift") || !(std::fabs(s.hold_off[0]) + std::fabs(s.hold_off[2]) < 300.f)) {
                s.hold_off[0] = 15.f;
                s.hold_off[1] = 180.f;
                s.hold_off[2] = 0.f;
            }
            static DWORD next_hold_log = 0;
            if (!next_hold_log || int(GetTickCount() - next_hold_log) >= 0) {
                next_hold_log = (GetTickCount() + 1000) | 1;
                logf("net: shared: holding enemy %d (%s): %.0f ahead, %+.0f up, %.0f right", s.hold_enemy, action_name(ch), s.hold_off[0],
                     s.hold_off[1], s.hold_off[2]);
            }
        }
    }
    EnterCriticalSection(&g_cap_lock);
    g_cap[i] = s;
    g_cap_tick[i] = GetTickCount() | 1;
    LeaveCriticalSection(&g_cap_lock);
}

uintptr_t current_stage_area()
{
    uintptr_t area = game::at<uintptr_t>(game::kArea, 0);
    uintptr_t cur = area ? game::at<uintptr_t>(area, game::kAreaCurrent) : 0;
    return cur && strncmp(enemies_class_of(cur), "aStage", 6) == 0 ? cur : 0;
}

void net_start(NetRole role, const char *peer, int port)
{
    InitializeCriticalSectionAndSpinCount(&g_cap_lock, 20000);
    char sw[8] = "";
    g_shared = !cfg_str("shared_world", sw, sizeof sw) || atoi(sw) != 0;
    InitializeCriticalSection(&g_qb_lock);
    g_loot_my_session = (GetTickCount() ^ (GetCurrentProcessId() << 16)) | 1;
    if (g_shared)
        logf("net: shared world: every player plays its own character in its own copy of the host's world");
    MemoryBarrier();
    g_role = role;
    for (Peer &p : g_peers)
        p.pawn = -1;
    char order[32];
    if (cfg_str("pawn_order", order, sizeof order)) {
        int a[kPawns], n = sscanf(order, "%d,%d,%d", &a[0], &a[1], &a[2]);
        for (int i = 0; i < n && i < kPawns; ++i)
            if (a[i] >= 0 && a[i] < kPawns)
                g_pawn_order[i] = a[i];
    }
    if (role == NET_HOST)
        logf("net: pawn order for guests 1-3: %d, %d, %d", g_pawn_order[0], g_pawn_order[1], g_pawn_order[2]);
    InitializeCriticalSectionAndSpinCount(&g_snap_lock, 20000);
    InitializeCriticalSectionAndSpinCount(&g_enemy_lock, 20000);
    InterlockedExchange(&g_enemy_ready, 1);
    char tp[16] = "udp";
    cfg_str("transport", tp, sizeof tp);
    g_steam = strcmp(tp, "steam") == 0;
    if (g_steam) {
        if (role == NET_GUEST)
            g_peers[0].used = true;
        logf("net: %s over Steam P2P, waiting for Steam", role == NET_HOST ? "host" : "guest");
        if (HANDLE t = CreateThread(nullptr, 0, net_thread, nullptr, 0, nullptr))
            CloseHandle(t);
        return;
    }
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    g_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    u_long nb = 1;
    ioctlsocket(g_sock, FIONBIO, &nb);
    sockaddr_in local = {};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    local.sin_port = htons(role == NET_HOST ? u_short(port) : 0);
    if (bind(g_sock, reinterpret_cast<sockaddr *>(&local), sizeof local) != 0) {
        logf("net: bind failed (%d)", WSAGetLastError());
        return;
    }
    if (role == NET_GUEST) {
        Peer &h = g_peers[0];
        h.used = true;
        h.addr.sin_family = AF_INET;
        h.addr.sin_port = htons(u_short(port));
        inet_pton(AF_INET, peer, &h.addr.sin_addr);
    }
    logf("net: %s, %s port %d", role == NET_HOST ? "host" : "guest", role == NET_HOST ? "listening on" : peer, port);
    if (HANDLE t = CreateThread(nullptr, 0, net_thread, nullptr, 0, nullptr))
        CloseHandle(t);
}

NetRole net_role() { return g_role; }

uintptr_t net_host_guest_camera_holder(int k)
{
    if (g_role != NET_HOST || k < 0 || k >= kMaxPeers || !g_peers[k].cam_valid)
        return 0;
    Peer &p = g_peers[k];
    *reinterpret_cast<uintptr_t *>(p.holder + 0x34) = reinterpret_cast<uintptr_t>(p.camera);
    return reinterpret_cast<uintptr_t>(p.holder);
}

void net_host_record(uintptr_t ch, int i)
{
    if (!(g_role == NET_HOST || (g_shared && i == 0)) || i < 0 || i >= kParty)
        return;
    uintptr_t cmd = ch + game::kCmd;
    if (uint32_t p = game::at<uint32_t>(cmd, 0x2C))
        InterlockedOr(&g_acc_press[i], LONG(p));
    if (uint32_t r = game::at<uint32_t>(cmd, 0x34))
        InterlockedOr(&g_acc_release[i], LONG(r));
}

void guest_world(uintptr_t gm, const StageState &h)
{
    static int32_t ev_logged = -1;
    int32_t ev = game::at<int32_t>(gm, game::kGsEvent);
    if (ev != ev_logged) {
        logf("world: guest event state %d -> %d (host %d)", ev_logged, ev, h.event);
        ev_logged = ev;
    }
    log_scene_flags("guest", gm);
    if (h.day < 0)
        return;
    int32_t d = game::at<int32_t>(gm, game::kGsDay), ms = game::at<int32_t>(gm, game::kGsDayMs);
    int64_t mine = int64_t(d) * 86400000 + ms, host = int64_t(h.day) * 86400000 + h.day_ms;
    int64_t gap = mine - host;
    if (gap > 1000 || gap < -1000) {
        game::at<int32_t>(gm, game::kGsDay) = h.day;
        game::at<int32_t>(gm, game::kGsDayMs) = h.day_ms;
        if (gap > 600000 || gap < -600000)
            logf("world: guest time day %d %02d:%02d set to the host's day %d %02d:%02d", d, ms / 3600000,
                 ms / 60000 % 60, h.day, h.day_ms / 3600000, h.day_ms / 60000 % 60);
    }
    static DWORD next_log = 0;
    if (!next_log || int(GetTickCount() - next_log) >= 0) {
        next_log = GetTickCount() + 5000;
        int32_t md = game::at<int32_t>(gm, game::kGsDayMs);
        logf("world: time guest day %d %d ms, host day %d %d ms", game::at<int32_t>(gm, game::kGsDay), md, h.day,
             h.day_ms);
    }
}

bool plain_stage(int32_t n) { return n >= 100 && n < 999; }

void net_guest_stage()
{
    if (g_role != NET_GUEST || !g_have_snap || net_guest_roaming() || roam_busy() || g_shared)
        return;
    EnterCriticalSection(&g_snap_lock);
    StageState h = g_snap.stage;
    LeaveCriticalSection(&g_snap_lock);
    static uint32_t done_seq = 0, done_reload = 0;
    static bool seq_known = false;
    static DWORD apart_since = 0, reload_seen = 0;
    static uint32_t reload_last = 0;
    uintptr_t st = current_stage_area(), gm = game::at<uintptr_t>(game::kGameMain, 0);
    if (!st || !gm || h.stage < 0)
        return;
    int32_t mine = game::at<int32_t>(st, 0x724), my_point = game::at<int32_t>(gm, 0x44);
    guest_world(gm, h);
    if (rejoin_busy())
        return;
    bool idle = game::at<int32_t>(st, 0x808) == 1;
    static int32_t logged = -2;
    if (mine != logged) {
        logged = mine;
        logf("net: stage: guest now in %d (host in %d)", mine, h.stage);
    }
    if (!seq_known) {
        seq_known = true;
        done_seq = h.jump_seq;
        done_reload = h.reload_seq;
    }
    int32_t to = -1, point = 0;
    const char *why = "";
    DWORD now = GetTickCount();
    if (h.reload_seq != reload_last) {
        reload_last = h.reload_seq;
        reload_seen = GetTickCount() | 1;
    }
    int32_t own_stage = -1;
    DWORD own_load = stages_last_load(&own_stage);
    if (h.reload_seq != done_reload && own_load && own_stage == h.stage && int(own_load - reload_seen) > -3000) {
        done_reload = h.reload_seq;
    }
    if (h.reload_seq != done_reload && !h.jumping && h.stage >= 0) {
        to = h.stage;
        point = h.point;
        why = "host reloaded";
    } else if (h.jump_seq != done_seq && h.jumping && !plain_stage(h.jump_stage)) {
    } else if (h.jump_seq != done_seq && h.jumping && h.jump_stage == mine && h.jump_point == my_point && idle) {
        done_seq = h.jump_seq;
    } else if (h.jump_seq != done_seq && h.jumping) {
        to = h.jump_stage;
        point = h.jump_point;
        why = "host jump";
    } else if (h.jump_seq != done_seq && !h.jumping) {
        done_seq = h.jump_seq;
        if (h.stage != mine) {
            to = h.stage;
            point = h.point;
            why = "host landed after a jump";
        }
    } else if (!h.jumping && h.stage != mine) {
        if (!apart_since)
            apart_since = now | 1;
        else if (now - apart_since > 10000) {
            to = h.stage;
            point = h.point;
            why = "stages differ for 10 s";
        }
    } else {
        apart_since = 0;
    }
    if (to < 0 || !idle)
        return;
    if (!plain_stage(to)) {
        logf("net: stage: not jumping to %d (%s): not a stage number", to, why);
        done_seq = h.jump_seq;
        done_reload = h.reload_seq;
        return;
    }
    game::at<int32_t>(st, 0x80C) = to;
    game::at<int32_t>(st, 0x810) = point;
    game::at<uint8_t>(st, 0x814) = 0;
    game::at<int32_t>(st, 0x808) = 2;
    game::at<int32_t>(st, 4) = 0x13;
    done_seq = h.jump_seq;
    done_reload = h.reload_seq;
    apart_since = 0;
    logf("net: stage: guest in %d jumps to %d (point %d): %s", mine, to, point, why);
}

void guest_talk_mirror(DWORD now)
{
    uintptr_t gm = game::at<uintptr_t>(game::kGameMain, 0);
    if (!gm || !g_have_snap)
        return;
    EnterCriticalSection(&g_snap_lock);
    StageState h = g_snap.stage;
    LeaveCriticalSection(&g_snap_lock);
    int32_t mine = game::at<int32_t>(gm, game::kGsEvent);
    static bool on = false;
    static DWORD host_out = 0, catch_from = 0;
    static bool followed = false;
    bool want = h.event != 256 && mine != 256 && h.stage >= 0;
    static uint32_t talks_seen = 0;
    static DWORD talk_at = 0;
    static bool other_npc = false;
    float mine_npc[3];
    uint32_t talks = talk_last(mine_npc);
    if (talks != talks_seen) {
        talks_seen = talks;
        talk_at = now | 1;
    }
    if (talk_at && now - talk_at < 1000) {
        float dx = mine_npc[0] - h.talk_npc[0], dz = mine_npc[2] - h.talk_npc[2];
        bool other = dx * dx + dz * dz > 150.f * 150.f;
        if (other != other_npc || now - talk_at < 40)
            logf("net: talk: guest's NPC %.0f u from the host's%s", std::sqrt(dx * dx + dz * dz), other ? ": another NPC" : "");
        other_npc = other;
    }
    if (mine == 256)
        other_npc = false;
    if (other_npc)
        want = false;
    if (want)
        followed = true;
    if (want != on) {
        on = want;
        logf("net: talk: guest %s the host's pad (host event %d, guest %d)", on ? "mirrors" : "stops mirroring", h.event, mine);
    }
    if ((h.event == 256 || other_npc) && mine != 256 && h.stage >= 0) {
        if (!host_out)
            host_out = now | 1;
        if (now - host_out > 1000) {
            if (!catch_from) {
                catch_from = now | 1;
                logf("net: talk: guest still in a talk 1 s after the host's ended: %s until it ends",
                     followed ? "B/A" : "B only (a talk the host did not have)");
            }
            if (now - catch_from < 20000) {
                XINPUT_GAMEPAD g = {};
                DWORD phase = (now - catch_from) / 250 % 4;
                g.wButtons = phase == 0 ? XINPUT_GAMEPAD_B : phase == 2 && followed ? XINPUT_GAMEPAD_A : 0;
                pads_override(0, g, now + 100);
            }
        }
        return;
    }
    if (catch_from)
        logf("net: talk: guest's talk ended after %lu ms of catch-up", now - catch_from);
    host_out = catch_from = 0;
    if (mine == 256)
        followed = false;
    if (!on)
        return;
    XINPUT_GAMEPAD g = {};
    g.wButtons = h.pad_buttons & WORD(~(XINPUT_GAMEPAD_BACK | XINPUT_GAMEPAD_START));
    g.sThumbLX = h.pad_lx;
    g.sThumbLY = h.pad_ly;
    pads_override(0, g, now + 150);
}

const char *party_label(int i, int my_pawn)
{
    static const char *others[kParty] = {"arisen", "party1", "party2", "party3"};
    return i > 0 && i == 1 + my_pawn ? "pawn" : others[i];
}

void net_apply_snapshot(uintptr_t ch, int i)
{
    if (g_shared) {
        shared_apply_commands(ch, i);
        return;
    }
    if (g_role != NET_GUEST || !g_have_snap || i < 0 || i >= kParty || net_guest_roaming())
        return;
    bool is_arisen = i == 0;
    EnterCriticalSection(&g_snap_lock);
    CharState s = g_snap.ch[i];
    uint32_t press = g_pending_press[i], release = g_pending_release[i];
    g_pending_press[i] = g_pending_release[i] = 0;
    LeaveCriticalSection(&g_snap_lock);
    if (is_arisen)
        net_guest_stage();
    if (!s.valid)
        return;
    uintptr_t cmd = ch + game::kCmd;
    memcpy(reinterpret_cast<void *>(cmd + kCmdCopyFrom), s.cmd, kCmdCopyLen);
    memcpy(reinterpret_cast<void *>(cmd + kCmdExtraFrom), s.cmd_extra, kCmdExtraLen);
    game::at<uint32_t>(cmd, 0x2C) = press;
    game::at<uint32_t>(cmd, 0x34) = release;
    if (press && is_arisen)
        logf("net: guest arisen puppet press %08X (event state %d)", press,
             game::at<int32_t>(game::at<uintptr_t>(game::kGameMain, 0), game::kGsEvent));
    static DWORD next_log[kParty] = {};
    if (!next_log[i] || int(GetTickCount() - next_log[i]) >= 0) {
        next_log[i] = GetTickCount() + 1000;
        logf("net: guest %s commands applied, held %08X", party_label(i, g_snap.my_pawn), game::at<uint32_t>(cmd, 0x30));
    }
}

void net_apply_transform(uintptr_t ch, int i)
{
    if (enemies_origin_settling() && !(g_shared && i == 0))
        return;
    if (g_shared) {
        shared_apply_transform(ch, i);
        return;
    }
    if (g_role != NET_GUEST || !g_have_snap || i < 0 || i >= kParty || net_guest_roaming())
        return;
    EnterCriticalSection(&g_snap_lock);
    CharState s = g_snap.ch[i];
    LeaveCriticalSection(&g_snap_lock);
    if (!s.valid)
        return;
    float *pos = reinterpret_cast<float *>(ch + 0x40);
    float dx = s.pos[0] - pos[0], dy = s.pos[1] - pos[1], dz = s.pos[2] - pos[2];
    float err = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (err > 50.f) {
        memcpy(pos, s.pos, sizeof s.pos);
        for (uintptr_t off : kPosCopies)
            memcpy(reinterpret_cast<void *>(ch + off), s.pos, 12);
    } else {
        pos[0] += dx * 0.2f;
        pos[1] += dy * 0.2f;
        pos[2] += dz * 0.2f;
    }
    game::at<float>(ch, kYawA) = s.yaw;
    game::at<float>(ch, kYawB) = s.yaw;
    game::at<float>(ch, kYawC) = s.yaw;
    memcpy(reinterpret_cast<void *>(ch + 0x50), s.rot, sizeof s.rot);
    if (s.hp_max > 0.f)
        if (float *hp = enemies_hp(ch))
            hp[0] = s.hp;
    static DWORD next_log[kParty] = {};
    if (!next_log[i] || int(GetTickCount() - next_log[i]) >= 0) {
        next_log[i] = GetTickCount() + 1000;
        const float *hp = enemies_hp(ch);
        logf("net: guest %s error %.1f u before correction, HP %.1f (host %.1f)", party_label(i, g_snap.my_pawn), err,
             hp ? hp[0] : -1.f, s.hp);
    }
}

void net_guest_camera()
{
    static int enabled = -1;
    if (enabled < 0)
        enabled = early_flag("guest_cam_pawn") ? 1 : 0;
    if (g_role != NET_GUEST || !enabled || net_guest_roaming() || g_shared)
        return;
    static uintptr_t followed = 0;
    uintptr_t arisen = possess_arisen(), gm = game::at<uintptr_t>(game::kGameMain, 0);
    int mine = g_have_snap ? g_snap.my_pawn : -1;
    uintptr_t pawn = mine >= 0 ? possess_pawn_ptr(mine) : 0;
    bool event = gm && game::at<int32_t>(gm, game::kGsEvent) != 256;
    uintptr_t want = event ? 0 : pawn;
    if (followed && followed != want && arisen) {
        int back = camera_follow(followed, arisen);
        logf("net: guest camera: back on the Arisen (%d references; %s)", back, event ? "an event" : "pawn gone");
        followed = 0;
    }
    if (want && arisen) {
        if (int moved = camera_follow(arisen, want))
            logf("net: guest camera: %d references moved from the Arisen to pawn %d", moved, mine);
        followed = want;
    }
}

int camera_follow(uintptr_t from, uintptr_t to)
{
    uintptr_t holder = game::at<uintptr_t>(0, game::va(0x018D0B6C));
    uintptr_t cam = holder ? game::at<uintptr_t>(holder, 0x34) : 0;
    if (!from || !to || !cam)
        return 0;
    int changed = 0;
    auto retarget = [&](uintptr_t obj, uintptr_t off) {
        uintptr_t &p = game::at<uintptr_t>(obj, off);
        if (p == from) {
            p = to;
            ++changed;
        }
    };
    retarget(cam, 0x70);
    const uintptr_t mode_slots[] = {0x180, 0x184, 0x188};
    const uintptr_t mode_fields[] = {0x70, 0x24C, 0x250, 0x254};
    for (uintptr_t slot : mode_slots)
        if (uintptr_t mode = game::at<uintptr_t>(cam, slot))
            for (uintptr_t f : mode_fields)
                retarget(mode, f);
    return changed;
}

struct ClassSet {
    uint32_t h[11];
    int n;
    bool has(uint32_t c) const
    {
        for (int k = 0; k < n; ++k)
            if (h[k] == c)
                return true;
        return false;
    }
};
static ClassSet class_family(const char *name)
{
    ClassSet out = {{class_hash(name)}, 1};
    size_t len = strlen(name);
    if (len == 0 || len >= 63 || name[len - 1] < '0' || name[len - 1] > '9')
        return out;
    char v[64];
    memcpy(v, name, len + 1);
    for (char d = '0'; d <= '9'; ++d)
        if (d != name[len - 1]) {
            v[len - 1] = d;
            out.h[out.n++] = class_hash(v);
        }
    return out;
}

volatile LONG g_test_hide = -1, g_test_hide_keep = 0;
void net_test_hide(int group, int entry, bool keep)
{
    InterlockedExchange(&g_test_hide_keep, keep ? 1 : 0);
    InterlockedExchange(&g_test_hide, group < 0 ? -1 : LONG(group << 16 | (entry & 0xFFFF)));
}

struct Refused {
    uint32_t key, id, split;
    DWORD load;
    int count;
};
std::vector<Refused> g_refused;
int g_refused_logged = 0;
DWORD g_refused_load = 0;

bool net_spawn_gated(uint32_t key, uint32_t id, uint32_t split)
{
    if (g_role == NET_OFF || !g_enemy_ready || !leadlist_on())
        return false;
    bool has = leadlist_any([&](const LeadUnit &u) { return u.key == key && u.id == id && u.split == split; });
    DWORD load = stages_last_load(nullptr);
    EnterCriticalSection(&g_enemy_lock);
    if (g_refused_load != load) {
        g_refused.clear();
        g_refused_logged = 0;
        g_refused_load = load;
    }
    int at = -1;
    for (size_t n = 0; n < g_refused.size() && at < 0; ++n)
        if (g_refused[n].key == key && g_refused[n].id == id && g_refused[n].split == split)
            at = int(n);
    if (has && at >= 0) {
        logf("net: follow: lot e%02u/#%u: the leader has it now, its spawn is let through (refused %d times before)",
             unsigned((key >> 10) & 0x1FF), unsigned(id), g_refused[at].count);
        g_refused.erase(g_refused.begin() + at);
    } else if (!has && at >= 0) {
        ++g_refused[at].count;
    } else if (!has) {
        g_refused.push_back({key, id, split, load, 1});
        if (++g_refused_logged <= 24)
            logf("net: follow: lot e%02u/#%u (split %08X): spawn refused, the leader has no such unit%s",
                 unsigned((key >> 10) & 0x1FF), unsigned(id), unsigned(split),
                 g_refused_logged == 24 ? " (no more of these logged this stage load)" : "");
    }
    LeaveCriticalSection(&g_enemy_lock);
    InterlockedIncrement(has ? &g_gate_passed : &g_gate_refused);
    return !has;
}

bool death_retry_ok(uintptr_t em);

void follower_actions(Follow &f, const EnemyState &s, uintptr_t em, int i, DWORD now)
{
    const char *name = enemies_class_of(em);
    bool alive = !s.died && !f.my_died && !(s.hp_max > 0.f && s.hp <= 0.f);
    int tc = s.attached >> 4;
    if (alive && tc != 15 && strncmp(name, "uEm", 3) == 0) {
        uintptr_t t = tc ? slot_character(tc - 1) : 0;
        if (!tc || t) {
            game::at<uintptr_t>(em, 0x2B98) = t;
            game::at<uintptr_t>(em, 0x5CA4) = t;
        }
        if (emact_logging() && f.tgt_seen != tc) {
            f.tgt_seen = tc;
            char who[40] = "nobody";
            if (tc)
                _snprintf(who, sizeof who, "player %d", tc - 1);
            logf("emact: %s #%d (%08X/%u) takes its leader's target: %s%s", name, i, unsigned(f.my_key), unsigned(f.my_id),
                 who, tc && !t ? " (no character here)" : "");
        }
    }
    bool stale = s.act < 0xFFFFFFFE && s.cls == class_hash(name) && emact_local_stale(em) && (s.act >> 24) != 3;
    if (s.act >= 0xFFFFFFFE || s.cls != class_hash(name) || (!stale && f.act_starts == int(s.act_starts) && f.act_seen == s.act))
        return;
    bool first = f.act_starts < 0;
    uint32_t running = game::at<uint32_t>(em, 0x2DD4);
    ActParams ap = {};
    bool have = false, wait = false;
    if (s.attached & 2) {
        EnterCriticalSection(&g_enemy_lock);
        ap = g_act_params[f.host_slot];
        have = g_act_params_tick[f.host_slot] && ap.starts == s.act_starts && ap.act == s.act;
        LeaveCriticalSection(&g_enemy_lock);
        if (!have && !f.act_wait)
            f.act_wait = now | 1;
        wait = !have && int(now - f.act_wait) < (first ? 1500 : 1000);
    }
    if (wait)
        return;
    if ((s.attached & 2) && !have)
        logf("net: enemy #%d %s: the parameters of its leader's action %08X did not come in 1 s, started without", i, name,
             unsigned(s.act));
    f.act_wait = 0;
    f.act_starts = s.act_starts;
    f.act_seen = s.act;
    if (strcmp(name, "uHumanEnemy") == 0 && emact_context_action(em, s.act)) {
        static LONG said = 0;
        if (InterlockedIncrement(&said) <= 20)
            logf("net: enemy #%d %s: its leader's action %08X needs more than its number here: not taken", i, name, unsigned(s.act));
        return;
    }
    if (have && (ap.has & 2)) {
        float ox = 0.f, oz = 0.f;
        if (ap.has & 4)
            enemies_world_origin(ox, oz);
        ap.dest[0] -= ox, ap.dest[2] -= oz;
        emact_set_dest(em, ap.dest);
        if (emact_logging())
            logf("emact: %s #%d (%08X/%u) takes its leader's jump point (%.0f, %.0f, %.0f) for action %08X%s", name, i,
                 unsigned(f.my_key), unsigned(f.my_id), ap.dest[0], ap.dest[1], ap.dest[2], unsigned(s.act),
                 (ap.has & 1) ? ", with its parameters" : "");
    }
    if (have && (ap.has & 8)) {
        uintptr_t victim = ap.grab >= 1 && ap.grab <= 4 ? slot_character(ap.grab - 1) : 0;
        bool done = victim && emact_apply_grab(em, victim, ap.grab_dti);
        logf("net: enemy #%d %s: its leader's grabbed %s as it started %08X: %s", i, name,
             ap.grab == 15 ? "someone who shows no player" : ap.grab == 1 ? "player 0" : ap.grab == 2 ? "player 1"
                           : ap.grab == 3 ? "player 2" : "player 3", unsigned(s.act),
             done ? "the same here" : victim ? "NOT done here (refused)" : "no such character here: not done");
    }
    if (!stale && emact_local_match(em, s.act)) {
        if (emact_logging())
            logf("emact: %s #%d (%08X/%u): its leader's reaction %08X to a blow already reacted to here: not started again", name, i,
                 unsigned(f.my_key), unsigned(f.my_id), unsigned(s.act));
    } else if (!first && emact_own_start(em, s.act, 1500)) {
        if (emact_logging())
            logf("emact: %s #%d (%08X/%u): its leader's %08X, already started here by its own game: not started again", name, i,
                 unsigned(f.my_key), unsigned(f.my_id), unsigned(s.act));
    } else if (have && (ap.has & 1)) {
        emact_request_ex(em, s.act, ap.p);
        ++g_enemy_stats.acted;
    } else if (have || !(first && running == s.act)) {
        emact_request(em, s.act);
        ++g_enemy_stats.acted;
    }
}

void enemy_net_before_action(int i, uintptr_t em)
{
    if (!g_enemy_ready || i < 0 || i >= kEnemySlots)
        return;
    EnterCriticalSection(&g_enemy_lock);
    Follow &f = g_follow[i];
    bool ok = f.ptr == em && f.host_slot >= 0 && f.paired_to == f.host_slot + 1 && g_enemy_tick[f.host_slot];
    EnemyState s = {};
    if (ok) {
        s = g_enemy_state[f.host_slot];
        ok = same_as_pair(s, f) || !f.by_id;
    }
    LeaveCriticalSection(&g_enemy_lock);
    if (ok)
        follower_actions(f, s, em, i, GetTickCount());
}

namespace {
struct HumanFix {
    DWORD key;
    int starts;
    uint32_t act;
    DWORD at;
    bool done;
};
HumanFix g_hc_fix[kEnemySlots] = {};

void human_fix_blow(uintptr_t ch, int i, int hs, DWORD now)
{
    EnterCriticalSection(&g_enemy_lock);
    EnemyState es = g_enemy_state[hs];
    DWORD tick = g_enemy_tick[hs];
    ActParams ap = g_act_params[hs];
    bool have_ap = g_act_params_tick[hs] != 0;
    DWORD key = g_follow[i].first_seen ^ (DWORD(hs) << 24);
    LeaveCriticalSection(&g_enemy_lock);
    if (!tick || es.act >= 0xFFFFFFFE)
        return;
    HumanFix &x = g_hc_fix[i];
    if (x.key != key) {
        x = {key, int(es.act_starts), es.act, now | 1, true};
        return;
    }
    if (int(es.act_starts) != x.starts || es.act != x.act) {
        x.starts = int(es.act_starts);
        x.act = es.act;
        x.at = now | 1;
        x.done = false;
    }
    if (x.done || int(now - x.at) < 150)
        return;
    x.done = true;
    const char *cls = emact_act_class(ch, x.act);
    bool dying = cls && strncmp(cls, "cPlAct", 6) == 0 && (strstr(cls, "Dead") || strstr(cls, "Die"));
    bool blow = cls && (strncmp(cls, "cPlActWpn", 9) == 0 || strncmp(cls, "cHmEm", 5) == 0 || strncmp(cls, "cNpcAct", 7) == 0 ||
                        strncmp(cls, "cEventAct", 9) == 0 || dying);
    if (!blow || emact_context_action(ch, x.act))
        return;
    DWORD since = 0;
    uint32_t run = emact_running(ch, &since);
    if (run == x.act && since && int(now - since) < 600)
        return;
    bool ex = have_ap && ap.act == x.act && ap.starts == uint8_t(x.starts) && (ap.has & 1);
    if (ex)
        emact_request_ex(ch, x.act, ap.p);
    else
        emact_request(ch, x.act);
    InterlockedIncrement(&g_hc_fixed);
    if (emact_logging())
        logf("emact: human #%d %p: its leader's #%d's %08X %s did not start here in 150 ms (running %08X %s): asked for%s", i,
             (void *)ch, hs, unsigned(x.act), cls, unsigned(run), emact_act_class(ch, run), ex ? " with its parameters" : "");
}
}

bool net_human_cmds_on()
{
    static int on = -1;
    if (on < 0) {
        char v[8] = "";
        on = emact_host_ai() && !(cfg_str("host_ai_human_cmds", v, sizeof v) && v[0] == '0') ? 1 : 0;
    }
    return on != 0;
}

void net_human_commands(uintptr_t ch)
{
    if (!g_enemy_ready || g_role == NET_OFF || !net_human_cmds_on() || net_guest_roaming())
        return;
    int lead = int(g_lead);
    if (g_shared && g_role == NET_GUEST && lead < 0)
        return;
    bool lead_self = g_role == NET_HOST || (g_shared && lead == net_my_slot());
    int i = enemies_slot_of(ch);
    if (i < 0 || i >= kEnemySlots)
        return;
    uintptr_t cmd = ch + game::kCmd;
    DWORD now = GetTickCount();
    if (lead_self) {
        uint32_t p = game::at<uint32_t>(cmd, 0x2C), r = game::at<uint32_t>(cmd, 0x34), held = game::at<uint32_t>(cmd, 0x30);
        DWORD since = 0;
        uint32_t act = p ? emact_running(ch, &since) : 0;
        int into = since ? int(now - since) : -1;
        EnterCriticalSection(&g_enemy_lock);
        HumanCmdState &s = g_hc_out[i];
        bool first = !s.tick || int(now - s.tick) > 3000;
        uint32_t was_held = 0;
        memcpy(&was_held, s.cmd + (0x30 - kHumanCmdFrom), 4);
        memcpy(s.cmd, reinterpret_cast<void *>(cmd + kHumanCmdFrom), kHumanCmdLen);
        s.aim = game::at<float>(ch, kAimPitch);
        s.press |= p;
        s.release |= r;
        s.tick = now | 1;
        if (p) {
            s.press_act = act;
            s.press_ms = into < 0 ? 0xFFFF : uint16_t(into > 0xFFFE ? 0xFFFE : into);
        }
        LeaveCriticalSection(&g_enemy_lock);
        if (p | r) {
            InterlockedExchange(&g_hc_now, 1);
            if (g_net_wake)
                SetEvent(g_net_wake);
        }
        if (first)
            logf("net: enemy #%d %s %p: its commands go to the followers (step 12f)", i, enemies_class_of(ch), (void *)ch);
        if (emact_logging() && (held != was_held || p)) {
            static volatile LONG lines = 0;
            if (InterlockedIncrement(&lines) <= 4000) {
                const float *st = reinterpret_cast<const float *>(cmd + 0x0C);
                logf("emact: human #%d %p buttons held %08X pressed %08X released %08X, sticks %.2f %.2f %.2f %.2f %.2f, "
                     "action %08X", i, (void *)ch, held, p, r, st[0], st[1], st[2], st[3], st[4], game::at<uint32_t>(ch, 0x2DD4));
            }
        }
        return;
    }
    EnterCriticalSection(&g_enemy_lock);
    Follow &f = g_follow[i];
    int hs = f.host_slot;
    bool ok = f.ptr == ch && hs >= 0 && hs < kEnemySlots && f.paired_to == hs + 1 && g_hc_in[hs].tick &&
              int(now - g_hc_in[hs].tick) < 500;
    HumanCmdState s = {};
    DWORD key = f.first_seen ^ (DWORD(hs) << 24);
    if (ok)
        s = g_hc_in[hs];
    LeaveCriticalSection(&g_enemy_lock);
    if (!ok)
        return;
    if (s.press || s.release) {
        EnterCriticalSection(&g_enemy_lock);
        g_hc_in[hs].press &= ~s.press;
        g_hc_in[hs].release &= ~s.release;
        LeaveCriticalSection(&g_enemy_lock);
    }
    memcpy(reinterpret_cast<void *>(cmd + kHumanCmdFrom), s.cmd, kHumanCmdLen);
    game::at<uint32_t>(cmd, 0x2C) = s.press;
    game::at<uint32_t>(cmd, 0x34) = s.release;
    human_fix_blow(ch, i, hs, now);
    if (s.aim == s.aim && std::fabs(s.aim) < 1.6f)
        game::at<float>(ch, kAimPitch) = s.aim;
    InterlockedIncrement(&g_hc_applied);
    if (s.press)
        InterlockedIncrement(&g_hc_presses);
    static DWORD said[kEnemySlots] = {};
    if (said[i] != key) {
        said[i] = key;
        logf("net: enemy #%d %s %p takes its leader's #%d's commands (its own AI's are written over, step 12f)", i,
             enemies_class_of(ch), (void *)ch, hs);
    }
    if (emact_logging() && s.press) {
        static volatile LONG lines = 0;
        if (InterlockedIncrement(&lines) <= 4000)
            logf("emact: human #%d %p gets its leader's #%d's presses %08X (held %08X), action %08X; %d ms after they came "
                 "(its leader pressed %u ms into %08X)", i, (void *)ch, hs, s.press,
                 *reinterpret_cast<const uint32_t *>(s.cmd + (0x30 - kHumanCmdFrom)), game::at<uint32_t>(ch, 0x2DD4),
                 s.press_rx ? int(now - s.press_rx) : -1, unsigned(s.press_ms), unsigned(s.press_act));
    }
}

uintptr_t enemy_net_after_finalize(EnemySlot &slot, int i, int *remove)
{
    uintptr_t pending_death = 0;
    if (!g_enemy_ready || g_role == NET_OFF || i < 0 || i >= kEnemySlots || net_guest_roaming())
        return 0;
    int lead = int(g_lead);
    if (g_shared && g_role == NET_GUEST && lead < 0)
        return 0;
    bool lead_self = g_role == NET_HOST || (g_shared && lead == net_my_slot());
    uintptr_t em = slot.ptr;
    DWORD now = GetTickCount();
    if (!slot.settled)
        return 0;
    if (lead_self) {
        LONG hide = g_test_hide;
        if (hide >= 0 && !slot.lot_part && lot_known(slot.lot_key, slot.lot_id) &&
            LONG((slot.lot_key >> 10) << 16 | (slot.lot_id & 0xFFFF)) == hide)
            return pending_death;
        EnemyState s = {};
        s.slot = uint8_t(i);
        s.cls = class_hash(slot.name);
        enemies_spawn_now(slot, s.spawn);
        memcpy(s.pos, reinterpret_cast<void *>(em + 0x40), sizeof s.pos);
        memcpy(s.rot, reinterpret_cast<void *>(em + 0x50), sizeof s.rot);
        s.yaw = game::at<float>(em, kYawA);
        if (float *hp = enemies_hp(em)) {
            s.hp = hp[0];
            s.hp_max = hp[1];
        }
        s.died = slot.died != 0;
        s.lot_key = slot.lot_key;
        s.lot_id = slot.lot_id;
        s.lot_split = slot.lot_split;
        s.lot_part = slot.lot_part;
        s.attached = slot.attached ? 1 : 0;
        s.act = 0xFFFFFFFF;
        if (slot.kind == 0 || (slot.kind == ENEMY_HUMAN && (emact_host_ai_humans() || net_human_cmds_on()))) {
            s.act = game::at<uint32_t>(em, 0x2DD4);
            s.act_starts = uint8_t(emact_starts(em));
            if (emact_host_ai())
                s.attached |= 4;
            ActParams ap = {};
            ap.slot = uint8_t(i), ap.starts = s.act_starts, ap.act = s.act;
            uintptr_t gv = 0;
            ap.has = uint8_t(emact_extras(em, ap.p, ap.dest, &gv, &ap.grab_dti));
            if (ap.has & 8) {
                ap.grab = 15;
                for (int k = 0; k < 4 && g_shared; ++k)
                    if (slot_character(k) == gv)
                        ap.grab = uint8_t(1 + k);
            }
            if (ap.has) {
                s.attached |= 2;
                float ox, oz;
                if ((ap.has & 2) && enemies_world_origin(ox, oz))
                    ap.dest[0] += ox, ap.dest[2] += oz, ap.has |= 4;
                EnterCriticalSection(&g_enemy_lock);
                if (!g_act_params_tick[i] || g_act_params[i].starts != ap.starts || g_act_params[i].act != ap.act) {
                    g_act_params[i] = ap;
                    g_act_params_tick[i] = now | 1;
                    InterlockedExchange(&g_act_params_now, 1);
                    if (ap.has & 8)
                        logf("net: enemy #%d %s grabs %s %p (%s) as it starts %08X: sent with the start", i, slot.name,
                             ap.grab == 15 ? "someone who shows no player" : ap.grab == 1 ? "player 0" : ap.grab == 2 ? "player 1"
                                           : ap.grab == 3 ? "player 2" : "player 3", (void *)gv, enemies_class_of(gv), unsigned(ap.act));
                }
                LeaveCriticalSection(&g_enemy_lock);
            }
            if (uintptr_t t = g_shared && slot.kind == 0 ? game::at<uintptr_t>(em, 0x2B98) : 0) {
                uint8_t code = 15;
                for (int k = 0; k < 4; ++k)
                    if (slot_character(k) == t)
                        code = uint8_t(1 + k);
                s.attached |= uint8_t(code << 4);
            }
            static DWORD aim_said[kEnemySlots] = {};
            if (emact_logging() && strncmp(slot.name, "uEm600", 6) == 0 && now - aim_said[i] > 2000) {
                aim_said[i] = now;
                const float *aim = reinterpret_cast<const float *>(em + 0x5BF0), *p = reinterpret_cast<const float *>(em + 0x40);
                uintptr_t t = game::at<uintptr_t>(em, 0x2B98);
                const float *tp = t ? reinterpret_cast<const float *>(t + 0x40) : aim;
                logf("emact: %s #%d at (%.0f, %.0f, %.0f): aim point (%.0f, %.0f, %.0f), target %p at (%.0f, %.0f, %.0f), move "
                     "flags %08X %08X, action %08X", slot.name, i, p[0], p[1], p[2], aim[0], aim[1], aim[2], (void *)t, tp[0],
                     tp[1], tp[2], game::at<uint32_t>(em, 0x2360), game::at<uint32_t>(em, 0x2364), unsigned(s.act));
            }
            static uint8_t said[kEnemySlots] = {};
            if (emact_logging() && said[i] != (s.attached >> 4)) {
                said[i] = s.attached >> 4;
                char who[40] = "nobody";
                if (said[i] == 15)
                    strcpy(who, "someone who shows no player");
                else if (said[i])
                    _snprintf(who, sizeof who, "player %d", said[i] - 1);
                logf("emact: %s #%d (%08X/%u) targets %s", slot.name, i, unsigned(slot.lot_key), unsigned(slot.lot_id), who);
            }
        }
        if (enemies_origin_settling())
            return pending_death;
        state_to_world(s);
        EnterCriticalSection(&g_enemy_lock);
        bool changed = emact_host_ai() && g_enemy_tick[i] &&
                       (g_enemy_state[i].act != s.act || g_enemy_state[i].act_starts != s.act_starts ||
                        (g_enemy_state[i].attached >> 4) != (s.attached >> 4));
        g_enemy_state[i] = s;
        g_enemy_tick[i] = now;
        if (changed)
            g_enemy_urgent[i] = 1;
        LeaveCriticalSection(&g_enemy_lock);
        if (changed && g_net_wake)
            SetEvent(g_net_wake);
        return pending_death;
    }
    int64_t tw = perf_now();
    EnterCriticalSection(&g_enemy_lock);
    perf_add(PERF_WAIT, tw);
    Follow &f = g_follow[i];
    float sp[3];
    enemies_spawn_now(slot, sp);
    if (f.first_seen != slot.first_seen) {
        f = {slot.first_seen, -1, now, em, 0, false, 0, 0};
        f.act_starts = -1;
        f.tgt_seen = -1;
        if (emact_host_ai() && (slot.kind == 0 || (slot.kind == ENEMY_HUMAN && emact_host_ai_humans())))
            emact_forget(em);
    }
    f.last = now;
    if (f.host_slot >= 0 && (!g_enemy_tick[f.host_slot] || now - g_enemy_tick[f.host_slot] > 3000)) {
        if (emact_host_ai() && (slot.kind == 0 || (slot.kind == ENEMY_HUMAN && emact_host_ai_humans())))
            logf("net: enemy #%d %s: nothing from its leader's #%d for %lu ms: unpaired", i, slot.name, f.host_slot,
                 g_enemy_tick[f.host_slot] ? now - g_enemy_tick[f.host_slot] : 0);
        f.host_slot = -1;
    }
    if (f.host_slot >= 0 && !f.by_id) {
        const float *hs = g_enemy_state[f.host_slot].spawn;
        if (std::fabs(hs[0] - f.host_spawn[0]) > 1.f || std::fabs(hs[2] - f.host_spawn[2]) > 1.f) {
            logf("net: enemy #%d %s: host #%d is another enemy now, pairing again", i, slot.name, f.host_slot);
            f.host_slot = -1;
        }
    }
    if (f.host_slot >= 0 && f.by_id) {
        const EnemyState &h = g_enemy_state[f.host_slot];
        if (!same_as_pair(slot, f) || !same_as_pair(h, f)) {
            logf("net: enemy #%d %s: spawn ID changed (here %08X/%u/%08X/%04X, host #%d %08X/%u/%08X/%04X, paired on "
                 "%08X/%u/%08X/%04X), pairing again", i, slot.name, unsigned(slot.lot_key), unsigned(slot.lot_id),
                 unsigned(slot.lot_split), unsigned(slot.lot_part), f.host_slot, unsigned(h.lot_key), unsigned(h.lot_id),
                 unsigned(h.lot_split), unsigned(h.lot_part), unsigned(f.pair_key), unsigned(f.pair_id),
                 unsigned(f.pair_split), unsigned(f.pair_part));
            f.host_slot = -1;
            f.by_id = false;
        }
    }
    if (f.host_slot < 0 && !slot.died && int(now - f.next_try) >= 0) {
        f.next_try = now + 500;
        f.by_id = false;
        uint32_t cls = class_hash(slot.name);
        ClassSet fam = class_family(slot.name);
        uint8_t claimed[kEnemySlots] = {};
        int claimer[kEnemySlots];
        for (int k = 0; k < kEnemySlots; ++k)
            if (k != i && g_follow[k].host_slot >= 0 && now - g_follow[k].last < 1000) {
                claimed[g_follow[k].host_slot] = g_follow[k].loose ? 2 : 1;
                claimer[g_follow[k].host_slot] = k;
            }
        bool known = lot_known(slot.lot_key, slot.lot_id);
        if (known)
            for (int k = 0; k < kEnemySlots && f.host_slot < 0; ++k) {
                const EnemyState &h = g_enemy_state[k];
                if (g_enemy_tick[k] && now - g_enemy_tick[k] <= 3000 && claimed[k] != 1 && same_spawn_id(h, slot))
                    f.host_slot = k;
            }
        bool twin_known = false;
        for (int k = 0; k < kEnemySlots && !twin_known; ++k) {
            const EnemyState &h = g_enemy_state[k];
            float dx = h.spawn[0] - sp[0], dy = h.spawn[1] - sp[1], dz = h.spawn[2] - sp[2];
            twin_known = g_enemy_tick[k] && now - g_enemy_tick[k] < 30000 && claimed[k] != 1 && !h.died &&
                         fam.has(h.cls) && dx * dx + dy * dy + dz * dz < 40.f * 40.f;
        }
        float here[3];
        memcpy(here, reinterpret_cast<void *>(em + 0x40), sizeof here);
        const char *how = "spawns";
        float best = 40.f * 40.f;
        if (f.host_slot >= 0) {
            const float *hs = g_enemy_state[f.host_slot].spawn;
            float dx = hs[0] - sp[0], dy = hs[1] - sp[1], dz = hs[2] - sp[2];
            best = dx * dx + dy * dy + dz * dz;
            how = "ID";
            f.by_id = true;
            f.pair_key = slot.lot_key;
            f.pair_id = slot.lot_id;
            f.pair_split = slot.lot_split;
            f.pair_part = slot.lot_part;
        }
        for (int pass = 0; pass < 3 && f.host_slot < 0 && !known; ++pass) {
            if (pass == 2) {
                if (slot.kind == ENEMY_NPC)
                    break;
                if (now - slot.first_seen < 6000)
                    break;
                if (twin_known)
                    break;
                best = g_shared ? 400.f * 400.f : 1500.f * 1500.f;
                how = "kind, closest";
            }
            const float *mine = pass == 0 ? sp : here;
            for (int k = 0; k < kEnemySlots; ++k) {
                const EnemyState &h = g_enemy_state[k];
                if (!g_enemy_tick[k] || now - g_enemy_tick[k] > 3000 || (pass < 2 ? h.cls != cls : !fam.has(h.cls)))
                    continue;
                if (claimed[k] == 1 || (claimed[k] == 2 && pass == 2))
                    continue;
                if (lot_known(h.lot_key, h.lot_id))
                    continue;
                const float *theirs = pass == 0 ? h.spawn : h.pos;
                float dx = theirs[0] - mine[0], dy = theirs[1] - mine[1], dz = theirs[2] - mine[2];
                float d2 = dx * dx + dy * dy + dz * dz;
                if (d2 < best) {
                    best = d2;
                    f.host_slot = k;
                }
            }
            if (pass == 0 && f.host_slot < 0)
                how = "positions";
        }
        if (f.host_slot >= 0) {
            memcpy(f.host_spawn, g_enemy_state[f.host_slot].spawn, sizeof f.host_spawn);
            f.loose = strcmp(how, "kind, closest") == 0;
            if (claimed[f.host_slot] == 2) {
                Follow &other = g_follow[claimer[f.host_slot]];
                logf("net: enemy #%d %s takes host #%d from enemy #%d (paired by kind only)", i, slot.name, f.host_slot,
                     claimer[f.host_slot]);
                other.host_slot = -1;
                other.loose = false;
                other.next_try = now;
            }
            ++g_enemy_stats.matched_new;
            logf("net: enemy #%d %s paired with host #%d (%s %.1f u apart)", i, slot.name, f.host_slot, how,
                 std::sqrt(best));
        } else if (known && now - slot.first_seen > 15000 && (now / 500) % 20 == 0) {
            logf("net: enemy #%d %s unpaired: no host enemy with its spawn ID (key %08X id %u split %08X part %04X)", i,
                 slot.name, unsigned(slot.lot_key), unsigned(slot.lot_id), unsigned(slot.lot_split), unsigned(slot.lot_part));
        } else if (now - slot.first_seen > 15000 && (now / 500) % 20 == 0) {
            int near_k = -1;
            float near_d2 = 1e30f;
            for (int k = 0; k < kEnemySlots; ++k) {
                const EnemyState &h = g_enemy_state[k];
                if (!g_enemy_tick[k] || !fam.has(h.cls))
                    continue;
                float dx = h.spawn[0] - sp[0], dy = h.spawn[1] - sp[1], dz = h.spawn[2] - sp[2];
                if (dx * dx + dy * dy + dz * dz < near_d2) {
                    near_d2 = dx * dx + dy * dy + dz * dz;
                    near_k = k;
                }
            }
            if (near_k >= 0)
                logf("net: enemy #%d %s unpaired: closest host #%d spawned %.1f u away, last received %lu ms ago%s", i,
                     slot.name, near_k, std::sqrt(near_d2), now - g_enemy_tick[near_k], claimed[near_k] ? ", taken" : "");
            else
                logf("net: enemy #%d %s unpaired: no host enemy of its class", i, slot.name);
        }
    }
    if (f.host_slot < 0) {
        ++g_enemy_stats.unmatched;
        if (emact_host_ai() && (slot.kind == 0 || (slot.kind == ENEMY_HUMAN && emact_host_ai_humans()))) {
            int ln = 0, lage = -1;
            int has = lot_known(slot.lot_key, slot.lot_id) ? lead_entries_has(slot.lot_key, slot.lot_id, slot.lot_split, &ln, &lage) : 0;
            bool listed = has == 1 || (has < 0 && leadlist_any([&](const LeadUnit &u) {
                                           return u.key == slot.lot_key && u.id == slot.lot_id && u.split == slot.lot_split;
                                       }));
            bool hold = f.drove && !slot.died && lot_known(slot.lot_key, slot.lot_id) && listed;
            if (hold) {
                emact_drive(em, true, -1);
                if (!f.held) {
                    f.held = now | 1;
                    const char *run = emact_running_class(em);
                    bool waits = strstr(run, "Wait") != nullptr || emact_request_if(em, 0x01000003, "cEmActWait");
                    logf("net: enemy #%d %s (%08X/%u): its leader's unit is not sent any more but the leader still has it: "
                         "held, its own AI stays off (was running %s%s)", i, slot.name, unsigned(slot.lot_key),
                         unsigned(slot.lot_id), run, waits ? "" : "; no cEmActWait in its banks: it runs on");
                }
                f.act_starts = -1;
            } else {
                if (f.held || f.drove)
                    logf("net: enemy #%d %s (%08X/%u/%08X): no leader's unit (%s; the leader's list: %s, %d units, %d ms old): its "
                         "own AI again", i, slot.name, unsigned(slot.lot_key), unsigned(slot.lot_id), unsigned(slot.lot_split),
                         slot.died ? "dead here" : f.held ? "the leader no longer has it" : "none sent, none in the leader's list",
                         has > 0 ? "has it" : has == 0 ? "lacks it" : "none fresh", ln, lage);
                emact_drive(em, false, -1);
                f.act_starts = -1;
                f.held = 0;
                f.drove = false;
            }
        }
        bool host_sending = g_last_enemies_rx && now - g_last_enemies_rx < 1000;
        bool known = lot_known(slot.lot_key, slot.lot_id), placed = known && !slot.lot_part;
        bool everywhere = placed && leadlist_on();
        if (g_shared && !everywhere) {
            float lx = sp[0] - g_lead_pos[0], lz = sp[2] - g_lead_pos[2];
            host_sending &= lx * lx + lz * lz < kRemoveRange * kRemoveRange;
        }
        bool host_knows = false;
        ClassSet fam = class_family(slot.name);
        DWORD knows_ms = everywhere ? 10000 : 30000;
        for (int k = 0; k < kEnemySlots && !host_knows; ++k) {
            const EnemyState &h = g_enemy_state[k];
            if (known) {
                host_knows = g_enemy_tick[k] && now - g_enemy_tick[k] < knows_ms && same_spawn_id(h, slot);
                continue;
            }
            float dx = h.spawn[0] - sp[0], dy = h.spawn[1] - sp[1], dz = h.spawn[2] - sp[2];
            host_knows = g_enemy_tick[k] && now - g_enemy_tick[k] < 30000 && fam.has(h.cls) &&
                         dx * dx + dy * dy + dz * dz < 40.f * 40.f;
        }
        if (!host_sending || host_knows || f.held || slot.died || f.forced || slot.kind == ENEMY_NPC || slot.lot_part) {
            f.unpaired_since = 0;
        } else if (!f.unpaired_since) {
            f.unpaired_since = now;
        } else if (int(now - f.unpaired_since) > (everywhere ? 5000 : 10000)) {
            f.forced = true;
            ++g_enemy_stats.removed;
            pending_death = em;
            if (remove)
                *remove = everywhere ? REMOVE_ONLY : REMOVE_DEATH;
            float lx = sp[0] - g_lead_pos[0], lz = sp[2] - g_lead_pos[2];
            logf("net: enemy #%d %s at (%.0f, %.0f, %.0f), %.0f u from the leader: not on the leader for %d s, removed (%s)", i,
                 slot.name, sp[0], sp[1], sp[2], std::sqrt(lx * lx + lz * lz), everywhere ? 5 : 10,
                 everywhere ? "deleted; its entry follows the leader's list" : "death, then deleted");
        }
        LeaveCriticalSection(&g_enemy_lock);
        return pending_death;
    }
    f.unpaired_since = 0;
    if (g_shared && (game::at<uintptr_t>(em, 0x2104) || carried_here(em))) {
        static uintptr_t said[kEnemySlots] = {};
        uintptr_t by = game::at<uintptr_t>(em, 0x2104);
        if (said[i] != (by | (carried_here(em) ? 1 : 0))) {
            said[i] = by | (carried_here(em) ? 1 : 0);
            logf("net: enemy #%d %s is held here (+0x2104 %p %s%s): its leader's state is not applied meanwhile", i, slot.name,
                 (void *)by, by ? enemies_class_of(by) : "-", carried_here(em) ? ", carried by another player's pawn" : "");
        }
        LeaveCriticalSection(&g_enemy_lock);
        return pending_death;
    }
    if (slot.kind == ENEMY_NPC) {
        uintptr_t gm = game::at<uintptr_t>(game::kGameMain, 0);
        if (!gm || game::at<int32_t>(gm, game::kGsEvent) != 256) {
            LeaveCriticalSection(&g_enemy_lock);
            return pending_death;
        }
    }
    if (enemies_origin_settling()) {
        LeaveCriticalSection(&g_enemy_lock);
        return pending_death;
    }
    EnemyState s = g_enemy_state[f.host_slot];
    bool lead_ai = (s.attached & 4) != 0;
    float *pos = reinterpret_cast<float *>(em + 0x40);
    ++g_enemy_stats.applied;
    if (slot.attached) {
        f.paired_to = f.host_slot + 1;
        LeaveCriticalSection(&g_enemy_lock);
    } else {
        float dx = s.pos[0] - pos[0], dy = s.pos[1] - pos[1], dz = s.pos[2] - pos[2];
        float err = std::sqrt(dx * dx + dy * dy + dz * dz);
        bool first = f.paired_to != f.host_slot + 1;
        f.paired_to = f.host_slot + 1;
        if (!first && err > g_enemy_stats.worst_err)
            g_enemy_stats.worst_err = err;
        LeaveCriticalSection(&g_enemy_lock);
        if (!first && err > 300.f) {
            static volatile LONG said = 0;
            if (int(now - DWORD(said)) > 500) {
                InterlockedExchange(&said, LONG(now));
                logf("net: enemy #%d %s (%08X/%u) is %.0f u from its leader's #%d: here (%.0f, %.0f, %.0f), there (%.0f, "
                     "%.0f, %.0f), state %lu ms old, action here %08X, there %08X",
                     i, slot.name, unsigned(slot.lot_key), unsigned(slot.lot_id), err, f.host_slot, pos[0], pos[1], pos[2],
                     s.pos[0], s.pos[1], s.pos[2], now - g_enemy_tick[f.host_slot], game::at<uint32_t>(em, 0x2DD4),
                     unsigned(s.act));
                if (emact_logging() && slot.kind == 0) {
                    const float *aim = reinterpret_cast<const float *>(em + 0x5BF0);
                    uintptr_t t = game::at<uintptr_t>(em, 0x2B98);
                    const float *tp = t ? reinterpret_cast<const float *>(t + 0x40) : aim;
                    logf("emact:   its aim point (+0x5BF0) (%.0f, %.0f, %.0f), target %p at (%.0f, %.0f, %.0f), move flags "
                         "%08X %08X", aim[0], aim[1], aim[2], (void *)t, tp[0], tp[1], tp[2], game::at<uint32_t>(em, 0x2360),
                         game::at<uint32_t>(em, 0x2364));
                    if (std::fabs(dy) > 200.f && mem_readable(reinterpret_cast<void *>(em), 0x7400)) {
                        char where[260] = "";
                        int n = 0, found = 0;
                        for (uintptr_t off = 0x30; off < 0x7400 && found < 24; off += 4) {
                            float v = game::at<float>(em, off);
                            if (std::fabs(v - pos[1]) < 1.f && std::fabs(v) > 50.f) {
                                n += _snprintf(where + n, sizeof where - n - 1, " +0x%X", unsigned(off));
                                ++found;
                            }
                        }
                        logf("emact:   its height %.1f is also at:%s", pos[1], where);
                    }
                }
            }
        }
        if (err > 50.f) {
            static const uintptr_t kPlaceCopies[] = {0xA0, 0xE0, 0x1AD0, 0x1AE0, 0x1AF0, 0x1B00, 0x29C0, 0x5DB0};
            bool off_origin = std::fabs(pos[0]) + std::fabs(pos[1]) + std::fabs(pos[2]) > 10.f;
            if (err > 200.f && !first && off_origin && emact_host_ai() && lead_ai && slot.kind == 0 &&
                mem_readable(reinterpret_cast<void *>(em), 0x6000)) {
                char where[160] = "";
                int n = 0;
                for (uintptr_t off : kPlaceCopies) {
                    float *v = reinterpret_cast<float *>(em + off);
                    if (std::fabs(v[0] - pos[0]) < 1.f && std::fabs(v[1] - pos[1]) < 1.f && std::fabs(v[2] - pos[2]) < 1.f) {
                        memcpy(v, s.pos, 12);
                        if (n < int(sizeof where) - 12)
                            n += _snprintf(where + n, sizeof where - n - 1, " +0x%X", unsigned(off));
                    }
                }
                static volatile LONG said = 0;
                if (emact_logging() && int(now - DWORD(said)) > 500) {
                    InterlockedExchange(&said, LONG(now));
                    logf("net: enemy #%d %s was %.0f u off: its place also set at%s", i, slot.name, err, where);
                }
            }
            memcpy(pos, s.pos, sizeof s.pos);
            for (uintptr_t off : kPosCopies)
                memcpy(reinterpret_cast<void *>(em + off), s.pos, 12);
        } else {
            pos[0] += dx * 0.3f;
            pos[1] += dy * 0.3f;
            pos[2] += dz * 0.3f;
        }
        game::at<float>(em, kYawA) = s.yaw;
        game::at<float>(em, kYawB) = s.yaw;
        game::at<float>(em, kYawC) = s.yaw;
        memcpy(reinterpret_cast<void *>(em + 0x50), s.rot, sizeof s.rot);
    }
    bool driven = emact_host_ai() && (slot.kind == 0 || (slot.kind == ENEMY_HUMAN && emact_host_ai_humans()));
    if (driven && !lead_ai) {
        static volatile LONG said = 0;
        if (!InterlockedExchange(&said, 1))
            logf("net: enemy #%d %s: its leader's game does not decide its units' actions (host_ai off there, or a GOG "
                 "game): they keep their own AI here (said once)", i, slot.name);
        if (f.drove || f.held) {
            emact_drive(em, false, -1);
            f.drove = false;
            f.held = 0;
            f.act_starts = -1;
        }
    }
    if (driven && lead_ai) {
        f.my_key = slot.lot_key, f.my_id = slot.lot_id, f.my_died = slot.died != 0;
        if (f.held) {
            logf("net: enemy #%d %s: its leader's unit is sent again after %lu ms held", i, slot.name, now - f.held);
            f.held = 0;
        }
        f.drove = true;
        emact_drive(em, true, i);
        follower_actions(f, s, em, i, now);
    }
    if (slot.kind == ENEMY_NPC)
        return pending_death;
    float *hp = s.hp_max > 0.f ? enemies_hp(em) : nullptr;
    bool local_dead = slot.died != 0;
    bool quick_death = lead_ai && emact_host_ai() && (slot.kind == 0 || (slot.kind == ENEMY_HUMAN && net_human_cmds_on()));
    if (!s.died) {
        if (hp && s.hp > 0.f)
            hp[0] = s.hp;
        return pending_death;
    }
    EnterCriticalSection(&g_enemy_lock);
    if (local_dead) {
        f.dead_since = 0;
        if (f.force_tries > 1)
            logf("net: enemy #%d %s: dead here after %d forced deaths", i, slot.name, f.force_tries);
        f.force_tries = 0;
    } else if (f.forced) {
        f.dead_since = 0;
        if (f.force_tries > 0 && f.force_tries < 20 && int(now - f.force_at) >= 1000 && death_retry_ok(em)) {
            ++f.force_tries;
            f.force_at = now;
            if (hp)
                hp[0] = 0.f;
            pending_death = em;
            if (f.force_tries == 2 || f.force_tries == 20)
                logf("net: enemy #%d %s: still alive after its forced death, forced again (try %d)", i, slot.name, f.force_tries);
        }
    } else if (!f.dead_since) {
        f.dead_since = now;
    } else if (now - f.dead_since > (quick_death ? 100u : 1500u)) {
        f.forced = true;
        f.dead_since = 0;
        f.force_tries = 1;
        f.force_at = now;
        ++g_enemy_stats.forced;
        if (hp)
            hp[0] = 0.f;
        pending_death = em;
        logf("net: enemy #%d %s: dead on the host, no local killing blow in %s, death forced", i, slot.name,
             quick_death ? "100 ms" : "1.5 s");
    }
    LeaveCriticalSection(&g_enemy_lock);
    return pending_death;
}

const char *const kStandInDeaths[] = {
    "uEm5500TentacleBase", "uEm5500_00", "uEm5500_01", "uEm5500C_00", "uEm5502_00", "uEm5502_01",
    "uEm0700", "uEm0701", "uEm0702", "uEm0703",
    "uEm5200", "uEm5201", "uEm5200_00", "uEm5201_00", "uEm5200_01", "uEm5201_01",
    "uEm5500", "uEm5500B", "uEm5500C", "uEm5502", "uEm5501",
    "uEm5000", "uEm5001",
    "uEm5100", "uEm5101",
};
static bool stand_in_death(const char *cls)
{
    for (const char *n : kStandInDeaths)
        if (strcmp(cls, n) == 0)
            return true;
    return false;
}
volatile LONG g_test_death_skip = 0;
void net_test_death_skip(int n)
{
    InterlockedExchange(&g_test_death_skip, n > 0 ? n : 0);
    logf("net: the next %d forced death(s) here are skipped, as a refusing handler would (tests)", n > 0 ? n : 0);
}
void enemy_net_death(uintptr_t em)
{
    if (g_test_death_skip > 0 && InterlockedDecrement(&g_test_death_skip) >= 0) {
        logf("net: death of %s %p skipped (tests: deathskip)", enemies_class_of(em), (void *)em);
        return;
    }
    if (enemies_npc_kind(em)) {
        if (float *hp = enemies_hp(em))
            enemies_apply_damage(em, hp[0] + 1.f);
        return;
    }
    using Death_t = void(__thiscall *)(uintptr_t, uint32_t, uint32_t);
    uintptr_t fn = game::at<uintptr_t>(game::at<uintptr_t>(em, 0), 0x120);
    uint32_t hit = 0;
    const char *cls = enemies_class_of(em);
    if (stand_in_death(cls)) {
        alignas(16) static thread_local uint8_t stand_in[0x400];
        memset(stand_in, 0, sizeof stand_in);
        hit = uint32_t(reinterpret_cast<uintptr_t>(stand_in));
        static volatile LONG said = 0;
        if (InterlockedIncrement(&said) <= 8)
            logf("net: death of %s %p with a stand-in hit (its death handler %08X needs one)", cls, (void *)em, unsigned(fn));
    }
    reinterpret_cast<Death_t>(fn)(em, 0, hit);
}

bool death_retry_ok(uintptr_t em)
{
    static const char *const kDragons[] = {"uEmDragonBase", "uEmZakoDragonBase", "uEm5800", "uEm5801", "uEm5802", "uEm5900",
                                           "uEm5901", "uEm5902", "uEm5903", "uEm5904", "uEm5905", "uEm5906"};
    const char *cls = em ? enemies_class_of(em) : "?";
    for (const char *n : kDragons)
        if (strcmp(cls, n) == 0)
            return false;
    return true;
}

int enemy_net_follow(int local_slot)
{
    if (g_role != NET_GUEST || !g_enemy_ready || local_slot < 0 || local_slot >= kEnemySlots)
        return -1;
    EnterCriticalSection(&g_enemy_lock);
    int k = g_follow[local_slot].host_slot;
    LeaveCriticalSection(&g_enemy_lock);
    return k;
}

bool enemy_net_block_damage(uintptr_t ch)
{
    if (g_shared && g_role != NET_OFF)
        for (int p = 0; p < kPawns; ++p)
            if (ch == possess_pawn_ptr(p) && net_puppet(1 + p))
                return true;
    if (g_role != NET_GUEST || !g_enemy_ready || net_guest_roaming())
        return false;
    bool block = false;
    EnterCriticalSection(&g_enemy_lock);
    bool party = !g_shared && ch == possess_arisen();
    for (int p = 0; p < kPawns; ++p)
        party |= ch == possess_pawn_ptr(p) && (!g_shared || net_puppet(1 + p));
    if (party) {
        ++g_enemy_stats.puppet_blocked;
        LeaveCriticalSection(&g_enemy_lock);
        return true;
    }
    for (int i = 0; i < kEnemySlots; ++i) {
        const Follow &f = g_follow[i];
        if (f.ptr != ch || (f.host_slot < 0 && !f.held) || GetTickCount() - f.last > 1000)
            continue;
        block = (f.held || !g_enemy_state[f.host_slot].died) && !(g_shared && g_lead == net_my_slot());
        ++(block ? g_enemy_stats.blocked : g_enemy_stats.allowed);
        break;
    }
    LeaveCriticalSection(&g_enemy_lock);
    return block;
}

bool enemy_net_feel_hit(uintptr_t ch)
{
    static int off = -1;
    if (off < 0)
        off = early_flag("no_hit_feel") ? 1 : 0;
    if (off || g_role != NET_GUEST || !g_enemy_ready)
        return false;
    for (int p = 0; p < kPawns; ++p)
        if (ch == possess_pawn_ptr(p))
            return false;
    if (ch == possess_arisen())
        return false;
    bool enemy = false;
    EnterCriticalSection(&g_enemy_lock);
    for (int i = 0; i < kEnemySlots && !enemy; ++i)
        enemy = g_follow[i].ptr == ch && (g_follow[i].host_slot >= 0 || g_follow[i].held);
    if (enemy)
        ++g_enemy_stats.felt;
    LeaveCriticalSection(&g_enemy_lock);
    return enemy;
}

int net_guest_my_pawn()
{
    if (g_role != NET_GUEST || !g_have_snap)
        return -1;
    EnterCriticalSection(&g_snap_lock);
    int p = g_snap.my_pawn;
    LeaveCriticalSection(&g_snap_lock);
    return p;
}

bool net_shared() { return g_shared; }

bool net_hidden_pawn_ptr(uintptr_t ch)
{
    if (!g_shared || g_role == NET_OFF || !ch)
        return false;
    for (int p = 0; p < kPawns; ++p)
        if (ch == possess_pawn_ptr(p))
            return g_rp_hidden[1 + p];
    return false;
}

bool net_puppet_ptr(uintptr_t ch)
{
    if (!g_shared || g_role == NET_OFF || !ch)
        return false;
    for (int p = 0; p < kPawns; ++p)
        if (ch == possess_pawn_ptr(p) && net_puppet(1 + p))
            return true;
    return false;
}

bool net_puppet_in_weapon_action(uintptr_t ch)
{
    if (!g_shared || g_role == NET_OFF || !ch)
        return false;
    for (int p = 0; p < kPawns; ++p)
        if (ch == possess_pawn_ptr(p) && net_puppet(1 + p)) {
            EnterCriticalSection(&g_cap_lock);
            unsigned table = unsigned(g_rp[1 + p].action) >> 24;
            bool valid = g_rp[1 + p].valid;
            LeaveCriticalSection(&g_cap_lock);
            return valid && table >= 1 && table <= 0x0F;
        }
    return false;
}

bool net_enemy_follower()
{
    int lead = int(g_lead);
    return g_shared && g_role == NET_GUEST && lead >= 0 && lead != net_my_slot();
}

bool net_puppet(int i)
{
    if (!g_shared || i < 1 || i >= kParty)
        return false;
    if (party_to_slot(i) >= 0)
        return true;
    EnterCriticalSection(&g_cap_lock);
    bool on = (g_rp[i].valid || g_rp_away[i] || g_rp_hidden[i]) && g_rp_tick[i] && int(GetTickCount() - g_rp_tick[i]) < 2000;
    LeaveCriticalSection(&g_cap_lock);
    return on;
}

int net_party_slot(int i) { return g_shared ? party_to_slot(i) : -1; }
uintptr_t net_slot_character(int slot) { return g_shared ? slot_character(slot) : 0; }
bool net_me_held_by_player() { return g_shared && g_me_held_at && int(GetTickCount() - g_me_held_at) < 400; }
bool net_fall_from_hold() { return g_shared && g_hold_fall != 0; }

int net_my_slot()
{
    if (g_role == NET_HOST)
        return 0;
    if (g_role != NET_GUEST || !g_have_snap)
        return -1;
    EnterCriticalSection(&g_snap_lock);
    int s = g_snap.my_slot;
    LeaveCriticalSection(&g_snap_lock);
    return s;
}

int32_t net_my_stage() { return my_stage(); }

bool net_player_info(int slot, PlayerInfo &out)
{
    out = PlayerInfo{};
    if (slot < 0 || slot >= 4 || !g_shared)
        return false;
    if (slot == net_my_slot()) {
        CharState c = {};
        read_state(0, c);
        out.present = true;
        out.downed = revive_down();
        out.stage = my_stage();
        memcpy(out.pos, c.pos, sizeof out.pos);
        return true;
    }
    if (g_role == NET_HOST) {
        const Peer &p = g_peers[slot - 1];
        if (slot < 1 || !p.used || !p.pstate_tick || !p.last_rx || int(GetTickCount() - p.last_rx) > 10000)
            return false;
        out.present = true;
        out.downed = p.downed;
        out.stage = p.pstage;
        memcpy(out.pos, p.pstate.pos, sizeof out.pos);
        return true;
    }
    if (!g_have_snap)
        return false;
    static PlayerInfo last[4];
    static DWORD last_at[4] = {};
    EnterCriticalSection(&g_snap_lock);
    int i = slot == 0 ? 0 : (g_snap.pawn_of[slot] >= 0 ? 1 + g_snap.pawn_of[slot] : -1);
    bool ok = i >= 0 && i < kParty && g_snap.ch[i].valid;
    if (ok) {
        out.present = true;
        out.downed = g_snap.down[slot] != 0;
        out.stage = g_snap.stage_of[i];
        memcpy(out.pos, g_snap.ch[i].pos, sizeof out.pos);
        last[slot] = out;
        last_at[slot] = GetTickCount() | 1;
    } else if (last_at[slot] && (slot == 0 || g_snap.pawn_of[slot] >= 0)) {
        out = last[slot];
        out.downed = g_snap.down[slot] != 0;
        ok = true;
    }
    if (!ok) {
        static DWORD next = 0;
        if (!next || int(GetTickCount() - next) >= 0) {
            next = GetTickCount() + 2000;
            logf("net: player %d unknown: entry %d, valid %d, pawn_of %d %d %d %d, my_slot %d", slot, i,
                 i >= 0 && i < kParty ? g_snap.ch[i].valid : -1, g_snap.pawn_of[0], g_snap.pawn_of[1], g_snap.pawn_of[2],
                 g_snap.pawn_of[3], g_snap.my_slot);
        }
    }
    LeaveCriticalSection(&g_snap_lock);
    return ok;
}

void net_send_event(int32_t event)
{
    if (!g_shared || g_role == NET_OFF)
        return;
    static uint16_t next_id = uint16_t(GetTickCount());
    EventMsg m = {{kMagic, MSG_EVENT, 0}, uint8_t(net_my_slot() < 0 ? 0 : net_my_slot()), 0, ++next_id, event, my_stage()};
    for (int r = 0; r < 3; ++r) {
        m.h.seq = ++g_seq;
        if (g_role == NET_GUEST)
            send_raw(&m, sizeof m);
        else
            for (int q = 0; q < kMaxPeers; ++q)
                if (g_peers[q].used)
                    send_to(g_peers[q], &m, sizeof m);
    }
}

void net_send_revive(int slot)
{
    ReviveMsg m = {{kMagic, MSG_REVIVE, ++g_seq}, uint8_t(slot)};
    if (g_role == NET_GUEST)
        send_raw(&m, sizeof m);
    else if (slot >= 1 && slot <= kMaxPeers && g_peers[slot - 1].used)
        send_to(g_peers[slot - 1], &m, sizeof m);
}

bool net_stage_jump(int32_t to, int32_t point)
{
    uintptr_t st = current_stage_area();
    if (!st || !plain_stage(to) || game::at<int32_t>(st, 0x808) != 1)
        return false;
    game::at<int32_t>(st, 0x80C) = to;
    game::at<int32_t>(st, 0x810) = point;
    game::at<uint8_t>(st, 0x814) = 0;
    game::at<int32_t>(st, 0x808) = 2;
    game::at<int32_t>(st, 4) = 0x13;
    logf("net: stage: jump to %d (point %d) to come back next to a player", to, point);
    return true;
}

void net_quest_publish(const std::vector<uint8_t> &blob, uint32_t crc)
{
    int me = g_role == NET_HOST ? 0 : net_my_slot();
    if (me < 0 || me >= 4)
        return;
    EnterCriticalSection(&g_qb_lock);
    g_qb[me].data = blob;
    g_qb[me].crc = crc;
    g_qb[me].since = GetTickCount() | 1;
    g_qb[me].next = 0;
    LeaveCriticalSection(&g_qb_lock);
}

bool net_old_peer() { return g_old_peer != 0; }

bool net_group_nearest(const float pos[3], const float own[3], float out[3], int *slot)
{
    return group_nearest(pos, own, out, slot);
}
void net_test_group_slots(bool on) { group_slots_set(on); }
int net_group_positions(float out[4][3]) { return group_positions(out); }
uint32_t net_group_party_bits() { return group_party_bits(); }

bool net_following() { return leadlist_on(); }
bool net_leader_has(uint32_t key, uint32_t id, uint32_t split)
{
    return leadlist_any([&](const LeadUnit &u) { return u.key == key && u.id == id && u.split == split; });
}
bool net_leader_has_set(uint32_t key, uint32_t split)
{
    return leadlist_any([&](const LeadUnit &u) { return u.key == key && u.split == split; });
}
int net_leader_list(LeadUnit *out, int cap)
{
    AcquireSRWLockShared(&g_leadlist_lock);
    int n = 0;
    if (g_leadlist_on && g_leadlist_tick && int(GetTickCount() - g_leadlist_tick) < 1000)
        for (; n < g_leadlist_n && n < cap; ++n)
            out[n] = g_leadlist[n];
    ReleaseSRWLockShared(&g_leadlist_lock);
    return n;
}
void net_test_follow_list(bool on)
{
    InterlockedExchange(&g_follow_list, on ? 1 : 0);
    logf("net: follow list %s (tests)", on ? "on" : "off: this game's spawns go by its own Arisen, as in step 10b");
}
bool net_leading() { return g_role != NET_OFF && g_enemy_ready && lead_self(); }
LONG g_test_keep_or_hide() { return g_test_hide_keep ? -1 : g_test_hide; }
void net_lead_entries_publish(const LeadUnit *u, int n)
{
    if (n > kLeadEntriesMax)
        n = kLeadEntriesMax;
    int32_t stage = my_stage();
    LONG hide = g_test_keep_or_hide();
    AcquireSRWLockExclusive(&g_entries_lock);
    int m = 0;
    bool kept = false;
    for (int k = 0; k < n; ++k) {
        if (hide < 0 || LONG((u[k].key >> 10) << 16 | (u[k].id & 0xFFFF)) != hide)
            g_my_entries[m++] = u[k];
        kept |= g_test_hide_keep && LONG((u[k].key >> 10) << 16 | (u[k].id & 0xFFFF)) == g_test_hide;
    }
    static DWORD said = 0;
    if (g_test_hide_keep && g_test_hide >= 0 && int(GetTickCount() - said) > 2000) {
        said = GetTickCount();
        logf("net: ehide keep: the unit is %sin this game's list of units (%d units)", kept ? "" : "NOT ", n);
    }
    n = m;
    g_my_entries_n = n;
    g_my_entries_stage = stage;
    g_my_entries_tick = GetTickCount() | 1;
    ReleaseSRWLockExclusive(&g_entries_lock);
}
void net_gate_stats(LONG *refused, LONG *passed)
{
    *refused = InterlockedExchange(&g_gate_refused, 0);
    *passed = InterlockedExchange(&g_gate_passed, 0);
}
