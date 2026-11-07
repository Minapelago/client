#pragma once

namespace mth
{

// FixedUpdate runs at 60 or 120 Hz, so thresholds are sized in ticks at 120 Hz and over-wait at 60; too short at 120 Hz re-opens the echo storm
// (#125). Counted in ticks because ticks stop when the game does.
inline constexpr int kMaxFixedUpdateHz = 120;
[[nodiscard]] inline constexpr int ticks_for_seconds(double s)
{
    return static_cast<int>(s * kMaxFixedUpdateHz);
}

// Decides when a local death is broadcast for deathlink and when an inbound death is safe to apply. Broadcasts once per death and re-arms only
// on a settled respawn (alive && !dying for kStableAliveTicks polls), so a flickering death signal during a transition can't re-broadcast it.
// An inbound deathlink suppresses our outbound until we settle again, which breaks the echo storm (#125). Pure.
// observe(dying, alive, gameplay_advanced) runs each tick. `dying` is the death-guard byte; it pulses mid-death, so it only triggers the latched
// broadcast. `alive` is health > 0. `gameplay_advanced` is false when the world's gameplay queues did not run (paused); no timer ages on such a
// tick, but a death edge still counts.
class DeathBroadcastGate
{
  public:
    // Consecutive stably-alive polls that count as a settled respawn; debounces the health/guard-byte flicker of a transition.
    static constexpr int kStableAliveTicks = ticks_for_seconds(0.25); // 30: 0.25s at 120 Hz, 0.5s at 60 Hz

    // Gameplay polls to wait for a requested death (inbound PlayerDie) to reach the guard byte / health; the game reads alive for ~0.7s after
    // PlayerDie returns. Counted in gameplay polls because a menu holds a requested death open, and frozen ticks would let the grace lapse mid-menu.
    static constexpr int kInboundDeathGraceTicks = ticks_for_seconds(2.0); // 240: 2s at 120 Hz, 4s at 60 Hz

    // Returns true iff this is a fresh local death that should be broadcast (not an echo of a received death).
    bool observe(bool dying, bool alive, bool gameplay_advanced)
    {
        if (!gameplay_advanced)
        {
            // Frozen tick: nothing queued can land and no settling time passes, so hold every timer.
        }
        else if (inbound_grace_ > 0)
        {
            // A requested death is still landing; counting these alive polls as settled would lift the suppression armed for it and echo it back (#125).
            --inbound_grace_;
            alive_streak_ = 0;
        }
        else if (alive && !dying)
        {
            if (alive_streak_ < kStableAliveTicks)
                ++alive_streak_;
            if (alive_streak_ >= kStableAliveTicks)
            {
                latched_ = false;  // a settled respawn re-arms the next broadcast
                suppress_ = false; // and lifts inbound-echo suppression
            }
        }
        else
        {
            alive_streak_ = 0; // dying or not-yet-alive (transition) breaks the streak
        }

        if (dying)
        {
            inbound_grace_ = 0; // the death we were waiting on registered; suppress-until-settled takes over
            if (latched_)
                return false; // already handled this death; survives the guard byte's mid-death flicker
            latched_ = true;
            if (suppress_)
            {
                suppressed_death_ = true; // log only, drained by take_suppressed_death()
                return false;             // a death taken from a received deathlink: do not echo it back
            }
            return true;
        }
        return false;
    }

    // A received inbound death: suppress our outbound until we settle, from arrival since applying may be deferred, so no death during the exchange
    // echoes back. No grace here: nothing is in flight until the death is applied, and grace would pin alive_streak_ at 0, which the retry waits on (#164).
    void note_inbound_death_received()
    {
        suppress_ = true;
    }

    // The PlayerDie for a received death just went out: hold "not settled" until it lands or the grace lapses (#125). Unsettle on this tick, since a
    // frozen tick ages nothing and the pre-freeze verdict would let a second bounce apply a second PlayerDie into this death.
    void note_inbound_death_applied()
    {
        suppress_ = true;
        inbound_grace_ = kInboundDeathGraceTicks;
        alive_streak_ = 0;
    }

    // Log only: true once per death edge swallowed as an inbound echo.
    bool take_suppressed_death()
    {
        const bool v = suppressed_death_;
        suppressed_death_ = false;
        return v;
    }

    // True from a death edge until the next settled respawn. A bounce arriving then is served by the death underway and is neither applied nor
    // latched, since dying twice for one exchange reads as a bug.
    [[nodiscard]] bool death_in_progress() const
    {
        return latched_;
    }

    // True after kStableAliveTicks polls of alive && !dying, when DeathHooks may apply an inbound PlayerDie; read it before
    // note_inbound_death_applied(), which unsettles it. It carries no freeze reason: a menu safely holds a queued death and a stalled room clock
    // does not, and only DeathHooks can tell them apart.
    [[nodiscard]] bool stably_alive() const
    {
        return alive_streak_ >= kStableAliveTicks;
    }

  private:
    int alive_streak_ = 0;
    int inbound_grace_ = 0;
    bool latched_ = false;
    bool suppress_ = false;
    bool suppressed_death_ = false; // log only, drained by take_suppressed_death()
};

} // namespace mth
