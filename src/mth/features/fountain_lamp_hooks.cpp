#include "mth/features/fountain_lamp_hooks.hpp"

#include <cstdint>

#include "mth/core/game/game_layout.hpp"
#include "mth/core/game/game_symbols.hpp"
#include "mth/core/rules/goal.hpp"
#include "pal/pal_log.hpp"
#include "pal/pal_mem.hpp"
#include "pal/pal_module.hpp"
#include "pal/scoped_hook.hpp"

namespace
{

// HubFountain::Bulb::Update(float dt, bool lit): forces lit for AP-granted generator lamps (bit i of either mask lights lamp i), read per bulb per frame. The
// prototype order (self, dt, lit) matches the mangled Update(float,bool) on both ABIs; keep it.
std::uint32_t g_slot_mask{0};    // slot_data lamps
std::uint32_t g_console_mask{0}; // sticky console override
[[clang::no_destroy]] pal::ScopedHook g_fountain_hook;
void (*g_orig_bulb_update)(void *, float, bool) = nullptr;

void repl_bulb_update(void *self, float dt, bool lit)
{
    if (self != nullptr)
    {
        const std::uint32_t mask = g_slot_mask | g_console_mask;
        const std::uint32_t idx = pal::at<std::uint32_t>(self, mth::layout::kBulbIndexOff);
        if (idx < static_cast<std::uint32_t>(mth::kGeneratorCount) && ((mask >> idx) & 1u))
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

void set_fountain_slot_mask(std::uint32_t mask)
{
    g_slot_mask = mask;
}

void set_fountain_console_mask(std::uint32_t mask)
{
    g_console_mask = mask;
}

} // namespace mth
