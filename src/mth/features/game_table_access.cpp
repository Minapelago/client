#include "mth/features/game_table_access.hpp"

#include "mth/core/game/game_layout.hpp"
#include "mth/core/game/game_symbols.hpp"
#include "pal/pal_log.hpp"
#include "pal/pal_mem.hpp"
#include "pal/pal_module.hpp"

namespace
{

std::uintptr_t g_s_r_items = 0;
std::uintptr_t g_s_r_item_collection = 0;
bool g_resolved = false;
bool g_dummy_patched = false;

std::uintptr_t item_row(int item_type)
{
    return g_s_r_items + static_cast<std::uintptr_t>(item_type) * mth::layout::kItemEntryStride;
}

std::uintptr_t coll_row(int idx)
{
    return g_s_r_item_collection + static_cast<std::uintptr_t>(idx) * mth::layout::kCollectionEntryStride;
}

// Item type placed at a collection slot, or -1 when out of range or unresolved.
int location_item_type(int loc_idx)
{
    if (loc_idx < 0 || loc_idx >= mth::layout::kLocationCount || g_s_r_item_collection == 0)
        return -1;
    return *reinterpret_cast<const int *>(coll_row(loc_idx) + mth::layout::kCollectionItemTypeOff);
}

} // namespace

namespace mth::tables
{

void resolve()
{
    if (g_resolved)
        return;
    g_resolved = true;
    g_s_r_items = pal::resolve_game_symbol(sym::s_r_items);
    if (g_s_r_items == 0)
        pal::logf(pal::LogLevel::Warn, "tables: s_rItems not found; item kind will log as -1 (grants still work)");
    g_s_r_item_collection = pal::resolve_game_symbol(sym::s_r_item_collection);
    if (g_s_r_item_collection == 0)
        pal::logf(pal::LogLevel::Warn, "tables: s_rItemCollection not found; location-kind lookups disabled");
}

bool collection_resolved()
{
    return g_s_r_item_collection != 0;
}

int storage_kind(int item_type)
{
    if (item_type < 0 || item_type >= layout::kItemTypeCount || g_s_r_items == 0)
        return -1; // out of range or unclassifiable
    return *reinterpret_cast<const int *>(item_row(item_type) + layout::kItemKindOff);
}

int native_location_kind(int loc_idx)
{
    return storage_kind(location_item_type(loc_idx)); // storage_kind(-1) is -1
}

bool is_durable_bit_kind(int kind)
{
    return kind == 8 || kind == 12 || kind == 19;
}

bool is_capacity_upgrade_location(int loc_idx)
{
    const int item_type = location_item_type(loc_idx);
    return item_type >= 0x44 && item_type <= 0x48; // Magic/Health/Spark/Vial/Trinket upgrade pieces
}

// Name-scan accessors bound at kCollectionScanCap (0x168), intentionally NOT kLocationCount (361): mirrors SetSaveUnlocked's scan.
std::uint64_t collection_name_key(int idx)
{
    if (idx < 0 || idx >= layout::kCollectionScanCap || g_s_r_item_collection == 0)
        return 0;
    return *reinterpret_cast<const std::uint64_t *>(coll_row(idx) + layout::kCollectionNameKeyOff);
}

int collection_warp_remap(int idx)
{
    if (idx < 0 || idx >= layout::kCollectionScanCap || g_s_r_item_collection == 0)
        return -1;
    return *reinterpret_cast<const int *>(coll_row(idx) + layout::kCollectionWarpRemapOff);
}

int collection_slot_for_name_key(std::uint64_t name_key)
{
    int matched = -1;
    for (int i = 0; i < layout::kCollectionScanCap; ++i)
    {
        if (collection_name_key(i) == name_key)
        {
            matched = i;
            break;
        }
    }
    return warp_resolved_slot(matched, collection_warp_remap(matched)); // remap read is range-guarded, so -1 is safe to pass
}

std::uint8_t collection_bit_index(int slot)
{
    if (slot < 0 || slot >= layout::kLocationCount || g_s_r_item_collection == 0)
        return 0;
    return *reinterpret_cast<const std::uint8_t *>(coll_row(slot) + layout::kCollectionBitIdxOff);
}

void repurpose_dummy_item()
{
    if (g_dummy_patched)
        return;
    if (g_s_r_items == 0)
    {
        pal::logf(pal::LogLevel::Warn, "dummy: s_rItems unresolved, AP pickups keep vanilla visual");
        return;
    }
    const std::uintptr_t dst = item_row(layout::kApDummyItemType);
    const std::uintptr_t src = item_row(layout::kDummyAssetDonor);

    if (!pal::make_writable(reinterpret_cast<void *>(dst), static_cast<std::size_t>(layout::kItemEntryStride)))
    {
        pal::logf(pal::LogLevel::Error, "dummy: make_writable failed; s_rItems[%d] NOT patched", layout::kApDummyItemType);
        return;
    }

    *reinterpret_cast<int *>(dst + layout::kItemKindOff) = 0; // storage-kind None -> no grant
    *reinterpret_cast<const char **>(dst + layout::kItemAtlasOff) = *reinterpret_cast<const char **>(src + layout::kItemAtlasOff);
    *reinterpret_cast<const char **>(dst + layout::kItemAnimOff) = *reinterpret_cast<const char **>(src + layout::kItemAnimOff);
    *reinterpret_cast<const char **>(dst + layout::kItemPaletteOff) = *reinterpret_cast<const char **>(src + layout::kItemPaletteOff);

    g_dummy_patched = true;
    pal::logf(pal::LogLevel::Info, "dummy: s_rItems[%d] -> kind 0, assets from [%d] (atlas=%s anim=%s)", layout::kApDummyItemType, layout::kDummyAssetDonor,
              *reinterpret_cast<const char **>(dst + layout::kItemAtlasOff), *reinterpret_cast<const char **>(dst + layout::kItemAnimOff));
}

bool item_row_shape_ok(int item_type)
{
    if (g_s_r_items == 0 || item_type < 0 || item_type >= layout::kItemTypeCount)
        return false;

    const std::uintptr_t row = item_row(item_type);
    const int kind = *reinterpret_cast<const int *>(row + layout::kItemKindOff);
    if (kind < 0 || kind > 64) // storage kinds are small ints; a drifted read is usually a pointer half
        return false;

    // Each asset field must be a readable, NUL-terminated string of printable characters. An
    // EMPTY string is valid and common: it is how a row says "no palette" (s_rItems[1] ships that
    // way), so requiring a printable first byte would reject intact data.
    constexpr std::size_t kMaxAssetNameLen = 128;
    for (const std::ptrdiff_t off : {layout::kItemAtlasOff, layout::kItemAnimOff, layout::kItemPaletteOff})
    {
        const char *s = *reinterpret_cast<const char *const *>(row + off);
        if (s == nullptr || !pal::pointer_looks_valid(s))
            return false;
        std::size_t i = 0;
        for (; i < kMaxAssetNameLen && s[i] != '\0'; ++i)
        {
            const auto c = static_cast<unsigned char>(s[i]);
            if (c < 0x20 || c > 0x7e)
                return false;
        }
        if (i == kMaxAssetNameLen)
            return false; // no terminator in range: not a string
    }
    return true;
}

bool collection_shape_ok(int sample_count)
{
    if (g_s_r_item_collection == 0)
        return false;

    const int n = sample_count < layout::kLocationCount ? sample_count : layout::kLocationCount;
    for (int i = 0; i < n; ++i)
    {
        const std::uintptr_t row = coll_row(i);
        const int item_type = *reinterpret_cast<const int *>(row + layout::kCollectionItemTypeOff);
        const auto bit = *reinterpret_cast<const unsigned char *>(row + layout::kCollectionBitIdxOff);
        if (item_type < 0 || item_type >= layout::kItemTypeCount)
            return false;
        if (!collection_bit_plausible(bit))
            return false;
    }
    return true;
}

} // namespace mth::tables
