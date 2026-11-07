#pragma once

#include <atomic>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "mth/core/save/zip_archive.hpp"

namespace mth
{

// "ap_<seed>_<slot>.zip", seed/slot sanitized. Unlike ap_save_filename there is no hash suffix: the
// container's manifest carries the raw key parts, so a sanitization collision is caught on load by
// comparing identity rather than by making the name unique.
std::string ap_bundle_filename(std::string_view seed, std::string_view slot);

// One container per (seed, slot) holding the game save blob and the AP state together, so the two
// can never disagree about where a run got to. Reads fall back to the pre-container layout and the
// first write migrates forward; the old files are left in place.
//
// The in-memory copy is the source of truth, and store() alone publishes it. AP mutations stage
// into that copy and wait there, so the bytes on disk are always the AP state as it stood when the
// game last saved. Publishing them as they happen would pair a newer AP state with an older save
// blob, and a death or a crash would then roll the run back while the state file still said the
// items had been handed out, which loses them permanently (see #77).
//
// store() writes the container before it returns, because the mod is leaked by design and never runs
// its destructor, so nothing deferred to process exit would land.
class ApSaveBundleStore
{
  public:
    struct LegacyDirs
    {
        std::filesystem::path state_dir;  // where the loose .state used to live
        std::filesystem::path ycsave_dir; // where the loose .ycsave used to live
    };

    ApSaveBundleStore(std::filesystem::path dir, LegacyDirs legacy);

    ApSaveBundleStore(const ApSaveBundleStore &) = delete;
    ApSaveBundleStore &operator=(const ApSaveBundleStore &) = delete;

    [[nodiscard]] std::filesystem::path path_for(std::string_view seed, std::string_view slot) const;

    // The game save blob. Named to match the store this replaced so the takeover call sites did
    // not have to change. store() is the commit point for the whole container, staged AP state
    // included, and returns once the write has been attempted.
    [[nodiscard]] std::optional<std::string> load(std::string_view seed, std::string_view slot) const;
    bool store(std::string_view seed, std::string_view slot, std::string_view blob);

    // Staged, not written: the bool says the state was accepted into the in-memory copy, where it
    // waits for the store() that pairs it with a save blob, and a session change discards it.
    // Read-back through load_state() is consistent either way, because reads come from memory.
    [[nodiscard]] std::optional<std::string> load_state(std::string_view seed, std::string_view slot) const;
    bool stage_state(std::string_view seed, std::string_view slot, std::string_view text);

    // True when a stage_state() is waiting for the store() that will publish it.
    [[nodiscard]] bool state_staged() const;

  private:
    // Immutable once published. Shared so the compressed-entry cache below can recognise an
    // unchanged payload by pointer.
    using Payload = std::shared_ptr<const std::string>;

    // Game thread only. One session is live at a time, so a key change reloads rather than growing
    // a map.
    struct Cache
    {
        bool loaded{false};
        std::string seed;
        std::string slot;
        // Whatever the container held, preserved verbatim even when we decline to USE it: load()
        // does the validating. Anything dropped here is dropped from the next write, which is how a
        // save the mod does not recognise would get deleted. Null means absent.
        Payload game_save;
        Payload ap_state;
        // A container at our path that names another run. Writing would destroy it, so we refuse.
        bool foreign{false};
    };

    // Outside Cache because the overlay render thread reads it for the status pane while the game
    // thread stages and commits. Cleared alongside the cache on a key change, which is what discards
    // the state a session we left never committed.
    mutable std::atomic<bool> state_staged_{false};

    void ensure_loaded(std::string_view seed, std::string_view slot) const;
    bool write_snapshot(); // writes cache_ to disk

    std::filesystem::path dir_;
    LegacyDirs legacy_;
    mutable Cache cache_;

    // Compressed form of an entry, kept next to the exact payload it came from. Identity is the
    // pointer, not the size: the save is a text format, so a bool flipping or a digit changing keeps
    // its length, and matching on size would re-emit the previous generation. Holding the payload
    // strongly is what makes the pointer test sound, since a freed address could otherwise be
    // recycled into a false match.
    struct CachedBlob
    {
        Payload source;
        zip::Blob blob;
    };

    // Owned by write_snapshot().
    std::string writer_key_;
    std::map<std::string, CachedBlob> writer_blobs_;
    // The backup is taken once per session, not per write. Rotating on every checked location would
    // leave it seconds old and worth nothing; this keeps the run as it was when the session opened.
    bool writer_backed_up_{false};
    bool dir_ready_{false};
};

} // namespace mth
