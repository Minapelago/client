#include "mth/features/fountain_lamp_hooks.hpp"

#include <atomic>
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
// Reads the force-lit masks (bit i => force lamp index i lit) per bulb per frame.
std::atomic<std::uint32_t> g_slot_mask{0};    // slot_data lamps, from the game-thread tick
std::atomic<std::uint32_t> g_console_mask{0}; // sticky console override, from the render thread
[[clang::no_destroy]] pal::ScopedHook g_fountain_hook;
void (*g_orig_bulb_update)(void *, float, bool) = nullptr;

void repl_bulb_update(void *self, float dt, bool lit)
{
    if (self != nullptr)
    {
        const std::uint32_t mask = g_slot_mask.load(std::memory_order_relaxed) | g_console_mask.load(std::memory_order_relaxed);
        const std::uint32_t idx = pal::at<std::uint32_t>(self, mth::layout::kBulbIndexOff);
        if (idx < static_cast<std::uint32_t>(mth::kGeneratorLampCount) && ((mask >> idx) & 1u))
            lit = true; // force this generator lamp lit; never writes SaveSlot+0x290
    }
    if (g_orig_bulb_update)
        g_orig_bulb_update(self, dt, lit);
}

} // namespace

namespace mth
{

void install_fountain_lamp_hook()
{
    const std::uintptr_t addr = pal::resolve_game_symbol(mth::sym::hub_fountain_bulb_update);
    if (addr == 0)
    {
        pal::logf(pal::LogLevel::Warn, "fountain: HubFountain::Bulb::Update not resolved; lamp pre-light off");
        return;
    }
    g_fountain_hook = pal::ScopedHook(addr, &repl_bulb_update, &g_orig_bulb_update);
    if (!g_fountain_hook.installed())
    {
        pal::logf(pal::LogLevel::Error, "fountain: failed to hook HubFountain::Bulb::Update");
        return;
    }
    pal::logf(pal::LogLevel::Info, "fountain: hooked HubFountain::Bulb::Update at %p", g_fountain_hook.target());
}

void remove_fountain_lamp_hook()
{
    g_fountain_hook.reset();
}

void set_fountain_slot_mask(std::uint32_t mask)
{
    g_slot_mask.store(mask, std::memory_order_relaxed);
}

void set_fountain_console_mask(std::uint32_t mask)
{
    g_console_mask.store(mask, std::memory_order_relaxed);
}

} // namespace mth
