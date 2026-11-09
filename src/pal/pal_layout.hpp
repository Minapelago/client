#pragma once

#include <cstddef>
#include <cstdint>

// Game data offsets and code patterns that differ between the Linux (GCC) and Windows (MSVC) builds, so mth/ need not branch on the platform. Layout
// shared by both lives in mth/core/game/game_layout.hpp or next to its user.
namespace pal::layout
{

#if defined(_WIN32)

// The Windows save_manager symbol already holds the live SaveSlot*, and the apply-path slot the Linux
// build keeps separately is the same slot here, so both offsets are 0.
inline constexpr std::ptrdiff_t kSaveManagerApplySlotOff = 0x0;
inline constexpr std::ptrdiff_t kSaveManagerLiveSlotOff = 0x0;
inline constexpr std::ptrdiff_t kSaveManagerSlotIndexOff = 0x8; // int: active 0-based save slot index
// SaveMasterData, holding the vanilla slot array. Linux reaches the same table at +0x28 off a
// differently based global.
inline constexpr std::ptrdiff_t kSaveManagerMasterOff = 0x10;

// TrainAuthority::OnNPCEvent gets a `this` adjusted +0x170 from the Linux struct base (MI thunk), so the
// menu owner sits at +0x40 where Linux reads +0x1b0. The case-0x15 chain is self+0x40 -> +0xc8 -> +0x21c.
inline constexpr std::ptrdiff_t kTrainAuthOwnerOff = 0x40;

// The TitleScreen::UpdateState detour here gets the secondary state-machine base (Ghidra's param_1), which mth::layout::kTitle* are not relative to.
// Linux's "non-virtual thunk to TitleScreen::UpdateState()" subtracts exactly this, and Windows RVA 0xb0a490 reads its cursor at param_1+0x128 where
// Linux reads true-base+0x160.
inline constexpr std::ptrdiff_t kTitleSecondaryBaseAdjust = 0x38;

// ShopMenu::SetupBoxes' always-shown train-line test. MSVC expands the mask into
// `cmp r8d,{0x5e,0x5f,0x63}; je shown`; the 0x63 (line 99, Coltrane Peak) compare makes its box
// selectable regardless of ticket. The patch turns that immediate into 0xff, an impossible itemType, so
// 99 falls through to the SaveSlot+0x1e0 gate while 0x5e/0x5f (board / Ossex-HUB) stay always-shown.
// Anchor: cmp r8d,0x5e;je .. cmp r8d,0x5f;je .. cmp r8d,0x63;je (0x63 at index 15).
inline constexpr std::uint8_t kSetupBoxesPattern[] = {0x41, 0x83, 0xF8, 0x5E, 0x74, 0x00, 0x41, 0x83, 0xF8,
                                                      0x5F, 0x74, 0x00, 0x41, 0x83, 0xF8, 0x63, 0x74, 0x00};
inline constexpr std::uint8_t kSetupBoxesMask[] = {1, 1, 1, 1, 1, 0, 1, 1, 1, 1, 1, 0, 1, 1, 1, 1, 1, 0};
inline constexpr std::size_t kSetupBoxesSiteIndex = 15;
inline constexpr std::uint8_t kSetupBoxesExpected = 0x63;
inline constexpr std::uint8_t kSetupBoxesPatched = 0xFF;

// MSVC folds SpringBellows::CollideWith into a single body, which the exported symbol reaches.
inline constexpr bool kHasSpringListenerBody = false;

// GameAnim::RecalcWorldTransform reads the AnimParent at +0x1e0 (0x1409890e2) and passes the iso block at +0x208 to IsoAnim::CalcTransform
// (0x140989369); AnimEffect::Init writes the mode (0x1404af779).
inline constexpr std::ptrdiff_t kGameAnimParentOff = 0x1e0;   // AnimParent*
inline constexpr std::ptrdiff_t kGameAnimIsoSortOff = 0x208;  // float, added to the sort y
inline constexpr std::ptrdiff_t kGameAnimIsoDepthOff = 0x20c; // float, subtracted from the depth
inline constexpr std::ptrdiff_t kGameAnimIsoModeOff = 0x214;  // int

#else

inline constexpr std::ptrdiff_t kSaveManagerApplySlotOff = 0x08; // apply-path slot (garbage on build 9cd1468c)
inline constexpr std::ptrdiff_t kSaveManagerLiveSlotOff = 0x18;  // live-gameplay slot (the real one)
inline constexpr std::ptrdiff_t kSaveManagerSlotIndexOff = 0x20; // int: active 0-based slot index (confirmed in-game 9cd1468c)
inline constexpr std::ptrdiff_t kSaveManagerMasterOff = 0x28;    // SaveMasterData, holding the vanilla slot array

inline constexpr std::ptrdiff_t kTrainAuthOwnerOff = 0x1b0; // this + 0x1b0 -> menu owner

// The UpdateState detour receives the TitleScreen base itself.
inline constexpr std::ptrdiff_t kTitleSecondaryBaseAdjust = 0x0;

// ShopMenu::SetupBoxes' hardcoded always-shown train-line mask. The menu shows a ticket box when its
// itemType is in a hardcoded set (mask 0x23 = lines 94/95/99) or its SaveSlot+0x1e0 bit is set, which
// makes Coltrane Peak (99) selectable regardless of ticket. The patch drops the mask to 0x03 so only 94
// (board) and 95 (Ossex/HUB) stay unconditional and 96-99 gate on +0x1e0.
// Anchor: lea eax,[rcx-0x5e]; cmp eax,5; ja +0xa; mov edx,0x23; bt edx,eax (0x23 immediate at index 9).
inline constexpr std::uint8_t kSetupBoxesPattern[] = {0x8D, 0x41, 0xA2, 0x83, 0xF8, 0x05, 0x77, 0x0A, 0xBA, 0x23, 0x00, 0x00, 0x00, 0x0F, 0xA3, 0xC2};
inline constexpr std::uint8_t kSetupBoxesMask[] = {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1};
inline constexpr std::size_t kSetupBoxesSiteIndex = 9;
inline constexpr std::uint8_t kSetupBoxesExpected = 0x23;
inline constexpr std::uint8_t kSetupBoxesPatched = 0x03;

// GCC emits SpringBellows::CollideWith twice: the exported body and the copy behind the +376 thunk
// (sym::spring_bellows_collide_listener) that physics dispatches to. Both need the gate (#188).
inline constexpr bool kHasSpringListenerBody = true;

// GameAnim::RecalcWorldTransform reads the AnimParent at +0x200 (0xce3612) and passes the iso block at +0x1d0 to IsoAnim::CalcTransform
// (0xce366e); AnimEffect::Init writes the mode (0x787243).
inline constexpr std::ptrdiff_t kGameAnimParentOff = 0x200;   // AnimParent*
inline constexpr std::ptrdiff_t kGameAnimIsoSortOff = 0x1d0;  // float, added to the sort y
inline constexpr std::ptrdiff_t kGameAnimIsoDepthOff = 0x1d4; // float, subtracted from the depth
inline constexpr std::ptrdiff_t kGameAnimIsoModeOff = 0x1dc;  // int

#endif

static_assert(sizeof(kSetupBoxesPattern) == sizeof(kSetupBoxesMask));
static_assert(kSetupBoxesSiteIndex < sizeof(kSetupBoxesPattern));

} // namespace pal::layout
