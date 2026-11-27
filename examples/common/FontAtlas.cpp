#include "FontAtlas.h"

#include "root_directory.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <stdexcept>

#ifdef RENDER_DEMO_HAS_FREETYPE
#include <ft2build.h>
#include FT_FREETYPE_H
#endif

namespace {

#ifdef RENDER_DEMO_HAS_FREETYPE

// Resolve like loadTexture() does: relative to the working directory first, so
// `./build/examples/text_rendering/text_rendering` works from the repo root and
// from the exe directory alike.
std::string resolveAsset(const std::string &path)
{
    if (std::filesystem::exists(path)) {
        return path;
    }
    const std::string candidate = std::string(PROJECT_ROOT_PATH) + "/" + path;
    if (std::filesystem::exists(candidate)) {
        return candidate;
    }
    return {};
}

// One rasterised glyph before it has an atlas position.
struct StagedGlyph {
    uint32_t codepoint = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    int bearingX = 0;
    int bearingY = 0;
    float advance = 0.0f;
    // Row-major, `width` bytes per row (FreeType's bitmap.pitch is *not* the
    // row length: it is padded to a multiple of 4).
    std::vector<uint8_t> coverage;

    // Filled in by the shelf packer.
    uint32_t x = 0;
    uint32_t y = 0;
};

// Shelf (Skyline) packing: glyphs are visited in codepoint order and each one is
// placed on the current shelf if it still fits, otherwise a new shelf starts
// below the tallest glyph of the previous one.  Deterministic, needs no sorting,
// and the resulting atlas is a pure function of (font, size, range) -- which is
// what makes the dumped atlas comparable between runs.
void packShelves(std::vector<StagedGlyph> &glyphs, uint32_t atlasWidth, uint32_t padding, uint32_t &atlasHeight,
                 uint32_t &shelfCount)
{
    uint32_t penX = 0;
    uint32_t penY = 0;
    uint32_t shelfHeight = 0;
    shelfCount = 1;
    for (StagedGlyph &glyph : glyphs) {
        if (glyph.width == 0 || glyph.height == 0) {
            continue; // blank glyph (space): advance only, no atlas entry
        }
        if (penX + glyph.width + padding > atlasWidth) {
            penY += shelfHeight + padding;
            penX = 0;
            shelfHeight = 0;
            ++shelfCount;
        }
        glyph.x = penX;
        glyph.y = penY;
        penX += glyph.width + padding;
        shelfHeight = std::max(shelfHeight, glyph.height);
    }
    atlasHeight = penY + shelfHeight;
    if (atlasHeight == 0) {
        atlasHeight = 1;
    }
}

#endif // RENDER_DEMO_HAS_FREETYPE

} // namespace

#ifdef RENDER_DEMO_HAS_FREETYPE

FontAtlas FontAtlas::load(const std::string &path, uint32_t pixelSize, uint32_t firstCodepoint,
                          uint32_t lastCodepoint, uint32_t atlasWidthHint)
{
    const std::string resolved = resolveAsset(path);
    if (resolved.empty()) {
        throw std::runtime_error("FontAtlas: font not found: " + path);
    }
    if (pixelSize == 0) {
        throw std::runtime_error("FontAtlas: pixel size must be non-zero");
    }
    if (lastCodepoint < firstCodepoint) {
        throw std::runtime_error("FontAtlas: empty codepoint range");
    }

    FT_Library library = nullptr;
    if (FT_Init_FreeType(&library) != 0) {
        throw std::runtime_error("FontAtlas: FT_Init_FreeType failed");
    }
    struct LibraryGuard {
        FT_Library handle = nullptr;
        ~LibraryGuard()
        {
            if (handle) {
                FT_Done_FreeType(handle);
            }
        }
    } libraryGuard{library};

    FT_Face face = nullptr;
    if (FT_New_Face(library, resolved.c_str(), 0, &face) != 0) {
        throw std::runtime_error("FontAtlas: FT_New_Face failed for " + resolved);
    }
    struct FaceGuard {
        FT_Face handle = nullptr;
        ~FaceGuard()
        {
            if (handle) {
                FT_Done_Face(handle);
            }
        }
    } faceGuard{face};

    // 0 width + pixelSize height = "pick the strike that matches this height".
    // FreeType 26.6 fixed point: metrics are /64 to get pixels.
    if (FT_Set_Pixel_Sizes(face, 0, pixelSize) != 0) {
        throw std::runtime_error("FontAtlas: FT_Set_Pixel_Sizes(" + std::to_string(pixelSize) + ") failed");
    }

    FontAtlas atlas;
    atlas.m_sourcePath = resolved;
    atlas.m_pixelSize = pixelSize;
    atlas.m_first = firstCodepoint;
    atlas.m_last = lastCodepoint;

    // ---- pass 1: rasterise ------------------------------------------------
    std::vector<StagedGlyph> staged;
    staged.reserve(lastCodepoint - firstCodepoint + 1);
    size_t totalArea = 0;
    for (uint32_t codepoint = firstCodepoint; codepoint <= lastCodepoint; ++codepoint) {
        // FT_LOAD_RENDER: rasterise into glyph->bitmap in the same call, which is
        // what the reference does.  No hinting flags are forced, so the result
        // matches the reference's FreeType output pixel for pixel.
        if (FT_Load_Char(face, codepoint, FT_LOAD_RENDER) != 0) {
            continue;
        }
        const FT_GlyphSlot slot = face->glyph;
        StagedGlyph glyph;
        glyph.codepoint = codepoint;
        glyph.width = slot->bitmap.width;
        glyph.height = slot->bitmap.rows;
        glyph.bearingX = slot->bitmap_left;
        glyph.bearingY = slot->bitmap_top;
        glyph.advance = static_cast<float>(slot->advance.x) / 64.0f;
        if (glyph.width > 0 && glyph.height > 0 && slot->bitmap.buffer != nullptr) {
            glyph.coverage.resize(static_cast<size_t>(glyph.width) * glyph.height);
            const int pitch = slot->bitmap.pitch;
            for (uint32_t row = 0; row < glyph.height; ++row) {
                const uint8_t *source = slot->bitmap.buffer + static_cast<ptrdiff_t>(row) * pitch;
                std::memcpy(glyph.coverage.data() + static_cast<size_t>(row) * glyph.width, source, glyph.width);
            }
            totalArea += static_cast<size_t>(glyph.width + 1) * (glyph.height + 1);
        }
        staged.push_back(std::move(glyph));
    }
    if (staged.empty()) {
        throw std::runtime_error("FontAtlas: FreeType returned no glyphs for the requested range");
    }

    // ---- pass 2: choose an atlas width and pack ---------------------------
    // A square atlas filled to ~75% is the usual compromise: square-ish atlases
    // sample as well as wide ones and waste less padding.
    uint32_t atlasWidth = atlasWidthHint;
    if (atlasWidth == 0) {
        const double side = std::sqrt(static_cast<double>(std::max<size_t>(totalArea, 1)) / 0.75);
        atlasWidth = static_cast<uint32_t>(std::ceil(side));
        atlasWidth = (atlasWidth + 15u) & ~15u; // keep the row length a multiple of 16
        atlasWidth = std::clamp(atlasWidth, 128u, 4096u);
    }
    constexpr uint32_t padding = 1; // one blank texel stops bilinear bleed between glyphs
    uint32_t atlasHeight = 0;
    uint32_t shelfCount = 0;
    packShelves(staged, atlasWidth, padding, atlasHeight, shelfCount);
    // A very tall atlas means the width hint was too small; widen once instead
    // of shipping a 128x4096 strip.
    if (atlasHeight > atlasWidth * 2 && atlasWidthHint == 0) {
        atlasWidth = std::min(4096u, atlasWidth * 2);
        packShelves(staged, atlasWidth, padding, atlasHeight, shelfCount);
    }

    atlas.m_width = atlasWidth;
    atlas.m_height = atlasHeight;
    atlas.m_shelves = shelfCount;
    atlas.m_pixels.assign(static_cast<size_t>(atlasWidth) * atlasHeight, 0u);

    // ---- pass 3: blit and build the metrics table -------------------------
    for (const StagedGlyph &glyph : staged) {
        GlyphMetrics metrics;
        metrics.bearing = glm::vec2(static_cast<float>(glyph.bearingX), static_cast<float>(glyph.bearingY));
        metrics.size = glm::vec2(static_cast<float>(glyph.width), static_cast<float>(glyph.height));
        metrics.advance = glyph.advance;
        if (glyph.width > 0 && glyph.height > 0) {
            for (uint32_t row = 0; row < glyph.height; ++row) {
                uint8_t *destination =
                    atlas.m_pixels.data() + static_cast<size_t>(glyph.y + row) * atlasWidth + glyph.x;
                std::memcpy(destination, glyph.coverage.data() + static_cast<size_t>(row) * glyph.width, glyph.width);
            }
            metrics.uv = glm::vec4(static_cast<float>(glyph.x) / static_cast<float>(atlasWidth),
                                   static_cast<float>(glyph.y) / static_cast<float>(atlasHeight),
                                   static_cast<float>(glyph.x + glyph.width) / static_cast<float>(atlasWidth),
                                   static_cast<float>(glyph.y + glyph.height) / static_cast<float>(atlasHeight));
        }
        atlas.m_glyphs.emplace(glyph.codepoint, metrics);
    }

    atlas.m_lineHeight = static_cast<float>(face->size->metrics.height) / 64.0f;
    atlas.m_ascender = static_cast<float>(face->size->metrics.ascender) / 64.0f;
    return atlas;
}

#else

FontAtlas FontAtlas::load(const std::string &, uint32_t, uint32_t, uint32_t, uint32_t)
{
    throw std::runtime_error("FontAtlas: this build has no FreeType (set RENDER_DEMO_FREETYPE_DIR and re-configure)");
}

#endif

const GlyphMetrics *FontAtlas::find(uint32_t codepoint) const
{
    const auto it = m_glyphs.find(codepoint);
    return it == m_glyphs.end() ? nullptr : &it->second;
}

float FontAtlas::coverageRatio() const
{
    if (m_pixels.empty()) {
        return 0.0f;
    }
    size_t covered = 0;
    for (uint8_t value : m_pixels) {
        if (value != 0u) {
            ++covered;
        }
    }
    return static_cast<float>(covered) / static_cast<float>(m_pixels.size());
}
