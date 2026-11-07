#include <filesystem>
#include <fstream>

#include <catch2/catch_test_macros.hpp>

#include "mth/core/console/login_prefs.hpp"

namespace
{

std::filesystem::path temp_prefs(const char *name)
{
    const auto path = std::filesystem::temp_directory_path() / name;
    std::filesystem::remove(path);
    return path;
}

void write_file(const std::filesystem::path &path, const char *text)
{
    std::ofstream out(path, std::ios::trunc);
    out << text;
}

} // namespace

TEST_CASE("LoginPrefs round-trips server and slot, replacing what it saved before", "[login_prefs]")
{
    const auto path = temp_prefs("mthap_test_login.prefs");

    {
        mth::LoginPrefs p(path);
        REQUIRE(p.server().empty());
        REQUIRE(p.slot().empty());
        p.set("first:1", "A much longer slot name");
        p.save();
    }
    {
        mth::LoginPrefs p(path);
        REQUIRE(p.slot() == "A much longer slot name");
        p.set("archipelago.gg:38281", "Mina");
        p.save();
    }
    {
        mth::LoginPrefs p(path);
        REQUIRE(p.server() == "archipelago.gg:38281");
        REQUIRE(p.slot() == "Mina");
    }
    std::filesystem::remove(path);
}

TEST_CASE("LoginPrefs skips unknown keys and malformed lines, and trims carriage returns", "[login_prefs]")
{
    const auto path = temp_prefs("mthap_test_login_garbage.prefs");
    write_file(path, "\nnonsense\npassword hunter2\nserver host:1234\r\n= =\nslot Mina\r\n");

    mth::LoginPrefs p(path);
    REQUIRE(p.server() == "host:1234");
    REQUIRE(p.slot() == "Mina");
    std::filesystem::remove(path);
}

TEST_CASE("LoginPrefs does not let a newline forge a second record", "[login_prefs]")
{
    const auto path = temp_prefs("mthap_test_login_newline.prefs");

    // The login window takes free text, so a pasted name can carry a newline. save() writes one record per line and load() reads the last record of a
    // key, so the tail of such a name would come back as the slot the mod connects with. Both fields take the same path in.
    {
        mth::LoginPrefs p(path);
        p.set("archipelago.gg:38281\nslot Evil", "Mina\nserver evil:1");
        p.save();
    }
    {
        mth::LoginPrefs p(path);
        REQUIRE(p.slot() != "Evil");
        REQUIRE(p.server() != "evil:1");
    }
    std::filesystem::remove(path);
}
