#pragma once
#include <cstdint>

#include "addr_gog.h"

namespace game {

enum Build { BUILD_UNKNOWN = 0, BUILD_STEAM = 1, BUILD_GOG = 2 };
constexpr uint32_t kStampSteam = 0x5A314C31, kStampGog = 0x5936ABAB;
Build build();
const char *build_name();
uintptr_t va(uintptr_t steam);
inline uintptr_t by_build(uintptr_t steam, uintptr_t gog) { return build() == BUILD_GOG ? gog : steam; }

inline const uintptr_t kUpdateCommands = game::va(0x00B6AEA0);

inline const uintptr_t kInput = game::va(0x018D8BB0);
inline const uintptr_t kKeyboard = game::va(0x018D0FAC);
inline const uintptr_t kMouse = game::va(0x018D103C);

inline const uintptr_t kVtPlayer = game::va(0x015E90D0);
inline const uintptr_t kVtCmc = game::va(0x015E8468);
inline const uintptr_t kVtPlayerBase = game::va(0x015EA430);
inline const uintptr_t kVtNpc = game::va(0x015F1AE0);
inline const uintptr_t kEnemyDeath = game::va(0x00AAB3B0);
inline const uintptr_t kArea = game::va(0x018D099C);
constexpr uintptr_t kAreaCurrent = 0x3834;
constexpr uintptr_t kFieldOriginPos = 0x110;
inline const uintptr_t kGameMain = game::va(0x018FA4BC);
constexpr uintptr_t kGsEvent = 0x9E3A8, kGsDay = 0xB8760, kGsDayMs = 0xB8768, kGsHour = 0xB876C,
                    kGsMinute = 0xB8770, kGsSecond = 0xB8774, kGsTimeSpeed = 0xB877C;
inline const uintptr_t kStageLoad = game::va(0x004FBD50);
inline const uintptr_t kApplyDamage = game::va(0x00776F50);
inline const uintptr_t kEnemyFinalizeRet = game::va(0x00AA9A08);

constexpr uintptr_t kCmd = 0x32B4;
constexpr uintptr_t kCmdMode = 0x60;
constexpr uintptr_t kCmdHeld = 0x30;
constexpr uintptr_t kCmdPress = 0x2C;

constexpr uintptr_t kCmcId = 0x5854;
constexpr uintptr_t kCmcPlayer = 0x585C;

inline const uintptr_t kPadInfo = by_build(0x1774, 0x1764);
constexpr uintptr_t kPadInfoSize = 0x2E8;

template <class T> inline T &at(uintptr_t base, uintptr_t off) { return *reinterpret_cast<T *>(base + off); }

}
