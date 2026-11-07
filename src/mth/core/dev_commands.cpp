#include "mth/core/dev_commands.hpp"

#include <cctype>
#include <sstream>
#include <string_view>

namespace mth
{

namespace
{

std::string to_lower(std::string s)
{
    for (char &c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

struct VerbKind
{
    std::string_view verb;
    CommandKind kind;
};

constexpr VerbKind kVerbKinds[] = {
    {"help", CommandKind::Help},
    {"clear", CommandKind::Clear},
    {"gate", CommandKind::Gate},
    {"status", CommandKind::Status},
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

CommandKind verb_to_kind(const std::string &lower)
{
    for (const VerbKind &row : kVerbKinds)
        if (row.verb == lower)
            return row.kind;
    return CommandKind::Unknown;
}

} // namespace

ParsedCommand parse_command(const std::string &line)
{
    std::istringstream iss(line);
    std::vector<std::string> tokens;
    for (std::string tok; iss >> tok;)
        tokens.push_back(tok);

    ParsedCommand cmd;
    if (tokens.empty())
        return cmd; // kind == None

    cmd.verb = tokens.front();
    cmd.kind = verb_to_kind(to_lower(tokens.front()));
    cmd.args.assign(tokens.begin() + 1, tokens.end());
    return cmd;
}

} // namespace mth
