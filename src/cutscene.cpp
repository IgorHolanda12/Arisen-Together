#include <windows.h>

#include <cstdio>
#include <cstring>
#include <tuple>

#include "MinHook.h"
#include "game.h"
#include "mod.h"

void net_send_event(int32_t event);

namespace {

using EventStart_t = void(__stdcall *)(uintptr_t ctl);
const uintptr_t kEventStart = game::va(0x0050BE20);
EventStart_t g_orig = nullptr;
volatile uintptr_t g_ctl = 0;

using CtlUpdate_t = void(__fastcall *)(uintptr_t ctl);
const uintptr_t kCtlUpdate = game::va(0x00504330);
CtlUpdate_t g_orig_upd = nullptr;

volatile LONG g_want = -1;
volatile DWORD g_want_until = 0;
LONG g_injected = -1;
DWORD g_played_at[0x100] = {};

bool special(int no)
{
    switch (no) {
    case 0x01: case 0x04: case 0x05: case 0x07: case 0x08: case 0x09: case 0x0A: case 0x0B: case 0x0C: case 0x0D:
    case 0x0E: case 0x0F: case 0x10: case 0x11: case 0x12: case 0x13: case 0x14: case 0x15: case 0x17: case 0x19:
    case 0x1A: case 0x1B: case 0x1C: case 0x1D: case 0x1E: case 0x1F: case 0x24: case 0x27: case 0x2B: case 0x2C:
    case 0x30: case 0x31: case 0x36: case 0x38: case 0x39: case 0x3E: case 0x40: case 0x41: case 0x43: case 0x4E:
    case 0x4F: case 0x50: case 0x51: case 0x53: case 0x59: case 0x5A: case 0x5B: case 0x5C: case 0x61:
        return true;
    }
    return no < 0 || no > 0x61;
}

void __stdcall hk_event_start(uintptr_t ctl)
{
    if (game::at<uint8_t>(ctl, 6) == 0) {
        g_ctl = ctl;
        int32_t no = game::at<int32_t>(ctl, 0x8C);
        bool ours = no == g_injected;
        g_injected = -1;
        if (no >= 0 && no < 0x100)
            g_played_at[no] = GetTickCount() | 1;
        logf("cutscene: event %d (0x%X) starts%s, controller %p, stage %d", no, unsigned(no),
             ours ? " (another player's)" : "", (void *)ctl, int(net_my_stage()));
        if (!ours && net_shared()) {
            if (special(no))
                logf("cutscene: event %d changes more than a scene: not sent to the others", no);
            else
                net_send_event(no);
        }
    }
    g_orig(ctl);
}

volatile LONG g_menu_world = 0;

void __fastcall hk_ctl_update(uintptr_t ctl)
{
    g_ctl = ctl;
    if (g_menu_world && game::at<uint8_t>(ctl, 4) != 9) {
        g_menu_world = 0;
        logf("scene: pause menu shut (controller state %d)", game::at<uint8_t>(ctl, 4));
    }
    LONG want = g_want;
    if (want >= 0) {
        uint8_t state = game::at<uint8_t>(ctl, 4);
        if (state == 2) {
            InterlockedExchange(&g_want, -1);
            g_injected = want;
            game::at<int32_t>(ctl, 0x8C) = want;
            game::at<uint8_t>(ctl, 5) = 0;
            game::at<uint8_t>(ctl, 6) = 0;
            game::at<uint8_t>(ctl, 7) = 0;
            game::at<uint8_t>(ctl, 4) = 3;
            logf("cutscene: event %ld put in", want);
        } else if (g_want_until && int(GetTickCount() - g_want_until) >= 0) {
            InterlockedExchange(&g_want, -1);
            logf("cutscene: event %ld dropped: no field to start it from (state %u/%u)", want, state,
                 game::at<uint8_t>(ctl, 5));
        }
    }
    g_orig_upd(ctl);
}

using Pause_t = void(__stdcall *)(char on);
const uintptr_t kPauseA = game::va(0x0043F6C0), kPauseB = game::va(0x0043F7E0);
Pause_t g_orig_pa = nullptr, g_orig_pb = nullptr;

void __stdcall hk_pause_a(char on)
{
    uintptr_t ret = reinterpret_cast<uintptr_t>(__builtin_return_address(0));
    uintptr_t c = g_ctl;
    int state = c ? game::at<uint8_t>(c, 4) : -1;
    logf("scene: world pause A (line 0x39) %s from %08X, controller state %d/%d", on ? "on" : "off", unsigned(ret),
         state, c ? game::at<uint8_t>(c, 5) : -1);
    g_orig_pa(on);
    static int keep = -1;
    if (keep < 0) {
        keep = 1;
        if (FILE *f = fopen("coop.ini", "r")) {
            char line[128];
            while (fgets(line, sizeof line, f))
                if (strncmp(line, "menu_runs_world=", 16) == 0)
                    keep = atoi(line + 16) != 0;
            fclose(f);
        }
        logf("scene: the pause menu %s the world in co-op (menu_runs_world=%d)", keep ? "does not stop" : "stops", keep);
    }
    if (on && state == 9 && keep && net_shared()) {
        uintptr_t unit = game::at<uintptr_t>(game::va(0x018D09E0), 0);
        if (unit && 0x39 < game::at<uint32_t>(unit, 0xD28)) {
            game::at<uint32_t>(unit, 0xD30 + (0x39 / 32) * 4) |= 1u << (0x39 % 32);
            g_menu_world = 1;
            logf("scene: pause menu in co-op: the world's line 0x39 kept on");
        }
    }
}
void __stdcall hk_pause_b(char on)
{
    uintptr_t ret = reinterpret_cast<uintptr_t>(__builtin_return_address(0));
    uintptr_t c = g_ctl;
    logf("scene: world pause B (line 0x3C) %s from %08X, controller state %d/%d", on ? "on" : "off", unsigned(ret),
         c ? game::at<uint8_t>(c, 4) : -1, c ? game::at<uint8_t>(c, 5) : -1);
    g_orig_pb(on);
}

}

bool scene_menu_world() { return g_menu_world != 0; }

void cutscene_play(int no)
{
    g_want_until = (GetTickCount() + 30000) | 1;
    InterlockedExchange(&g_want, no);
}

void cutscene_remote(int no, int stage, int from)
{
    int here = int(net_my_stage());
    DWORD t = GetTickCount();
    const char *why = nullptr;
    if (stage != here)
        why = "this player is in another area";
    else if (special(no))
        why = "it changes more than a scene";
    else if (no < 0x100 && g_played_at[no] && int(t - g_played_at[no]) < 30000)
        why = "this player's game played it just now";
    if (why) {
        logf("cutscene: player %d's event %d (stage %d) not started here: %s", from, no, stage, why);
        return;
    }
    logf("cutscene: player %d's event %d (stage %d): started here too", from, no, stage);
    g_want_until = (t + 6000) | 1;
    InterlockedExchange(&g_want, no);
}

bool cutscene_install()
{
    MH_STATUS us = MH_CreateHook(reinterpret_cast<void *>(kCtlUpdate), reinterpret_cast<void *>(&hk_ctl_update),
                                 reinterpret_cast<void **>(&g_orig_upd));
    if (us == MH_OK)
        us = MH_EnableHook(reinterpret_cast<void *>(kCtlUpdate));
    MH_STATUS st = MH_CreateHook(reinterpret_cast<void *>(kEventStart), reinterpret_cast<void *>(&hk_event_start),
                                 reinterpret_cast<void **>(&g_orig));
    if (st == MH_OK)
        st = MH_EnableHook(reinterpret_cast<void *>(kEventStart));
    logf("cutscene: hooks controller update %08X: %s, event start %08X: %s", unsigned(kCtlUpdate),
         MH_StatusToString(us), unsigned(kEventStart), MH_StatusToString(st));
    for (auto [at, hk, orig] : {std::tuple{kPauseA, reinterpret_cast<void *>(&hk_pause_a), reinterpret_cast<void **>(&g_orig_pa)},
                                std::tuple{kPauseB, reinterpret_cast<void *>(&hk_pause_b), reinterpret_cast<void **>(&g_orig_pb)}}) {
        MH_STATUS ps = MH_CreateHook(reinterpret_cast<void *>(at), hk, orig);
        if (ps == MH_OK)
            ps = MH_EnableHook(reinterpret_cast<void *>(at));
        logf("scene: hook world pause %08X: %s", unsigned(at), MH_StatusToString(ps));
    }
    return st == MH_OK && us == MH_OK;
}
