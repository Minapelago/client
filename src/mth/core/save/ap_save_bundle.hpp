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

// "ap_<seed>_<slot>.zip", seed/slot sanitized, with no hash suffix: the manifest carries the raw key parts, so load catches a sanitization
// collision by comparing identity.
std::string ap_bundle_filename(std::string_view seed, std::string_view slot);

// One container per (seed, slot) holding the game save blob and the AP state, so the two never disagree about where a run got to. Reads fall
// back to the pre-container layout and the first write migrates forward, leaving the old files in place.
// The in-memory copy is the source of truth and only store() publishes it, so the disk holds the AP state as of the game's last save. Publishing
// AP mutations as they happen would pair newer state with an older blob, and a death or crash would then lose items for good (#77).
// store() writes before returning because the mod is leaked and never runs its destructor.
class ApSaveBundleStore
{
  public:
    struct LegacyDirs
    {
        std::filesystem::path state_dir;  // pre-container loose .state directory
        std::filesystem::path ycsave_dir; // pre-container loose .ycsave directory
    };

    ApSaveBundleStore(std::filesystem::path dir, LegacyDirs legacy);

    ApSaveBundleStore(const ApSaveBundleStore &) = delete;
    ApSaveBundleStore &operator=(const ApSaveBundleStore &) = delete;

    [[nodiscard]] std::filesystem::path path_for(std::string_view seed, std::string_view slot) const;

    // The game save blob. store() commits the whole container, staged AP state included, and returns once the write has been attempted.
    [[nodiscard]] std::optional<std::string> load(std::string_view seed, std::string_view slot) const;
    bool store(std::string_view seed, std::string_view slot, std::string_view blob);

    // Staged only: true means the state is in the in-memory copy, waiting for the store() that pairs it with a save blob; a session change discards
    // it. load_state() reads from memory, so it is consistent either way.
    [[nodiscard]] std::optional<std::string> load_state(std::string_view seed, std::string_view slot) const;
    bool stage_state(std::string_view seed, std::string_view slot, std::string_view text);

    // True when a stage_state() is waiting for the store() that will publish it.
    [[nodiscard]] bool state_staged() const;

  private:
    // Immutable once published; shared so the compressed-entry cache can recognise an unchanged payload by pointer.
    using Payload = std::shared_ptr<const std::string>;

    // Game thread only. One session is live at a time, so a key change reloads.
    struct Cache
    {
        bool loaded{false};
        std::string seed;
        std::string slot;
        // Whatever the container held, preserved verbatim even when load() declines to use it, since anything dropped here is dropped from the next
        // write. Null means absent.
        Payload game_save;
        Payload ap_state;
        // A container at our path that names another run. Writing would destroy it, so we refuse.
        bool foreign{false};
    };

    // Outside Cache because the overlay render thread reads it for the status pane while the game thread stages and commits. Cleared with the cache
    // on a key change, which discards state the old session never committed.
    mutable std::atomic<bool> state_staged_{false};

    void ensure_loaded(std::string_view seed, std::string_view slot) const;
    bool write_snapshot(); // writes cache_ to disk

    std::filesystem::path dir_;
    LegacyDirs legacy_;
    mutable Cache cache_;

    // Compressed form of an entry, matched by pointer to the exact payload it came from (the save is text, so a flipped bool keeps its size).
    // Holding the payload strongly keeps a freed address from being recycled into a false match.
    struct CachedBlob
    {
        Payload source;
        zip::Blob blob;
    };

    // Owned by write_snapshot().
    std::string writer_key_;
    std::map<std::string, CachedBlob> writer_blobs_;
    // Backup taken once per session, keeping the run as it was when the session opened; rotating per write would leave it seconds old.
    bool writer_backed_up_{false};
    bool dir_ready_{false};
};

} // namespace mth
