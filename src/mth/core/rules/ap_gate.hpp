#pragma once

#include <string>

namespace mth
{

// All-or-nothing: a half-working client produces unwinnable seeds and releases items into a multiworld it can't reciprocate.
enum class GateVerdict
{
    Pending, // static checks passed; waiting for proof the native mod hooks fire
    Clear,
    Refused
};

// Only worldupdate_observed changes at runtime; the rest are properties of the loaded binary (the mod hooks no heap addresses).
struct GateInputs
{
    bool mod_api_present{false};
    bool symbols_resolved{false};
    bool item_table_shape_ok{false};
    bool layout_probes_ok{false};
    bool revision_known{false};   // informational: drives a banner, never the verdict
    bool mod_api_shape_ok{false}; // informational: drives a banner, never the verdict
    bool worldupdate_observed{false};
    int ticks_since_probe_installed{0};
};

// WorldUpdate ticks at the title screen, reached within a second or two of launch.
inline constexpr int kLivenessTimeoutTicks = 600;

[[nodiscard]] GateVerdict evaluate(const GateInputs &in);

[[nodiscard]] const char *verdict_name(GateVerdict v);

// Empty unless refused. Names the first failing input, so the banner alone is diagnosable.
[[nodiscard]] std::string refusal_reason(const GateInputs &in);

// The connect chokepoint's rule, outside App so tests can reach it without the PAL. Pending passes: the only input left by then is the
// WorldUpdate liveness proof, which can't fire until the game runs, and every static probe predicting it has passed.
[[nodiscard]] bool should_refuse_connect(bool enforcing, GateVerdict v);

// Terminal: once the verdict leaves Pending it never changes, so a late input cannot re-arm AP
// behavior that was already refused.
class GateLatch
{
  public:
    GateVerdict update(const GateInputs &in);

    [[nodiscard]] GateVerdict verdict() const
    {
        return verdict_;
    }

    [[nodiscard]] bool settled() const
    {
        return verdict_ != GateVerdict::Pending;
    }

  private:
    GateVerdict verdict_{GateVerdict::Pending};
};

} // namespace mth
