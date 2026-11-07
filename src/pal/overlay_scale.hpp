#pragma once

#include <algorithm>
#include <cmath>

#include <imgui.h>

#include "pal/overlay_font.hpp"
#include "pal/pal_log.hpp"

namespace pal
{

// Sizes the overlay's font and style for a framebuffer this tall. Call once after CreateContext and before the backend init so the atlas is baked at its
// final size; scaling a built atlas blurs. The game scales its UI with the framebuffer, and at native fullscreen the OS DPI says nothing about that.
inline void scale_overlay_for_height(unsigned framebuffer_height)
{
    const float scale = std::clamp(static_cast<float>(framebuffer_height) / 1080.0f, 1.0f, 4.0f);

    ImFontConfig cfg;
    cfg.SizePixels = std::round(13.0f * scale);
    // Snapped positions and no oversampling, as ImGui sets up this font, so glyphs land on the pixel grid like the 13px bitmap font.
    cfg.OversampleH = 1;
    cfg.OversampleV = 1;
    cfg.PixelSnapH = true;
    // Vertical centering, kept whole: RenderText truncates the draw position and the atlas rounds the ascent, so a fractional offset here would land
    // glyphs between rows, where bilinear sampling smears horizontal strokes.
    cfg.GlyphOffset.y = std::trunc(0.5f * (cfg.SizePixels / 16.0f));
    cfg.FontDataOwnedByAtlas = false; // the array has static storage; the atlas must not free it
    // Between whole multiples of 13px the outlines miss the pixel grid and stems spread over two half-lit columns; boosting coverage makes them solid.
    cfg.RasterizerMultiply = 1.6f;
    int font_size = 0;
    const void *font_data = overlay_font_ttf(&font_size);
    ImGui::GetIO().Fonts->AddFontFromMemoryTTF(const_cast<void *>(font_data), font_size, cfg.SizePixels, &cfg);

    ImGui::GetStyle().ScaleAllSizes(scale);

    pal::logf(pal::LogLevel::Info, "overlay: ui scale %.2f (font %dpx, framebuffer height %u)", static_cast<double>(scale), static_cast<int>(cfg.SizePixels),
              framebuffer_height);
}

} // namespace pal
