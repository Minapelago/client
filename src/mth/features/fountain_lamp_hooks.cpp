#include "mth/features/fountain_lamp_hooks.hpp"

#include <cstdint>

#include "mth/core/data/game_layout.hpp"
#include "mth/core/data/game_symbols.hpp"
#include "mth/core/fountain_lamps.hpp"
#include "pal/pal_log.hpp"
#include "pal/pal_mem.hpp"
#include "pal/pal_module.hpp"
#include "pal/scoped_hook.hpp"

namespace
{

// HubFountain::Bulb::Update(float dt, bool lit): forces lit=true for AP-granted generator lamps.
// Prototype order (self, dt, lit) matches the mangled Update(float,bool) on both ABIs; never reorder.
// Reads the force-lit mask (bit i => force lamp index i lit) per bulb per frame.
const std::atomic<std::uint32_t> *g_fountain_mask = nullptr;
[[clang::no_destroy]] pal::ScopedHook g_fountain_hook;
void (*g_orig_bulb_update)(void *, float, bool) = nullptr;

void repl_bulb_update(void *self, float dt, bool lit)
{
    if (g_fountain_mask != nullptr && self != nullptr)
    {
        const std::uint32_t idx = pal::at<std::uint32_t>(self, mth::layout::kBulbIndexOff);
        if (idx < static_cast<std::uint32_t>(mth::kGeneratorLampCount) && ((g_fountain_mask->load(std::memory_order_relaxed) >> idx) & 1u))
            lit = true; // force this generator lamp lit; never writes SaveSlot+0x290
    }
    if (g_orig_bulb_update)
        g_orig_bulb_update(self, dt, lit);
}

bool install_fountain_lamp_hook(const std::atomic<std::uint32_t> *lit_mask)
{
    g_fountain_mask = lit_mask;
    const std::uintptr_t addr = pal::resolve_game_symbol(mth::sym::hub_fountain_bulb_update);
    if (addr == 0)
    {
        pal::logf(pal::LogLevel::Warn, "fountain: HubFountain::Bulb::Update not resolved; lamp pre-light off");
        g_fountain_mask = nullptr;
        return false;
    }
    g_fountain_hook = pal::ScopedHook(addr, &repl_bulb_update, &g_orig_bulb_update);
    if (!g_fountain_hook.installed())
    {
        pal::logf(pal::LogLevel::Error, "fountain: failed to hook HubFountain::Bulb::Update");
        g_fountain_mask = nullptr;
        return false;
    }
    pal::logf(pal::LogLevel::Info, "fountain: hooked HubFountain::Bulb::Update at %p", g_fountain_hook.target());
    return true;
}

} // namespace

namespace mth
{

FountainLampHooks::FountainLampHooks()
{
    if (!install_fountain_lamp_hook(&mask_))
        pal::logf(pal::LogLevel::Warn, "FountainLampHooks: HubFountain::Bulb::Update unresolved; lamp pre-light inactive");
}

FountainLampHooks::~FountainLampHooks()
{
    g_fountain_hook.reset();
    g_fountain_mask = nullptr;
}

} // namespace mth
