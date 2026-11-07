// What the mod lets the player keep or use: stat caps, wallet, upgrades, weapons, train lines, abilities.
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "mth/core/ap/ability_ids.hpp"
#include "mth/core/ap/ap_events.hpp"
#include "mth/core/ap/ap_ids.hpp"
#include "mth/core/ap/ap_state.hpp"
#include "mth/core/game/game_layout.hpp"
#include "mth/core/rules/ability_gate.hpp"
#include "mth/core/rules/burrow_boundary.hpp"
#include "mth/core/rules/progression.hpp"

using mth::Ability;
using mth::BoundaryAction;
using mth::BurrowBoundaryGate;

namespace
{
// ApState is non-copyable/non-movable; populate in-place.
void make_state(mth::ApState &s, const std::vector<std::int64_t> &item_ids)
{
    int idx = 0;
    for (auto id : item_ids)
        s.apply(mth::ApItemReceived{{id, idx++, 1, 0}});
}

constexpr std::int64_t kMagic = mth::kUpgradeItemBase + 0;
constexpr std::int64_t kHealth = mth::kUpgradeItemBase + 1;
constexpr std::int64_t kTrinket = mth::kUpgradeItemBase + 4;

bool any_authorized(const mth::WeaponTally &tally)
{
    std::uint32_t authorized[mth::kWeaponFamilyCount]{};
    for (int fam = 0; fam < mth::kWeaponFamilyCount; ++fam)
        authorized[fam] = tally.owned_mask(fam);
    return mth::any_weapon_authorized(authorized);
}

// Hold one water reading for several seconds of polls, stopping at the first action.
BoundaryAction settle(BurrowBoundaryGate &g, bool deep, bool burrow_blocked, bool swim_blocked)
{
    BoundaryAction act = BoundaryAction::None;
    for (int i = 0; i < 300 && act == BoundaryAction::None; ++i)
        act = g.observe(deep, burrow_blocked, swim_blocked);
    return act;
}

// The annotation edits the title line in place: everything from the first newline on survives and the title only gains a suffix.
void require_first_line_grew(const std::string &in, const std::string &out)
{
    const std::size_t in_eol = in.find('\n');
    const std::size_t out_eol = out.find('\n');
    REQUIRE(out.substr(out_eol == std::string::npos ? out.size() : out_eol) == in.substr(in_eol == std::string::npos ? in.size() : in_eol));

    const std::string in_head = in.substr(0, in_eol);
    const std::string out_head = out.substr(0, out_eol);
    REQUIRE(out_head.size() > in_head.size());
    REQUIRE(out_head.starts_with(in_head));
}

// The panel's level widget wraps at a fixed width and renders the overflow above the box. "LVL 9 (14)" rendered on one line and "LVL 14 (14)"
// wrapped, so ten characters is the longest label with in-game evidence.
constexpr std::size_t kPanelFitChars = 10;
} // namespace

TEST_CASE("stat caps: with no receipts every stat is frozen at its start level", "[stat_cap]")
{
    mth::StatCapState caps;
    REQUIRE(caps.enforced_cap(0, 9) == 0);
    REQUIRE(caps.enforced_cap(1, 9) == 0);
    REQUIRE(caps.enforced_cap(2, 9) == 0);
}

TEST_CASE("stat caps: a cap-up item raises only its own stat", "[stat_cap]")
{
    mth::StatCapState caps;
    mth::ApState s;
    make_state(s, {mth::kProgStatCapBase + 0, mth::kProgStatCapBase + 0, mth::kProgStatCapBase + 2, mth::ap_item_id(5), mth::ap_item_id(42)});
    caps.recompute(s);
    REQUIRE(caps.enforced_cap(0, 9) == 2);
    REQUIRE(caps.enforced_cap(1, 9) == 0);
    REQUIRE(caps.enforced_cap(2, 9) == 1);
}

TEST_CASE("stat caps: the all-stat cap-up raises every stat per receipt", "[stat_cap]")
{
    mth::StatCapState caps;
    mth::ApState s;
    make_state(s, {mth::kProgStatCapAllId, mth::kProgStatCapAllId});
    caps.recompute(s);
    REQUIRE(caps.granted(0) == 2);
    REQUIRE(caps.granted(1) == 2);
    REQUIRE(caps.granted(2) == 2);
}

TEST_CASE("stat caps: the enforced cap is the granted count, never above vanilla", "[stat_cap]")
{
    mth::StatCapState caps;
    caps.set_counts(20, 5, 9);
    REQUIRE(caps.enforced_cap(0, 9) == 9);
    REQUIRE(caps.enforced_cap(1, 9) == 5);
    REQUIRE(caps.enforced_cap(2, 9) == 9);
    REQUIRE(caps.enforced_cap(3, 9) == 9);
    REQUIRE(caps.enforced_cap(-1, 9) == 9);
}

TEST_CASE("stat caps: a console-injected cap-up counts like a socket item and leaves the cursor", "[stat_cap]")
{
    mth::ApState s;
    s.apply(mth::ApItemReceived{{mth::kProgStatCapBase + 0, 0, 1, 0}});
    s.inject_received_item(mth::kProgStatCapBase + 0);

    mth::StatCapState caps;
    caps.recompute(s);
    REQUIRE(caps.granted(0) == 2);

    // The injection must not advance the server's item cursor, so a later server item (index 1) still applies.
    s.apply(mth::ApItemReceived{{mth::kProgStatCapBase + 1, 1, 1, 0}});
    caps.recompute(s);
    REQUIRE(caps.granted(0) == 2);
    REQUIRE(caps.granted(1) == 1);
}

TEST_CASE("stat_cap_ceiling: real stats use the slot_data max, others pass vanilla", "[stat_cap]")
{
    REQUIRE(mth::stat_cap_ceiling(0, 30, 14) == 30);
    REQUIRE(mth::stat_cap_ceiling(1, 30, 14) == 30);
    REQUIRE(mth::stat_cap_ceiling(2, 30, 14) == 30);
    // The bone bank (3) and out-of-range stats keep the native vanilla cap.
    REQUIRE(mth::stat_cap_ceiling(3, 30, 14) == 14);
    REQUIRE(mth::stat_cap_ceiling(-1, 30, 14) == 14);
}

TEST_CASE("boneup_fake_capped_stat maxes only the selected stat at its cap while the menu is interactive", "[boneup]")
{
    REQUIRE_FALSE(mth::boneup_fake_capped_stat(false, true, 5, 5));
    REQUIRE(mth::boneup_fake_capped_stat(true, true, 5, 5));
    REQUIRE(mth::boneup_fake_capped_stat(true, true, 6, 5));
    // An unselected stat stays real, else the sentinel leaks into the commit's defense re-apply.
    REQUIRE_FALSE(mth::boneup_fake_capped_stat(true, false, 9, 3));

    // The gate is `raw_level < cap` and the menu renders raw+1, so with a cap of 9 raw 8 is the last buyable step and 10 is where it lands.
    REQUIRE_FALSE(mth::boneup_fake_capped_stat(true, true, 8, 9));
    REQUIRE(mth::boneup_fake_capped_stat(true, true, 9, 9));
    REQUIRE(mth::boneup_display_cap(9) == 10);
}

TEST_CASE("boneup_with_cap_suffix annotates the title line once, whatever the cap was before", "[boneup]")
{
    const std::string desc = "Attack Level 1\nNext level at 175 Bones\nThe power of your main attack.";
    require_first_line_grew(desc, mth::boneup_with_cap_suffix(desc, 3));
    require_first_line_grew("Attack Level 1", mth::boneup_with_cap_suffix("Attack Level 1", 3));

    // An annotated description must read as the vanilla one, or a re-annotation per frame or a cap raised mid-menu stacks suffixes.
    const std::string once = mth::boneup_with_cap_suffix(desc, 3);
    REQUIRE(mth::boneup_with_cap_suffix(once, 3) == once);
    REQUIRE(mth::boneup_with_cap_suffix(mth::boneup_with_cap_suffix(desc, 2), 3) == once);
}

TEST_CASE("status_panel_with_cap_suffix fits every reachable level and cap, without stacking", "[boneup]")
{
    // Levels and caps both top out at the game's absolute ceiling of 99, displayed as level+1.
    for (int level = 1; level <= 100; ++level)
    {
        const std::string label = "LVL " + std::to_string(level);
        for (int cap = 1; cap <= 100; ++cap)
        {
            const std::string annotated = mth::status_panel_with_cap_suffix(label, cap);
            INFO("level " << level << " cap " << cap << " -> " << annotated);
            REQUIRE(annotated.size() <= kPanelFitChars);
            REQUIRE(annotated.size() > label.size());
        }
    }

    const std::string annotated = mth::status_panel_with_cap_suffix("LVL 14", 3);
    REQUIRE(mth::status_panel_with_cap_suffix(annotated, 3) == annotated);
    REQUIRE(mth::status_panel_with_cap_suffix(annotated, 15) == mth::status_panel_with_cap_suffix("LVL 14", 15));
}

TEST_CASE("wallet_cap_for: 750 base plus 500 per wallet, uncapped from the eighth", "[wallet_cap]")
{
    REQUIRE(mth::wallet_cap_for(-1) == 750);
    REQUIRE(mth::wallet_cap_for(0) == 750);
    REQUIRE(mth::wallet_cap_for(1) == 1250);
    REQUIRE(mth::wallet_cap_for(7) == 4250);
    REQUIRE_FALSE(mth::wallet_cap_for(8).has_value());
    REQUIRE_FALSE(mth::wallet_cap_for(100).has_value());
}

TEST_CASE("wallet_cap counts only received wallet items", "[wallet_cap]")
{
    mth::ApState s;
    make_state(s, {mth::kProgWalletId, mth::ap_item_id(5), mth::kProgWalletId, mth::kProgFishingRodId, mth::kMapItem, mth::kProgWalletId});
    REQUIRE(mth::wallet_cap(s) == 2250);

    mth::ApState full;
    make_state(full, std::vector<std::int64_t>(8, mth::kProgWalletId));
    REQUIRE_FALSE(mth::wallet_cap(full).has_value());
}

TEST_CASE("upgrade: counts receipts per type and ignores other items", "[upgrade]")
{
    mth::UpgradeState up;
    mth::ApState s;
    make_state(s, {kHealth, kHealth, kMagic, mth::ap_item_id(5), mth::kProgWeaponBase, mth::kProgStatCapBase});
    up.recompute(s);
    REQUIRE(up.counts()[0] == 1);
    REQUIRE(up.counts()[1] == 2);
    for (int i = 2; i < mth::kUpgradeCount; ++i)
        REQUIRE(up.counts()[i] == 0);
}

TEST_CASE("upgrade: counts saturate at the per-type cap", "[upgrade]")
{
    mth::UpgradeState up;
    mth::ApState s;
    make_state(s, std::vector<std::int64_t>(static_cast<std::size_t>(mth::kUpgradeCaps[4]) + 1, kTrinket));
    up.recompute(s);
    REQUIRE(up.counts()[4] == mth::kUpgradeCaps[4]);
}

TEST_CASE("upgrade: force_dirty re-arms a re-apply that survives recompute", "[upgrade]")
{
    // SaveSlot::Clear zeros the upgrade bitfields without changing the counts, so only force_dirty gets them refilled.
    mth::UpgradeState up;
    mth::ApState s;
    make_state(s, {kHealth});
    up.recompute(s);
    up.mark_applied();
    up.recompute(s);
    REQUIRE_FALSE(up.dirty());

    up.force_dirty();
    up.recompute(s);
    REQUIRE(up.dirty());
    up.mark_applied();
    up.recompute(s);
    REQUIRE_FALSE(up.dirty());
}

TEST_CASE("upgrade_field_value: vials are authoritative, other pools accumulate", "[upgrade]")
{
    // Vanilla seeds 3 vials as SaveSlot+0x18c = 0x7, so OR-ing receipts could never take the count below 3 (#83).
    REQUIRE(mth::upgrade_field_value(mth::kVialUpgradeIndex, 0, 0x7u) == 0x0u);
    REQUIRE(mth::upgrade_field_value(mth::kVialUpgradeIndex, 2, 0x7u) == 0x3u);
    REQUIRE(mth::upgrade_field_value(mth::kVialUpgradeIndex, 10, 0xFFFFu) == 0x3FFu);

    REQUIRE(mth::upgrade_field_value(1, 0, 0x5u) == 0x5u);
    REQUIRE(mth::upgrade_field_value(1, 2, 0x4u) == 0x7u);
}

TEST_CASE("maintained_vial_held preserves the missing flask count across a capacity change", "[upgrade]")
{
    REQUIRE(mth::maintained_vial_held(3, 3, 1) == 1);
    REQUIRE(mth::maintained_vial_held(3, 3, 5) == 5);
    REQUIRE(mth::maintained_vial_held(2, 0, 3) == 1);
    REQUIRE(mth::maintained_vial_held(1, 1, 1) == 1);
    REQUIRE(mth::maintained_vial_held(3, 1, 0) == 0);
    REQUIRE(mth::maintained_vial_held(0, 0, 4) == 4);
}

// The per-tick clamp revokes weapons the game hands out itself (intro chest, no-weapon fallback) unless the tally authorized them.
TEST_CASE("weapon tally: the Nth receipt of a family authorizes its low N tier bits and nothing else", "[weapon]")
{
    mth::WeaponTally tally;
    for (int fam = 0; fam < mth::kWeaponFamilyCount; ++fam)
        CHECK(tally.owned_mask(fam) == 0u);

    CHECK(tally.add(mth::ap_item_id(42)) == 0);
    CHECK(tally.add(mth::kProgStatCapBase) == 0);
    CHECK(tally.add(mth::kProgFishingRodId) == 0);
    CHECK(tally.add(mth::kProgWeaponBase + 2) == 1);
    CHECK(tally.add(mth::kProgWeaponBase + 4) == 1);
    CHECK(tally.add(mth::kProgWeaponBase + 4) == 2);
    CHECK(tally.add(mth::kProgWeaponBase + 4) == 3);
    // A fourth receipt grants no itemType, so it authorizes no fourth bit.
    CHECK(tally.add(mth::kProgWeaponBase + 4) == 4);

    CHECK(tally.owned_mask(2) == 0b001u);
    CHECK(tally.owned_mask(4) == 0b111u);
    CHECK(tally.owned_mask(0) == 0u);
    CHECK(tally.owned_mask(1) == 0u);
    CHECK(tally.owned_mask(3) == 0u);
}

// The intro weapon chest is demoted to its equip-only mode once a weapon is authorized; demoting it earlier leaves an unarmed player.
TEST_CASE("weapon authorization needs one weapon receipt in any family", "[weapon]")
{
    mth::WeaponTally none;
    none.add(mth::ap_item_id(42));
    none.add(mth::kProgStatCapBase);
    none.add(mth::kProgFishingRodId);
    CHECK_FALSE(any_authorized(none));

    for (int fam = 0; fam < mth::kWeaponFamilyCount; ++fam)
    {
        mth::WeaponTally tally;
        tally.add(mth::kProgWeaponBase + fam);
        CHECK(any_authorized(tally));
    }
}

TEST_CASE("item types: only segment 0 ids are vanilla items, and only s_rItems rows are valid types", "[ap_ids]")
{
    REQUIRE(mth::is_vanilla_game_item(mth::ap_item_id(0)));
    REQUIRE_FALSE(mth::is_vanilla_game_item(mth::kProgWeaponBase));
    REQUIRE_FALSE(mth::is_vanilla_game_item(mth::kTrapItemBase));
    REQUIRE_FALSE(mth::is_vanilla_game_item(-1));

    REQUIRE(mth::is_valid_item_type(0));
    REQUIRE(mth::is_valid_item_type(mth::layout::kItemTypeCount - 1));
    // Item segment 0 runs to 999, but only the first kItemTypeCount ids are itemTypes.
    REQUIRE_FALSE(mth::is_valid_item_type(mth::layout::kItemTypeCount));
    REQUIRE_FALSE(mth::is_valid_item_type(999));
    REQUIRE_FALSE(mth::is_valid_item_type(-1));
}

TEST_CASE("train_destination_blocked cancels un-granted ticket lines", "[train]")
{
    // Line 0 (Ossex/HUB, 95) is the free hub, rideable on the Train Pass alone, so it is never blocked.
    REQUIRE_FALSE(mth::train_destination_blocked(0x5f, 0x00));

    REQUIRE(mth::train_destination_blocked(0x60, 0x01));
    REQUIRE_FALSE(mth::train_destination_blocked(0x60, 0x02));

    // Line 4 (99, Coltrane) is always shown in the menu, so the gate must still block it.
    REQUIRE(mth::train_destination_blocked(0x63, 0x00));
    REQUIRE_FALSE(mth::train_destination_blocked(0x63, 0x10));

    // Non-ticket codes are never blocked (100 = Exit/cancel sentinel; a normal item).
    REQUIRE_FALSE(mth::train_destination_blocked(100, 0x00));
    REQUIRE_FALSE(mth::train_destination_blocked(0x5e, 0x00));
    REQUIRE_FALSE(mth::train_destination_blocked(41, 0x00));
}

TEST_CASE("is_train_pass_machine_grant matches only the donation machine's dispense", "[train]")
{
    REQUIRE(mth::is_train_pass_machine_grant(149, 0x5e));

    REQUIRE_FALSE(mth::is_train_pass_machine_grant(-1, 0x5e));
    REQUIRE_FALSE(mth::is_train_pass_machine_grant(149, 0x5f));
    REQUIRE_FALSE(mth::is_train_pass_machine_grant(148, 0x5e));
    REQUIRE_FALSE(mth::is_train_pass_machine_grant(149, 41));
}

TEST_CASE("ticket_machine_seed pre-pays the donation goal down to the configured cost", "[train]")
{
    // Seed + cost equals the compiled-in goal, so the player owes exactly the cost.
    REQUIRE(mth::ticket_machine_seed(1000) == 9000u);
    REQUIRE(mth::ticket_machine_seed(1) == 9999u);

    // Vanilla cost (or more) seeds nothing.
    REQUIRE(mth::ticket_machine_seed(10000) == 0u);
    REQUIRE(mth::ticket_machine_seed(50000) == 0u);

    // A free donation would complete the machine before the player opened it, so zero and negatives floor at one bone owing.
    REQUIRE(mth::ticket_machine_seed(0) == 9999u);
    REQUIRE(mth::ticket_machine_seed(-1) == 9999u);
}

TEST_CASE("AbilityGate blocks only randomized, ungranted abilities on the AP slot", "[ability]")
{
    mth::AbilityGate gate;
    gate.set_randomized(Ability::Burrow, true);

    const std::function<bool(std::int64_t)> granted = [](std::int64_t) { return true; };
    const std::function<bool(std::int64_t)> ungranted = [](std::int64_t) { return false; };

    REQUIRE(gate.blocked(Ability::Burrow, true, ungranted));
    REQUIRE_FALSE(gate.blocked(Ability::Burrow, true, granted));
    REQUIRE_FALSE(gate.blocked(Ability::Burrow, false, ungranted));
    REQUIRE_FALSE(gate.blocked(Ability::Swim, true, ungranted));
}

TEST_CASE("burrow boundary: burrowing into deep water without Swim falls in", "[ability][burrow]")
{
    BurrowBoundaryGate gate;
    gate.arm(false);

    REQUIRE(settle(gate, true, /*burrow_blocked=*/false, /*swim_blocked=*/true) == BoundaryAction::FallIn);
}

TEST_CASE("burrow boundary: swimming onto land without Burrow surfaces the player", "[ability][burrow]")
{
    BurrowBoundaryGate gate;
    gate.arm(true);

    REQUIRE(settle(gate, false, /*burrow_blocked=*/true, /*swim_blocked=*/false) == BoundaryAction::Emerge);
}

// Owning the ability the player crosses into stays silent, including on the crossing after it, which holds only if the gate tracked the mode change.
TEST_CASE("burrow boundary: a granted crossing is silent and updates the tracked mode", "[ability][burrow]")
{
    BurrowBoundaryGate gate;
    gate.arm(false);

    REQUIRE(settle(gate, true, false, /*swim_blocked=*/false) == BoundaryAction::None);
    REQUIRE(settle(gate, false, /*burrow_blocked=*/true, false) == BoundaryAction::Emerge);
}

TEST_CASE("burrow boundary: no crossing means no action", "[ability][burrow]")
{
    BurrowBoundaryGate gate;
    gate.arm(false);

    REQUIRE(settle(gate, false, true, true) == BoundaryAction::None);
    REQUIRE(settle(gate, false, true, true) == BoundaryAction::None);
}

TEST_CASE("burrow boundary: an unarmed gate never acts", "[ability][burrow]")
{
    BurrowBoundaryGate gate;

    REQUIRE(settle(gate, true, true, true) == BoundaryAction::None);
    REQUIRE(settle(gate, false, true, true) == BoundaryAction::None);
}

TEST_CASE("burrow boundary: disarming stops the gate acting on a later crossing", "[ability][burrow]")
{
    BurrowBoundaryGate gate;
    gate.arm(false);
    gate.disarm();

    REQUIRE(settle(gate, true, true, /*swim_blocked=*/true) == BoundaryAction::None);
}

TEST_CASE("burrow boundary: acting disarms so it does not repeat every tick", "[ability][burrow]")
{
    BurrowBoundaryGate gate;
    gate.arm(false);

    REQUIRE(settle(gate, true, false, true) == BoundaryAction::FallIn);
    REQUIRE(settle(gate, true, false, true) == BoundaryAction::None);
}
