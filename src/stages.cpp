#include <windows.h>

#include "MinHook.h"
#include "game.h"
#include "mod.h"

extern "C" void __cdecl stages_on_load(const int32_t *req, uintptr_t ret) asm("stages_on_load");
extern "C" void *g_stage_load_orig asm("g_stage_load_orig");
void *g_stage_load_orig = nullptr;

const uintptr_t kJumpLoadRet = game::va(0x0050F28A);
volatile LONG g_other_loads = 0;
volatile LONG g_last_load_tick = 0, g_last_load_stage = -1;

extern "C" void __cdecl stages_on_load(const int32_t *req, uintptr_t ret)
{
    hang_note_main_thread();
    if (!req || req[0] == -1)
        return;
    if (ret != kJumpLoadRet)
        InterlockedIncrement(&g_other_loads);
    InterlockedExchange(&g_last_load_stage, req[0]);
    InterlockedExchange(&g_last_load_tick, LONG(GetTickCount() | 1));
    uintptr_t gm = game::at<uintptr_t>(game::kGameMain, 0);
    logf("stage: load %d point %d flag %d (from stage %d point %d; caller %08X)", req[0], req[1] & 0xFF, req[2],
         gm ? game::at<int32_t>(gm, 0x34) : -1, gm ? game::at<int32_t>(gm, 0x44) : -1, unsigned(ret));
}

extern "C" __attribute__((naked)) void stages_load_thunk() asm("stages_load_thunk");
extern "C" __attribute__((naked)) void stages_load_thunk()
{
    asm volatile("pushal\n\t"
                 "pushl 32(%esp)\n\t"
                 "pushl %eax\n\t"
                 "call stages_on_load\n\t"
                 "addl $8, %esp\n\t"
                 "popal\n\t"
                 "jmp *g_stage_load_orig\n\t");
}

uint32_t stages_other_loads() { return uint32_t(g_other_loads); }
DWORD stages_last_load(int32_t *stage)
{
    if (stage)
        *stage = g_last_load_stage;
    return DWORD(g_last_load_tick);
}

bool stages_install()
{
    void *target = reinterpret_cast<void *>(game::kStageLoad);
    MH_STATUS st = MH_CreateHook(target, reinterpret_cast<void *>(&stages_load_thunk), &g_stage_load_orig);
    if (st == MH_OK)
        st = MH_EnableHook(target);
    logf("stages: hook StageLoad %p: %s", target, MH_StatusToString(st));
    return st == MH_OK;
}
