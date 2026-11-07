#pragma once

#include <cstdint>
#include <span>

namespace mth
{

inline constexpr int kCheatCount = 254; // valid modifier indices are 0..253

// Modifiers force-enabled on AP save slots (indices from the verified g_cheats master table).
// Warp Home is always forced; Landing Done (kCheat_StartProgIntro: skip the intro, start at the
// Ossex hub) is forced only when slot_data sets "ossex_start".
inline constexpr int kCheatWarpHome = 121;
inline constexpr int kCheatLandingDone = 128;
inline constexpr int kCheatCheaperBoneUp = 102;
inline constexpr int kCheatUnlockBoneUps = 106;

// Classification of a modifier index, from decompiling ApplyNewFileCheat (switch(idx-0x13)).
enum class CheatClass
{
    Invalid,    // idx outside 0..253
    Continuous, // no apply-time field write; safe to set/toggle live
    Grant,      // one-shot irreversible save mutation (idx 19, 54, 128..172)
    Randomizer, // sets randomize flags; shuffle runs in ActivateSaveSlot (idx 122..125)
    Combo,      // recurses into sub-cheats via ApplyFileCheats (idx 214..253)
};

[[nodiscard]] CheatClass class_of(int idx);

// Continuous == safe to force on a save and toggle live. Returns false for invalid indices.
[[nodiscard]] bool is_safe(int idx);

// The AP lockdown's deny list: modifiers a randomized run cannot survive. Everything outside it
// stays the player's to pick, difficulty and cosmetic sets included.
[[nodiscard]] bool is_ap_denied(int idx);

// Sets or clears one bit of the 254-entry modifier ("cheat") bitmask, stored as four 64-bit words
// covering indices 0..63, 64..127, 128..191 and 192..255. Both builds use this layout for the
// runtime mask at CheatManager+0x20.
//
// Total over int: an idx outside 0..255, the full range the four-word array can address, does
// nothing. A caller enforcing the narrower real cheat count (0..253) still has to clear that bound
// itself.
inline void cheat_mask_set(std::uint64_t *mask, int idx, bool on)
{
    if (idx < 0 || idx >= 256)
        return;
    const std::uint64_t bit = std::uint64_t{1} << (idx & 63);
    if (on)
        mask[idx >> 6] |= bit;
    else
        mask[idx >> 6] &= ~bit;
}

// One trap: the modifier it forces on, an operator-facing label derived from that modifier's
// kCheat_* enum name (kCheat_Mirror becomes "Mirror"), and how long it lasts in gameplay seconds.
// The label is for our own logs and status output, not the game's menu text: the randomizer names
// the item for the player, so the mod does not need to carry a second copy of that name.
struct TrapDef
{
    int modifier_index;
    const char *label;
    float seconds;
};

// Every trap runs this long for now. Each row carries its own duration so tuning one trap later is
// a table edit, and so a slot_data duration can multiply a per-trap baseline.
inline constexpr float kDefaultTrapSeconds = 30.0f;

// Each row's modifier_index is also the AP item id offset for that trap (kTrapItemBase + index), and
// the randomizer hands those same ids out, so removing a row or renumbering one retires or repoints
// an id a seed may already carry. Adding a row is safe; changing an existing one is not.
//
// The curated trap set. Curation is deliberate: being a good trap is narrower than either
// is_safe or !is_ap_denied, because a modifier also has to be read every frame (so the effect lands
// without a room change) and has to be worth the interruption. The unit test pins the two
// predicates so the curation cannot drift into something that breaks seed logic.
//
// Deliberately excluded: the prank set (179 to 182), Large Apple (206), Customize Flowers (213) and
// No Wind (175) are read at room load, so a trap would not show until the player walks through a
// door. The color and weather set (185 to 188, 199, 212) reads a value field in the save slot rather
// than a mask bit, which would put save writes back into the path.
[[nodiscard]] std::span<const TrapDef> traps();

// Null when that modifier is not a trap.
[[nodiscard]] const TrapDef *trap_for_modifier(int modifier_index);

} // namespace mth
