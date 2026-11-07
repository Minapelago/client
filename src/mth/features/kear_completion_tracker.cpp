#include "mth/features/kear_completion_tracker.hpp"

#include "mth/core/ap/ap_ids.hpp"
#include "mth/core/ap/ap_state.hpp"
#include "mth/core/data/game_layout.hpp"
#include "mth/core/kear_completion.hpp"
#include "pal/pal_game.hpp"
#include "pal/pal_log.hpp"
#include "pal/pal_mem.hpp"

namespace mth
{

void KearCompletionTracker::evaluate(const ApState &state)
{
    if (!state.authenticated())
        return;
    // A seed that does not carry the completion location must never get a stray latch.
    if (!state.is_valid_location(ap_loc_id(kKearCompletionLocIdx)))
        return;
    void *slot = pal::active_save_slot();
    if (slot == nullptr)
        return; // title/menus: nothing durable to write through

    auto &flags = pal::at<std::uint64_t>(slot, layout::kSaveKeyMiserFlagOff);
    const std::uint64_t bit = std::uint64_t{1} << layout::kSaveKeyMiserTradeBit;
    if ((flags & bit) != 0)
        return; // already traded, natively or by an earlier tick

    if (!have_all_kears(state.kear_mode(), state.received_items()))
        return;

    flags |= bit;
    pal::logf(pal::LogLevel::Info, "kear: all kears held; latched the KeyMiser trade flag (the reward spawns on the next entry to the Kear Institute)");
}

} // namespace mth
