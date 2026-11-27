#pragma once
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>
#include <glm/glm.hpp>

// ---------------------------------------------------------------------------
// Glyph atlas: FreeType rasterisation packed into one single-channel texture.
//
// The reference (LearnOpenGL 7.in_practice/2.text_rendering) creates one GL
// texture per character and rebinds it before every glyph draw.  That mapping
// does not survive the trip to Vulkan here: descriptor sets are written once at
// pipeline-creation time (see DESIGN.md 20.6), so 95 glyph textures would mean
// 95 sets and 95 rebinds per frame.  Packing every glyph into one R8 image
// turns a text run into a single draw: the per-glyph state that GL carried in
// the texture binding and the uniform moves into a per-instance record
// (rectangle + atlas rect + colour).
//
// Metrics are kept in *pixels at the loaded pixel size*, exactly as FreeType
// reports them, because the layout code is then a line-for-line copy of the
// reference's RenderText():
//
//     xpos = x + bearing.x * scale
//     ypos = y - (size.y - bearing.y) * scale     // y up, baseline at y
//
// `uv` is normalised with v = 0 at the top of the glyph, matching both the
// atlas row order written here and Vulkan's image origin.
// ---------------------------------------------------------------------------

struct GlyphMetrics {
    glm::vec2 bearing{0.0f}; // bitmap_left / bitmap_top, relative to the baseline
    glm::vec2 size{0.0f};    // bitmap width / rows; zero for blank glyphs (space)
    float advance = 0.0f;    // pen step in pixels (FreeType 26.6 -> /64)
    // (u0, v0, u1, v1) inside the atlas; all zero when the glyph has no bitmap.
    glm::vec4 uv{0.0f};
};

class FontAtlas {
public:
    // Rasterises [firstCodepoint, lastCodepoint] at `pixelSize`.  Throws
    // std::runtime_error with the FreeType error string when the font cannot be
    // opened; callers that want a soft failure can use tryLoad().
    static FontAtlas load(const std::string &path, uint32_t pixelSize, uint32_t firstCodepoint = 32,
                          uint32_t lastCodepoint = 126, uint32_t atlasWidthHint = 0);

    bool valid() const { return m_width > 0 && m_height > 0; }
    uint32_t width() const { return m_width; }
    uint32_t height() const { return m_height; }
    uint32_t pixelSize() const { return m_pixelSize; }
    // Row-major, tightly packed, one byte per texel, row 0 = top.
    const std::vector<uint8_t> &pixels() const { return m_pixels; }
    // Face height in pixels (ascender - descender), for line spacing.
    float lineHeight() const { return m_lineHeight; }
    float ascender() const { return m_ascender; }

    const GlyphMetrics *find(uint32_t codepoint) const;
    uint32_t glyphCount() const { return static_cast<uint32_t>(m_glyphs.size()); }
    uint32_t firstCodepoint() const { return m_first; }
    uint32_t lastCodepoint() const { return m_last; }
    // Fraction of atlas texels with non-zero coverage.  A packing bug (wrong
    // pitch, overlapping shelves) shows up here long before it is visible.
    float coverageRatio() const;
    const std::string &sourcePath() const { return m_sourcePath; }
    // Atlas width actually used before the shelf packer ran out of luck; the
    // exposed size already includes the growth.
    uint32_t shelfCount() const { return m_shelves; }

private:
    std::string m_sourcePath;
    uint32_t m_width = 0;
    uint32_t m_height = 0;
    uint32_t m_pixelSize = 0;
    uint32_t m_first = 0;
    uint32_t m_last = 0;
    uint32_t m_shelves = 0;
    float m_lineHeight = 0.0f;
    float m_ascender = 0.0f;
    std::vector<uint8_t> m_pixels;
    std::unordered_map<uint32_t, GlyphMetrics> m_glyphs;
};
