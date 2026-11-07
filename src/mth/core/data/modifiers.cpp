#include "mth/core/data/modifiers.hpp"

#include "MinaModEnums.h"

namespace mth
{

CheatClass class_of(int idx)
{
    if (idx < 0 || idx >= kCheatCount)
        return CheatClass::Invalid;
    // Checked in ascending index order so the non-overlap of the ranges is obvious.
    if (idx >= 122 && idx <= 125)
        return CheatClass::Randomizer;
    if (idx == 19 || idx == 54 || (idx >= 128 && idx <= 172))
        return CheatClass::Grant;
    if (idx >= 214 && idx <= 253)
        return CheatClass::Combo;
    // Remaining valid indices (0-18, 20-53, 55-121, 126-127, 173-213) write no apply-time field.
    return CheatClass::Continuous;
}

bool is_safe(int idx)
{
    return class_of(idx) == CheatClass::Continuous;
}

bool is_ap_denied(int idx)
{
    if (idx < 0 || idx >= kCheatCount)
        return false; // not a modifier index at all, so there is nothing for the lockdown to refuse

    // Grouped by why the index is denied. Trailing names are the ones the options menu shows,
    // which differ from the kCheat_ enum for a few entries.
    switch (idx)
    {
    case 0:          // High Jump
    case 2:          // Infinite Jump
    case 3:          // 2x Walk Speed
    case 4:          // 4x Walk Speed
    case 5:          // Floatier Jumps
    case 6:          // 2x Burrow Speed
    case 7:          // 4x Burrow Speed
    case 17:         // Grapple Mode, which lets the whip grapple to walls in mid-air
    case 21:         // Walk On Pits
        return true; // reach past what the apworld's logic rules assume
    case 19:         // Hedgehog grants a trinket despite the toggle-shaped name
    case 54:         // EarlyWeapons grants weapons, likewise
    case 55:         // AnyWeaponStarter moves the starter weapon the apworld keys on
    case 86:         // UnlimitedKeys
    case 87:         // FreeLocks
    case 88:         // TrainPass
        return true; // hand out items or bypass the checks that gate them
    case 48:         // NoUnderlab
    case 74:         // NoSidearms
    case 78:         // NoTrinket
    case 107:        // NoBones leaves no currency for the bone-priced shop checks
        return true; // delete AP locations
    case 69:         // SidearmDrop
    case 70:         // SidearmRandom
    case 72:         // SidearmRoulette
        return true; // reroll the equipped sidearm over an AP-granted one
    case 126:
        // SaveSlot::Clear reads bit 126 as the custom-game-mode flag and skips the starting-kit
        // seed when it is set, even though ApplyNewFileCheat has no case for the index.
        return true;
    default:
        break;
    }

    // Ranges, ascending.
    if (idx >= 89 && idx <= 98)
        return true; // the All* blanket unlocks
    if (idx >= 116 && idx <= 120)
        return true; // NoGeneral, NoMerchantWeapon, NoMerchantTrinket, NoPawnShop, NoSandwich
    if (idx >= 122 && idx <= 125)
        return true; // the game's own item/warp/sidearm/level shuffle; AP owns placement
    if (idx >= 128 && idx <= 172)
        return true; // every Start*: one-shot save writes that double-grant when re-applied
    // Combos recurse into sub-cheats through ApplyFileCheats and their member lists were never
    // recovered from the binary. 238 and 239 provably pull in the warp shuffle, and 253 sets bit 126.
    return idx >= 214 && idx <= 253;
}

namespace
{

constexpr TrapDef make_trap(int modifier_index, const char *label, float seconds)
{
    return TrapDef{modifier_index, label, seconds};
}

// Builds one row from the modifier's own kCheat_* name, so the label has exactly one source.
// MTH_TRAP(Mirror, 30.0f) is make_trap(kCheat_Mirror, "Mirror", 30.0f):
// the ## paste forms the token kCheat_Mirror, which is then itself macro-expanded to 204,
// and the # stringizes the bare argument. The duration stays a per-row argument so one trap
// can be retuned without touching the others.
#define MTH_TRAP(name, seconds) make_trap(kCheat_##name, #name, seconds)

constexpr TrapDef kTrapTable[] = {
    MTH_TRAP(FlipControls, kDefaultTrapSeconds),      // input axes reversed
    MTH_TRAP(FloorIsLava, kDefaultTrapSeconds),       // floor is lava
    MTH_TRAP(Giant, kDefaultTrapSeconds),             // player render scale
    MTH_TRAP(Giant2, kDefaultTrapSeconds),            // player render scale
    MTH_TRAP(GiantEnemies, kDefaultTrapSeconds),      // enemy render scale
    MTH_TRAP(GiantEnemies2, kDefaultTrapSeconds),     // enemy render scale
    MTH_TRAP(StartInvisible, kDefaultTrapSeconds),    // Mina invisible
    MTH_TRAP(NoHUD, kDefaultTrapSeconds),             // HUD hidden
    MTH_TRAP(RotateCamera, kDefaultTrapSeconds / 5),  // continuous camera roll
    MTH_TRAP(RotateCameraInput, kDefaultTrapSeconds), // camera tilts with horizontal input
    MTH_TRAP(Mirror, kDefaultTrapSeconds),            // scene flipped horizontally, controls flip with it
    MTH_TRAP(Upsidedown, kDefaultTrapSeconds),        // scene flipped vertically
};

#undef MTH_TRAP

} // namespace

std::span<const TrapDef> traps()
{
    return std::span<const TrapDef>(kTrapTable);
}

const TrapDef *trap_for_modifier(int modifier_index)
{
    for (const auto &t : kTrapTable)
        if (t.modifier_index == modifier_index)
            return &t;
    return nullptr;
}

} // namespace mth
