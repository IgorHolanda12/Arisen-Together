#include <windows.h>

#include "MinHook.h"
#include "game.h"
#include "mod.h"

namespace {

const uintptr_t kRiftStoneUpdate = game::va(0x00C495A0);
constexpr uintptr_t kState = 0x28, kFlags = 0x29A6, kA47 = 0x291C, kPrompt = 0x29F0;
using Update_t = void(__fastcall *)(uintptr_t);
Update_t g_orig = nullptr;
bool g_block = false, g_kill = false;
volatile LONG g_blocked = 0;

void __fastcall hk_update(uintptr_t stone)
{
    uint8_t before = game::at<uint8_t>(stone, kState);
    if (g_block && before == 1 && (game::at<uint8_t>(stone, kFlags) & 0x10)) {
        game::at<uint8_t>(stone, kFlags) &= uint8_t(~0x10);
        game::at<uint32_t>(stone, kA47) = 0;
        game::at<uint32_t>(stone, kPrompt) = 7;
        LONG n = InterlockedIncrement(&g_blocked);
        logf("rift: Rift stone %p examined: the Rift stays shut while co-op is on (%ld so far)", (void *)stone, n);
    }
    g_orig(stone);
    uint8_t after = game::at<uint8_t>(stone, kState);
    if (after != before && after != 0 && after != 1 && after != 6 && after != 7)
        logf("rift: Rift stone %p state %u -> %u", (void *)stone, unsigned(before), unsigned(after));
    if (g_kill && before == 1 && after != 1 && after != 0) {
        logf("rift: test_rift_kill: the stone left state 1 (%u): ending the game before the Rift", unsigned(after));
        TerminateProcess(GetCurrentProcess(), 0x52494654);
    }
}

}

bool rift_install()
{
    g_block = cfg_role() != NET_OFF && !early_flag("allow_rift");
    g_kill = early_flag("test_rift_kill");
    void *target = reinterpret_cast<void *>(kRiftStoneUpdate);
    MH_STATUS st = MH_CreateHook(target, reinterpret_cast<void *>(&hk_update), reinterpret_cast<void **>(&g_orig));
    if (st == MH_OK)
        st = MH_EnableHook(target);
    logf("rift: hook RiftStoneUpdate %08X: %s; the Rift is %s", unsigned(kRiftStoneUpdate), MH_StatusToString(st),
         g_block ? "shut (co-op on)" : "open");
    return st == MH_OK;
}

uint32_t rift_blocked() { return uint32_t(g_blocked); }
