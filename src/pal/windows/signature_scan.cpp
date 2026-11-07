#include "pal/windows/signature_scan.hpp"

#include <cstring>
#include <span>
#include <string>
#include <unordered_map>

#include "mod/mod_api.hpp"
#include "mth/core/game/hook_hash.hpp"
#include "mth/core/game/native_sym_names.hpp"
#include "mth/core/game/sig_scan.hpp"
#include "pal/pal_log.hpp"
#include "pal/pal_module.hpp"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace pal
{

// The AP gate logs every required symbol's result, so the signature path is quiet on success. Logged here is what the gate cannot see: which source
// produced a non-signature address, and how a signature failed (miss, ambiguous pattern, out-of-range DataRef).
std::uintptr_t resolve_by_hook_anchor(std::uint64_t hook_hash)
{
    const TextRange tr = game_text_range();
    if (tr.size < sizeof(hook_hash))
        return 0;
    const auto *text = reinterpret_cast<const std::uint8_t *>(tr.base);

    // The immediate must be unique. Several hooks have multiple dispatch sites, either from repeated
    // calls or from the compiler inlining the target, and those cannot anchor anything.
    std::uintptr_t anchor = 0;
    std::uint8_t want[sizeof(hook_hash)];
    std::memcpy(want, &hook_hash, sizeof(want));
    // memchr on the first byte: .text is ~13MB and this runs at init.
    const std::uint8_t *cur = text;
    const std::uint8_t *end = text + tr.size;
    while (cur + sizeof(want) <= end)
    {
        const auto *hit = static_cast<const std::uint8_t *>(std::memchr(cur, want[0], static_cast<std::size_t>(end - cur) - sizeof(want) + 1));
        if (hit == nullptr)
            break;
        if (std::memcmp(hit, want, sizeof(want)) == 0)
        {
            if (anchor != 0)
            {
                logf(LogLevel::Error, "anchor: hook hash 0x%llx is not unique in .text", static_cast<unsigned long long>(hook_hash));
                return 0;
            }
            anchor = tr.base + static_cast<std::size_t>(hit - text);
        }
        cur = hit + 1;
    }
    if (anchor == 0)
        return 0;

    // .pdata turns the interior address into the exact entry point. Anything dispatching a hook is
    // non-leaf by construction, so it has an entry.
    DWORD64 image_base = 0;
    PRUNTIME_FUNCTION rf = RtlLookupFunctionEntry(static_cast<DWORD64>(anchor), &image_base, nullptr);
    if (rf == nullptr || image_base == 0)
    {
        logf(LogLevel::Error, "anchor: no .pdata entry covering 0x%llx", static_cast<unsigned long long>(anchor));
        return 0;
    }

    // Read the entry as its fixed x64 layout {BeginAddress, EndAddress, UnwindInfoAddress}; MSVC and mingw spell the third member differently.
    const auto *entry = reinterpret_cast<const std::uint32_t *>(rf);

    // Follow chained unwind info: the compiler splits cold blocks into fragments whose BeginAddress
    // is the fragment, not the function. Bounded so a malformed chain cannot spin.
    for (int hops = 0; hops < 8; ++hops)
    {
        const auto *info = reinterpret_cast<const std::uint8_t *>(image_base + entry[2]);
        constexpr std::uint8_t kChainInfo = 0x4;
        if (((info[0] >> 3) & 0x1f) != kChainInfo)
            break;
        const std::uint8_t code_count = info[2];
        const std::size_t chain_off = 4 + 2 * ((code_count + 1) & ~1);
        entry = reinterpret_cast<const std::uint32_t *>(info + chain_off);
    }

    return static_cast<std::uintptr_t>(image_base + entry[0]);
}

// Signature-table resolution alone, no cache and no GetSymAddr, for cross-checking one source
// against the other. Returns 0 when the name has no entry, the entry is not `want`, or it misses.
static std::uintptr_t scan_only(const char *mangled_name, mth::sig::Kind want)
{
    const TextRange tr = game_text_range();
    const std::span<const std::uint8_t> text{reinterpret_cast<const std::uint8_t *>(tr.base), tr.size};
    if (text.empty())
        return 0;
    for (const mth::sig::Entry &e : sig_table())
        if (e.kind == want && std::strcmp(e.name, mangled_name) == 0)
            return mth::sig::resolve(text, reinterpret_cast<std::uintptr_t>(text.data()), e);
    return 0;
}

std::uintptr_t scan_resolve(const char *mangled_name)
{
    if (!mangled_name)
        return 0;

    // Not thread-safe; symbol resolution runs single-threaded during init.
    static std::unordered_map<std::string, std::uintptr_t> cache;
    if (auto it = cache.find(mangled_name); it != cache.end())
        return it->second;

    // Ask the game first for the names it exposes, since that address cannot drift. The source is logged for diagnosing game updates.
    if (const char *plain = mth::sym::native_sym_name(mangled_name); plain != nullptr)
    {
        if (void *p = mod::sym_addr(plain); p != nullptr)
        {
            const std::uintptr_t addr = reinterpret_cast<std::uintptr_t>(p);
            // The mod writes through DataRef entries (tables::repurpose_dummy_item patches s_rItems), so a wrong base means stores into arbitrary game
            // memory. Cross-check them against the carve.
            if (const std::uintptr_t scanned = scan_only(mangled_name, mth::sig::Kind::DataRef); scanned != 0 && scanned != addr)
                logf(LogLevel::Error, "sig: %s DISAGREEMENT GetSymAddr=0x%llx scan=0x%llx; using GetSymAddr (check gate item_table_shape_ok)", mangled_name,
                     static_cast<unsigned long long>(addr), static_cast<unsigned long long>(scanned));
            cache[mangled_name] = addr;
            return addr;
        }
    }

    // Then a hook the function dispatches; that hash comes from the hook name, so it survives the codegen churn that breaks a carve.
    if (const char *hook = mth::sym::hook_anchor_for(mangled_name); hook != nullptr)
    {
        if (const std::uintptr_t addr = resolve_by_hook_anchor(mth::hookhash::hash64(hook)); addr != 0)
        {
            logf(LogLevel::Info, "sig: %s via \"%s\" hook anchor -> 0x%llx", mangled_name, hook, static_cast<unsigned long long>(addr));
            cache[mangled_name] = addr;
            return addr;
        }
        logf(LogLevel::Warn, "sig: %s hook anchor \"%s\" did not resolve; trying the signature table", mangled_name, hook);
    }

    // g_saveManager has ~2300 xrefs, too many to carve as a DataRef. The game's only `cmove r9,[rip+g_saveManager]` (4c 0f 44 0d, 8 bytes), the "default
    // a null SaveSlot* to the active slot" idiom, is unique in .text, so scan for it and read its RIP target. A second such cmov makes the scan fail.
    if (std::strcmp(mangled_name, "g_saveManager") == 0)
    {
        std::uintptr_t addr = 0;
        const TextRange tr = game_text_range();
        const std::span<const std::uint8_t> text{reinterpret_cast<const std::uint8_t *>(tr.base), tr.size};
        if (!text.empty())
        {
            static const std::uint8_t kCmove[] = {0x4c, 0x0f, 0x44, 0x0d};
            const std::uintptr_t text_base = reinterpret_cast<std::uintptr_t>(text.data());
            addr = mth::sig::find_riprel_load(text, text_base, kCmove, sizeof(kCmove), /*disp_off=*/4, /*insn_len=*/8);
        }
        if (addr == 0)
            logf(LogLevel::Error, "sig: g_saveManager not resolved (no unique `cmov r9,[rip]` in .text)");
        cache[mangled_name] = addr;
        return addr;
    }

    const TextRange tr = game_text_range();
    const std::span<const std::uint8_t> text{reinterpret_cast<const std::uint8_t *>(tr.base), tr.size};
    if (text.empty())
    {
        logf(LogLevel::Error, "sig: could not locate .text in game module");
        return 0;
    }
    const std::uintptr_t region_base = reinterpret_cast<std::uintptr_t>(text.data());

    for (const mth::sig::Entry &e : sig_table())
    {
        if (std::strcmp(e.name, mangled_name) != 0)
            continue;
        const std::uintptr_t addr = mth::sig::resolve(text, region_base, e);
        if (addr == 0)
        {
            // Classify the failure: a miss means the function changed or moved, ambiguous means the pattern needs re-carving longer.
            const mth::sig::Match m = mth::sig::find_masked(text, e.pattern, e.mask, e.len);
            const char *why = !m.found    ? "no .text match (function moved/changed?)"
                              : !m.unique ? "ambiguous: multiple .text matches"
                                          : "DataRef disp32 out of range";
            logf(LogLevel::Error, "sig: %s did not resolve (%s)", mangled_name, why);
        }
        cache[mangled_name] = addr;
        return addr;
    }

    logf(LogLevel::Error, "sig: no signature table entry for %s", mangled_name);
    cache[mangled_name] = 0;
    return 0;
}

} // namespace pal
