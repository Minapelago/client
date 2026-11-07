#pragma once

namespace pal
{

// ProggyForever as a byte array from cmake/OverlayFont.cmake. Static storage, so the atlas must not take ownership.
const void *overlay_font_ttf(int *out_size);

} // namespace pal
