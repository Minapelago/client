#pragma once

#include <cstdint>

#include "mth/core/game/game_tables.hpp"

namespace mth::tables
{

// Resolved s_rItems / s_rItemCollection accessors, shared by the hook modules.
// resolve() is idempotent (each consumer's ctor calls it); every accessor is safe
// while unresolved (returns -1 / 0 / no-ops).
void resolve();

[[nodiscard]] bool collection_resolved();

// storage-kind int of an itemType row; -1 if out of range or unresolved.
[[nodiscard]] int storage_kind(int item_type);

// storage-kind of a location's VANILLA contents (the native reload gate keys on vanilla).
[[nodiscard]] int native_location_kind(int loc_idx);

// Bitfield-only kinds: SetItemCollected is side-effect-free for these (8=key, 12=bonestone, 19=fish).
// Kinds 1/9/11 write a global "have item" bit and are excluded; QueueDestroy handles them instead.
[[nodiscard]] bool is_durable_bit_kind(int kind);

// Capacity-upgrade location: vanilla contents itemType in 0x44..0x48 (Magic/Health/Spark/Vial/Trinket
// piece). IsItemCollected for these reads the same SaveSlot bitfield apply_upgrades repurposes as a
// capacity counter, so a per-location collected query is aliased; the mod overrides it (issue #8).
[[nodiscard]] bool is_capacity_upgrade_location(int loc_idx);

// s_rItemCollection row reads (0 / -1 when unresolved or out of range).
[[nodiscard]] std::uint64_t collection_name_key(int idx);
[[nodiscard]] int collection_warp_remap(int idx); // <0 = no remap
[[nodiscard]] std::uint8_t collection_bit_index(int slot);

// locked Chest): scan s_rItemCollection for the key, then warp-resolve the hit. -1 if unmatched or
// unresolved. Mirrors KeyBlock::SetSaveUnlocked's own scan.
[[nodiscard]] int collection_slot_for_name_key(std::uint64_t name_key);

// Patch s_rItems[kApDummyItemType]: kind 0 (no-op grant) + sprite assets from the donor row.
// Idempotent; best-effort (skipped + logged if s_rItems unresolved or mprotect fails).
void repurpose_dummy_item();

// Startup validation for the AP gate. Read-only, and must run before repurpose_dummy_item()
// patches s_rItems[kApDummyItemType], or they validate our own write instead of the game's data.

// Plausible storage kind, and asset pointers that are non-null and reach a NUL-terminated
// printable string. An EMPTY string counts: that is how a row says "no palette".
// repurpose_dummy_item already assumes this shape (it logs the pointers with %s).
[[nodiscard]] bool item_row_shape_ok(int item_type);

// itemType in range and a plausible bit index across the first `sample_count` rows.
[[nodiscard]] bool collection_shape_ok(int sample_count);

} // namespace mth::tables
