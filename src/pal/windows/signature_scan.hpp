#pragma once

#include <cstdint>
#include <span>

#include "mth/core/game/sig_scan.hpp"

namespace pal
{

// Defined in win_signatures.cpp.
std::span<const mth::sig::Entry> sig_table();

// Defined in signature_scan.cpp. Resolves a mangled name by scanning the game module's .text for the table entry's signature; 0 on miss.
std::uintptr_t scan_resolve(const char *mangled_name);

// Resolves a function from the name hash of a mod hook it dispatches. RunHooks inlines that hash as a movabs immediate inside the target, and .pdata maps
// the interior address back to the entry point. The hash comes from the hook name, so it cannot drift between builds. 0 if the constant is missing, not
// unique, or has no .pdata entry.
std::uintptr_t resolve_by_hook_anchor(std::uint64_t hook_hash);

} // namespace pal
