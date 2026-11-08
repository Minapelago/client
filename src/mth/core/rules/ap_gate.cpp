#include "mth/core/rules/ap_gate.hpp"

#include <utility>

#include "pal/pal_log.hpp"

namespace mth
{

namespace
{

bool static_checks_passed(const GateInputs &in)
{
    return in.mod_api_present && in.symbols_resolved && in.item_table_shape_ok && in.layout_probes_ok;
}

bool live(const GateInputs &in)
{
    return in.worldupdate_observed && in.gamestate_world_observed && in.other_world_observed;
}

bool liveness_expired(const GateInputs &in)
{
    return !live(in) && in.ticks_since_probe_installed >= kLivenessTimeoutTicks;
}

} // namespace

GateVerdict evaluate(const GateInputs &in)
{
    if (!static_checks_passed(in))
        return GateVerdict::Refused;
    if (live(in))
        return GateVerdict::Clear;
    if (liveness_expired(in))
        return GateVerdict::Refused;
    return GateVerdict::Pending;
}

const char *verdict_name(GateVerdict v)
{
    switch (v)
    {
    case GateVerdict::Pending:
        return "Pending";
    case GateVerdict::Clear:
        return "Clear";
    case GateVerdict::Refused:
        return "Refused";
    }
    return "Unknown";
}

std::string refusal_reason(const GateInputs &in)
{
    // Most fundamental first: a missing mod API also explains every check that depends on it, so the first failure is the one to report.
    if (!in.mod_api_present)
        return "the game's mod API is unavailable (is this the experimental-modding build?)";
    if (!in.symbols_resolved)
        return "one or more required game functions could not be located (game updated?)";
    if (!in.item_table_shape_ok)
        return "the game's item table does not match the expected layout (game updated?)";
    if (!in.layout_probes_ok)
        return "a game struct layout probe failed (game updated?)";
    if (!liveness_expired(in))
        return std::string();
    if (!in.worldupdate_observed)
        return "the game never ran the mod's WorldUpdate hook";
    // Usually a slow boot load, an auto-pause or a system prompt, which clears by itself.
    if (!in.gamestate_world_observed)
        return "no gamestate World has updated yet (still loading, paused, or a system prompt is up); if this never clears, the game's World layout may have "
               "changed";
    return "the gamestate flag (World+0x1864) was set on every World; the game's World layout may have changed";
}

bool should_refuse_connect(bool enforcing, GateVerdict v)
{
    return enforcing && v == GateVerdict::Refused;
}

GateVerdict GateLatch::update(const GateInputs &in)
{
    if (verdict_ == GateVerdict::Clear)
        return verdict_;
    verdict_ = evaluate(in);
    return verdict_;
}

void SafetyGate::begin(const GateInputs &static_inputs)
{
    inputs_ = static_inputs;
    update();
    pal::logf(pal::LogLevel::Info, "gate: verdict=%s enforcing=%d (console: `gate enforce off` to override)", verdict_name(verdict_.load()),
              enforcing_.load() ? 1 : 0);
}

bool SafetyGate::tick()
{
    if (latch_.verdict() == GateVerdict::Clear)
        return false;
    ++inputs_.ticks_since_probe_installed;
    return update();
}

bool SafetyGate::note_worldupdate(bool gamestate_world)
{
    bool &seen = gamestate_world ? inputs_.gamestate_world_observed : inputs_.other_world_observed;
    if (seen)
        return false;
    seen = true;
    if (!inputs_.worldupdate_observed)
    {
        inputs_.worldupdate_observed = true;
        pal::logf(pal::LogLevel::Info, "gate: WorldUpdate observed after %d tick(s); native mod hooks are live", inputs_.ticks_since_probe_installed);
    }
    pal::logf(pal::LogLevel::Info, "gate: a World with the gamestate flag %s updated after %d tick(s)", gamestate_world ? "set" : "clear",
              inputs_.ticks_since_probe_installed);
    return update();
}

bool SafetyGate::update()
{
    const GateVerdict before = latch_.verdict();
    const GateVerdict after = latch_.update(inputs_);
    if (after == before)
        return false;

    verdict_.store(after);
    std::string reason = refusal_reason(inputs_);
    bool drop = false;
    if (after == GateVerdict::Refused)
    {
        pal::logf(pal::LogLevel::Error, "gate: REFUSED - %s", reason.c_str());
        drop = enforcing_.load();
        if (!drop)
            pal::logf(pal::LogLevel::Warn, "gate: enforcement is off, so AP behavior is NOT being blocked; run `gate enforce on` to enforce");
    }
    else if (after == GateVerdict::Clear)
    {
        if (before == GateVerdict::Refused)
            pal::logf(pal::LogLevel::Warn, "gate: the missing World observations arrived after the deadline; lifting the refusal");
        pal::logf(pal::LogLevel::Info, "gate: CLEAR after %d tick(s); AP behavior permitted", inputs_.ticks_since_probe_installed);
        if (!inputs_.revision_known)
            pal::logf(pal::LogLevel::Warn, "gate: this game build is outside the validated revision range; proceeding because every probe passed");
        if (!inputs_.mod_api_shape_ok)
            pal::logf(pal::LogLevel::Error, "gate: the game's mod API version differs from the one this build expects; entries may be misaligned");
    }
    {
        std::lock_guard<std::mutex> lk(reason_mutex_);
        reason_ = std::move(reason);
    }
    return drop;
}

GateStatus SafetyGate::status() const
{
    GateStatus s;
    s.verdict = verdict_.load();
    s.enforcing = enforcing_.load();
    {
        std::lock_guard<std::mutex> lk(reason_mutex_);
        s.reason = reason_;
    }
    return s;
}

void SafetyGate::set_enforcing(bool on)
{
    enforcing_.store(on);
    pal::logf(pal::LogLevel::Info, "gate: enforcing=%d (verdict=%s)", on ? 1 : 0, verdict_name(verdict_.load()));
}

} // namespace mth
