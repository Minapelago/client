#pragma once

#include <string>
#include <string_view>

namespace mth
{

// Replaces anything outside [A-Za-z0-9_-] with '_' so an AP seed or slot name is filename-safe; '.' goes too, so no key part can traverse paths.
// Empty input yields "unnamed".
std::string sanitize_save_key_part(std::string_view raw);

// "ap_<seed>_<slot>_<hash>.ycsave", seed/slot sanitized. Sanitizing is lossy, so a hash of the raw parts keeps distinct sessions on distinct files.
std::string ap_save_filename(std::string_view seed, std::string_view slot);

// A serialized single slot starts with the ycData header and carries a SaveSlot body. Cheap
// structural check only; the blob is otherwise opaque to us.
bool looks_like_save_blob(std::string_view blob);

} // namespace mth
