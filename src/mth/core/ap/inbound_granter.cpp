#include "mth/core/ap/inbound_granter.hpp"

#include <cstdio>

#include "mth/core/ap/ap_ids.hpp"
#include "mth/core/ap/ap_state.hpp"
#include "mth/core/ap/item_granter_interface.hpp"
#include "mth/core/save/ap_save_state.hpp"
#include "pal/pal_log.hpp"

namespace mth
{

// ~10s of gameplay ticks: past a room load or spawn window, and soon enough to log while the player is still in that session.
constexpr int kStuckTickWarn = 600;

InboundGranter::InboundGranter(IItemGranter &granter, ApState &state, ApSaveState &save, std::function<bool()> credit_kear_key,
                               std::function<TrapArm(int)> arm_trap)
    : granter_(granter), state_(state), save_(save), credit_kear_key_(std::move(credit_kear_key)), arm_trap_(std::move(arm_trap)),
      alive_(std::make_shared<bool>(true))
{
    granter_.set_applied_sink(
        [this, alive = alive_](int receipt)
        {
            if (*alive)
                on_applied(receipt);
        });
}

InboundGranter::~InboundGranter()
{
    *alive_ = false;
}

// Reached only after the item is applied, so a queue that dies before its drain window marks nothing and the receipt retries (#175).
void InboundGranter::on_applied(int receipt)
{
    in_flight_.erase(receipt);
    consume(receipt);
}

// The one place a grant is recorded.
void InboundGranter::consume(int index)
{
    save_.mark_granted(index);
    save_.stage();
}

bool InboundGranter::handled(int index) const
{
    return save_.is_granted(index) || in_flight_.count(index) != 0;
}

bool InboundGranter::offer(int game_type, int index)
{
    in_flight_.insert(index); // before the call: grant() may ack synchronously
    if (granter_.grant(game_type, index))
        return true;
    in_flight_.erase(index);
    return false;
}

void InboundGranter::tick()
{
    WeaponTally weapon_tally; // running per-family receipt count -> progressive tier (shared with the ownership clamp)
    int fishing_tier = 0;     // running fishing-rod receipt count -> progressive tier
    int map_tier = 0;
    bool trap_stalled = false; // a trap returned NotReady this tick; drives the stall warning below

    const bool vanilla_kear = state_.config().kear_mode == KearMode::Vanilla;

    int n_handled = 0;     // skipped: already recorded or in flight
    int n_ungrantable = 0; // skipped: this path does not grant that category

    // One progressive receipt at the given tier; a tier past the top is consumed so it doesn't retry forever. False when the granter isn't ready,
    // which ends the pass (tiers are recomputed next tick). A negative family is left out of the log.
    auto offer_tier = [&](int index, int game_type, int tier, const char *label, int family = -1)
    {
        if (handled(index))
        {
            ++n_handled;
            return true;
        }
        char what[48];
        if (family < 0)
            std::snprintf(what, sizeof(what), "%s tier=%d", label, tier);
        else
            std::snprintf(what, sizeof(what), "%s family=%d tier=%d", label, family, tier);
        if (game_type < 0)
        {
            pal::logf(pal::LogLevel::Warn, "inbound_granter: %s exceeds max; ignored (index=%d)", what, index);
            consume(index);
            return true;
        }
        if (!offer(game_type, index))
            return false;
        pal::logf(pal::LogLevel::Info, "inbound_granter: %s -> itemType=%d (index=%d) queued", what, game_type, index);
        return true;
    };

    for (const auto &it : state_.received_items())
    {
        // Vanilla kear mode (#130): an itemType grant can't raise the usable-key count (an inbound replay uses slot=-1, aliasing every kear onto
        // bit 63), so the injected effect lowers the spent-counter by one per receipt, marked like a grant.
        if (vanilla_kear && is_vanilla_kear_item(it.item_id))
        {
            if (handled(it.index))
            {
                ++n_handled;
                continue;
            }
            if (!credit_kear_key_ || !credit_kear_key_())
                break; // no live save/player yet; retry next tick (do not mark)
            // Applies inline (no granter queue), so it is recorded right away.
            consume(it.index);
            pal::logf(pal::LogLevel::Info, "inbound_granter: credited vanilla kear key (index=%d)", it.index);
            continue;
        }

        // Progressive items: the Nth receipt grants the Nth tier's itemType (per family for weapons). Count
        // every receipt (already-granted ones too) so the tier survives reloads; grant only the new ones.
        if (is_fishing_rod_item(it.item_id))
        {
            const int tier = ++fishing_tier;
            if (!offer_tier(it.index, fishing_rod_itemtype(tier), tier, "fishing rod"))
                break;
            continue;
        }
        if (is_weapon_item(it.item_id))
        {
            const int fam = weapon_family(it.item_id);
            const int tier = weapon_tally.add(it.item_id);
            if (!offer_tier(it.index, weapon_itemtype(fam, tier), tier, "weapon", fam))
                break;
            continue;
        }
        if (is_map_item(it.item_id))
        {
            const int tier = ++map_tier;
            if (!offer_tier(it.index, map_itemtype(tier), tier, "map"))
                break;
            continue;
        }

        // Traps are one-shot edges with a lifetime, so they can't be re-derived from the receipt list each tick; they use the marked-granted path.
        if (is_trap_item(it.item_id))
        {
            if (handled(it.index))
            {
                ++n_handled;
                continue;
            }
            const int modifier_index = trap_modifier_index(it.item_id);
            const TrapArm result = arm_trap_ ? arm_trap_(modifier_index) : TrapArm::Unavailable;
            if (result == TrapArm::NotReady)
            {
                // Modifier write refused this frame; retry next tick unmarked. Traps have no tier order and say nothing about the granter, so later
                // receipts keep going.
                trap_stalled = true;
                continue;
            }
            if (result == TrapArm::Unavailable)
                pal::logf(pal::LogLevel::Warn, "inbound_granter: trap id=%lld (modifier %d) cannot run here; consuming it (index=%d)",
                          static_cast<long long>(it.item_id), modifier_index, it.index);
            else
                pal::logf(pal::LogLevel::Info, "inbound_granter: trap id=%lld -> modifier %d (index=%d) %s", static_cast<long long>(it.item_id), modifier_index,
                          it.index, result == TrapArm::Armed ? "armed" : "skipped");
            // Consumed at arm time: a crash before the next game save replays the trap (a double prank), where marking at expiry would lose it.
            consume(it.index);
            continue;
        }

        // Non-vanilla/non-weapon ids are handled elsewhere (stat-caps, kear) or unhandled; capacity upgrades are vanilla ids applied by UpgradeState
        // (popcount bits).
        if (!is_vanilla_game_item(it.item_id) || is_capacity_upgrade_item(it.item_id))
        {
            ++n_ungrantable;
            continue;
        }
        if (handled(it.index)) // already granted or still in flight: silent (runs every tick)
        {
            ++n_handled;
            continue;
        }
        const int game_type = game_item_type(it.item_id);
        if (!offer(game_type, it.index))
        {
            break; // not available now; retry next tick (do not mark)
        }
        pal::logf(pal::LogLevel::Info, "inbound_granter: queued grant index=%d id=%lld (type=%d)", it.index, static_cast<long long>(it.item_id), game_type);
    }

    // Census of what this pass declined to grant, logged when it changes. Per-item skips are silent, so a stream yielding far fewer grants than
    // receipts is otherwise invisible (#175).
    const int n_in_flight = static_cast<int>(in_flight_.size());
    if (n_handled != last_handled_ || n_ungrantable != last_ungrantable_ || n_in_flight != last_in_flight_)
    {
        pal::logf(pal::LogLevel::Debug, "inbound_granter: %zu receipt(s): %d already handled, %d not grantable here, %d in flight",
                  state_.received_items().size(), n_handled, n_ungrantable, n_in_flight);
        last_handled_ = n_handled;
        last_ungrantable_ = n_ungrantable;
        last_in_flight_ = n_in_flight;
    }

    // Warn once per stuck episode when accepted grants never drain: receipts stay unmarked and retry, but nothing lands and the census goes quiet.
    if (in_flight_.empty())
    {
        stuck_ticks_ = 0;
        stuck_warned_ = false;
    }
    else if (++stuck_ticks_ >= kStuckTickWarn && !stuck_warned_)
    {
        pal::logf(pal::LogLevel::Warn, "inbound_granter: %d grant(s) accepted but not applied after %d ticks; is the World::Update drain running?", n_in_flight,
                  stuck_ticks_);
        stuck_warned_ = true;
    }

    // A trap that can't arm doesn't block the receipt list, so latch its stall like the queue above; clear when one arms.
    if (!trap_stalled)
    {
        trap_notready_ticks_ = 0;
        trap_notready_warned_ = false;
    }
    else if (++trap_notready_ticks_ >= kStuckTickWarn && !trap_notready_warned_)
    {
        pal::logf(pal::LogLevel::Warn, "inbound_granter: a trap has not armed after %d ticks; still retrying", trap_notready_ticks_);
        trap_notready_warned_ = true;
    }
}

} // namespace mth
