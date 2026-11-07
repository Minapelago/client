#include <cctype>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "mth/core/console/dev_commands.hpp"
#include "mth/core/console/log_ring.hpp"
#include "pal/overlay_keys.hpp"
#include "pal/pal_log.hpp"

using mth::CommandKind;
using mth::parse_command;

namespace
{

struct VerbRow
{
    const char *verb;
    CommandKind kind;
};

// Every kind the console dispatches, in enum order. None and Unknown are parse outcomes, not verbs.
constexpr VerbRow kVerbs[] = {
    {"help", CommandKind::Help},
    {"clear", CommandKind::Clear},
    {"status", CommandKind::Status},
    {"gate", CommandKind::Gate},
    {"items", CommandKind::Items},
    {"giveapitem", CommandKind::GiveItem},
    {"removelock", CommandKind::RemoveLock},
    {"modifier", CommandKind::Modifier},
    {"modifiers", CommandKind::ModifierLock},
    {"caps", CommandKind::StatCaps},
    {"ability", CommandKind::Ability},
    {"connect", CommandKind::Connect},
    {"disconnect", CommandKind::Disconnect},
    {"deathlink", CommandKind::Deathlink},
    {"litlamps", CommandKind::LitLamps},
    {"savetest", CommandKind::SaveTest},
    {"trap", CommandKind::Trap},
    {"switches", CommandKind::Switches},
};

std::string to_upper(std::string s)
{
    for (char &c : s)
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

class CapturingLog final : public pal::ILog
{
  public:
    void write(pal::LogLevel level, std::string_view message) override
    {
        std::lock_guard<std::mutex> lock(mu_);
        entries_.emplace_back(level, std::string(message));
    }

    std::vector<std::pair<pal::LogLevel, std::string>> entries()
    {
        std::lock_guard<std::mutex> lock(mu_);
        return entries_;
    }

  private:
    std::mutex mu_;
    std::vector<std::pair<pal::LogLevel, std::string>> entries_;
};

// Restores the process-wide log sink and observer even when a REQUIRE throws.
struct ScopedLog
{
    explicit ScopedLog(pal::ILog *sink)
    {
        pal::set_default_log(sink);
    }
    ~ScopedLog()
    {
        pal::set_log_observer(nullptr);
        pal::set_default_log(nullptr);
    }
    ScopedLog(const ScopedLog &) = delete;
    ScopedLog &operator=(const ScopedLog &) = delete;
};

} // namespace

TEST_CASE("parse_command: every dispatchable kind has a verb, mapped case-insensitively", "[mth][commands]")
{
    REQUIRE(parse_command("").kind == CommandKind::None);
    REQUIRE(parse_command("   \t ").kind == CommandKind::None);

    for (const VerbRow &row : kVerbs)
    {
        INFO(row.verb);
        REQUIRE(parse_command(row.verb).kind == row.kind);
        REQUIRE(parse_command(to_upper(row.verb)).kind == row.kind);
        // Nothing past token 0 reaches the mapping, so one trailing-argument shape stands in for all of them.
        REQUIRE(parse_command(std::string(row.verb) + " 1 2 3").kind == row.kind);
    }

    // Driven off the enum so a kind missing or misspelled in kVerbKinds fails here instead of parsing as Unknown. Switches is the last enumerator;
    // a kind added after it has to move this bound.
    for (int i = static_cast<int>(CommandKind::Help); i <= static_cast<int>(CommandKind::Switches); ++i)
    {
        const auto kind = static_cast<CommandKind>(i);
        INFO(i);
        int rows = 0;
        for (const VerbRow &row : kVerbs)
            rows += (row.kind == kind) ? 1 : 0;
        REQUIRE(rows == 1);
    }
}

TEST_CASE("parse_command: arguments are captured and an unknown verb comes back as typed", "[mth][commands]")
{
    const auto c = parse_command("connect localhost:38281 Mina secret");
    REQUIRE(c.kind == CommandKind::Connect);
    REQUIRE(c.args == std::vector<std::string>{"localhost:38281", "Mina", "secret"});
    REQUIRE(parse_command("connect").args.empty());

    const auto u = parse_command("frobnicate x y");
    REQUIRE(u.kind == CommandKind::Unknown);
    REQUIRE(u.verb == "frobnicate");
    // The console echoes the verb back at the player.
    REQUIRE(parse_command("FROBNICATE").verb == "FROBNICATE");
}

TEST_CASE("LogRing keeps the newest lines oldest-first, at least one", "[mth][logring]")
{
    mth::LogRing ring(3);
    ring.push("a");
    ring.push("b");
    REQUIRE(ring.snapshot() == std::vector<std::string>{"a", "b"});
    for (const char *line : {"c", "d", "e"})
        ring.push(line);
    REQUIRE(ring.snapshot() == std::vector<std::string>{"c", "d", "e"});
    ring.clear();
    REQUIRE(ring.snapshot().empty());

    mth::LogRing tiny(0);
    tiny.push("a");
    tiny.push("b");
    REQUIRE(tiny.snapshot() == std::vector<std::string>{"b"});
}

TEST_CASE("log: the injected sink and the observer both receive each formatted line", "[pal][log]")
{
    CapturingLog sink;
    std::vector<std::pair<pal::LogLevel, std::string>> seen;
    {
        ScopedLog guard(&sink);
        pal::set_log_observer([&seen](pal::LogLevel lvl, std::string_view msg) { seen.emplace_back(lvl, std::string(msg)); });

        pal::logf(pal::LogLevel::Info, "hello %s %d", "world", 42);
        pal::logf(pal::LogLevel::Warn, "count=%d", 7);

        const std::vector<std::pair<pal::LogLevel, std::string>> expected{{pal::LogLevel::Info, "hello world 42"}, {pal::LogLevel::Warn, "count=7"}};
        REQUIRE(sink.entries() == expected);
        REQUIRE(seen == expected);
    }
    pal::logf(pal::LogLevel::Info, "nobody home");
    REQUIRE(seen.size() == 2);
}

TEST_CASE("overlay keys: F1..F12 and backquote names parse, anything else falls back, and the login key avoids the console key", "[overlay_keys]")
{
    constexpr int kF1 = 100;
    constexpr int kBackquote = 500;
    const auto console_key = [](const char *name) { return pal::parse_console_key<int>(name, kF1, kBackquote); };

    REQUIRE(console_key("F1") == kF1);
    REQUIRE(console_key("F10") == kF1 + 9);
    REQUIRE(console_key("F12") == kF1 + 11);
    REQUIRE(console_key("BACKQUOTE") == kBackquote);
    REQUIRE(console_key("GRAVE") == kBackquote);
    REQUIRE(console_key("`") == kBackquote);
    for (const char *bad : {"", "F", "F0", "F01", "F13", "F+1", "F1x", "f1", "F 1", "q"})
        REQUIRE(console_key(bad) == kF1);
    REQUIRE(console_key(nullptr) == kF1);

    REQUIRE(pal::parse_login_key<int>(nullptr, kF1, kF1, kBackquote) == kF1 + 1);
    REQUIRE(pal::parse_login_key<int>("F5", kF1, kF1, kBackquote) == kF1 + 4);
    REQUIRE(pal::parse_login_key<int>("F5", kF1 + 4, kF1, kBackquote) == kF1 + 1);
}
