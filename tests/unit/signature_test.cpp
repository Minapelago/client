// Windows hooks resolve through these matchers; a silent wrong match hooks the wrong function.
#include <cstdint>
#include <cstring>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "mth/core/game/component_types.hpp" // MM_Rtti type ids: same hash, pinned to GetTypeId() bodies
#include "mth/core/game/hook_hash.hpp"
#include "mth/core/game/native_sym_names.hpp"
#include "mth/core/game/sig_scan.hpp"

using mth::hookhash::hash64;
using mth::sig::Entry;
using mth::sig::find_masked;
using mth::sig::Kind;
using mth::sig::Match;
using mth::sig::resolve;
using mth::sym::kNativeSymNames;
using mth::sym::native_sym_name;

namespace
{
// 1-bytes for every position: exact match required.
std::vector<std::uint8_t> all_ones(std::size_t n)
{
    return std::vector<std::uint8_t>(n, 1);
}
} // namespace

TEST_CASE("find_masked locates a pattern, honouring wildcard bytes, and reports a miss", "[sig]")
{
    std::vector<std::uint8_t> region{0x00, 0x11, 0x48, 0x89, 0xE5, 0x22};
    std::vector<std::uint8_t> pat{0x48, 0x89, 0xE5};
    auto mask = all_ones(3);
    Match m = find_masked(region, pat.data(), mask.data(), pat.size());
    REQUIRE(m.found);
    REQUIRE(m.unique);
    REQUIRE(m.offset == 2);

    std::vector<std::uint8_t> ref{0x48, 0x8B, 0x05, 0xDE, 0xAD, 0xBE, 0xEF, 0x90};
    std::vector<std::uint8_t> wild{0x48, 0x8B, 0x05, 0x00, 0x00, 0x00, 0x00};
    std::vector<std::uint8_t> wild_mask{1, 1, 1, 0, 0, 0, 0};
    m = find_masked(ref, wild.data(), wild_mask.data(), wild.size());
    REQUIRE(m.found);
    REQUIRE(m.offset == 0);

    std::vector<std::uint8_t> other{0x01, 0x02, 0x03};
    REQUIRE_FALSE(find_masked(other, pat.data(), mask.data(), pat.size()).found);
    std::vector<std::uint8_t> short_region{0x48, 0x89};
    REQUIRE_FALSE(find_masked(short_region, pat.data(), mask.data(), pat.size()).found);
}

TEST_CASE("resolve returns the match address for a Code entry", "[sig]")
{
    std::vector<std::uint8_t> region{0x00, 0x55, 0x48, 0x89, 0xE5};
    std::vector<std::uint8_t> pat{0x55, 0x48, 0x89, 0xE5};
    auto mask = all_ones(4);
    Entry e{"f", Kind::Code, pat.data(), mask.data(), pat.size(), 0, 0};
    // region[0] maps to runtime 0x140001000; the match begins at offset 1, the last valid one.
    REQUIRE(resolve(region, 0x140001000ull, e) == 0x140001001ull);
}

TEST_CASE("resolve extracts a RIP-relative global for a DataRef entry", "[sig]")
{
    // At offset 4: lea rax,[rip+0x10]  -> 48 8D 05 10 00 00 00 (7 bytes)
    std::vector<std::uint8_t> region{0x00, 0x00, 0x00, 0x00, 0x48, 0x8D, 0x05, 0x10, 0x00, 0x00, 0x00, 0x90};
    std::vector<std::uint8_t> pat{0x48, 0x8D, 0x05, 0x00, 0x00, 0x00, 0x00};
    std::vector<std::uint8_t> mask{1, 1, 1, 0, 0, 0, 0};
    Entry e{"g", Kind::DataRef, pat.data(), mask.data(), pat.size(), /*disp_off=*/3, /*next_insn=*/7};
    // region_base 0x140001000; match offset 4; next-insn runtime addr = 0x140001000+4+7 = 0x14000100B; +0x10
    REQUIRE(resolve(region, 0x140001000ull, e) == 0x14000101Bull);
}

TEST_CASE("resolve returns 0 for an ambiguous match", "[sig]")
{
    std::vector<std::uint8_t> region{0xC3, 0xC3};
    std::vector<std::uint8_t> pat{0xC3};
    auto mask = all_ones(1);
    Entry e{"h", Kind::Code, pat.data(), mask.data(), pat.size(), 0, 0};
    REQUIRE(resolve(region, 0x1000, e) == 0);
}

TEST_CASE("resolve handles a negative RIP-relative displacement", "[sig]")
{
    // At offset 4: lea rax,[rip-0x10] -> 48 8D 05 F0 FF FF FF (disp32 = -16)
    std::vector<std::uint8_t> region{0x00, 0x00, 0x00, 0x00, 0x48, 0x8D, 0x05, 0xF0, 0xFF, 0xFF, 0xFF, 0x90};
    std::vector<std::uint8_t> pat{0x48, 0x8D, 0x05, 0x00, 0x00, 0x00, 0x00};
    std::vector<std::uint8_t> mask{1, 1, 1, 0, 0, 0, 0};
    Entry e{"g", Kind::DataRef, pat.data(), mask.data(), pat.size(), /*disp_off=*/3, /*next_insn=*/7};
    // next-insn runtime addr = 0x140001000+4+7 = 0x14000100B; + (-0x10) = 0x140000FFB (global below the ref-site)
    REQUIRE(resolve(region, 0x140001000ull, e) == 0x140000FFBull);
}

TEST_CASE("resolve returns 0 when a DataRef disp32 runs past the region", "[sig]")
{
    // Pattern (len 3) matches, but disp_off=3 puts the 4-byte read at [3,7) in a 3-byte region.
    std::vector<std::uint8_t> region{0x48, 0x8D, 0x05};
    std::vector<std::uint8_t> pat{0x48, 0x8D, 0x05};
    auto mask = all_ones(3);
    Entry e{"oob", Kind::DataRef, pat.data(), mask.data(), pat.size(), /*disp_off=*/3, /*next_insn=*/7};
    REQUIRE(resolve(region, 0x140001000ull, e) == 0);
}

TEST_CASE("read_riprel_target computes a RIP-relative global, either side of the instruction", "[sig]")
{
    // cmove r9,[rip+0x04a3a76e] at pretend addr 0x140823392 (8-byte insn): target = insn_addr + insn_len + disp32.
    const std::uint8_t insn[] = {0x4c, 0x0f, 0x44, 0x0d, 0x6e, 0xa7, 0xa3, 0x04};
    REQUIRE(mth::sig::read_riprel_target(insn, 0x140823392, /*disp_off=*/4, /*insn_len=*/8) == 0x14525db08ULL);

    // disp32 = -0x100 (little-endian bytes 00 ff ff ff).
    const std::uint8_t back[] = {0x4c, 0x0f, 0x44, 0x0d, 0x00, 0xff, 0xff, 0xff};
    REQUIRE(mth::sig::read_riprel_target(back, 0x140001000, /*disp_off=*/4, /*insn_len=*/8) == 0x140000F08ULL);
}

TEST_CASE("find_riprel_load returns 0 when no opcode matches", "[sig]")
{
    std::uint8_t buf[32] = {}; // all zeros: opcode bytes never appear
    const std::uint8_t op[] = {0x4c, 0x0f, 0x44, 0x0d};
    REQUIRE(mth::sig::find_riprel_load({buf, sizeof(buf)}, 0x1000, op, sizeof(op), /*disp_off=*/4, /*insn_len=*/8) == 0ULL);

    // Region smaller than insn_len -> 0.
    std::uint8_t small[4] = {0x4c, 0x0f, 0x44, 0x0d};
    REQUIRE(mth::sig::find_riprel_load({small, sizeof(small)}, 0x1000, op, sizeof(op), /*disp_off=*/4, /*insn_len=*/8) == 0ULL);
}

TEST_CASE("find_riprel_load fails on an ambiguous match", "[sig]")
{
    // Two identical loads in the window: nothing says which global the fingerprint meant, and picking one would point g_saveManager at a foreign global.
    std::uint8_t buf[32] = {};
    const std::uint8_t op[] = {0x4c, 0x0f, 0x44, 0x0d};
    for (std::size_t at : {std::size_t{1}, std::size_t{16}})
    {
        buf[at + 0] = 0x4c;
        buf[at + 1] = 0x0f;
        buf[at + 2] = 0x44;
        buf[at + 3] = 0x0d;
        buf[at + 5] = 0x10; // disp32 = 0x1000
    }
    REQUIRE(mth::sig::find_riprel_load({buf, sizeof(buf)}, 0x1000, op, sizeof(op), /*disp_off=*/4, /*insn_len=*/8) == 0ULL);
}

TEST_CASE("find_riprel_load locates the first matching opcode in a window", "[sig]")
{
    std::uint8_t buf[32] = {};
    buf[0] = 0x55; // push rbp (filler)
    buf[1] = 0x4c;
    buf[2] = 0x0f;
    buf[3] = 0x44;
    buf[4] = 0x0d; // cmove r9,[rip+..] at offset 1
    buf[5] = 0x00;
    buf[6] = 0x10;
    buf[7] = 0x00;
    buf[8] = 0x00; // disp32 = 0x1000
    const std::uint8_t op[] = {0x4c, 0x0f, 0x44, 0x0d};
    // base addr 0x1000; match at off 1 -> insn_addr 0x1001; target 0x1001 + 8 + 0x1000 = 0x2009
    REQUIRE(mth::sig::find_riprel_load({buf, sizeof(buf)}, 0x1000, op, sizeof(op), /*disp_off=*/4, /*insn_len=*/8) == 0x2009ULL);
}

// The reference values are the movabs immediates read out of the game's own RunHooks dispatch sites, so this pins the implementation to the
// shipping binary.
static_assert(hash64("FixedUpdate") == 0xd7aa418c4961d6baULL);
static_assert(hash64("GameInit") == 0xcd6c390c7a5949a6ULL);
static_assert(hash64("GameShutdown") == 0xfa1005f0dfb0b999ULL);
static_assert(hash64("GameStateTransition") == 0xb9cbbdeee4f465aaULL);
static_assert(hash64("WorldConstruct") == 0xe7ce860f2892fb7eULL);
static_assert(hash64("WorldDestroy") == 0x6ef6ff73b71bee4eULL);
static_assert(hash64("WorldUpdate") == 0x2f3672e14c657ffdULL);
static_assert(hash64("WorldUpdateEnd") == 0x687c151ff01cd1f9ULL);
static_assert(hash64("IsItemCollected") == 0xf9fd1d4efa1e21c4ULL);

// Added 2026-08-05, located in r149150 by scanning for these constants.
static_assert(hash64("ItemsOnPickup") == 0xfc3dd61d893bb4efULL);
static_assert(hash64("ItemsOnPickupDone") == 0xc33f1d10e2025ea2ULL);
static_assert(hash64("PickupOnPickup") == 0x00ce3d82b0e25e35ULL);
static_assert(hash64("ShopItemRefresh") == 0x553c7634843b755bULL);
static_assert(hash64("AreaManagerNewArea") == 0x762f9cb2d804d61bULL);
static_assert(hash64("ChestConstruct") == 0x04588da3e185326cULL);

// One of the two input hooks the save takeover installs; the keyboard one is not pinned.
static_assert(hash64("ycControllerUpdate") == 0x7cc4f96a2b42b94aULL);

// The set also covers the hash's 12-byte block seam: "GameInit" (8) and "GameShutdown" (12) take the
// byte-wise tail alone, while "GameStateTransition" (19) takes one whole block plus a tail.

TEST_CASE("hook_hash separates names that differ slightly", "[hookhash]")
{
    REQUIRE(hash64("ItemsOnPickup") != hash64("ItemsOnPickupDone"));
    REQUIRE(hash64("WorldUpdate") != hash64("WorldUpdateEnd"));
    REQUIRE(hash64("") != hash64("WorldUpdate"));
}

TEST_CASE("native_sym_name maps every table row's mangled name to its plain name", "[symnames]")
{
    for (const auto &n : kNativeSymNames)
    {
        INFO(n.mangled);
        REQUIRE(std::string_view(native_sym_name(n.mangled)) == n.plain);
    }
}

TEST_CASE("native_sym_name returns null for a name the API does not expose", "[symnames]")
{
    // Not in the documented GetSymAddr list, so it must keep its own resolution path.
    REQUIRE(native_sym_name(mth::sym::on_pickup_done) == nullptr);
    REQUIRE(native_sym_name("g_saveManager") == nullptr);
    REQUIRE(native_sym_name(nullptr) == nullptr);
}

TEST_CASE("the native symbol table has no empty or duplicated entries", "[symnames]")
{
    for (const auto &n : kNativeSymNames)
    {
        REQUIRE(n.mangled != nullptr);
        REQUIRE(n.plain != nullptr);
        REQUIRE(std::strlen(n.mangled) > 0);
        REQUIRE(std::strlen(n.plain) > 0);
        // A plain name must not be mangled: that would mean a row was filled in from the wrong column.
        REQUIRE(std::string_view(n.plain).substr(0, 2) != "_Z");
    }

    for (const auto &a : kNativeSymNames)
    {
        int mangled_hits = 0;
        int plain_hits = 0;
        for (const auto &b : kNativeSymNames)
        {
            mangled_hits += (std::strcmp(a.mangled, b.mangled) == 0) ? 1 : 0;
            plain_hits += (std::strcmp(a.plain, b.plain) == 0) ? 1 : 0;
        }
        REQUIRE(mangled_hits == 1);
        REQUIRE(plain_hits == 1);
    }
}
