#pragma once

#include "mth/core/rules/lock_registry.hpp"

namespace mth
{
class ApState;

// twin: mth/core/rules/lock_registry.hpp (pure removed-set).
// Kear-lock removal for KeyBlock, KeyBlockChain and locked Chest. sweep() walks the world's own "KeyBlock"
// and "KeyBlockChain" entity lists: it removes already-spawned single-block locks whose slot is in the
// registry, and drives multi-block KeyBlockChain locks (a distinct class) to their open state. Both
// handlers only read then write the entity, so neither needs to run at a point inside the game's own update.
// seed_removed_locks() sets the persistent SaveSlot unlock bit each tick for re-entry.
//
// A locked Chest gates on the same SaveSlot+0x200 bit a removed lock sets, so a registered slot has its
// locked flag cleared live by sweep() too; reload rides the lock seed and the chest ctor's own gate. The
// world exposes no "Chest" entity list, so live chests come from the native ChestConstruct hook and are
// held as weak pointers until the world tears them down.
class LockHooks
{
  public:
    LockHooks();
    ~LockHooks();
    LockHooks(const LockHooks &) = delete;
    LockHooks &operator=(const LockHooks &) = delete;

    LockRegistry &locks(); // populated by seed_kear_blocks and the console
    // Received kear-block and area-lock items -> registry removals. Only ever adds; a session boundary
    // clears the registry. Game thread, per tick.
    void seed_kear_blocks(const ApState &state);
    void seed_removed_locks(); // game-thread, pre-World::Update window
    void sweep();              // game-thread, same window
    void on_world_destroy();   // the world's chests are gone; drop every tracked handle

  private:
    LockRegistry locks_;
};

} // namespace mth
