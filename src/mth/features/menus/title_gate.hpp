#pragma once

#include <functional>

namespace mth
{

// Keeps the title-screen cursor off "Start Game" while AP is disconnected; the game has no disabled-option concept, so the cursor position is the gate.
// TitleScreen::UpdateState writes the cursor and dispatches the confirm in one call, so StartGame is also suppressed directly as the backstop.
//
// `claim_start` runs on a connected StartGame confirm and returns whether something took ownership of the launch. false blocks the launch so it never reaches
// the vanilla profile-select.
void install_title_gate(std::function<bool()> connected, std::function<bool()> claim_start);
void remove_title_gate();

} // namespace mth
