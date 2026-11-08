#include "mth/features/save_takeover.hpp"

#include "mod/mod_api.hpp"
#include "mth/core/game/component_types.hpp"
#include "mth/core/game/game_layout.hpp"
#include "mth/core/game/game_state_ids.hpp"
#include "mth/core/game/game_symbols.hpp"
#include "mth/features/scene_gates/scene_walk.hpp"
#include "pal/pal_game.hpp"
#include "pal/pal_layout.hpp"
#include "pal/pal_log.hpp"
#include "pal/pal_mem.hpp"
#include "pal/pal_module.hpp"
#include "pal/scoped_hook.hpp"

namespace mth
{
namespace
{
// SaveManager::WriteSaveData(bool), observed as the mod-save flush trigger on the game's save cadence. SetSaveWriteEnabled(false) via MinaModAPI is what keeps
// saveData.yc untouched.
std::function<void()> g_save_request_fn;
[[clang::no_destroy]] pal::ScopedHook g_save_request_hook;
void (*g_orig_write_save_data)(void *, bool) = nullptr;

void repl_write_save_data(void *self, bool flag)
{
    if (g_save_request_fn)
        g_save_request_fn();
    if (g_orig_write_save_data)
        g_orig_write_save_data(self, flag);
}

bool install_save_request_hook(std::function<void()> on_save)
{
    g_save_request_fn = std::move(on_save);
    const std::uintptr_t addr = pal::resolve_game_symbol(sym::save_manager_write_save_data);
    if (addr == 0)
    {
        pal::logf(pal::LogLevel::Warn, "takeover: SaveManager::WriteSaveData not resolved; mod saves will not flush");
        g_save_request_fn = nullptr;
        return false;
    }
    g_save_request_hook = pal::ScopedHook(addr, &repl_write_save_data, &g_orig_write_save_data);
    if (!g_save_request_hook.installed())
    {
        pal::logf(pal::LogLevel::Error, "takeover: failed to hook SaveManager::WriteSaveData");
        g_save_request_fn = nullptr;
        return false;
    }
    pal::logf(pal::LogLevel::Info, "takeover: hooked SaveManager::WriteSaveData at %p", g_save_request_hook.target());
    return true;
}

// The vanilla slot staged into, fixed so the takeover is deterministic. Its on-disk contents are never modified.
constexpr unsigned int kScratchSlot = 0;

// The scene walk returns the object's primary pointer on both platforms, so the shared mth::layout offsets apply with no per-platform subobject adjust.
//
// A menu outside the state range its dispatch tables accept is mid-construction or mis-based, and writing through a mis-based one corrupts whatever the offsets
// hit.
bool menu_drivable(void *menu)
{
    if (!pal::pointer_looks_valid(menu))
        return false;
    const int state = pal::at<int>(menu, layout::kProfileMenuStateOff);
    if (state >= layout::kProfileMenuStateMin && state <= layout::kProfileMenuStateMax)
        return true;
    static bool warned = false;
    if (!warned)
    {
        warned = true;
        pal::logf(pal::LogLevel::Warn, "takeover: profile menu %p state %d outside [%d,%d]; not driving it", menu, state, layout::kProfileMenuStateMin,
                  layout::kProfileMenuStateMax);
    }
    return false;
}

int menu_state(void *menu)
{
    return menu_drivable(menu) ? pal::at<int>(menu, layout::kProfileMenuStateOff) : -1;
}

// The live slot holds the player's run only while gameplay owns it. Returning to the title runs
// SaveSlot::Clear over it and deactivates it, so anything read from there is a blank save.
bool in_gameplay()
{
    const int gs = mod::current_game_state();
    // -1 (accessor unavailable) must not read as gameplay. Profile-select is its own gamestate, so excluding only the title would count the menu as gameplay.
    return gs >= 0 && gs != kGameStateTitleScreen && gs != kGameStateProfileSelect;
}

// Requests the state so the menu's own state machine runs its enter/exit. Idempotent, since re-requesting a pending state would restart it.
bool menu_request_state(void *menu, int state)
{
    if (!menu_drivable(menu))
        return false;
    int *current = &pal::at<int>(menu, layout::kProfileMenuStateOff);
    int *next = &pal::at<int>(menu, layout::kProfileMenuNextStateOff);
    if (*current == state || *next == state)
        return true;
    *next = state;
    pal::at<unsigned char>(menu, layout::kProfileMenuStateReqOff) = 1;
    return true;
}
// Resolved on first use: on Windows each resolve is a full .text signature scan, and the staging path runs on a game frame.
std::uintptr_t g_takeover_slot_clear = 0;
std::uintptr_t g_takeover_init_gamestate = 0;
bool g_takeover_syms_resolved = false;

void resolve_takeover_symbols()
{
    if (g_takeover_syms_resolved)
        return;
    g_takeover_syms_resolved = true;
    g_takeover_slot_clear = pal::resolve_game_symbol(sym::save_slot_clear);
    g_takeover_init_gamestate = pal::resolve_game_symbol(sym::save_slot_init_gamestate);
    if (pal::save_manager_global() == 0 || g_takeover_slot_clear == 0 || g_takeover_init_gamestate == 0)
        pal::logf(pal::LogLevel::Warn, "takeover: g_saveManager/SaveSlot::Clear/InitGamestate not all resolved; a new save cannot be staged");
}

// Runs the game's new-file init on the vanilla SaveSlot array element for `slot`; the launch copies array over working, so a working-slot write would be
// discarded. Clear(false) then InitGamestate(). Does not persist (WriteSaveData is not on the launch path). false if the symbols or slot address do not
// resolve.
bool init_new_save_file(unsigned int slot)
{
    resolve_takeover_symbols();
    if (pal::save_manager_global() == 0 || g_takeover_slot_clear == 0 || g_takeover_init_gamestate == 0)
    {
        pal::logf(pal::LogLevel::Warn, "takeover: SaveSlot::Clear/InitGamestate or g_saveManager not resolved; cannot init a new save");
        return false;
    }
    const std::uintptr_t master = pal::at<std::uintptr_t>(pal::save_manager_global(), pal::layout::kSaveManagerMasterOff);
    if (master == 0)
    {
        pal::logf(pal::LogLevel::Warn, "takeover: save master table not resolved; cannot init a new save");
        return false;
    }
    void *file_slot = reinterpret_cast<void *>(master + layout::kSaveSlotArrayOff + static_cast<std::uintptr_t>(slot) * layout::kSaveSlotStride);
    if (!pal::pointer_looks_valid(file_slot))
    {
        pal::logf(pal::LogLevel::Warn, "takeover: save slot %u address looks invalid; cannot init a new save", slot);
        return false;
    }
    auto clear_fn = reinterpret_cast<void (*)(void *, bool)>(g_takeover_slot_clear);
    auto init_fn = reinterpret_cast<void (*)(void *)>(g_takeover_init_gamestate);
    pal::logf(pal::LogLevel::Info, "takeover: initializing new save file (array slot %u at %p)", slot, file_slot);
    // false = fresh file, as the vanilla profile-select new-file sites pass; true is the NG+ cycle. Calling the real address re-triggers the newfile-kit
    // suppressor, so AP's starting kit wins.
    clear_fn(file_slot, false);
    init_fn(file_slot);
    return true;
}

} // namespace

SaveTakeover::SaveTakeover(ApSaveBundleStore &store, IdentityFn identity) : store_(store), identity_(std::move(identity))
{
    if (!install_save_request_hook([this] { on_game_save_requested(); }))
        pal::logf(pal::LogLevel::Warn, "takeover: save-request hook unavailable; mod saves will not flush");
    if (!mod::install_input_suppress_hooks())
        pal::logf(pal::LogLevel::Warn, "takeover: input hooks unavailable; a held confirm can still reach the file-select menu");
}

std::filesystem::path mod_save_dir()
{
    return pal::log_dir() / "saves";
}

bool SaveTakeover::begin()
{
    // Without the scene walk nothing finds the menu to drive, and the bounce back to the title runs off the same pass, so refuse the launch.
    if (!mod::entity_walk_api_available() || !mod::world_menu_root_api_available())
    {
        pal::logf(pal::LogLevel::Error, "takeover: scene-walk API unavailable; refusing to start");
        return false;
    }

    // Running (back at the title from a live session) and Failed both re-arm; other non-Idle steps are mid-sequence. No flush here: begin() runs only from the
    // title, where the slot is already cleared, so it would store a blank save over the run (#152).
    if (step_ != TakeoverStep::Running && !takeover_settled(step_))
        return true; // already claimed, mid-flight

    auto [seed, slot] = identity_();
    seed_ = std::move(seed);
    slot_ = std::move(slot);

    mod::set_save_write_enabled(false); // re-assert: off since connect; covers a savetest re-enable
    step_ = TakeoverStep::AwaitingMenu;
    frames_in_step_ = 0;
    input_block_.rearm();
    // Run now, inside the StartGame confirm, because the very next input update would carry a held button into the file-select menu.
    update_input_block();
    // InstallHook succeeds for a name the build never dispatches, so only firing proves the hooks. They fire every input frame, so silence by the time a launch
    // is claimed means this build has none.
    if (!mod::input_suppress_hooks_fired() && !warned_input_inert_)
    {
        warned_input_inert_ = true;
        pal::logf(pal::LogLevel::Error, "takeover: input hooks have never fired; a held confirm can still open a vanilla file");
    }
    pal::logf(pal::LogLevel::Info, "takeover: begin seed=%s slot=%s", seed_.c_str(), slot_.c_str());
    return true;
}

// The block outlives the steps that want it: a failed takeover still leaves the player on a live file-select for the bounce frames.
void SaveTakeover::update_input_block()
{
    // Only the Failed bounce reads this, so the accessor stays off otherwise. An unavailable accessor reads as "menu gone" and releases, unlike in_gameplay():
    // a dead controller on a build that cannot answer is worse than the window it would cover.
    const bool on_profile_select = step_ == TakeoverStep::Failed && mod::current_game_state() == kGameStateProfileSelect;

    const bool was_blocked = input_block_.blocked();
    const bool block = input_block_.advance(step_, on_profile_select);
    mod::set_input_suppressed(block); // unconditional, so the flag cannot drift from what this decided
    if (block != was_blocked)
        pal::logf(pal::LogLevel::Info, "takeover: game input %s (step=%s)", block ? "swallowed" : "restored", takeover_step_name(step_));
}

void SaveTakeover::set_step(TakeoverStep next)
{
    pal::logf(pal::LogLevel::Info, "takeover: %s -> %s", takeover_step_name(step_), takeover_step_name(next));
    step_ = next;
    frames_in_step_ = 0;
}

void SaveTakeover::tick()
{
    update_input_block(); // ahead of the early return: Failed still blocks while the menu is up

    if (takeover_settled(step_))
        return; // begin() re-arms these; the save-request hook drives the flush

    TakeoverInputs in;
    in.save_api_ready = mod::save_api_available();
    in.gameplay_entered = in_gameplay();
    in.frames_in_step = frames_in_step_;

    const TakeoverStep next = next_takeover_step(step_, in);
    if (next == step_)
    {
        ++frames_in_step_;
        return;
    }
    if (next == TakeoverStep::Failed)
    {
        fail("the sequence stalled or the save api dropped");
        return;
    }
    set_step(next);
}

void SaveTakeover::fail(const char *why)
{
    // Writes stay disabled: a half-taken-over session must not persist anything. The profile-menu
    // callback returns the player to the title from here.
    set_step(TakeoverStep::Failed);
    pal::logf(pal::LogLevel::Error, "takeover: aborted (%s); save writes remain disabled", why);
}

// The ProfileSelectMenu is docked on the menu world's root entity, so this is normally one level; descending covers a build that nests it. Menu worlds are
// tiny, so this runs every pass and the launch does not wait for a cadence.
void SaveTakeover::on_world_update_end(void *world)
{
    // Only the AwaitingMenu claim and the Failed bounce need the menu; every other step ignores it.
    if (world == nullptr || (step_ != TakeoverStep::AwaitingMenu && step_ != TakeoverStep::Failed))
        return;
    if (!mod::entity_walk_api_available() || !mod::world_menu_root_api_available())
        return;
    void *root = mod::world_menu_root_entity(world);
    if (root == nullptr)
        return;

    // ProfileSelectMenu is a plain ycComponent, so it is a leaf: the walk never descends into it.
    const SceneWalk walk = walker_.find_first(root, rtti::kProfileSelectMenu, [&](void *c) { drive_profile_menu(c); });
    // Silence is normal here too: the menu exists only between title and launch, so it is a real signal only while a claim waits on it.
    walker_.report_silence(walk, step_ == TakeoverStep::AwaitingMenu);
}

void SaveTakeover::drive_profile_menu(void *menu)
{
    // A takeover that gave up must not leave the player on a live profile-select, where they could
    // start a vanilla save inside a mod session.
    if (step_ == TakeoverStep::Failed)
    {
        menu_request_state(menu, layout::kProfileMenuBackState);
        return;
    }
    if (step_ != TakeoverStep::AwaitingMenu)
        return;
    // Browse state only. The animate-in states run while the intro cinematic still scrolls backdrop layers out of the resident world region; launching there
    // swaps that region away and the menu never reaches the state that watches for the launch.
    if (menu_state(menu) != layout::kProfileMenuBrowseState)
        return;

    pal::at<int>(menu, layout::kProfileMenuSlotOff) = static_cast<int>(kScratchSlot);
    if (!stage_save() || !menu_request_state(menu, layout::kProfileMenuLaunchState))
    {
        fail("could not stage the save or drive the profile menu");
        menu_request_state(menu, layout::kProfileMenuBackState);
        return;
    }
    pal::logf(pal::LogLevel::Info, "takeover: profile menu %p driven to launch state on slot %u", menu, kScratchSlot);
    set_step(TakeoverStep::Launching);
}

bool SaveTakeover::stage_save()
{
    // Writes the vanilla save file; the launch copies file over working, so a working-slot write would be discarded.
    if (const auto blob = store_.load(seed_, slot_))
    {
        if (!mod::set_save_slot_contents(kScratchSlot, blob->c_str()))
        {
            pal::logf(pal::LogLevel::Error, "takeover: failed to stage the mod save into slot %u", kScratchSlot);
            return false;
        }
        pal::logf(pal::LogLevel::Info, "takeover: staged existing mod save into slot %u", kScratchSlot);
        return true;
    }
    if (!init_new_save_file(kScratchSlot))
    {
        pal::logf(pal::LogLevel::Error, "takeover: failed to initialize a new save");
        return false;
    }
    return true;
}

void SaveTakeover::on_game_save_requested()
{
    // Logged before the step test so "the game never asked to save" and "we declined to commit" leave different logs.
    pal::logf(pal::LogLevel::Debug, "takeover: game save requested (step=%s)", takeover_step_name(step_));
    flush();
}

void SaveTakeover::flush()
{
    const auto [current_seed, current_slot] = identity_();
    if (const char *refusal = flush_refusal({step_, in_gameplay(), seed_, slot_, current_seed, current_slot}); refusal != nullptr)
    {
        // The game asking with no claim is every vanilla save, so Debug. Otherwise a commit the player expected did not happen.
        const pal::LogLevel level = step_ != TakeoverStep::Running ? pal::LogLevel::Debug : pal::LogLevel::Warn;
        pal::logf(level, "takeover: skipping flush (%s; step=%s seed=%s slot=%s now seed=%s slot=%s)", refusal, takeover_step_name(step_), seed_.c_str(),
                  slot_.c_str(), current_seed.c_str(), current_slot.c_str());
        return;
    }

    // Only under a live claim: suppressing the game's writes outside one would break vanilla saving without an AP session.
    if (mod::save_write_enabled())
    {
        pal::logf(pal::LogLevel::Warn, "takeover: game save writes were re-enabled; suppressing");
        mod::set_save_write_enabled(false);
    }

    const std::string blob = mod::active_save_slot_contents();
    if (blob.empty())
    {
        pal::logf(pal::LogLevel::Warn, "takeover: empty save blob; skipping flush");
        return;
    }
    // The only place AP state reaches disk, so this log shows a run's progress and checks were committed together.
    if (!store_.store(seed_, slot_, blob))
        pal::logf(pal::LogLevel::Error, "takeover: failed to write mod save for seed=%s slot=%s", seed_.c_str(), slot_.c_str());
    else
        pal::logf(pal::LogLevel::Debug, "takeover: committed save and ap state for seed=%s slot=%s", seed_.c_str(), slot_.c_str());
}

std::vector<std::string> SaveTakeover::status_lines() const
{
    std::vector<std::string> out;
    out.push_back(std::string("takeover: ") + takeover_step_name(step_) + (input_block_.blocked() ? " (game input swallowed)" : ""));
    if (!seed_.empty())
        out.push_back("takeover save: " + store_.path_for(seed_, slot_).string());
    return out;
}

} // namespace mth
