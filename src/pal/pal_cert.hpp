#pragma once

#include <filesystem>
#include <optional>

namespace pal
{

// nullopt if no bundle is found or writable.
std::optional<std::filesystem::path> ca_bundle_path();

} // namespace pal
