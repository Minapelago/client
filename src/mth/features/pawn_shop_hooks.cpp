#include "mth/features/pawn_shop_hooks.hpp"

#include <cstddef>
#include <cstdint>
#include <utility>

#include "mth/core/data/game_symbols.hpp"
#include "pal/pal_log.hpp"
#include "pal/pal_mem.hpp"
#include "pal/pal_module.hpp"
#include "pal/scoped_hook.hpp"

namespace
{

// Pawnty (PawnShopNPC::OnNPCEvent) disable. event 0x1f is InteractComponent::IsInteractable's veto
// query: a nonzero float at info+0x8 means "not interactable" -> no prompt. No-op everything else so
// no dialogue line is set and the sell menu never opens.
constexpr unsigned kPawnInteractableQueryEvent = 0x1f;
constexpr std::ptrdiff_t kPawnVetoFloatOff = 0x8; // InteractEventInfo veto float
std::function<bool()> g_pawn_disable;
[[clang::no_destroy]] pal::ScopedHook g_pawn_hook;
void (*g_orig_pawn_npc)(void *, unsigned, void *) = nullptr;

void repl_pawn_npc(void *self, unsigned event, void *info)
{
    if (g_pawn_disable && g_pawn_disable())
    {
        if (event == kPawnInteractableQueryEvent && info != nullptr)
            pal::at<float>(info, kPawnVetoFloatOff) = 1.0f;
        return; // swallow dialogue/menu/can-interact; never call the original
    }
    if (g_orig_pawn_npc)
        g_orig_pawn_npc(self, event, info);
}

} // namespace

namespace mth
{

// When disable() returns true the detour no-ops every event and vetoes the interactable query (no
// prompt); otherwise it calls through.
void install_pawn_shop_hook(std::function<bool()> disable)
{
    g_pawn_disable = std::move(disable);
    const std::uintptr_t addr = pal::resolve_game_symbol(mth::sym::pawn_shop_on_npc_event);
    if (addr == 0)
    {
        pal::logf(pal::LogLevel::Warn, "pawnty: PawnShopNPC::OnNPCEvent not resolved; pawn-shop disable off");
        g_pawn_disable = nullptr;
        return;
    }
    g_pawn_hook = pal::ScopedHook(addr, &repl_pawn_npc, &g_orig_pawn_npc);
    if (!g_pawn_hook.installed())
    {
        pal::logf(pal::LogLevel::Error, "pawnty: failed to hook PawnShopNPC::OnNPCEvent");
        g_pawn_disable = nullptr;
        return;
    }
    pal::logf(pal::LogLevel::Info, "pawnty: hooked PawnShopNPC::OnNPCEvent at %p", g_pawn_hook.target());
}

void remove_pawn_shop_hook()
{
    g_pawn_hook.reset();
    g_pawn_disable = nullptr;
}

} // namespace mth
