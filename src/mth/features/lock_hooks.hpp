#pragma once

#include "mth/core/rules/lock_registry.hpp"

namespace mth
{
class ApState;

// twin: mth/core/rules/lock_registry.hpp (pure removed-set).
// Kear-lock removal for KeyBlock, KeyBlockChain and locked Chest. sweep() walks the world's "KeyBlock" and "KeyBlockChain" entity lists, removing spawned
// single-block locks whose slot is registered and driving multi-block chains (a distinct class) open. Both only read then write the entity, so they can run
// outside the game's update. seed_removed_locks() sets the persistent SaveSlot unlock bit each tick for re-entry.
//
// A locked Chest checks the same SaveSlot+0x200 bit, so sweep() also clears a registered chest's locked flag live; on reload the lock seed and the chest ctor's
// own check cover it. There is no "Chest" entity list, so live chests come from the native ChestConstruct hook and are held as weak pointers until world
// teardown.
class LockHooks
{
  public:
    LockHooks();
    ~LockHooks();
    LockHooks(const LockHooks &) = delete;
    LockHooks &operator=(const LockHooks &) = delete;

    LockRegistry &locks(); // populated by seed_kear_blocks and the console
    // Received kear-block and area-lock items become registry removals. Add-only; a session boundary clears the registry. Game thread, per tick.
    void seed_kear_blocks(const ApState &state);
    void seed_removed_locks(); // game-thread, pre-World::Update window
    void sweep();              // game-thread, same window
    void on_world_destroy();   // the world's chests are gone; drop every tracked handle

  private:
    LockRegistry locks_;
};

} // namespace mth
