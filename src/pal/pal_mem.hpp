#pragma once

#include <cstddef>
#include <cstdint>

namespace pal
{

// RW-protect [addr, addr+len); not restored. Linux page-aligns internally. Drops execute permission, so
// this is for DATA only - never use it on .text (the game will fault executing the de-exec'd page).
bool make_writable(void *addr, std::size_t len);

// Overwrite len bytes of executable code at addr: temporarily add write permission, copy, restore the
// original (executable) protection, and flush the instruction cache. Use for .text patches.
bool patch_code(void *addr, const void *bytes, std::size_t len);

// A pointer that could plausibly be a live game object: a canonical low-half user address.
// NOT a bounds check against the game module (heap objects live outside it) - it just rejects
// null-ish, kernel, and uninitialized-garbage values before we dereference one for a write.
// Identical range on both x64 platforms, so header-only (no per-platform seam impl needed).
[[nodiscard]] inline bool pointer_looks_valid(const void *p) noexcept
{
    const auto v = reinterpret_cast<std::uintptr_t>(p);
    return v >= 0x10000 && v < 0x0000800000000000;
}

// The T at byte offset off from base. No checks of its own: validate base before calling.
template <class T> [[nodiscard]] T &at(void *base, std::ptrdiff_t off) noexcept
{
    return *reinterpret_cast<T *>(static_cast<char *>(base) + off);
}

template <class T> [[nodiscard]] const T &at(const void *base, std::ptrdiff_t off) noexcept
{
    return *reinterpret_cast<const T *>(static_cast<const char *>(base) + off);
}

// Same, for a base held as an address (a resolved game global).
template <class T> [[nodiscard]] T &at(std::uintptr_t base, std::ptrdiff_t off) noexcept
{
    return *reinterpret_cast<T *>(base + off);
}

} // namespace pal
