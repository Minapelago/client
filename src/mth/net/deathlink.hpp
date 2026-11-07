#pragma once

#include <optional>
#include <string>

namespace mth::net
{

struct DeathLinkBounce
{
    std::string source;
    std::string cause;
};

// Composes the `cause` sentence for an outbound bounce. Receivers show it verbatim and AP convention names the dying player; `detail` is the predicate.
[[nodiscard]] std::string deathlink_cause(const std::string &slot_name, const std::string &detail);

// Serialize a DeathLink Bounce payload (JSON object as a string).
[[nodiscard]] std::string make_deathlink_payload(const std::string &source, const std::string &cause, double time_epoch_s);

// Parse a DeathLink Bounce payload; returns {source, cause} (either may be ""), or nullopt if malformed.
[[nodiscard]] std::optional<DeathLinkBounce> parse_deathlink_payload(const std::string &json_text);

} // namespace mth::net
