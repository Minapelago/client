#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "mth/core/save/zip_archive.hpp"

namespace
{
std::map<std::string, std::string> by_name(const std::vector<mth::zip::Entry> &entries)
{
    std::map<std::string, std::string> out;
    for (const auto &e : entries)
        out[e.name] = e.data;
    return out;
}

// A short pseudo-random run, which deflates larger than it started.
std::string noise()
{
    std::string body;
    std::uint32_t x = 123456789;
    for (int i = 0; i < 64; ++i)
    {
        x = x * 1664525u + 1013904223u;
        body.push_back(static_cast<char>(x >> 24));
    }
    return body;
}
} // namespace

TEST_CASE("zip round-trips text, binary, incompressible and empty entries", "[zip]")
{
    std::string binary;
    for (int i = 0; i < 512; ++i)
        binary.push_back(static_cast<char>(i & 0xFF));
    const std::map<std::string, std::string> files{
        {"save.ycsave", "[YCD Version: 1]\nSaveSlot\n{ m_iFoo: 1 }"}, {"ap.state", std::string(4096, 'a')}, {"bin", binary}, {"noise", noise()}, {"empty", ""},
    };

    std::vector<mth::zip::Blob> blobs;
    for (const auto &[name, data] : files)
        blobs.push_back(mth::zip::compress(name, data));
    const auto entries = mth::zip::read(mth::zip::write(blobs));
    REQUIRE(entries.has_value());
    REQUIRE(entries->size() == files.size());
    REQUIRE(by_name(*entries) == files);
}

TEST_CASE("zip rejects malformed images", "[zip]")
{
    const std::string good = mth::zip::write({mth::zip::compress("ap.state", "c 1\n")});

    REQUIRE_FALSE(mth::zip::read("").has_value());
    REQUIRE_FALSE(mth::zip::read("not a zip at all").has_value());
    REQUIRE_FALSE(mth::zip::read(good.substr(0, good.size() - 4)).has_value());
    REQUIRE_FALSE(mth::zip::read(good.substr(0, good.size() / 2)).has_value());
}

// Covers the CRC check on payload bytes and the size sanity checks on header bytes.
TEST_CASE("zip never returns wrong data from a corrupted image", "[zip]")
{
    const std::string body(2048, 'q');
    const std::string good = mth::zip::write({mth::zip::compress("ap.state", body)});
    for (std::size_t i = 0; i < good.size(); ++i)
    {
        INFO("flipped byte " << i);
        std::string image = good;
        image[i] = static_cast<char>(image[i] ^ 0xFF);
        const auto entries = mth::zip::read(image);
        if (entries)
            for (const auto &e : *entries)
                REQUIRE(e.data == body);
    }
}

TEST_CASE("zip rejects unsafe entry names on read and keeps safe nested ones", "[zip]")
{
    for (const char *name : {"../escape", "/abs/path", "a/../../b", "C:\\win"})
    {
        INFO(name);
        auto blob = mth::zip::compress("placeholder", "x");
        blob.name = name; // bypass the writer; simulate a hostile container
        REQUIRE_FALSE(mth::zip::read(mth::zip::write({blob})).has_value());
    }

    const auto entries = mth::zip::read(mth::zip::write({mth::zip::compress("sub/dir/file.txt", "x")}));
    REQUIRE(entries.has_value());
    REQUIRE((*entries)[0].name == "sub/dir/file.txt");
}
