#include "mth/features/weapon_grants.hpp"

#include <bit>
#include <cstdint>

#include "mth/core/ap/ap_ids.hpp"
#include "mth/core/game/game_layout.hpp"
#include "mth/core/game/game_tables.hpp"
#include "pal/pal_game.hpp"
#include "pal/pal_log.hpp"
#include "pal/pal_mem.hpp"

// The SaveSlot weapon fields are identical on both builds; the one platform-divergent read, the
// g_saveManager deref, is behind pal::active_save_slot.
namespace mth
{

void clear_starter_weapon_swap(bool authed, bool slot_ok)
{
    static bool logged = false;
    void *slot = pal::active_save_slot();
    if (slot == nullptr)
        return;
    auto &type = pal::at<int>(slot, layout::kSaveStarterWeaponTypeOff);
    if (!tables::should_clear_starter_swap(authed, slot_ok, type))
        return;
    const int prev = type;
    type = -1;
    // A save load re-seeds the field, so this can fire more than once per session; only the first is Info.
    const pal::LogLevel level = logged ? pal::LogLevel::Debug : pal::LogLevel::Info;
    logged = true;
    pal::logf(level, "starter: cleared weapon swap (was type=%d); belowdecks weapon stands restored to vanilla slots", prev);
}

void enforce_weapon_ownership(const std::uint32_t *authorized, bool authed, bool slot_ok)
{
    static bool logged[kWeaponFamilyCount] = {}; // per family: Info on the first correction, Debug on repeats
    static bool warned = false;
    static bool layout_ok = true;
    if (!layout_ok)
        return; // an implausible read already disabled the clamp for the rest of the process
    if (!authed || !slot_ok || authorized == nullptr)
        return; // durable and destructive: bound AP save only (slot_ok alone is true while offline)
    void *slot = pal::active_save_slot();
    if (slot == nullptr)
        return;
    auto *owned = &pal::at<std::uint32_t>(slot, layout::kSaveWeaponOwnedBitsOff);
    auto *active = &pal::at<int>(slot, layout::kSaveWeaponActiveTierOff);

    std::uint32_t authorized_any = 0;
    std::uint32_t owned_any = 0;
    for (int fam = 0; fam < kWeaponFamilyCount; ++fam)
    {
        // Every family is checked before the first write: the clamp rewrites all 40 bytes of the pair each tick (see weapon_fields_in_domain).
        if (!tables::weapon_fields_in_domain(owned[fam], active[fam]))
        {
            layout_ok = false;
            pal::logf(pal::LogLevel::Error,
                      "weapons: family=%d owned=0x%x tier=%d outside the tier domain (mask 0x%x, tier 0..%d); offsets may have shifted, weapon writes DISABLED",
                      fam, owned[fam], active[fam], layout::kWeaponTierBits, std::bit_width(layout::kWeaponTierBits) - 1);
            return;
        }
        authorized_any |= authorized[fam];
        owned_any |= owned[fam] & layout::kWeaponTierBits;
    }
    if (!tables::weapon_clamp_ready(authorized_any, owned_any))
    {
        if (!warned)
            pal::logf(pal::LogLevel::Warn, "weapons: save owns 0x%x but AP has granted nothing yet; ownership clamp skipped until the receipts load",
                      owned_any);
        warned = true;
        return;
    }
    warned = false;

    for (int fam = 0; fam < kWeaponFamilyCount; ++fam)
    {
        const std::uint32_t cur = owned[fam];
        const std::uint32_t want = authorized[fam];
        const int want_tier = tables::weapon_active_bit(want);
        if ((cur & layout::kWeaponTierBits) == want && active[fam] == want_tier)
            continue; // the common case is a pure read
        const pal::LogLevel level = logged[fam] ? pal::LogLevel::Debug : pal::LogLevel::Info;
        logged[fam] = true;
        pal::logf(level, "weapons: family=%d owned 0x%x -> 0x%x, tier %d -> %d (clamped to the AP grants)", fam, cur & layout::kWeaponTierBits, want,
                  active[fam], want_tier);
        owned[fam] = (cur & ~layout::kWeaponTierBits) | want; // anything outside the three tier bits is not ours
        active[fam] = want_tier;
    }
}

} // namespace mth
