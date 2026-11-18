#include <windows.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "game.h"
#include "mod.h"

bool save_machine_request(uint8_t mode, uint8_t slot);
bool save_machine_idle();
bool rejoin_world_quiet();
void rejoin_reload_world();

namespace {

enum Step { IDLE, SNAP_SEND, SNAP_APPLY, LOADING, RELOADING };
const char *kName[] = {"idle", "snapshot to send", "snapshot to update", "loading", "reloading"};
Step g_step = IDLE;
DWORD g_step_at = 0;
volatile LONG g_dirty_at = 0;
uint32_t g_own_before = 0;
uint32_t g_last_crc = 0;
uint32_t g_my_list = 0;
CRITICAL_SECTION g_lock;
volatile LONG g_ready = 0;
std::string g_pending;
uint32_t g_pending_crc = 0;
const uintptr_t kQuestManager = game::va(0x018FA4F0), kGetOrCreateQuest = game::va(0x00484D90), kStartQuest = game::va(0x0078D6D0);
constexpr int kLiveMax = 16;
uint32_t g_live_ids[kLiveMax] = {};
int g_live_n = 0;
std::vector<uint32_t> g_journal;
bool g_journal_known = false;
constexpr DWORD kQuietMs = 5000, kStepMs = 20000;

std::vector<uint32_t> state_quest_ids(const std::string &state)
{
    std::vector<uint32_t> ids;
    size_t a = state.find("name=\"mQuestData\""), b = state.find("name=\"mQuestSubData\"", a);
    if (a == std::string::npos)
        return ids;
    const std::string key = "name=\"mQuestID\" value=\"";
    for (size_t p = state.find(key, a); p != std::string::npos && (b == std::string::npos || p < b);
         p = state.find(key, p + 1)) {
        long v = strtol(state.c_str() + p + key.size(), nullptr, 10);
        if (v != -1)
            ids.push_back(uint32_t(v));
    }
    return ids;
}

bool try_live(const std::string &theirs)
{
    std::vector<uint32_t> want = state_quest_ids(theirs), have;
    EnterCriticalSection(&g_lock);
    bool known = g_journal_known;
    have = g_journal;
    LeaveCriticalSection(&g_lock);
    if (!known || want.empty())
        return false;
    for (uint32_t h : have)
        if (std::find(want.begin(), want.end(), h) == want.end())
            return false;
    int added = 0;
    for (uint32_t w : want)
        if (std::find(have.begin(), have.end(), w) == have.end()) {
            quest_live_queue(w);
            ++added;
        }
    logf("quests: the other player's quest list adds %d quest(s) to this world's %u: added live, no reload", added,
         unsigned(have.size()));
    return true;
}

void go(Step s, DWORD now)
{
    logf("quests: %s -> %s", kName[g_step], kName[s]);
    g_step = s;
    g_step_at = now | 1;
}

}

void quest_sync_init()
{
    InitializeCriticalSection(&g_lock);
    InterlockedExchange(&g_ready, 1);
}

void quest_sync_local_change() { InterlockedExchange(&g_dirty_at, LONG(GetTickCount() | 1)); }

void quest_sync_received(const std::string &state, uint32_t crc)
{
    if (!g_ready || crc == g_last_crc)
        return;
    EnterCriticalSection(&g_lock);
    g_pending = state;
    g_pending_crc = crc;
    LeaveCriticalSection(&g_lock);
    logf("quests: another player's quest state (%08X, %u bytes): this world takes it at the next quiet moment", crc,
         unsigned(state.size()));
}

void quest_sync_tick()
{
    if (!g_ready || !net_shared())
        return;
    DWORD now = GetTickCount();
    if (g_step != IDLE && int(now - g_step_at) > int(kStepMs)) {
        logf("quests: %s took too long, given up", kName[g_step]);
        storage_set_override_save({});
        g_step = IDLE;
        return;
    }
    std::vector<uint8_t> file;
    uint32_t version = 0;
    switch (g_step) {
    case IDLE: {
        bool quiet = rejoin_world_quiet() && !rejoin_busy() && !revive_down();
        EnterCriticalSection(&g_lock);
        bool pending = !g_pending.empty();
        LeaveCriticalSection(&g_lock);
        LONG dirty = g_dirty_at;
        if (!quiet)
            return;
        if (pending) {
            EnterCriticalSection(&g_lock);
            std::string theirs = g_pending;
            uint32_t crc = g_pending_crc;
            LeaveCriticalSection(&g_lock);
            if (try_live(theirs)) {
                g_last_crc = crc;
                g_my_list = save_quest_list_crc(theirs);
                EnterCriticalSection(&g_lock);
                g_pending.clear();
                LeaveCriticalSection(&g_lock);
                return;
            }
        }
        if (pending || (dirty && int(now - DWORD(dirty)) > int(kQuietMs))) {
            storage_own_raw(file, g_own_before);
            if (save_machine_request(7, 0)) {
                if (!pending)
                    InterlockedExchange(&g_dirty_at, 0);
                go(pending ? SNAP_APPLY : SNAP_SEND, now);
            }
        }
        return;
    }
    case SNAP_SEND:
    case SNAP_APPLY: {
        if (!save_machine_idle() || int(now - g_step_at) < 500)
            return;
        if (!storage_own_raw(file, version) || version == g_own_before)
            return;
        std::string xml, state;
        uint32_t head[8];
        if (!save_unpack(file, xml, head) || !save_quest_state(xml, state)) {
            logf("quests: the snapshot could not be read");
            g_step = IDLE;
            return;
        }
        if (g_step == SNAP_SEND) {
            uint32_t crc = save_crc(state.data(), state.size()) | 1, list = save_quest_list_crc(state);
            if (list == g_my_list) {
                g_last_crc = crc;
                g_step = IDLE;
                return;
            }
            g_my_list = list;
            if (crc != g_last_crc) {
                std::vector<uint8_t> blob;
                if (save_deflate(state, blob)) {
                    g_last_crc = crc;
                    net_quest_publish(blob, crc);
                    logf("quests: this world's quest state %08X (%u bytes, %u deflated) sent to the other players", crc,
                         unsigned(state.size()), unsigned(blob.size()));
                }
            }
            g_step = IDLE;
            return;
        }
        if (net_role() == NET_HOST) {
            uint32_t loaded = 0, own = 0;
            bool stable = false, arrivals = false;
            storage_host_sigs(&loaded, &stable, &own, &arrivals);
            std::vector<uint8_t> merged;
            uint32_t v = 0;
            if (!stable)
                return;
            if (storage_own_save(merged, v) && save_unpack(merged, xml, head)) {
            } else {
                return;
            }
        }
        EnterCriticalSection(&g_lock);
        std::string theirs = g_pending;
        uint32_t crc = g_pending_crc;
        LeaveCriticalSection(&g_lock);
        uint32_t mine_list = save_quest_list_crc(state), their_list = save_quest_list_crc(theirs);
        if (mine_list == their_list) {
            logf("quests: the other player's quest list is this world's already: no reload");
            g_my_list = mine_list;
            g_last_crc = crc;
            EnterCriticalSection(&g_lock);
            g_pending.clear();
            LeaveCriticalSection(&g_lock);
            g_step = IDLE;
            return;
        }
        std::vector<uint8_t> out;
        if (!save_put_quest_state(xml, theirs) || !save_pack(xml, head, out)) {
            logf("quests: the other player's quest state could not be put in this world's save");
            EnterCriticalSection(&g_lock);
            g_pending.clear();
            LeaveCriticalSection(&g_lock);
            g_step = IDLE;
            return;
        }
        storage_set_override_save(out);
        g_last_crc = crc;
        g_my_list = their_list;
        if (save_machine_request(0xB, 0))
            go(LOADING, now);
        return;
    }
    case LOADING:
        if (!save_machine_idle() || int(now - g_step_at) < 500)
            return;
        rejoin_reload_world();
        go(RELOADING, now);
        return;
    case RELOADING: {
        int32_t stage = 0;
        DWORD load = stages_last_load(&stage);
        if (!load || int(load - g_step_at) < 0)
            return;
        storage_set_override_save({});
        EnterCriticalSection(&g_lock);
        g_pending.clear();
        LeaveCriticalSection(&g_lock);
        logf("quests: this world reloaded with the other player's quest state %08X", g_last_crc);
        g_step = IDLE;
        return;
    }
    }
}

bool quest_sync_busy() { return g_step != IDLE; }

void quest_live_queue(uint32_t id)
{
    EnterCriticalSection(&g_lock);
    if (g_live_n < kLiveMax)
        g_live_ids[g_live_n++] = id;
    LeaveCriticalSection(&g_lock);
}

int quest_live_count()
{
    uintptr_t qm = game::at<uintptr_t>(kQuestManager, 0);
    return qm ? int(game::at<uint32_t>(qm, 0x12C)) : -1;
}

void quest_live_tick()
{
    if (!g_ready)
        return;
    uintptr_t qm = game::at<uintptr_t>(kQuestManager, 0);
    uint32_t ids[kLiveMax];
    EnterCriticalSection(&g_lock);
    int n = qm ? g_live_n : 0;
    memcpy(ids, g_live_ids, sizeof ids);
    if (qm)
        g_live_n = 0;
    LeaveCriticalSection(&g_lock);
    for (int k = 0; k < n; ++k) {
        int before = quest_live_count();
        using GetOrCreate_t = uintptr_t(__stdcall *)(uintptr_t, int32_t);
        uintptr_t q = reinterpret_cast<GetOrCreate_t>(kGetOrCreateQuest)(qm, int32_t(ids[k]));
        uint32_t flags_before = q ? game::at<uint32_t>(q, 0x2C) : 0;
        if (q) {
            uintptr_t fn = kStartQuest, quest = q;
            asm volatile("call *%%ecx"
                         : "+c"(fn), "+d"(quest)
                         :
                         : "eax", "memory", "cc");
        }
        logf("quests: live add %08X: quest object %p, flags %08X -> %08X, journal %d -> %d", unsigned(ids[k]),
             (void *)q, flags_before, q ? game::at<uint32_t>(q, 0x2C) : 0, before, quest_live_count());
    }
    std::vector<uint32_t> j;
    if (qm) {
        uint32_t count = game::at<uint32_t>(qm, 0x12C);
        uintptr_t items = game::at<uintptr_t>(qm, 0x138);
        for (uint32_t k = 0; items && k < count && k < 256; ++k)
            if (uintptr_t q = game::at<uintptr_t>(items, k * 4))
                j.push_back(game::at<uint32_t>(q, 0x30));
    }
    EnterCriticalSection(&g_lock);
    g_journal.swap(j);
    g_journal_known = qm != 0;
    LeaveCriticalSection(&g_lock);
}
