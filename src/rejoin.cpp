#include <windows.h>

#include "game.h"
#include "mod.h"

namespace {

const uintptr_t kSaveMachine = game::va(0x018FA518);
constexpr uint32_t kReloadBit = 1u << 26, kGameOverBit = 1u << 25;
constexpr DWORD kQuietMs = 3000;
constexpr DWORD kStepMs = 15000;

enum Step { IDLE, SAVING, MERGING, LOADING, RELOADING, GUEST_SNAP };
const char *kStepName[] = {"idle", "saving", "merging", "loading", "reloading", "guest snapshot"};
Step g_step = IDLE;
DWORD g_step_at = 0, g_quiet_since = 0, g_retry_at = 0;
uint32_t g_target = 0, g_own_before = 0;
volatile LONG g_loaded_sig = 0;

uintptr_t machine() { return game::at<uintptr_t>(kSaveMachine, 0); }
bool machine_idle()
{
    uintptr_t sm = machine();
    return sm && game::at<uint8_t>(sm, 0x648) == 0;
}

bool machine_request(uint8_t mode, uint8_t slot)
{
    uintptr_t sm = machine();
    if (!sm || game::at<uint8_t>(sm, 0x648) != 0)
        return false;
    if (mode == 7)
        actions_quiet_save();
    game::at<int32_t>(sm, 0xA44) = -1;
    game::at<int32_t>(sm, 0xA48) = -1;
    game::at<uint8_t>(sm, 0x638) = slot;
    game::at<uint8_t>(sm, 0x63B) = 1;
    game::at<int32_t>(sm, 0xA4C) = 0;
    game::at<int32_t>(sm, 0xA50) = 0;
    game::at<uint8_t>(sm, 0x829) = 1;
    game::at<uint8_t>(sm, 0x64D) = 0;
    game::at<uint8_t>(sm, 0x82A) = mode == 0x12;
    game::at<uint8_t>(sm, 0x649) = 0;
    MemoryBarrier();
    game::at<uint8_t>(sm, 0x648) = mode;
    return true;
}

bool world_quiet()
{
    uintptr_t gs = game::at<uintptr_t>(game::kGameMain, 0), st = current_stage_area();
    if (!gs || !st || !possess_arisen())
        return false;
    return game::at<int32_t>(st, 0x808) == 1 && game::at<int32_t>(gs, game::kGsEvent) == 256 &&
           (game::at<uint32_t>(gs, 0xB8840) & (kReloadBit | kGameOverBit)) == 0 && machine_idle();
}

void go(Step s, DWORD now)
{
    logf("rejoin: %s -> %s", kStepName[g_step], kStepName[s]);
    g_step = s;
    g_step_at = now | 1;
}

void give_up(DWORD now, const char *why)
{
    storage_set_override_save({});
    logf("rejoin: %s gave up (%s), trying again in 20 s", kStepName[g_step], why);
    g_step = IDLE;
    g_retry_at = (now + 20000) | 1;
}

}

bool rejoin_busy() { return g_step != IDLE; }
uint32_t rejoin_loaded_sig() { return uint32_t(g_loaded_sig); }

void rejoin_tick(int role, uint32_t host_loaded_sig)
{
    DWORD now = GetTickCount();
    uint32_t want = 0, loaded = 0, own = 0;
    bool stable = false, read_any = false, arrivals = true;
    if (role == NET_HOST) {
        want = storage_host_sigs(&loaded, &stable, &own, &arrivals);
        InterlockedExchange(&g_loaded_sig, LONG(loaded));
    } else if (role == NET_GUEST) {
        want = storage_guest_sigs(&loaded, &read_any);
        stable = read_any && want == host_loaded_sig;
    } else {
        return;
    }
    if (g_step != IDLE && int(now - g_step_at) > int(kStepMs)) {
        give_up(now, "too long");
        return;
    }
    switch (g_step) {
    case IDLE: {
        bool quiet = world_quiet();
        if (!quiet || want == loaded || want == 0 || !arrivals || !stable) {
            g_quiet_since = 0;
            return;
        }
        if (!g_quiet_since)
            g_quiet_since = now | 1;
        if (int(now - g_quiet_since) < int(kQuietMs) || (g_retry_at && int(now - g_retry_at) < 0))
            return;
        g_target = want;
        g_own_before = own;
        logf("rejoin: the %s's world shows characters %08X, the save holds %08X: %s", role == NET_HOST ? "host" : "guest",
             loaded, want,
             role == NET_HOST ? "save, then load the merged save"
             : net_shared()   ? "its own snapshot with the new party, then a reload in place"
                              : "load the host's save");
        if (role == NET_GUEST && net_shared()) {
            std::vector<uint8_t> file;
            storage_own_raw(file, g_own_before);
            if (machine_request(7, 0))
                go(GUEST_SNAP, now);
            return;
        }
        if (role == NET_HOST ? machine_request(7, 0) : machine_request(0xB, 0))
            go(role == NET_HOST ? SAVING : LOADING, now);
        return;
    }
    case GUEST_SNAP: {
        std::vector<uint8_t> file, host, out;
        uint32_t version = 0;
        if (!machine_idle() || int(now - g_step_at) < 500 || !storage_own_raw(file, version) || version == g_own_before)
            return;
        if (!storage_peer_raw(host) || !roam_refresh_party(file, host, out)) {
            give_up(now, "the new party could not be put in this guest's snapshot");
            return;
        }
        storage_set_override_save(out);
        if (machine_request(0xB, 0))
            go(LOADING, now);
        return;
    }
    case SAVING:
        if (machine_idle() && int(now - g_step_at) > 500)
            go(MERGING, now);
        return;
    case MERGING:
        if (!stable)
            return;
        if (own == g_own_before)
            logf("rejoin: the save request wrote nothing new (own save version %u)", own);
        if (machine_request(0xB, 0))
            go(LOADING, now);
        return;
    case LOADING:
        if (!machine_idle() || int(now - g_step_at) < 500)
            return;
        if (role == NET_GUEST && net_shared()) {
            storage_guest_mark_read(g_target);
        } else if (loaded != g_target) {
            give_up(now, "the game did not read the expected save");
            return;
        }
        if (uintptr_t gs = game::at<uintptr_t>(game::kGameMain, 0))
            InterlockedOr(reinterpret_cast<volatile LONG *>(gs + 0xB8840), LONG(kReloadBit));
        go(RELOADING, now);
        return;
    case RELOADING: {
        int32_t stage = 0;
        DWORD load = stages_last_load(&stage);
        if (load && int(load - g_step_at) >= 0) {
            if (role == NET_GUEST && net_shared())
                storage_set_override_save({});
            logf("rejoin: world reloaded with characters %08X", role == NET_GUEST && net_shared() ? g_target : loaded);
            g_step = IDLE;
        }
        return;
    }
    }
}

bool save_machine_request(uint8_t mode, uint8_t slot) { return machine_request(mode, slot); }
bool save_machine_idle() { return machine_idle(); }
bool rejoin_world_quiet() { return world_quiet(); }
void rejoin_reload_world()
{
    if (uintptr_t gs = game::at<uintptr_t>(game::kGameMain, 0))
        InterlockedOr(reinterpret_cast<volatile LONG *>(gs + 0xB8840), LONG(kReloadBit));
}
