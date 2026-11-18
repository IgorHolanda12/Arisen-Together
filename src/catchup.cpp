#include <windows.h>
#include <cstdint>

#include "MinHook.h"
#include "game.h"
#include "mod.h"

extern "C" {
void *g_catchup_orig __asm__("g_catchup_orig") = nullptr;
volatile LONG g_catchup_skipped __asm__("g_catchup_skipped") = 0;
bool catchup_skip(uintptr_t pawn) __asm__("catchup_skip");
bool catchup_skip(uintptr_t pawn)
{
    if (!net_puppet_ptr(pawn))
        return false;
    InterlockedIncrement(&g_catchup_skipped);
    return true;
}
}

__attribute__((naked)) static void catchup_stub()
{
    __asm__ volatile(".intel_syntax noprefix\n"
                     "pushad\n"
                     "push edi\n"
                     "call catchup_skip\n"
                     "add esp, 4\n"
                     "test al, al\n"
                     "popad\n"
                     "jnz 1f\n"
                     "jmp dword ptr [g_catchup_orig]\n"
                     "1:\n"
                     "xor eax, eax\n"
                     "ret\n"
                     ".att_syntax\n");
}

void catchup_install()
{
    void *target = reinterpret_cast<void *>(game::va(0x00B88710));
    MH_STATUS st = MH_CreateHook(target, reinterpret_cast<void *>(&catchup_stub), &g_catchup_orig);
    if (st == MH_OK)
        st = MH_EnableHook(target);
    logf("catch-up: hook %p (no catch-up for the pawns showing players): %s", target, MH_StatusToString(st));
}

long catchup_skipped_take() { return InterlockedExchange(&g_catchup_skipped, 0); }
