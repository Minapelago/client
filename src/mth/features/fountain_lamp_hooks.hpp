#pragma once

#include <cstdint>

namespace mth
{

// HubFountain::Bulb::Update detour that forces chosen Ossex fountain lamps lit (visual only; never writes SaveSlot+0x290). Lamp i is forced when bit i is set
// in either mask: the slot_data mask from the tick, or the sticky console override, which works offline. Game thread only, like the detour.
void install_fountain_lamp_hook();
void set_fountain_slot_mask(std::uint32_t mask);
void set_fountain_console_mask(std::uint32_t mask);

} // namespace mth
