#pragma once

#include <atomic>
#include <mutex>
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

// Only the observed flags change at runtime; the rest are properties of the loaded binary (the mod hooks no heap addresses).
struct GateInputs
{
    bool mod_api_present{false};
    bool symbols_resolved{false};
    bool item_table_shape_ok{false};
    bool layout_probes_ok{false};
    bool revision_known{false};   // informational: drives a banner, never the verdict
    bool mod_api_shape_ok{false}; // informational: drives a banner, never the verdict
    bool worldupdate_observed{false};
    // The World hooks act only on Worlds with the gamestate flag set. Seeing the flag both set and clear catches an offset that reads the same on every
    // World. It misses a shift onto +0x1863 (isGameWorld), which is 0 on the process Worlds and 1 on the boot World, so boot looks the same and overlays
    // would then pass the filters.
    bool gamestate_world_observed{false}; // a WorldUpdate World had the flag set
    bool other_world_observed{false};     // a WorldUpdate World had it clear; s_popupWorld always does
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

// Clear is terminal. A static-probe failure keeps evaluating to Refused because the loaded binary cannot change, while a liveness refusal moves to Clear
// once the missing observations arrive: the deadline counts fixed updates, which keep running through auto-pause, a system prompt and the boot load.
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

// Snapshot of the gate, copied by value because the UI reads it on the render thread while the game thread writes the gate.
struct GateStatus
{
    GateVerdict verdict{GateVerdict::Pending};
    bool enforcing{false};
    std::string reason; // empty unless refused
};

// The gate's runtime state. The game thread drives it; status() and set_enforcing() are safe from the render thread because the verdict and the enforcing
// flag are atomic and the reason is copied out under its own lock.
class SafetyGate
{
  public:
    // Game thread, once, before anything touches game memory. No AP session can exist yet, so a refusal here has nothing to drop.
    void begin(const GateInputs &static_inputs);
    // Game thread, each fixed update: advances the liveness deadline until the verdict clears. True when this call refused while enforcing, so the caller
    // drops any session that connected before the verdict settled.
    [[nodiscard]] bool tick();
    // Game thread, on every native WorldUpdate, which proves the mod-hook path is live. gamestate_world says whether that World had the gamestate flag
    // set. Returns as tick() does.
    [[nodiscard]] bool note_worldupdate(bool gamestate_world);

    [[nodiscard]] GateStatus status() const;
    void set_enforcing(bool on); // dev override of the default-on enforcement

  private:
    bool update(); // latch, publish and log a transition; returns as tick() does

    GateInputs inputs_;
    GateLatch latch_;
    std::atomic<GateVerdict> verdict_{GateVerdict::Pending};
    std::atomic<bool> enforcing_{true}; // `gate enforce off` clears it
    mutable std::mutex reason_mutex_;
    std::string reason_;
};

} // namespace mth
