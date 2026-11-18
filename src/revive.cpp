#include <windows.h>

#include <cmath>
#include <cstring>

#include "game.h"
#include "mod.h"

namespace {

constexpr DWORD kDownMs = 60000, kHelpMs = 3000;
constexpr float kHelpReach = 250.f, kBackShare = 0.3f;
volatile LONG g_down = 0;
DWORD g_down_at = 0;
volatile LONG g_get_up = 0;
volatile LONG g_die = 0;
volatile LONG g_move_back = 0;
volatile LONG g_shore = 0;
float g_to[3] = {};
int32_t g_to_stage = -1;
DWORD g_help_since = 0;
int g_help_target = -1;

float *arisen_hp(uintptr_t a) { return a ? enemies_hp(a) : nullptr; }

int someone_up(bool here, PlayerInfo *out)
{
    int me = net_my_slot(), found = -1;
    int32_t stage = net_my_stage();
    for (int s = 0; s < 4 && found < 0; ++s) {
        PlayerInfo p;
        if (s == me || !net_player_info(s, p) || !p.present || p.downed)
            continue;
        if (here && p.stage != stage)
            continue;
        if (out && p.pos[0] == 0.f && p.pos[2] == 0.f)
            continue;
        found = s;
        if (out)
            *out = p;
    }
    return found;
}

}

bool revive_down() { return g_down != 0; }
bool revive_helping() { return g_help_target >= 0; }

bool revive_on_damage(uintptr_t ch, float *dmg)
{
    if (!net_shared() || !dmg || ch != possess_arisen())
        return false;
    if (g_down)
        return true;
    float *hp = arisen_hp(ch);
    if (!hp || hp[0] - *dmg > 0.f)
        return false;
    if (someone_up(false, nullptr) < 0)
        return false;
    *dmg = hp[0] > 1.f ? hp[0] - 1.f : 0.f;
    g_down_at = GetTickCount() | 1;
    InterlockedExchange(&g_down, 1);
    logf("revive: down (HP %.0f, the blow cut to %.0f): another player can help with B, or back in 60 s", hp[0], *dmg);
    return false;
}

bool revive_on_fatal(uintptr_t ch, const char *why, bool move_back)
{
    if (!net_shared() || !ch || ch != possess_arisen())
        return false;
    if (g_down)
        return true;
    if (why && (strstr(why, "DipWater") || strstr(why, "HugebleDead"))) {
        if (!InterlockedExchange(&g_shore, 1))
            logf("revive: deep water (%s): back on the shore", why);
        return true;
    }
    if (someone_up(false, nullptr) < 0)
        return false;
    g_down_at = GetTickCount() | 1;
    InterlockedExchange(&g_move_back, move_back ? 1 : 0);
    InterlockedExchange(&g_down, 1);
    logf("revive: down (%s)%s: another player can help with B, or back in 60 s", why,
         move_back ? ", back where it last stood" : "");
    return true;
}

void revive_tick()
{
    if (!net_shared())
        return;
    DWORD now = GetTickCount();
    if (g_down) {
        PlayerInfo p;
        bool here = someone_up(true, &p) >= 0;
        static DWORD all_down_since = 0;
        bool all_down = !here && someone_up(false, nullptr) < 0;
        all_down_since = all_down ? (all_down_since ? all_down_since : now | 1) : 0;
        if (all_down && int(now - all_down_since) < 3000)
            return;
        if (all_down) {
            if (!g_die) {
                for (int q = 0; q < 4; ++q) {
                    PlayerInfo i;
                    bool ok = net_player_info(q, i);
                    logf("revive:   place %d%s: %s present %d down %d area %d", q, q == net_my_slot() ? " (me)" : "",
                         ok ? "known" : "unknown", i.present, i.downed, i.stage);
                }
                logf("revive: every player is down: the game's death");
                InterlockedExchange(&g_die, 1);
            }
            return;
        }
        if (!here || int(now - g_down_at) > int(kDownMs)) {
            if (!here && someone_up(false, &p) < 0)
                return;
            memcpy(g_to, p.pos, sizeof g_to);
            g_to_stage = p.stage;
            if (!g_get_up) {
                logf("revive: back next to a player in area %d (%s)", p.stage, here ? "60 s down" : "nobody up in this area");
                InterlockedExchange(&g_get_up, 2);
            }
        }
        return;
    }
    XINPUT_STATE s = {};
    static DWORD b_at = 0;
    if (pads_read_raw(0, &s) == ERROR_SUCCESS && (s.Gamepad.wButtons & XINPUT_GAMEPAD_B))
        b_at = now | 1;
    bool b = b_at && int(now - b_at) < 400;
    uintptr_t me = possess_arisen();
    int target = -1;
    for (int t = 0; t < 4 && b && me && target < 0; ++t) {
        PlayerInfo p;
        if (t == net_my_slot() || !net_player_info(t, p) || !p.present || !p.downed || p.stage != net_my_stage())
            continue;
        float dx = p.pos[0] - game::at<float>(me, 0x40), dz = p.pos[2] - game::at<float>(me, 0x48);
        float d = p.pos[0] == 0.f && p.pos[2] == 0.f ? 1e9f : std::sqrt(dx * dx + dz * dz);
        if (uintptr_t c = net_slot_character(t)) {
            float cx = game::at<float>(c, 0x40) - game::at<float>(me, 0x40), cz = game::at<float>(c, 0x48) - game::at<float>(me, 0x48);
            float dc = std::sqrt(cx * cx + cz * cz);
            if (dc < d)
                d = dc;
        }
        if (d < kHelpReach)
            target = t;
    }
    static DWORD next_dbg = 0;
    if (b && (!next_dbg || int(now - next_dbg) >= 0)) {
        next_dbg = (now + 1000) | 1;
        for (int t = 0; t < 4; ++t) {
            PlayerInfo p;
            bool ok = t != net_my_slot() && net_player_info(t, p);
            if (ok)
                logf("revive: B held: place %d present %d down %d area %d (mine %d) at (%.0f, %.0f), me at (%.0f, %.0f)", t,
                     p.present, p.downed, p.stage, net_my_stage(), p.pos[0], p.pos[2], me ? game::at<float>(me, 0x40) : 0.f,
                     me ? game::at<float>(me, 0x48) : 0.f);
        }
    }
    static DWORD last_ok = 0;
    if (target >= 0 && target == g_help_target) {
        last_ok = now | 1;
    } else if (target >= 0) {
        g_help_target = target;
        g_help_since = last_ok = now | 1;
        return;
    } else if (g_help_target >= 0 && b && last_ok && int(now - last_ok) < 500) {
    } else {
        g_help_target = -1;
        g_help_since = 0;
        return;
    }
    if (g_help_since && int(now - g_help_since) >= int(kHelpMs)) {
        logf("revive: helped player %d up", g_help_target);
        net_send_revive(g_help_target);
        g_help_since = 0;
        g_help_target = -1;
    }
}

void revive_helped()
{
    if (g_down && !g_get_up) {
        logf("revive: helped up by another player");
        InterlockedExchange(&g_get_up, 1);
    }
}

void revive_game_tick(uintptr_t arisen)
{
    if (!arisen)
        return;
    if (InterlockedExchange(&g_die, 0)) {
        uintptr_t e = game::at<uintptr_t>(game::at<uintptr_t>(arisen, 0x1D18), 0);
        if (e) {
            game::at<float>(e, 8) = 0.f;
            game::at<uint32_t>(e, 0x110) |= 1;
        }
        InterlockedExchange(&g_down, 0);
        return;
    }
    static DWORD next_look = 0;
    DWORD now = GetTickCount();
    static float stood[2][3] = {};
    static DWORD stood_at = 0;
    static int stood_n = 0;
    int act = game::at<int32_t>(arisen, 0x2DD4);
    if (!g_down && (act == 0 || act == 1 || act == 2) && (!stood_at || int(now - stood_at) >= 1000)) {
        memcpy(stood[0], stood[1], sizeof stood[0]);
        memcpy(stood[1], reinterpret_cast<void *>(arisen + 0x40), sizeof stood[1]);
        stood_at = now | 1;
        ++stood_n;
    }
    if (InterlockedExchange(&g_shore, 0)) {
        const float *here = reinterpret_cast<const float *>(arisen + 0x40);
        const float *to = here;
        for (int k = 0; k < 2 && to == here; ++k) {
            const float *p = stood[k];
            float dx = p[0] - here[0], dy = p[1] - here[1], dz = p[2] - here[2];
            if (stood_n > 1 - k && dx * dx + dy * dy + dz * dz < 2000.f * 2000.f)
                to = p;
        }
        float at[3] = {to[0], to[1], to[2]};
        for (uintptr_t off : {uintptr_t(0x40), uintptr_t(0xE70), uintptr_t(0xE80), uintptr_t(0x1990)})
            memcpy(reinterpret_cast<void *>(arisen + off), at, 12);
        action_set(arisen, 0);
        if (float *hp = enemies_hp(arisen)) {
            float cut = hp[1] > 0.f ? hp[1] * 0.1f : 0.f;
            hp[0] = hp[0] - cut < 1.f ? 1.f : hp[0] - cut;
        }
        logf("revive: back on the shore at (%.0f, %.0f, %.0f)%s", at[0], at[1], at[2], to == here ? " (no place it stood near)" : "");
    }
    if (g_down && InterlockedExchange(&g_move_back, 0) && stood_n > 0) {
        const float *here = reinterpret_cast<const float *>(arisen + 0x40);
        auto close_by = [&](const float *p) {
            float dx = p[0] - here[0], dy = p[1] - here[1], dz = p[2] - here[2];
            return dx * dx + dy * dy + dz * dz < 2000.f * 2000.f;
        };
        const float *to = stood_n >= 2 && close_by(stood[0]) ? stood[0] : close_by(stood[1]) ? stood[1] : nullptr;
        if (!to) {
            logf("revive: no place it stood near (%.0f, %.0f, %.0f): left where it is", here[0], here[1], here[2]);
            to = here;
        }
        memcpy(reinterpret_cast<void *>(arisen + 0x40), to, 12);
        for (uintptr_t off : {uintptr_t(0xE70), uintptr_t(0xE80), uintptr_t(0x1990)})
            memcpy(reinterpret_cast<void *>(arisen + off), to, 12);
        logf("revive: put back where it last stood (%.0f, %.0f, %.0f)", to[0], to[1], to[2]);
    }
    if (g_down) {
        uintptr_t e = game::at<uintptr_t>(game::at<uintptr_t>(arisen, 0x1D18), 0);
        if (e && game::at<float>(e, 8) < 1.f)
            game::at<float>(e, 8) = 1.f;
        if (e && (game::at<uint32_t>(e, 0x110) & 1))
            game::at<uint32_t>(e, 0x110) &= ~1u;
    }
    if (g_down && !g_get_up && (!next_look || int(now - next_look) >= 0)) {
        next_look = (now + 500) | 1;
        if (strcmp(action_name(arisen), "cPlActCmcNeardeath") != 0)
            action_lie_down(arisen);
    }
    LONG how = g_get_up;
    if (!how)
        return;
    if (how == 2 && g_to_stage != net_my_stage()) {
        static DWORD asked = 0;
        if (!asked || int(GetTickCount() - asked) > 15000) {
            asked = GetTickCount() | 1;
            net_stage_jump(g_to_stage, 0);
        }
        return;
    }
    if (how == 2) {
        float to[3] = {g_to[0] + 80.f, g_to[1], g_to[2]};
        memcpy(reinterpret_cast<void *>(arisen + 0x40), to, sizeof to);
        for (uintptr_t off : {uintptr_t(0xE70), uintptr_t(0xE80), uintptr_t(0x1990)})
            memcpy(reinterpret_cast<void *>(arisen + off), to, sizeof to);
    }
    if (float *hp = arisen_hp(arisen))
        hp[0] = hp[1] * kBackShare;
    InterlockedExchange(&g_get_up, 0);
    InterlockedExchange(&g_down, 0);
    if (strcmp(action_name(arisen), "cPlActCmcNeardeath") == 0)
        action_stand_up(arisen);
    logf("revive: up again (%s)", how == 1 ? "helped" : "next to another player");
}
