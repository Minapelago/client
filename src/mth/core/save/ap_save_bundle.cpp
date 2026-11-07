#include "mth/core/save/ap_save_bundle.hpp"

#include <exception>
#include <fstream>
#include <iterator>
#include <memory>
#include <system_error>
#include <utility>

#include <nlohmann/json.hpp>

#include "mth/core/save/ap_save_store.hpp"
#include "pal/pal_log.hpp"

namespace mth
{
namespace
{
constexpr const char *kEntrySave = "save.ycsave";
constexpr const char *kEntryState = "ap.state";
constexpr const char *kEntryManifest = "manifest.json";
constexpr int kFormatVersion = 1;

std::optional<std::string> read_file(const std::filesystem::path &path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return std::nullopt;
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>{});
}

// The pre-container .state name interpolated the raw seed, so a seed with a separator would escape its directory; refuse to build such a path.
std::optional<std::string> legacy_state_filename(std::string_view seed, std::string_view slot)
{
    for (std::string_view part : {seed, slot})
    {
        if (part.find('/') != std::string_view::npos || part.find('\\') != std::string_view::npos)
            return std::nullopt;
        if (part.find(':') != std::string_view::npos || part.find('\0') != std::string_view::npos)
            return std::nullopt;
        if (part == "." || part == "..")
            return std::nullopt;
    }
    return "ap_" + std::string(seed) + "_" + std::string(slot) + ".state";
}

// Absent/unreadable and "belongs to a different run" must be told apart: the first is safe to
// overwrite, the second never is.
enum class ContainerVerdict
{
    Absent,  // no file, or nothing we can parse
    Foreign, // parsed, but its manifest names another (seed, slot)
    Ours,
};

struct ContainerRead
{
    ContainerVerdict verdict{ContainerVerdict::Absent};
    std::vector<zip::Entry> entries;
};

ContainerRead read_container(const std::filesystem::path &path, std::string_view seed, std::string_view slot)
{
    const auto image = read_file(path);
    if (!image)
        return {};
    auto entries = zip::read(*image);
    if (!entries)
        return {};

    for (const zip::Entry &e : *entries)
    {
        if (e.name != kEntryManifest)
            continue;
        // nlohmann throws on a type mismatch, and this runs under a native game hook with no catch
        // between here and the game's C ABI, so an odd manifest must not become a terminate.
        try
        {
            const auto doc = nlohmann::json::parse(e.data, nullptr, false);
            if (doc.is_discarded() || !doc.is_object())
                return {};
            if (doc.value("format", 0) != kFormatVersion)
                return {};
            if (doc.value("seed", std::string{}) != seed || doc.value("slot", std::string{}) != slot)
                return {ContainerVerdict::Foreign, {}};
        }
        catch (...)
        {
            return {};
        }
        return {ContainerVerdict::Ours, std::move(*entries)};
    }
    return {}; // no manifest: not one of ours
}
} // namespace

std::string ap_bundle_filename(std::string_view seed, std::string_view slot)
{
    return "ap_" + sanitize_save_key_part(seed) + "_" + sanitize_save_key_part(slot) + ".zip";
}

ApSaveBundleStore::ApSaveBundleStore(std::filesystem::path dir, LegacyDirs legacy) : dir_(std::move(dir)), legacy_(std::move(legacy))
{
}

std::filesystem::path ApSaveBundleStore::path_for(std::string_view seed, std::string_view slot) const
{
    return dir_ / ap_bundle_filename(seed, slot);
}

void ApSaveBundleStore::ensure_loaded(std::string_view seed, std::string_view slot) const
{
    if (cache_.loaded && cache_.seed == seed && cache_.slot == slot)
        return;

    state_staged_.store(false, std::memory_order_relaxed);
    cache_ = Cache{};
    cache_.loaded = true;
    cache_.seed = std::string(seed);
    cache_.slot = std::string(slot);

    const auto path = path_for(seed, slot);
    auto read = read_container(path, seed, slot);
    if (read.verdict == ContainerVerdict::Absent)
    {
        // A crash between the backup rotation and the replace leaves the previous generation here.
        auto bak = path;
        bak += ".bak";
        auto from_bak = read_container(bak, seed, slot);
        if (from_bak.verdict == ContainerVerdict::Ours)
            read = std::move(from_bak);
    }

    if (read.verdict == ContainerVerdict::Foreign)
    {
        // Sanitizing is lossy, so two raw seeds can land on one filename. Writing here would destroy
        // whichever run got there first.
        cache_.foreign = true;
        pal::logf(pal::LogLevel::Error, "save: %s belongs to a different seed/slot; this session will not persist (move or delete that file)",
                  path.string().c_str());
        return;
    }

    if (read.verdict == ContainerVerdict::Ours)
    {
        // Preserved verbatim, valid or not; load() decides what is usable, and dropping it here would delete it on the next write.
        for (const zip::Entry &e : read.entries)
        {
            if (e.name == kEntrySave)
                cache_.game_save = std::make_shared<const std::string>(e.data);
            else if (e.name == kEntryState)
                cache_.ap_state = std::make_shared<const std::string>(e.data);
        }
    }
    else if (std::filesystem::exists(path))
    {
        pal::logf(pal::LogLevel::Warn, "save: container %s is unreadable; falling back to the previous layout", path.string().c_str());
    }

    // Per entry, so a container holding only one payload still picks the other up from the old layout.
    if (!cache_.game_save)
    {
        if (auto blob = read_file(legacy_.ycsave_dir / ap_save_filename(seed, slot)); blob && looks_like_save_blob(*blob))
        {
            cache_.game_save = std::make_shared<const std::string>(std::move(*blob));
            pal::logf(pal::LogLevel::Info, "save: adopted the previous-layout game save for seed=%s slot=%s", cache_.seed.c_str(), cache_.slot.c_str());
        }
    }
    if (!cache_.ap_state)
    {
        if (const auto name = legacy_state_filename(seed, slot))
        {
            if (auto text = read_file(legacy_.state_dir / *name))
            {
                cache_.ap_state = std::make_shared<const std::string>(std::move(*text));
                pal::logf(pal::LogLevel::Info, "save: adopted the previous-layout AP state for seed=%s slot=%s", cache_.seed.c_str(), cache_.slot.c_str());
            }
        }
    }
}

bool ApSaveBundleStore::write_snapshot()
{
    const std::string key = cache_.seed + '\0' + cache_.slot;
    if (key != writer_key_)
    {
        writer_key_ = key;
        writer_blobs_.clear();
        writer_backed_up_ = false;
    }

    std::error_code ec;
    if (!dir_ready_)
    {
        std::filesystem::create_directories(dir_, ec);
        if (ec)
        {
            pal::logf(pal::LogLevel::Error, "save: cannot create %s: %s", dir_.string().c_str(), ec.message().c_str());
            return false;
        }
        dir_ready_ = true;
    }

    nlohmann::json manifest;
    manifest["format"] = kFormatVersion;
    manifest["seed"] = cache_.seed;
    manifest["slot"] = cache_.slot;

    std::vector<zip::Blob> blobs;
    blobs.push_back(zip::compress(kEntryManifest, manifest.dump()));

    // Re-emit an unchanged entry from its cached compressed form so a state write doesn't pay deflate on the much larger save blob. Every mutation
    // publishes a fresh payload, so pointer identity matches exactly the unchanged entries.
    const auto emit = [this, &blobs](const char *name, const Payload &value)
    {
        if (!value)
            return;
        const auto it = writer_blobs_.find(name);
        if (it != writer_blobs_.end() && it->second.source == value)
        {
            blobs.push_back(it->second.blob);
            return;
        }
        auto blob = zip::compress(name, *value);
        writer_blobs_[name] = CachedBlob{value, blob};
        blobs.push_back(std::move(blob));
    };
    emit(kEntrySave, cache_.game_save);
    emit(kEntryState, cache_.ap_state);

    // Dropping a held payload is a bug.
    const std::size_t expected = 1 + (cache_.game_save ? 1u : 0u) + (cache_.ap_state ? 1u : 0u);
    if (blobs.size() != expected)
    {
        pal::logf(pal::LogLevel::Error, "save: refusing to write a container with %zu of %zu entries", blobs.size(), expected);
        return false;
    }

    const std::string image = zip::write(blobs);
    const auto final_path = path_for(cache_.seed, cache_.slot);
    auto tmp_path = final_path;
    tmp_path += ".tmp";
    auto bak_path = final_path;
    bak_path += ".bak";

    {
        std::ofstream out(tmp_path, std::ios::binary | std::ios::trunc);
        if (!out)
        {
            pal::logf(pal::LogLevel::Error, "save: cannot open %s for writing", tmp_path.string().c_str());
            return false;
        }
        out.write(image.data(), static_cast<std::streamsize>(image.size()));
        // close() explicitly: a container this small sits in the stream buffer, so the real I/O (and its ENOSPC) happens here. The destructor would
        // report success for a file that never landed, and the rotation below would destroy the good copy.
        out.close();
        if (!out)
        {
            pal::logf(pal::LogLevel::Error, "save: failed to write %s", tmp_path.string().c_str());
            std::filesystem::remove(tmp_path, ec);
            ec.clear();
            return false;
        }
    }

    // Second guard: a short write that reported success must not reach the rotation, which consumes the only other copy.
    const auto written = std::filesystem::file_size(tmp_path, ec);
    if (ec || written != image.size())
    {
        pal::logf(pal::LogLevel::Error, "save: %s is %llu bytes, expected %zu; not replacing the container", tmp_path.string().c_str(),
                  static_cast<unsigned long long>(written), image.size());
        std::filesystem::remove(tmp_path, ec);
        ec.clear();
        return false;
    }

    // Rotate before the replace: a crash in between leaves a good .bak and no container, which load recovers from. Latched only when a rotation
    // happens, since a brand-new run's first write has nothing to back up.
    if (!writer_backed_up_ && std::filesystem::exists(final_path, ec))
    {
        std::filesystem::rename(final_path, bak_path, ec);
        ec.clear(); // a failed rotation must not block the write
        writer_backed_up_ = true;
    }

    std::filesystem::rename(tmp_path, final_path, ec);
    if (ec)
    {
        pal::logf(pal::LogLevel::Error, "save: cannot replace %s: %s", final_path.string().c_str(), ec.message().c_str());
        std::filesystem::remove(tmp_path, ec);
        return false;
    }
    return true;
}

std::optional<std::string> ApSaveBundleStore::load(std::string_view seed, std::string_view slot) const
{
    ensure_loaded(seed, slot);
    // A container is user-writable, and staging a malformed blob into a vanilla slot is worse than starting a new file. The bytes stay in the
    // cache, so declining deletes nothing.
    if (!cache_.game_save)
        return std::nullopt;
    if (!looks_like_save_blob(*cache_.game_save))
    {
        pal::logf(pal::LogLevel::Warn, "save: the stored game save for seed=%s slot=%s is not a save blob; ignoring it but keeping it on disk",
                  cache_.seed.c_str(), cache_.slot.c_str());
        return std::nullopt;
    }
    return *cache_.game_save;
}

bool ApSaveBundleStore::store(std::string_view seed, std::string_view slot, std::string_view blob)
{
    if (!looks_like_save_blob(blob))
        return false;
    ensure_loaded(seed, slot);
    if (cache_.foreign)
    {
        pal::logf(pal::LogLevel::Error, "save: refusing to overwrite %s, which belongs to a different seed/slot", path_for(seed, slot).string().c_str());
        return false;
    }
    cache_.game_save = std::make_shared<const std::string>(blob);
    state_staged_.store(false, std::memory_order_relaxed);
    // The game saving is the durability point, since the mod never runs its destructor. This runs under a native game hook with no catch before the
    // game's C ABI, so a throwing write reports as failed.
    try
    {
        return write_snapshot();
    }
    catch (const std::exception &e)
    {
        pal::logf(pal::LogLevel::Error, "save: writing the container threw: %s", e.what());
    }
    catch (...)
    {
        pal::logf(pal::LogLevel::Error, "save: writing the container threw");
    }
    return false;
}

std::optional<std::string> ApSaveBundleStore::load_state(std::string_view seed, std::string_view slot) const
{
    ensure_loaded(seed, slot);
    if (!cache_.ap_state)
        return std::nullopt;
    return *cache_.ap_state;
}

bool ApSaveBundleStore::state_staged() const
{
    return state_staged_.load(std::memory_order_relaxed);
}

bool ApSaveBundleStore::stage_state(std::string_view seed, std::string_view slot, std::string_view text)
{
    ensure_loaded(seed, slot);
    if (cache_.foreign)
    {
        pal::logf(pal::LogLevel::Error, "save: refusing to overwrite %s, which belongs to a different seed/slot", path_for(seed, slot).string().c_str());
        return false;
    }
    // No write: store() publishes the container, so the AP state that lands is the one true when the game captured its save blob.
    cache_.ap_state = std::make_shared<const std::string>(text);
    state_staged_.store(true, std::memory_order_relaxed);
    return true;
}

} // namespace mth
