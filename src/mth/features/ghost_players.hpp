#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

#include "mth/core/ap/ap_events.hpp"
#include "mth/core/ap/ghost_stream.hpp"

namespace mth
{

// Samples the local body for ghost::Peers to send, and draws each ghost Peers plays back as an AnimEffect wearing Mina's sheet. Game thread only.
class GhostPlayers
{
  public:
    explicit GhostPlayers(IApLink &link) : peers_(link)
    {
    }

    void toggle_echo();                         // console command
    void tick(void *world, float step_seconds); // gamestate World's WorldUpdate pre-hook, the spawn window; step is 1/60 or 1/120
    void on_world_destroy();                    // the World frees the ghosts with it
    void on_ap_event(const ApEvent &ev)
    {
        peers_.apply(ev);
    }

  private:
    // The drawing for Peers::ghosts()[i]; slot shows when that entry went to another sender.
    struct Drawn
    {
        int slot = -1;
        void *weak = nullptr; // MM_WeakPtr to the AnimEffect
        std::uint32_t sheet = 0;
        std::uint32_t applied_seq = 0;
        std::uint32_t missed_seq = 0; // last received sequence the sheet lacks
        std::int64_t next_spawn = 0;  // respawn cooldown end, on clock_
    };

    // ycStringRef. Init takes both by pointer and `this` first on both builds (Linux 0x7b18be, Windows RVA 0x4f1b4b).
    struct StrRef
    {
        const char *str;
        std::size_t len;
    };
    using InitFn = void (*)(void *effect, const StrRef *anim, const StrRef *seq, void *palette);

    bool can_draw();
    std::optional<ghost::Sample> sample();
    void warn_layout(const char *what);
    void show(void *world, Drawn &d, const std::optional<ghost::Sample> &sample, std::optional<std::uint32_t> room, bool settled);
    void *spawn(void *world, Drawn &d, const ghost::Sample &sample);
    void apply(Drawn &d, void *effect, const ghost::Sample &sample);
    void fail(const char *why);

    ghost::Peers peers_;
    InitFn init_ = nullptr;
    std::optional<bool> drawable_; // resolved on first use; fail() turns it off
    bool warned_layout_ = false;
    bool room_stale_ = true; // room_tracker names the last World's room until the new one settles
    std::int64_t clock_ = 0; // half steps (1/120 s) of unpaused game time
    ghost::Recorder recorder_;
    std::array<Drawn, ghost::Peers::kMaxGhosts> drawn_;
};

} // namespace mth
