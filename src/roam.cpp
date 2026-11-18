#include <windows.h>

#include "game.h"
#include "mod.h"

bool save_machine_request(uint8_t mode, uint8_t slot);
bool save_machine_idle();
bool rejoin_world_quiet();
void rejoin_reload_world();
void storage_set_roam_save(const std::vector<uint8_t> &file);
bool storage_guest_own_save(std::vector<uint8_t> &out);
void progress_roam_begin(const std::string &xml);
void progress_roam_end(const std::string &xml);

namespace {

enum Step { ATTACHED, OUT_SAVING, OUT_LOADING, OUT_RELOADING, ROAMING, BACK_SAVING, BACK_LOADING, BACK_RELOADING };
const char *kName[] = {"with the host", "leaving: saving", "leaving: loading", "leaving: reloading",
                       "roaming",       "back: saving",    "back: loading",    "back: reloading"};
Step g_step = ATTACHED;
DWORD g_step_at = 0, g_held_since = 0;
uint32_t g_own_before = 0;
bool g_was_down = false;
volatile LONG g_roaming = 0;
constexpr DWORD kStepMs = 20000;

void go(Step s, DWORD now)
{
    logf("roam: %s -> %s", kName[g_step], kName[s]);
    g_step = s;
    g_step_at = now | 1;
}

bool toggle_pressed(DWORD now)
{
    XINPUT_STATE s = {};
    WORD want = XINPUT_GAMEPAD_LEFT_SHOULDER | XINPUT_GAMEPAD_RIGHT_SHOULDER | XINPUT_GAMEPAD_Y;
    bool pad = pads_read_raw(0, &s) == ERROR_SUCCESS && (s.Gamepad.wButtons & want) == want;
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    bool key = pid == GetCurrentProcessId() && (GetAsyncKeyState(VK_F7) & 0x8000);
    if (pad && !g_held_since)
        g_held_since = now | 1;
    if (!pad)
        g_held_since = 0;
    bool down = key || (g_held_since && int(now - g_held_since) >= 1500);
    bool fire = down && !g_was_down;
    g_was_down = down;
    return fire;
}

bool own_write(std::string &xml, uint32_t head[8], uint32_t &version)
{
    std::vector<uint8_t> file;
    return storage_own_save(file, version) && save_unpack(file, xml, head);
}

bool make_roam_save(std::vector<uint8_t> &out)
{
    std::string xml, own_char, bag;
    uint32_t head[8], version = 0;
    if (!own_write(xml, head, version) || !storage_guest_character(own_char)) {
        logf("roam: the snapshot or the guest's own character could not be read");
        return false;
    }
    int pawn = net_guest_my_pawn();
    std::string pawn_char;
    bool swapped = pawn >= 0 && save_character(xml, pawn, pawn_char) &&
                   save_character_name(pawn_char) == save_character_name(own_char) && save_swap_arisen_pawn(xml, pawn);
    if (!swapped && !save_put_arisen_everywhere(xml, own_char)) {
        logf("roam: the guest's character could not be made the Arisen");
        return false;
    }
    std::vector<uint8_t> own;
    std::string own_xml;
    uint32_t own_head[8];
    if (storage_guest_own_save(own) && save_unpack(own, own_xml, own_head) && save_bag(own_xml, 0, bag))
        save_put_bag_everywhere(xml, 0, bag);
    else
        logf("roam: the guest's own bag could not be read (the Arisen keeps the host's)");
    if (!save_pack(xml, head, out)) {
        logf("roam: the roam save does not fit");
        return false;
    }
    progress_roam_begin(xml);
    logf("roam: roam save made: %s is the Arisen%s", save_character_name(own_char).c_str(),
         swapped ? ", the host's Arisen takes its pawn's place" : " (it was not in the party: replaces the host's Arisen)");
    return true;
}

}

static std::string g_pawn_names[kPawns];
static int g_pawn_levels[kPawns] = {};
static volatile LONG g_names_lock = 0;
static void keep_pawn_names(const std::string &xml)
{
    std::string names[kPawns];
    int levels[kPawns] = {};
    for (int p = 0; p < kPawns; ++p) {
        std::string ch;
        if (save_character(xml, p, ch)) {
            names[p] = save_character_name(ch);
            levels[p] = save_character_level(ch);
        }
    }
    while (InterlockedExchange(&g_names_lock, 1))
        Sleep(0);
    for (int p = 0; p < kPawns; ++p) {
        g_pawn_names[p] = names[p];
        g_pawn_levels[p] = levels[p];
    }
    InterlockedExchange(&g_names_lock, 0);
}

bool roam_world_from_host(const std::vector<uint8_t> &host, std::vector<uint8_t> &out)
{
    std::string xml, own_char, bag;
    uint32_t head[8];
    if (!save_unpack(host, xml, head) || !storage_guest_character(own_char))
        return false;
    std::string name = save_character_name(own_char);
    int pawn = -1, given = net_guest_my_pawn();
    std::string on_given;
    if (given >= 0 && save_character(xml, given, on_given) && save_character_name(on_given) == name)
        pawn = given;
    for (int p = 0; p < kPawns && pawn < 0 && given < 0; ++p) {
        std::string ch;
        if (save_character(xml, p, ch) && save_character_name(ch) == name)
            pawn = p;
    }
    bool swapped = pawn >= 0 && save_swap_arisen_pawn(xml, pawn);
    bool early = false;
    std::string host_char;
    if (!swapped && given >= 0 && save_character(xml, -1, host_char))
        early = save_put_character(xml, given, host_char);
    if (!swapped && !save_put_arisen_everywhere(xml, own_char))
        return false;
    std::vector<uint8_t> own;
    std::string own_xml;
    uint32_t own_head[8];
    if (storage_guest_own_save(own) && save_unpack(own, own_xml, own_head) && save_bag(own_xml, 0, bag))
        save_put_bag_everywhere(xml, 0, bag);
    if (!save_pack(xml, head, out))
        return false;
    logf("roam: shared world: %s is this guest's Arisen%s", name.c_str(),
         swapped ? " (the host's Arisen on its pawn)"
                 : early ? " (not merged by the host yet: the host's Arisen put on the pawn it gave)" : " (the host has not merged it yet)");
    logf("roam: shared world party: %s", save_copy_names(xml).c_str());
    keep_pawn_names(xml);
    return true;
}

bool roam_refresh_party(const std::vector<uint8_t> &own, const std::vector<uint8_t> &host, std::vector<uint8_t> &out)
{
    std::string mine, theirs;
    uint32_t head[8], hhead[8];
    int me = net_guest_my_pawn();
    if (me < 0 || !save_unpack(own, mine, head) || !save_unpack(host, theirs, hhead))
        return false;
    for (int p = 0; p < kPawns; ++p) {
        std::string ch;
        if (!save_character(theirs, p == me ? -1 : p, ch) || !save_put_character(mine, p, ch))
            return false;
    }
    if (!save_pack(mine, head, out))
        return false;
    logf("roam: shared world: this guest's own snapshot with the host's latest party (it stays where it is)");
    logf("roam: shared world party: %s", save_copy_names(mine).c_str());
    keep_pawn_names(mine);
    return true;
}

bool net_guest_roaming() { return g_roaming != 0; }

int roam_pawn_level(int p)
{
    if (p < 0 || p >= kPawns)
        return 0;
    while (InterlockedExchange(&g_names_lock, 1))
        Sleep(0);
    int n = g_pawn_levels[p];
    InterlockedExchange(&g_names_lock, 0);
    return n;
}

std::string roam_pawn_name(int p)
{
    if (p < 0 || p >= kPawns)
        return "";
    while (InterlockedExchange(&g_names_lock, 1))
        Sleep(0);
    std::string n = g_pawn_names[p];
    InterlockedExchange(&g_names_lock, 0);
    return n;
}
bool roam_busy() { return g_step != ATTACHED && g_step != ROAMING; }

void roam_tick()
{
    DWORD now = GetTickCount();
    bool press = toggle_pressed(now);
    if (g_step != ATTACHED && g_step != ROAMING && int(now - g_step_at) > int(kStepMs)) {
        logf("roam: %s took too long, back to %s", kName[g_step], g_roaming ? "roaming" : "the host");
        storage_set_roam_save({});
        g_step = g_roaming ? ROAMING : ATTACHED;
        return;
    }
    uint32_t version = 0;
    std::vector<uint8_t> file;
    static DWORD next_where = 0;
    if (g_step == ROAMING && (!next_where || int(now - next_where) >= 0)) {
        next_where = (now + 5000) | 1;
        int32_t stage = -1;
        stages_last_load(&stage);
        if (uintptr_t a = possess_arisen())
            logf("roam: exploring at (%.0f, %.0f), stage %d", game::at<float>(a, 0x40), game::at<float>(a, 0x48), stage);
    }
    switch (g_step) {
    case ATTACHED:
    case ROAMING:
        if (!press)
            return;
        if (!rejoin_world_quiet() || rejoin_busy()) {
            logf("roam: not now (a talk, a load, an area change or a save under way)");
            return;
        }
        storage_own_save(file, g_own_before);
        if (save_machine_request(7, 0))
            go(g_step == ATTACHED ? OUT_SAVING : BACK_SAVING, now);
        return;
    case OUT_SAVING:
    case BACK_SAVING:
        if (!save_machine_idle() || int(now - g_step_at) < 500)
            return;
        if (!storage_own_save(file, version) || version == g_own_before) {
            logf("roam: the save request wrote nothing");
            g_step = g_step == OUT_SAVING ? ATTACHED : ROAMING;
            return;
        }
        if (g_step == OUT_SAVING) {
            std::vector<uint8_t> roam;
            if (!make_roam_save(roam)) {
                g_step = ATTACHED;
                return;
            }
            storage_set_roam_save(roam);
            InterlockedExchange(&g_roaming, 1);
        } else {
            std::string xml;
            uint32_t head[8];
            if (own_write(xml, head, version))
                progress_roam_end(xml);
            storage_set_roam_save({});
        }
        if (save_machine_request(0xB, 0))
            go(g_step == OUT_SAVING ? OUT_LOADING : BACK_LOADING, now);
        return;
    case OUT_LOADING:
    case BACK_LOADING:
        if (!save_machine_idle() || int(now - g_step_at) < 500)
            return;
        rejoin_reload_world();
        go(g_step == OUT_LOADING ? OUT_RELOADING : BACK_RELOADING, now);
        return;
    case OUT_RELOADING:
    case BACK_RELOADING: {
        int32_t stage = 0;
        DWORD load = stages_last_load(&stage);
        if (!load || int(load - g_step_at) < 0)
            return;
        if (g_step == BACK_RELOADING)
            InterlockedExchange(&g_roaming, 0);
        logf("roam: %s", g_step == OUT_RELOADING ? "exploring alone (the host's game no longer moves this one)"
                                                 : "back with the host");
        g_step = g_step == OUT_RELOADING ? ROAMING : ATTACHED;
        return;
    }
    }
}
