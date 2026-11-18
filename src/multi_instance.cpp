#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>

#include "MinHook.h"
#include "game.h"
#include "mod.h"

namespace {

const uintptr_t kIatCreateMutexA = game::va(0x0139D254);
using CreateMutexA_t = HANDLE(WINAPI *)(LPSECURITY_ATTRIBUTES, BOOL, LPCSTR);
CreateMutexA_t g_real = nullptr;

HANDLE WINAPI my_create_mutex(LPSECURITY_ATTRIBUTES sa, BOOL own, LPCSTR name)
{
    if (!name)
        return g_real(sa, own, name);
    char unique[MAX_PATH];
    _snprintf(unique, sizeof unique, "%s.coop%lu", name, GetCurrentProcessId());
    unique[sizeof unique - 1] = 0;
    return g_real(sa, own, unique);
}

}

void multi_instance_install()
{
    auto slot = reinterpret_cast<CreateMutexA_t *>(kIatCreateMutexA);
    CreateMutexA_t expected = reinterpret_cast<CreateMutexA_t>(GetProcAddress(GetModuleHandleA("kernel32.dll"), "CreateMutexA"));
    if (*slot != expected) {
        logf("multi-instance: IAT slot does not hold kernel32!CreateMutexA (%p vs %p), not patched", (void *)*slot,
             (void *)expected);
        return;
    }
    g_real = *slot;
    DWORD old;
    VirtualProtect(slot, sizeof *slot, PAGE_READWRITE, &old);
    *slot = &my_create_mutex;
    VirtualProtect(slot, sizeof *slot, old, &old);
    logf("multi-instance: CreateMutexA names made unique per process");
}

namespace {
using SetActive_t = void(__attribute__((thiscall)) *)(void *, char);
SetActive_t g_set_active = nullptr;
bool g_keep_active = false;
volatile LONG g_background = 0;
void __attribute__((thiscall)) my_set_active(void *self, char on)
{
    if (g_keep_active) {
        g_set_active(self, 1);
        return;
    }
    g_set_active(self, on);
    bool coop = cfg_role() != NET_OFF;
    if (!on && coop) {
        if (uintptr_t sys = *reinterpret_cast<uintptr_t *>(game::va(0x018D0F50)))
            *reinterpret_cast<uint8_t *>(sys + game::by_build(0x2566, 0x2565)) = 1;
        InterlockedExchange(&g_background, 1);
        logf("window: in the background: the game goes on (co-op), its pads and keyboard off meanwhile");
    } else if (on && InterlockedExchange(&g_background, 0)) {
        logf("window: active again");
    }
}
using KeyboardOff_t = void(__attribute__((thiscall)) *)(void *, int);
KeyboardOff_t g_keyboard_off = nullptr;
void __attribute__((thiscall)) my_keyboard_off(void *self, int off)
{
    g_keyboard_off(self, off);
    if (!off && InterlockedExchange(&g_background, 0))
        logf("window: active again");
}
}

bool game_in_background() { return g_background != 0; }

DWORD g_bg_at, g_bg_len;
DWORD WINAPI background_test_thread(LPVOID)
{
    Sleep(g_bg_at);
    uintptr_t sys = *reinterpret_cast<uintptr_t *>(game::va(0x018D0F50));
    HWND hwnd = sys ? *reinterpret_cast<HWND *>(sys + 0x12C) : nullptr;
    logf("window: tests: WM_ACTIVATEAPP(FALSE) sent to %p", (void *)hwnd);
    PostMessageW(hwnd, WM_ACTIVATEAPP, FALSE, 0);
    if (g_bg_len) {
        Sleep(g_bg_len);
        logf("window: tests: WM_ACTIVATEAPP(TRUE) sent");
        PostMessageW(hwnd, WM_ACTIVATEAPP, TRUE, 0);
    }
    return 0;
}

void background_test_install()
{
    char v[32];
    if (cfg_str("test_background_at", v, sizeof v)) {
        g_bg_at = DWORD(atoi(v)) * 1000;
        g_bg_len = cfg_str("test_background_for", v, sizeof v) ? DWORD(atoi(v)) * 1000 : 0;
        CreateThread(nullptr, 0, background_test_thread, nullptr, 0, nullptr);
    }
}

void keep_active_install(bool tests)
{
    g_keep_active = tests;
    void *target = reinterpret_cast<void *>(game::va(0x00DF0ED0));
    MH_STATUS st = MH_CreateHook(target, reinterpret_cast<void *>(&my_set_active), reinterpret_cast<void **>(&g_set_active));
    if (st == MH_OK)
        st = MH_EnableHook(target);
    if (!tests) {
        void *kb = reinterpret_cast<void *>(game::va(0x00DF3D20));
        MH_STATUS k = MH_CreateHook(kb, reinterpret_cast<void *>(&my_keyboard_off), reinterpret_cast<void **>(&g_keyboard_off));
        if (k == MH_OK)
            k = MH_EnableHook(kb);
        logf("keep-active: keyboard hook %p: %s", kb, MH_StatusToString(k));
    }
    logf("keep-active: hook %p (%s): %s", target, tests ? "tests: always active" : "co-op: frames go on in the background",
         MH_StatusToString(st));
}

namespace {
const uintptr_t kIatSHGetFolderPathW = game::va(0x0139D288);
using SHGetFolderPathW_t = HRESULT(WINAPI *)(HWND, int, HANDLE, DWORD, LPWSTR);
SHGetFolderPathW_t g_real_shgfp = nullptr;
HRESULT WINAPI my_shgfp(HWND w, int csidl, HANDLE tok, DWORD flags, LPWSTR out)
{
    if ((csidl & 0xFF) == 0x1C  && out) {
        wchar_t dir[MAX_PATH];
        GetModuleFileNameW(nullptr, dir, MAX_PATH);
        if (wchar_t *slash = wcsrchr(dir, L'\\'))
            *slash = 0;
        wcscat(dir, L"\\coop_appdata");
        CreateDirectoryW(dir, nullptr);
        wcscpy(out, dir);
        return S_OK;
    }
    return g_real_shgfp(w, csidl, tok, flags, out);
}
}

void private_appdata_install()
{
    auto slot = reinterpret_cast<SHGetFolderPathW_t *>(kIatSHGetFolderPathW);
    HMODULE shell = GetModuleHandleA("shell32.dll");
    auto expected = shell ? reinterpret_cast<SHGetFolderPathW_t>(GetProcAddress(shell, "SHGetFolderPathW")) : nullptr;
    if (!expected || *slot != expected) {
        logf("private-appdata: IAT slot does not hold shell32!SHGetFolderPathW (%p vs %p), not patched", (void *)*slot,
             (void *)expected);
        return;
    }
    g_real_shgfp = *slot;
    DWORD old;
    VirtualProtect(slot, sizeof *slot, PAGE_READWRITE, &old);
    *slot = &my_shgfp;
    VirtualProtect(slot, sizeof *slot, old, &old);
    logf("private-appdata: LOCAL_APPDATA -> <game dir>\\coop_appdata");
}

bool window_place(int x, int y, int w, int h)
{
    uintptr_t sys = *reinterpret_cast<uintptr_t *>(game::va(0x018D0F50));
    HWND hwnd = sys ? *reinterpret_cast<HWND *>(sys + 0x12C) : nullptr;
    if (!hwnd || !IsWindow(hwnd))
        return false;
    SetWindowLongW(hwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
    SetWindowPos(hwnd, HWND_TOP, x, y, w, h, SWP_FRAMECHANGED | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    logf("window: placed at %d,%d %dx%d", x, y, w, h);
    return true;
}

bool early_flag(const char *key)
{
    FILE *f = fopen("coop.ini", "r");
    if (!f)
        return false;
    char line[128];
    size_t n = strlen(key);
    bool on = false;
    while (fgets(line, sizeof line, f))
        if (strncmp(line, key, n) == 0 && line[n] == '=')
            on = atoi(line + n + 1) != 0;
    fclose(f);
    return on;
}

bool cfg_str(const char *key, char *out, size_t cap)
{
    FILE *f = fopen("coop.ini", "r");
    if (!f)
        return false;
    char line[256];
    size_t n = strlen(key);
    bool found = false;
    while (fgets(line, sizeof line, f)) {
        if (strncmp(line, key, n) != 0 || line[n] != '=')
            continue;
        char *v = line + n + 1;
        v[strcspn(v, "\r\n")] = 0;
        snprintf(out, cap, "%s", v);
        found = true;
    }
    fclose(f);
    return found;
}
