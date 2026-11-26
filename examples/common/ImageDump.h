#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>
#include <glm/glm.hpp>
#include <vulkan/vulkan.hpp>
#include "ImageReadback.h"

// ---------------------------------------------------------------------------
// PPM dump: writes an intermediate render target to disk so it can be inspected
// offline (in any image viewer) instead of only on screen.
//
// The on-screen Debug View (DebugViewMode.h) answers "does this look right?";
// a PPM capture answers "what exactly is in this buffer?" -- it keeps the raw
// texel values, so normals can be checked with a colour picker, the depth range
// can be measured, and two runs can be diffed byte by byte.
//
// Encoding rules are identical to the Debug View (see the comment above
// DebugViewModeName) so the file and the screen always agree.
// ---------------------------------------------------------------------------

// How raw texel values are turned into displayable RGB.
enum class DumpEncoding {
    eColor,       // colour as stored, clamped to [0, 1]
    eNormal,      // unit normal: n * 0.5 + 0.5
    ePosition,    // world position: p / referenceScale * 0.5 + 0.5
    eLinearDepth, // world position + eye: distance in [near, far], near = bright
    eToneMap,     // HDR colour: Reinhard v / (1 + v), clamped to [0, 1]
};

inline const char *DumpEncodingName(DumpEncoding encoding)
{
    switch (encoding) {
    case DumpEncoding::eColor: return "color";
    case DumpEncoding::eNormal: return "normal (n*0.5+0.5)";
    case DumpEncoding::ePosition: return "position (p/ref*0.5+0.5)";
    case DumpEncoding::eLinearDepth: return "linear-depth (near=bright)";
    case DumpEncoding::eToneMap: return "tone-map (v/(1+v), HDR -> LDR)";
    }
    return "unknown";
}

struct DumpRequest {
    std::string label; // file stem, e.g. "gbuffer-normal"
    vk::Image image{};
    vk::Format format = vk::Format::eUndefined;
    vk::Extent2D extent{};
    // Layout the image is in when the capture runs.  G-Buffer and offscreen
    // colour targets end their pass in SHADER_READ_ONLY_OPTIMAL.
    vk::ImageLayout layout = vk::ImageLayout::eShaderReadOnlyOptimal;
    DumpEncoding encoding = DumpEncoding::eColor;
    // Array layer to capture.  A depth array (cascaded shadow map) needs one
    // dump per cascade because a PPM holds a single 2D slice.
    uint32_t layer = 0;
    // Only read by ePosition (world scale of the colour coding) and
    // eLinearDepth (eye position and the near/far range).
    float referenceScale = 5.0f;
    glm::vec3 eyePosition = glm::vec3(0.0f);
    float nearPlane = 0.1f;
    float farPlane = 100.0f;
};

struct DumpResult {
    std::filesystem::path path;
    vk::Extent2D extent{};
    // Fraction of texels whose alpha is >= 0.5.  The G-Buffer clears alpha to 0
    // and writes 1 for every covered pixel, so this is the geometry coverage of
    // the frame; for targets that are fully opaque it simply reports 1.0.
    float alphaCoverage = 0.0f;
    // Mean of the written RGB (uncovered texels count as black), useful to tell
    // "black because nothing was drawn" from "black because the channel is
    // broken".
    glm::vec3 meanColor = glm::vec3(0.0f);
    // Mean length of the sampled normals over the covered texels; 0 when the
    // request is not an eNormal capture.  A value far from 1.0 means the
    // G-Buffer normal was written non-normalised (or is garbage).
    float meanNormalLength = 0.0f;
    // The exact 8-bit RGB bytes written to the PPM.  Examples use it for
    // pixel-level assertions (e.g. counting outline-coloured texels) without
    // having to parse the file they just wrote.
    std::vector<uint8_t> rgb8;
};

// Writes an 8-bit RGB buffer (3 bytes per texel, row 0 = top) as a binary PPM
// (P6).  `comments` become '#' lines in the header, which is the only metadata
// slot a bare PPM has: the debug capture uses it for channel / encoding / source
// format, and raw2rgb uses it to record how the image was produced.
void WritePpmRgb8(const std::filesystem::path &path, uint32_t width, uint32_t height,
                  const std::vector<uint8_t> &rgb, const std::vector<std::string> &comments);

// Captures every request and writes one binary PPM (P6) per request into
// `directory` (created when missing), named "<index>_<label>.ppm" with the index
// starting at `firstIndex`.  Returns one result per request, in order.
std::vector<DumpResult> DumpImages(ImageReadback &readback, const std::vector<DumpRequest> &requests,
                                  const std::filesystem::path &directory, uint32_t firstIndex = 0);

// Optional command-line switches shared by the examples:
//   --capture <frames>  render that many frames, dump once on the last one, exit
//   --view <index>      start on the debug view that digit key <index> selects
// Together they make the debug output reproducible (smoke test, regression
// dump) without having to hit a key at the right moment.  No switch means
// interactive only.
struct DebugCaptureOptions {
    uint32_t framesBeforeCapture = 0;
    uint32_t initialViewIndex = 0;
    bool viewSpecified = false;

    bool requested() const { return framesBeforeCapture > 0; }
    // True on the frame the automatic capture is due.
    bool isCaptureFrame(uint32_t framesRendered) const
    {
        return requested() && framesRendered + 1 >= framesBeforeCapture;
    }
    // True once the requested number of frames has been rendered.
    bool shouldStop(uint32_t framesRendered) const
    {
        return requested() && framesRendered >= framesBeforeCapture;
    }
};

DebugCaptureOptions ParseDebugCaptureOptions(int argc, char **argv);
