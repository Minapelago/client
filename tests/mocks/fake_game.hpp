#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "MinaModAPI.h"
#include "mod/mod_api.hpp"
#include "mth/core/game/game_layout.hpp"
#include "mth/core/game/game_state_ids.hpp"
#include "pal/pal_module.hpp"

namespace mth::test
{

// A scene-graph node. The walk reads the first word for something vtable-shaped, so a node is its own handle.
struct FakeComponent
{
    void *vtable{nullptr};
    std::uint64_t type{0};
    std::uint64_t base{0}; // a second id the node also answers to (ComponentIsa is a hierarchy test)
    std::vector<void *> children;
};

struct FakeScene
{
    std::deque<FakeComponent> nodes; // node addresses are the handles, so they must be stable
    int vtable_anchor{0};

    void *add(std::uint64_t type, void *parent, std::uint64_t base = 0)
    {
        FakeComponent &n = nodes.emplace_back();
        n.vtable = &vtable_anchor;
        n.type = type;
        n.base = base;
        if (parent != nullptr)
            static_cast<FakeComponent *>(parent)->children.push_back(&n);
        return &n;
    }
};

// The game behind MinaModAPI: a player in a room, the world's pause and transition states, the death sequence, save slots and a scene graph. `log`
// collects what a player would observe. Constructing one installs its API; destruction restores the previous API and the published .text range.
class FakeGame
{
  public:
    std::uint32_t revision{148716};
    bool install_fails{false};
    int game_state{kGameStateFirstWorld};
    int sparks{0};
    bool proto_spark{false};  // the death sequence ends in a revive in place
    bool invulnerable{false}; // PlayerDie does nothing
    std::set<int> applied_cheats;
    int active_save_slot{0};
    std::string active_save_contents;
    bool save_write_enabled{true};
    FakeScene scene;
    std::vector<std::string> log;
    std::function<void()> on_tick; // the mod's per-tick work, run after the game's own update
    MinaModAPI api;

    FakeGame() : api(make_api()), previous_api_(mod::set_api(&api)), previous_range_(pal::text_range_storage())
    {
        previous_game_ = current();
        current() = this;
    }
    ~FakeGame()
    {
        current() = previous_game_;
        mod::set_api(previous_api_);
        pal::set_game_text_range(previous_range_);
    }
    FakeGame(const FakeGame &) = delete;
    FakeGame &operator=(const FakeGame &) = delete;

    void note(std::string event)
    {
        log.push_back(std::move(event));
    }
    [[nodiscard]] long count(const std::string &event) const
    {
        return std::count(log.begin(), log.end(), event);
    }

    // The Player object the mod reads the death-guard byte and state from; null while no player exists.
    [[nodiscard]] void *player()
    {
        return present_ ? player_bytes_.data() : nullptr;
    }

    void tick()
    {
        const bool gameplay = !menu_open_ && !game_paused_ && !room_changing_ && death_screen_ticks_ == 0;
        if (gameplay)
            room_time_ += 1.0f / kTicksPerSecond;
        if (gameplay && present_)
            advance_requested_deaths();
        advance_death(gameplay && present_);
        if (on_tick)
            on_tick();
    }
    void play_for(double seconds)
    {
        for (int i = 0; i < static_cast<int>(seconds * kTicksPerSecond); ++i)
            tick();
    }

    void die_locally()
    {
        register_death();
    }
    void open_menu()
    {
        menu_open_ = true;
    }
    void close_menu()
    {
        menu_open_ = false;
    }
    // A game-level pause skips the world update queue: the room clock stops but WorldIsPaused stays false.
    void pause_game()
    {
        game_paused_ = true;
    }
    void resume_game()
    {
        game_paused_ = false;
    }
    // The old area is torn down and the room clock stalls until the next room binds its area.
    void begin_room_change()
    {
        room_changing_ = true;
        area_bound_ = false;
    }
    void finish_room_change()
    {
        room_changing_ = false;
        area_bound_ = true;
    }
    void despawn_player()
    {
        present_ = false;
    }
    void spawn_player()
    {
        present_ = true;
        health_ = 1.0f;
        set_dying(false);
        set_player_state(0);
    }
    void enter_cutscene()
    {
        set_player_state(kAreaChangeState);
    }
    void leave_cutscene()
    {
        set_player_state(0);
    }
    // The ending keeps a settled player in a World with no area bound.
    void enter_credits()
    {
        area_bound_ = false;
    }
    void leave_credits()
    {
        area_bound_ = true;
    }

  private:
    // Model values, none measured in game: the 0.7s register delay, the 1.5s sequence and its 0.3s guard-byte pulse, the 2s death screen.
    // Also unverified: the death sequence advances only on room-clock ticks, and the room clock runs with no player.
    static constexpr int kTicksPerSecond = 120;
    static constexpr int kDeathRegisterTicks = kTicksPerSecond * 7 / 10;
    static constexpr int kDeathSequenceTicks = kTicksPerSecond * 3 / 2;
    static constexpr int kDeathPulseTicks = kTicksPerSecond * 3 / 10;
    static constexpr int kDeathScreenTicks = kTicksPerSecond * 2;
    static constexpr int kAreaChangeState = 7;

    static FakeGame *&current()
    {
        static FakeGame *game = nullptr;
        return game;
    }

    void set_dying(bool dying)
    {
        player_bytes_[static_cast<std::size_t>(layout::kPlayerDeathGuardOff)] = dying ? 1 : 0;
    }
    [[nodiscard]] bool dying() const
    {
        return dying_ticks_ > 0 || death_screen_ticks_ > 0;
    }
    void set_player_state(int s)
    {
        std::memcpy(player_bytes_.data() + layout::kPlayerStateOff, &s, sizeof(s));
    }
    [[nodiscard]] int player_state() const
    {
        int s = 0;
        std::memcpy(&s, player_bytes_.data() + layout::kPlayerStateOff, sizeof(s));
        return s;
    }

    // A death that registers while one is under way runs the teardown again.
    void register_death()
    {
        note("died");
        set_dying(true);
        health_ = 0.0f;
        sparks = 0;
        dying_ticks_ = kDeathSequenceTicks;
    }
    void player_die()
    {
        if (!present_ || invulnerable || dying())
            return;
        if (room_changing_ || player_state() != 0)
        {
            note("softlocked");
            return;
        }
        if (!area_bound_)
        {
            note("crashed");
            return;
        }
        requested_deaths_.push_back(kDeathRegisterTicks);
    }
    void advance_requested_deaths()
    {
        for (std::size_t i = 0; i < requested_deaths_.size();)
        {
            if (--requested_deaths_[i] > 0)
            {
                ++i;
                continue;
            }
            requested_deaths_.erase(requested_deaths_.begin() + static_cast<std::ptrdiff_t>(i));
            register_death();
        }
    }
    // Mid-sequence the guard byte drops at 0 health. The sequence ends in a revive or the death screen, which has no player, then a respawn.
    void advance_death(bool gameplay)
    {
        if (dying_ticks_ > 0 && gameplay)
        {
            --dying_ticks_;
            const int pulse_end = (kDeathSequenceTicks - kDeathPulseTicks) / 2;
            set_dying(dying_ticks_ >= pulse_end + kDeathPulseTicks || dying_ticks_ < pulse_end);
            if (dying_ticks_ > 0)
                return;
            if (proto_spark)
            {
                proto_spark = false;
                set_dying(false);
                health_ = 1.0f;
                note("revived");
                return;
            }
            present_ = false;
            game_state = kGameStateDeath;
            death_screen_ticks_ = kDeathScreenTicks;
            note("death screen");
        }
        else if (death_screen_ticks_ > 0 && --death_screen_ticks_ == 0)
        {
            spawn_player();
            game_state = kGameStateFirstWorld;
            note("respawned");
        }
    }

    // Carries the AreaManager slot Player::InitDeath walks, where only non-null matters. Null without a player.
    World *world()
    {
        void *area = area_bound_ ? static_cast<void *>(this) : nullptr;
        std::memcpy(world_bytes_.data() + layout::kWorldAreaManagerOff, &area, sizeof(area));
        return present_ ? reinterpret_cast<World *>(world_bytes_.data()) : nullptr;
    }

    static MinaModAPI make_api()
    {
        MinaModAPI mm{};
        mm.APIVersion = MinaModAPI_Version;
        mm.InstallHook = [](const char *, std::int32_t, MM_HookCallback) -> void * { return current()->install_fails ? nullptr : &current()->api; };
        mm.RemoveHook = [](void *) {};
        mm.GetGameRevision = [] { return current()->revision; };
        mm.GetCurrentGameState = [] { return static_cast<std::int32_t>(current()->game_state); };

        mm.PlayerGetHealth = [] { return current()->health_; };
        mm.PlayerGetSpark = [] { return static_cast<std::int32_t>(current()->sparks); };
        mm.PlayerDie = [] { current()->player_die(); };
        mm.PlayerGetWorld = [] { return current()->world(); };
        mm.GetRoomTime = [] { return current()->room_time_; };
        mm.WorldIsPaused = [](World *) { return current()->menu_open_; };

        mm.GetActiveSaveSlot = [] { return static_cast<std::int32_t>(current()->active_save_slot); };
        mm.SetActiveSaveSlotContents = [](const char *d)
        {
            current()->active_save_contents = d != nullptr ? d : "";
            current()->note("active save slot written");
            return true;
        };
        mm.SetSaveSlotContents = [](std::uint32_t slot, const char *)
        {
            current()->note("save slot " + std::to_string(slot) + " written");
            return true;
        };
        mm.GetActiveSaveSlotContents = []() -> char *
        {
            const std::string &s = current()->active_save_contents;
            char *buf = static_cast<char *>(std::malloc(s.size() + 1));
            std::memcpy(buf, s.c_str(), s.size() + 1);
            return buf;
        };
        mm.Free = [](void *p) { std::free(p); };
        mm.SetSaveWriteEnabled = [](bool on) { current()->save_write_enabled = on; };
        mm.IsSaveWriteEnabled = [] { return current()->save_write_enabled; };

        mm.WorldGetGameRootEntity = [](World *) -> ycEntity * { return nullptr; };
        mm.EntityGetChildren = [](ycEntity *entity, ycComponent **out, std::size_t cap) -> std::size_t
        {
            auto *n = reinterpret_cast<FakeComponent *>(entity);
            if (n == nullptr)
                return 0;
            if (out != nullptr)
                for (std::size_t i = 0; i < n->children.size() && i < cap; ++i)
                    out[i] = reinterpret_cast<ycComponent *>(n->children[i]);
            return n->children.size();
        };
        mm.ComponentIsa = [](ycComponent *component, MM_Rtti rtti)
        {
            auto *n = reinterpret_cast<FakeComponent *>(component);
            return n != nullptr && (n->type == rtti.typeId || (n->base != 0 && n->base == rtti.typeId));
        };
        mm.CreateWeakPtr = [](void *target) { return static_cast<MM_WeakPtr *>(target); };
        mm.WeakPtrGet = [](MM_WeakPtr *weak) -> void * { return weak; };
        mm.DestroyWeakPtr = [](MM_WeakPtr *) {};

        // Entries appended after revision 148905. Each answers so a caller can tell the call reached the game.
        mm.WorldQueueDestroyEntity = [](World *, ycEntity *, bool) {};
        mm.ItemsSetItemCollected = [](std::int32_t index, bool, ItemCollection *, SaveSlot *)
        { current()->note("item " + std::to_string(index) + " collected"); };
        mm.TextComponentSetColor = [](ycComponent *, MM_Color) {};
        mm.TextComponentSetText = [](ycComponent *, const char *const) {};
        mm.TextComponentGetText = [](ycComponent *) { return ""; };
        mm.ClonePalette = [](ycPaletteTexture *pal) { return pal; };
        mm.PaletteSetGroup = [](ycPaletteTexture *, std::int32_t) {};
        mm.PaletteWriteIndex = [](ycPaletteTexture *, std::int32_t, MM_Color) {};
        mm.PaletteGetIndex = [](ycPaletteTexture *, MM_Color) { return std::int32_t{0}; };
        mm.PaletteGetWidth = [](ycPaletteTexture *) { return std::uint32_t{0}; };
        mm.WaterListenerIsInDeepWater = [](WaterListener *, bool) { return true; };
        mm.PhysicsComponentGetAABB = [](PhysicsComponent *, MM_AABB *, bool, std::uint32_t) {};
        mm.CarryManagerGetClosestCarryableObject = [](CarryManager *manager, MM_AABB, std::int32_t, float, std::int32_t *, std::uint64_t)
        { return reinterpret_cast<CarryableObject *>(manager); };
        mm.PlayerUpdateStats = [] {};
        mm.WorldGetEntityList = [](World *, const char *, GameComponent **, std::size_t) { return std::size_t{1}; };
        mm.GetSymAddr = [](const char *name) { return static_cast<void *>(const_cast<char *>(name)); };
        mm.CheatManagerSetCheatApplied = [](std::int32_t cheat, bool active, std::uint32_t)
        {
            if (active)
                current()->applied_cheats.insert(cheat);
            else
                current()->applied_cheats.erase(cheat);
            current()->note("cheat " + std::to_string(cheat) + (active ? " on" : " off"));
        };
        mm.CheatManagerIsCheatApplied = [](std::int32_t cheat, std::uint32_t) { return current()->applied_cheats.count(cheat) != 0; };
        mm.CheatManagerActivateSaveCheats = [] {};
        return mm;
    }

    std::vector<unsigned char> player_bytes_ = std::vector<unsigned char>(layout::kPlayerDeathGuardOff + 16, 0);
    std::vector<unsigned char> world_bytes_ = std::vector<unsigned char>(layout::kWorldAreaManagerOff + sizeof(void *), 0);
    bool present_{true};
    float health_{1.0f};
    bool menu_open_{false};
    bool game_paused_{false};
    bool room_changing_{false};
    bool area_bound_{true};
    float room_time_{0.0f};
    std::vector<int> requested_deaths_; // gameplay ticks until each requested death registers
    int dying_ticks_{0};
    int death_screen_ticks_{0};
    MinaModAPI *previous_api_;
    pal::TextRange previous_range_;
    FakeGame *previous_game_{nullptr};
};

} // namespace mth::test
