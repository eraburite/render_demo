// text_rendering: FreeType glyph atlas + one instanced draw, migrated from
// LearnOpenGL/src/7.in_practice/2.text_rendering.
//
// The reference creates one GL texture per printable ASCII character and, for
// every glyph, uploads six vertices into a VBO and issues a draw -- 95 textures,
// 95 buffer updates, 95 draws for a short line.  Vulkan here cannot follow that
// shape: a descriptor set is written once when the pipeline is created (see
// DESIGN.md 20.6), so per-glyph texture switching would need 95 descriptor sets
// and 95 rebinds.  Instead:
//
//   * all glyphs are rasterised by FreeType into one R8 atlas (FontAtlas),
//   * the per-glyph state GL kept in the texture binding and a uniform becomes
//     one 48-byte instance record (pixel rect + atlas rect + colour),
//   * the whole run is a single instanced draw of 6 vertices per glyph.
//
// Layout is a line-for-line copy of the reference's RenderText():
//     xpos = x + bearing.x * scale
//     ypos = y - (size.y - bearing.y) * scale
// with (x, y) measured from the bottom-left corner of the frame in pixels, which
// is what glm::ortho(0, width, 0, height) gives you in GL.  Vulkan's NDC has y
// pointing down, so the projection's [1][1] is negated -- the same fix every
// perspective example applies.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include "Camera.h"
#include "FontAtlas.h"
#include "GLFWwindow.h"
#include "ImageDump.h"

#include "AttachmentDetailConfig.h"
#include "DescriptorLayoutConfig.h"
#include "VertexInputStateConfig.h"

#include "CmdOpDrawFrame.h"
#include "CmdSyncDevice.h"
#include "IRenderPass.h"
#include "LoopSequence.h"
#include "OnceSequence.h"
#include "RenderManager.h"
#include "TextureImage.h"

#include "shader/text_frag.hpp"
#include "shader/text_vert.hpp"

namespace {

// 6 vertices per glyph are generated in the shader from gl_VertexIndex, so the
// only vertex input is the instance buffer.
constexpr uint32_t kVertexCountPerGlyph = 6;
// Upper bound on the instance buffer.  A text run longer than this is a mistake
// worth reporting, not something to silently concatenate.
constexpr uint32_t kMaxQuads = 2048;

// std140: a bare mat4 is 64 bytes.
struct ScreenBlock {
    alignas(16) glm::mat4 projection{1.0f};
};
static_assert(sizeof(ScreenBlock) == 64, "ScreenBlock must match the std140 ScreenBlock");

// std430: three vec4s, 48 bytes.  The colour is per-instance because the
// reference's two lines have different colours and scales but share one draw.
struct GlyphQuad {
    alignas(16) glm::vec4 rect{0.0f};  // x0, y0, x1, y1 in pixels, +y up
    alignas(16) glm::vec4 uv{0.0f};    // u0, v0, u1, v1
    alignas(16) glm::vec4 color{1.0f}; // rgb = text colour
};
static_assert(sizeof(GlyphQuad) == 48, "GlyphQuad must be rect(16) + uv(16) + color(16)");

enum class ScreenComponent { eScreen };
using ScreenConfig = DescriptorComponentSpec<ScreenComponent>;
inline constexpr auto ScreenComponentTable = std::make_tuple(ScreenConfig{
    ScreenComponent::eScreen, vk::DescriptorType::eUniformBuffer, 1,
    vk::ShaderStageFlagBits::eVertex});

enum class AtlasComponent { eAtlas };
using AtlasConfig = DescriptorComponentSpec<AtlasComponent>;
inline constexpr auto AtlasComponentTable = std::make_tuple(AtlasConfig{
    AtlasComponent::eAtlas, vk::DescriptorType::eCombinedImageSampler, 1,
    vk::ShaderStageFlagBits::eFragment});

enum class QuadComponent { eQuads };
using QuadConfig = DescriptorComponentSpec<QuadComponent>;
inline constexpr auto QuadComponentTable = std::make_tuple(QuadConfig{
    QuadComponent::eQuads, vk::DescriptorType::eStorageBuffer, 1, vk::ShaderStageFlagBits::eVertex});

// One line of text: the reference calls RenderText() twice with different
// position, scale and colour.
struct TextRun {
    std::string text;
    float x = 0.0f;
    float y = 0.0f;
    float scale = 1.0f;
    glm::vec3 color{1.0f};
};

struct Options {
    std::string font = "assets/fonts/Antonio-Bold.ttf";
    uint32_t fontSize = 48;   // reference: FT_Set_Pixel_Sizes(face, 0, 48)
    uint32_t atlasWidth = 0;  // 0 = let the packer choose
    std::optional<std::string> text;
    std::optional<float> scale;
    std::optional<float> x;
    std::optional<float> y;
    std::optional<glm::vec3> color;
    bool copyright = true;
    bool blend = true;
    bool stats = false;
    bool verify = false;
    bool dump = false;
    uint32_t dumpAfter = 3;
};

Options parseOptions(int argc, char **argv)
{
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const bool hasValue = i + 1 < argc;
        if (arg == "--font" && hasValue) {
            options.font = argv[++i];
        } else if (arg == "--size" && hasValue) {
            options.fontSize = static_cast<uint32_t>(std::stoul(argv[++i]));
        } else if (arg == "--atlas-width" && hasValue) {
            options.atlasWidth = static_cast<uint32_t>(std::stoul(argv[++i]));
        } else if (arg == "--text" && hasValue) {
            options.text = std::string(argv[++i]);
        } else if (arg == "--scale" && hasValue) {
            options.scale = std::stof(argv[++i]);
        } else if (arg == "--x" && hasValue) {
            options.x = std::stof(argv[++i]);
        } else if (arg == "--y" && hasValue) {
            options.y = std::stof(argv[++i]);
        } else if (arg == "--color" && i + 3 < argc) {
            const glm::vec3 color(std::stof(argv[i + 1]), std::stof(argv[i + 2]), std::stof(argv[i + 3]));
            options.color = color;
            i += 3;
        } else if (arg == "--no-copyright") {
            options.copyright = false;
        } else if (arg == "--no-blend") {
            options.blend = false;
        } else if (arg == "--stats") {
            options.stats = true;
        } else if (arg == "--verify") {
            options.verify = true;
        } else if (arg == "--dump") {
            options.dump = true;
        } else if (arg == "--dump-after" && hasValue) {
            options.dumpAfter = static_cast<uint32_t>(std::stoul(argv[++i]));
        } else {
            throw std::runtime_error("text_rendering: unknown argument " + arg);
        }
    }
    if (options.fontSize == 0 || options.fontSize > 512) {
        throw std::runtime_error("text_rendering: --size must be in 1..512");
    }
    return options;
}

// The reference's two lines, verbatim (same strings, positions, scales, colours).
std::vector<TextRun> makeRuns(const Options &options)
{
    TextRun sample{"This is sample text", 25.0f, 25.0f, 1.0f, glm::vec3(0.5f, 0.8f, 0.2f)};
    TextRun credit{"(C) LearnOpenGL.com", 540.0f, 570.0f, 0.5f, glm::vec3(0.3f, 0.7f, 0.9f)};

    if (options.text) {
        sample.text = *options.text;
    }
    if (options.scale) {
        sample.scale = *options.scale;
    }
    if (options.x) {
        sample.x = *options.x;
    }
    if (options.y) {
        sample.y = *options.y;
    }
    if (options.color) {
        sample.color = *options.color;
    }
    if (sample.scale <= 0.0f) {
        throw std::runtime_error("text_rendering: --scale must be positive");
    }

    std::vector<TextRun> runs{sample};
    if (options.copyright) {
        runs.push_back(credit);
    }
    return runs;
}

// UTF-8 -> codepoint.  The reference indexes a std::map<GLchar, Character> and
// therefore only handles single-byte characters; decoding properly costs ten
// lines and makes --text behave for anything the atlas happens to contain.
std::vector<uint32_t> decodeUtf8(const std::string &text)
{
    std::vector<uint32_t> codepoints;
    codepoints.reserve(text.size());
    for (size_t i = 0; i < text.size();) {
        const unsigned char lead = static_cast<unsigned char>(text[i]);
        uint32_t codepoint = lead;
        size_t length = 1;
        if ((lead & 0x80u) == 0u) {
            codepoint = lead;
        } else if ((lead & 0xE0u) == 0xC0u && i + 1 < text.size()) {
            codepoint = ((lead & 0x1Fu) << 6) | (static_cast<unsigned char>(text[i + 1]) & 0x3Fu);
            length = 2;
        } else if ((lead & 0xF0u) == 0xE0u && i + 2 < text.size()) {
            codepoint = ((lead & 0x0Fu) << 12) | ((static_cast<unsigned char>(text[i + 1]) & 0x3Fu) << 6)
                        | (static_cast<unsigned char>(text[i + 2]) & 0x3Fu);
            length = 3;
        } else if ((lead & 0xF8u) == 0xF0u && i + 3 < text.size()) {
            codepoint = ((lead & 0x07u) << 18) | ((static_cast<unsigned char>(text[i + 1]) & 0x3Fu) << 12)
                        | ((static_cast<unsigned char>(text[i + 2]) & 0x3Fu) << 6)
                        | (static_cast<unsigned char>(text[i + 3]) & 0x3Fu);
            length = 4;
        }
        codepoints.push_back(codepoint);
        i += length;
    }
    return codepoints;
}

struct Layout {
    std::vector<GlyphQuad> quads;
    uint32_t missingGlyphs = 0;
    // Union of the glyph rectangles in pixel space, +y up.  This is the ink box:
    // FreeType's bitmaps are cropped to the ink, so the union of the rects is
    // also the union of the non-background pixels (give or take a texel of
    // antialiasing).  It is what the frame dump is checked against.
    glm::vec4 inkBox{0.0f};
    bool hasInk = false;
};

void layoutRun(const FontAtlas &atlas, const TextRun &run, Layout &layout)
{
    float penX = run.x;
    const float penY = run.y;
    for (uint32_t codepoint : decodeUtf8(run.text)) {
        const GlyphMetrics *glyph = atlas.find(codepoint);
        if (glyph == nullptr) {
            ++layout.missingGlyphs;
            continue;
        }
        // Blank glyphs (space) still advance the pen but have no quad.
        if (glyph->size.x > 0.0f && glyph->size.y > 0.0f) {
            if (layout.quads.size() >= kMaxQuads) {
                throw std::runtime_error("text_rendering: text needs more than " + std::to_string(kMaxQuads)
                                         + " quads");
            }
            const float xpos = penX + glyph->bearing.x * run.scale;
            const float ypos = penY - (glyph->size.y - glyph->bearing.y) * run.scale;
            const float width = glyph->size.x * run.scale;
            const float height = glyph->size.y * run.scale;

            GlyphQuad quad;
            quad.rect = glm::vec4(xpos, ypos, xpos + width, ypos + height);
            quad.uv = glyph->uv;
            quad.color = glm::vec4(run.color, 1.0f);
            layout.quads.push_back(quad);

            if (!layout.hasInk) {
                layout.inkBox = quad.rect;
                layout.hasInk = true;
            } else {
                layout.inkBox.x = std::min(layout.inkBox.x, quad.rect.x);
                layout.inkBox.y = std::min(layout.inkBox.y, quad.rect.y);
                layout.inkBox.z = std::max(layout.inkBox.z, quad.rect.z);
                layout.inkBox.w = std::max(layout.inkBox.w, quad.rect.w);
            }
        }
        // advance is stored in pixels already (FreeType 26.6 / 64); the
        // reference shifts >> 6 for the same reason.
        penX += glyph->advance * run.scale;
    }
}

// Rows of a PPM are top-first while the layout is bottom-left-first, so the
// expected box has to be flipped before it can be compared with the image.
glm::ivec4 expectedImageBox(const glm::vec4 &inkBox, uint32_t height)
{
    const int x0 = static_cast<int>(std::floor(inkBox.x));
    const int x1 = static_cast<int>(std::ceil(inkBox.z)) - 1;
    const int y0 = static_cast<int>(height) - static_cast<int>(std::ceil(inkBox.w));
    const int y1 = static_cast<int>(height) - 1 - static_cast<int>(std::floor(inkBox.y));
    return glm::ivec4(x0, y0, x1, y1);
}

// Background is "whatever colour covers the most pixels": the frame is a clear
// colour plus text, so the histogram mode is exact and does not depend on
// knowing that the swapchain is sRGB (the clear value 0.05 linear is stored as
// 63/255, and hard-coding that here would be a landmine).
glm::ivec4 findInkBox(const std::vector<uint8_t> &rgb, uint32_t width, uint32_t height, int threshold)
{
    const size_t texels = static_cast<size_t>(width) * height;
    std::vector<uint32_t> histogram(1u << 24, 0u);
    for (size_t i = 0; i < texels; ++i) {
        const uint32_t key = (static_cast<uint32_t>(rgb[i * 3]) << 16) | (static_cast<uint32_t>(rgb[i * 3 + 1]) << 8)
                             | static_cast<uint32_t>(rgb[i * 3 + 2]);
        ++histogram[key];
    }
    uint32_t background = 0;
    uint32_t best = 0;
    for (uint32_t key = 0; key < histogram.size(); ++key) {
        if (histogram[key] > best) {
            best = histogram[key];
            background = key;
        }
    }
    const int bgR = static_cast<int>((background >> 16) & 0xFFu);
    const int bgG = static_cast<int>((background >> 8) & 0xFFu);
    const int bgB = static_cast<int>(background & 0xFFu);

    glm::ivec4 box(static_cast<int>(width), static_cast<int>(height), -1, -1);
    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            const size_t index = (static_cast<size_t>(y) * width + x) * 3;
            const int dr = std::abs(static_cast<int>(rgb[index]) - bgR);
            const int dg = std::abs(static_cast<int>(rgb[index + 1]) - bgG);
            const int db = std::abs(static_cast<int>(rgb[index + 2]) - bgB);
            if (std::max({dr, dg, db}) > threshold) {
                box.x = std::min(box.x, static_cast<int>(x));
                box.y = std::min(box.y, static_cast<int>(y));
                box.z = std::max(box.z, static_cast<int>(x));
                box.w = std::max(box.w, static_cast<int>(y));
            }
        }
    }
    return box;
}

void printStats(const FontAtlas &atlas, const Layout &layout, const std::vector<TextRun> &runs)
{
    std::cout << "[text] font " << atlas.sourcePath() << ", " << atlas.pixelSize() << " px, "
              << atlas.glyphCount() << " glyphs (" << atlas.firstCodepoint() << ".." << atlas.lastCodepoint()
              << ") -> atlas " << atlas.width() << "x" << atlas.height() << " (" << atlas.shelfCount() << " shelves, "
              << atlas.coverageRatio() * 100.0f << "% covered), line height " << atlas.lineHeight() << "\n";
    std::cout << "[text] " << runs.size() << " run(s), " << layout.quads.size() << " quads, ink box (px, y up) = ("
              << layout.inkBox.x << ", " << layout.inkBox.y << ") - (" << layout.inkBox.z << ", " << layout.inkBox.w
              << ")";
    if (layout.missingGlyphs > 0) {
        std::cout << ", " << layout.missingGlyphs << " codepoints outside the baked range";
    }
    std::cout << "\n";

    // A handful of glyphs is enough to see whether the metrics are plausible:
    // bearing.y should be near the ascender for capitals, size should be
    // non-zero, and advance should be wider than the ink for most letters.
    static const uint32_t kProbe[] = {'T', 'h', 'g', 'A', '.', ' '};
    for (uint32_t codepoint : kProbe) {
        const GlyphMetrics *glyph = atlas.find(codepoint);
        if (glyph == nullptr) {
            continue;
        }
        std::cout << "[glyph] '" << static_cast<char>(codepoint) << "' bearing=(" << glyph->bearing.x << ","
                  << glyph->bearing.y << ") size=(" << glyph->size.x << "," << glyph->size.y << ") advance="
                  << glyph->advance << " uv=(" << glyph->uv.x << "," << glyph->uv.y << ")-(" << glyph->uv.z << ","
                  << glyph->uv.w << ")\n";
    }
}

} // namespace

void test(int argc, char **argv)
{
    const Options options = parseOptions(argc, argv);
    const std::vector<TextRun> runs = makeRuns(options);

    // The atlas is built once on the CPU: FreeType is not needed afterwards.
    const FontAtlas atlas = FontAtlas::load(options.font, options.fontSize, 32, 126, options.atlasWidth);

    Layout layout;
    for (const TextRun &run : runs) {
        layoutRun(atlas, run, layout);
    }
    printStats(atlas, layout, runs);

    auto camera = std::make_shared<Camera>(glm::vec3(0.0f, 0.0f, 3.0f)); // unused, the window requires one
    GLFWwindow window(1280, 720, camera);
    auto mgr = RenderManager::Create(window, MAX_FRAMES_IN_FLIGHT);

    const vk::Extent2D initialExtent = *mgr->getSwapChain()->getExtent2D();
    std::cout << "[text] " << (options.blend ? "alpha blending on" : "blending OFF (every glyph is a box)")
              << ", extent " << initialExtent.width << "x" << initialExtent.height << "\n";

    // No depth attachment: text is one layer of alpha-blended quads, and asking
    // for a depth buffer would only add a clear and a layout transition.
    auto renderPass = mgr->CreateRenderPass(std::make_shared<SimpleAttachmentDetail>(
        SimpleAttachmentDetail::Mode::eDirect, *mgr->getSwapChain()->getImageFormat(), vk::Format::eUndefined));

    auto layoutAtlas = std::make_shared<DescriptorLayoutAtlas>(MAX_FRAMES_IN_FLIGHT);
    auto screenSet = DescriptorLayoutAtlas::makeSet<ScreenComponent, ScreenComponentTable>(
        0, {ScreenComponent::eScreen}, UpdateFrequency::eEachFrame, false);
    screenSet.specs[0] = GBufferSpec{1, static_cast<uint32_t>(sizeof(ScreenBlock)), DataTypes::eCustom};

    auto atlasSet = DescriptorLayoutAtlas::makeSet<AtlasComponent, AtlasComponentTable>(
        1, {AtlasComponent::eAtlas}, UpdateFrequency::eStatic, true);
    TextureImageSpec atlasSpec;
    atlasSpec.width = atlas.width();
    atlasSpec.height = atlas.height();
    atlasSpec.channels = 1;                    // FreeType coverage: one byte per texel
    atlasSpec.format = vk::Format::eR8Unorm;   // not sRGB: coverage is not a colour
    atlasSpec.miplevels = 1;                   // sampled 1:1, no mip chain to build
    atlasSpec.anisotropy = false;
    atlasSpec.addressMode = vk::SamplerAddressMode::eClampToEdge;
    atlasSet.specs[0] = atlasSpec;

    // Static + shared: the quads depend only on the text and the font, so one
    // upload serves every frame.  (Making this per-frame would re-upload 50 KB
    // of unchanging data and, worse, repeat the frame-slot bug from 24.12.)
    auto quadSet = DescriptorLayoutAtlas::makeSet<QuadComponent, QuadComponentTable>(
        2, {QuadComponent::eQuads}, UpdateFrequency::eStatic, true);
    quadSet.specs[0] = GBufferSpec{static_cast<uint32_t>(layout.quads.size()),
                                   static_cast<uint32_t>(sizeof(GlyphQuad)), DataTypes::eCustom};
    layoutAtlas->setDetails({screenSet, atlasSet, quadSet});

    auto asset = mgr->CreateShaderAssetGroup(layoutAtlas);
    asset->addShaderSpirvCode("vert",
                              std::vector<uint32_t>(shader::TEXT_VERT_SPV.begin(), shader::TEXT_VERT_SPV.end()));
    asset->addShaderSpirvCode("frag",
                              std::vector<uint32_t>(shader::TEXT_FRAG_SPV.begin(), shader::TEXT_FRAG_SPV.end()));

    auto vertexInput = std::make_shared<VertexInputAtlas>(0, std::vector<VertexComponent>{}, 0u, 0u,
                                                          DataTypes::eUnsignedInt, true, kVertexCountPerGlyph,
                                                          static_cast<uint32_t>(layout.quads.size()));
    GraphicsPipelineConfig config;
    config.depthTestEnable = false;
    config.depthWriteEnable = false;
    config.blendEnable = options.blend;
    auto pipeline = mgr->CreatePipeline(vertexInput, layoutAtlas, asset, renderPass, 0, config);

    // One upload: the atlas bytes and the (never changing) glyph quads.
    auto upload = mgr->UploadSequence();
    {
        auto sync = std::make_shared<CmdSyncDevice>(asset);
        sync->OnUpdateRawData(ResourceId(1, 0), 0u, atlas.pixels());
        sync->OnUpdateRawData(ResourceId(2, 0), 0u, layout.quads);
        upload->submit(sync, asset);
    }

    // The instance buffer is the only thing standing between the CPU layout and
    // the GPU; reading it back turns "nothing on screen" into "the buffer is
    // zero" or "the buffer is fine, look at the shader".
    if (options.verify) {
        const size_t expectedBytes = layout.quads.size() * sizeof(GlyphQuad);
        const auto bytes = upload->ReadBack(asset, ResourceKey::make_key(ResourceId(2, 0), 0u), expectedBytes);
        if (bytes.size() < expectedBytes) {
            throw std::runtime_error("text_rendering: instance buffer readback returned no data");
        }
        const bool same = std::memcmp(bytes.data(), layout.quads.data(), expectedBytes) == 0;
        const auto *gpuQuads = reinterpret_cast<const GlyphQuad *>(bytes.data());
        std::cout << "[verify] instance buffer " << expectedBytes << " B, equals CPU layout: " << (same ? "yes" : "NO")
                  << "; quad[0] rect=(" << gpuQuads[0].rect.x << "," << gpuQuads[0].rect.y << ","
                  << gpuQuads[0].rect.z << "," << gpuQuads[0].rect.w << ") uv=(" << gpuQuads[0].uv.x << ","
                  << gpuQuads[0].uv.y << "," << gpuQuads[0].uv.z << "," << gpuQuads[0].uv.w << ")\n";
        if (!same) {
            throw std::runtime_error("text_rendering: the instance buffer does not match the CPU layout");
        }
    }

    auto sequence = mgr->DrawFrameSequence();
    auto draw = std::make_shared<CmdOpDrawFrame>(pipeline);
    auto frameSync = std::make_shared<CmdSyncDevice>(asset);

    uint32_t framesRendered = 0;
    bool dumpDone = false;
    while (!window.shouldClose()) {
        window.poolEvents();
        float currentTime = 0.0f;
        window.processInputEvents(currentTime);

        if (!sequence->prepareFrame()) {
            continue;
        }
        const uint32_t frame = sequence->getFrameInFlight();
        const vk::Extent2D extent = *mgr->getSwapChain()->getExtent2D();

        // Pixel-space ortho rebuilt every frame from the *current* extent, so a
        // resize keeps the text anchored to the bottom-left corner at its
        // original size.  The reference pins the matrix to 800x600 at startup,
        // which silently misplaces the text after a resize.
        //
        // The bottom/top arguments are swapped (h, 0) instead of the perspective
        // examples' `projection[1][1] *= -1`: Vulkan's NDC has y pointing down, so
        // pixel y must map to 1 - 2y/h.  Negating m11 alone leaves the constant
        // term at -1, which sends *every* vertex off-screen (2y/h - 1 negated is
        // -2y/h - 1, not 1 - 2y/h) -- no validation error, just an empty frame.
        const glm::mat4 projection = glm::ortho(0.0f, static_cast<float>(extent.width),
                                                static_cast<float>(extent.height), 0.0f);

        ScreenBlock block;
        block.projection = projection;
        draw->OnUpdateRawData(ResourceId(0, 0), frame, block);

        sequence->beginRecording();
        sequence->record(frameSync);
        sequence->recordPass(renderPass, [&]() { sequence->record(draw); });
        sequence->endRecording();
        sequence->submitFrame();

        ++framesRendered;
        if (options.dump && !dumpDone && framesRendered >= options.dumpAfter) {
            auto readback = ImageReadback(mgr->getLogicalDevice(), mgr->memoryProperties(),
                                          mgr->getMemoryAllocator(), mgr->getGraphicsQueue(),
                                          *mgr->getQueueFamilyIndices().graphicsFamily);

            DumpRequest frameRequest;
            frameRequest.label = options.blend ? "frame" : "frame-no-blend";
            frameRequest.image = mgr->getSwapChain()->getImages()[sequence->getImageIndex()];
            frameRequest.format = *mgr->getSwapChain()->getImageFormat();
            frameRequest.extent = extent;
            frameRequest.layout = vk::ImageLayout::ePresentSrcKHR;
            // ImageDump treats alpha < 0.5 as "uncovered" and writes black there,
            // which is what makes a G-Buffer dump readable.  Text is the one case
            // where alpha carries coverage instead of a mask: with blending on the
            // framebuffer alpha stays 1 (ONE_MINUS_SRC_ALPHA over a clear alpha of
            // 1) so nothing is masked, but with --no-blend alpha == coverage and the
            // transparent part of each glyph box reads as black in the file even
            // though the screen shows it in the text colour.
            frameRequest.encoding = DumpEncoding::eColor;

            // The atlas itself, grey on black: the glyph metrics can only be
            // right if the coverage is, and this file is what makes a packing
            // bug visible instead of merely "text looks slightly off".
            auto atlasImage = std::dynamic_pointer_cast<TextureImage>(
                asset->DescriptorResource(ResourceId(1, 0), 0u));
            if (!atlasImage) {
                throw std::runtime_error("text_rendering: the atlas texture resource was not created");
            }
            DumpRequest atlasRequest;
            atlasRequest.label = "font-atlas";
            atlasRequest.image = atlasImage->imageHandle();
            atlasRequest.format = vk::Format::eR8Unorm;
            atlasRequest.extent = vk::Extent2D{atlas.width(), atlas.height()};
            atlasRequest.layout = vk::ImageLayout::eShaderReadOnlyOptimal;
            atlasRequest.encoding = DumpEncoding::eCoverage;

            const auto results = DumpImages(readback, {frameRequest, atlasRequest}, "dump", 0);
            for (const DumpResult &result : results) {
                std::cout << "[dump] " << result.path.string() << " mean=(" << result.meanColor.r << ","
                          << result.meanColor.g << "," << result.meanColor.b << ")\n";
            }

            // End-to-end check: the ink box measured in the framebuffer must
            // match the box the CPU layout predicts.  This is what catches a
            // flipped projection, a wrong bearing sign or a bad scale -- none of
            // which produce a validation error.
            if (!results.empty() && layout.hasInk) {
                const glm::ivec4 expected = expectedImageBox(layout.inkBox, extent.height);
                const glm::ivec4 measured = findInkBox(results[0].rgb8, extent.width, extent.height, 6);
                const int dx0 = std::abs(expected.x - measured.x);
                const int dy0 = std::abs(expected.y - measured.y);
                const int dx1 = std::abs(expected.z - measured.z);
                const int dy1 = std::abs(expected.w - measured.w);
                const int worst = std::max({dx0, dy0, dx1, dy1});
                std::cout << "[verify] ink box expected (" << expected.x << "," << expected.y << ")-(" << expected.z
                          << "," << expected.w << "), measured (" << measured.x << "," << measured.y << ")-("
                          << measured.z << "," << measured.w << "), worst edge delta " << worst << " px\n";
                if (worst > 2) {
                    throw std::runtime_error("text_rendering: rendered ink box does not match the layout");
                }
            }

            dumpDone = true;
            if (!sequence->presentFrame()) {
                continue;
            }
            break;
        }

        if (!sequence->presentFrame()) {
            continue;
        }
    }
}

int main(int argc, char **argv)
{
    try {
        test(argc, argv);
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << "\n";
        return 1;
    }
}
