// heightmap_terrain: LearnOpenGL guest 2021 3.tessellation, both variants in one
// example.  The two reference programs differ along two axes at once -- *who*
// displaces the heightmap and *how dense* the mesh is -- and that is exactly the
// point of the pair, so both are reproduced faithfully:
//
//   --mode cpu   the host reads every texel, builds a full-resolution lattice of
//                vertices with per-vertex normals and uploads it.  No
//                tessellation stage: the mesh the host built is the mesh drawn.
//                --step decimates the lattice (1 = one vertex per texel).
//
//   --mode gpu   the host uploads a 32 x 32 grid of flat quads (4 control points
//                each, 4096 vertices for a 4.6 M-texel heightmap).  The
//                tessellation control shader picks a subdivision level per patch
//                edge from its distance to the camera, the evaluation shader
//                samples the heightmap, pushes the vertex along the patch normal
//                and rebuilds the shading normal from the height gradient.
//
// Both paths write the same three varyings and run the *same* fragment shader,
// so a pixel difference between the two dumps is a geometry difference and
// nothing else.  --grayscale drops the lighting and shows the bare height ramp,
// which is what the tutorial did and the cheapest way to see a displacement bug.
//
// Two deliberate corrections against the reference, both of which show up as
// soon as the two variants are compared side by side:
//
//   * Height normalisation.  The CPU variant scales the byte by 64/256 and then
//     divides by 32 for display; the GPU variant scales by 64.0 and divides by
//     64.  Same heightmap, two different mountains, one of them blown out to
//     white.  Here both use span = 64, shift = 16 and divide by the span.
//   * Patch corner order.  The reference emits the corners as
//     (i,j) (i+1,j) (i,j+1) (i+1,j+1), which is not perimeter order, while the
//     quad tessellator reads corner 2 as (u=1, v=1) and corner 3 as (u=0, v=1).
//     The result is a mirrored patch that only looks plausible because the
//     heightmap is sampled with the same mirrored UVs.  This example emits the
//     corners around the perimeter, so u runs along +x and v along +z.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include <glm/gtc/matrix_transform.hpp>

#include "Camera.h"
#include "GLFWwindow.h"
#include "ImageDump.h"
#include "ImageReadback.h"
#include "RenderCommon.h"
#include "TextureLoader.h"

#include "AttachmentDetailConfig.h"
#include "DescriptorLayoutConfig.h"
#include "VertexInputStateConfig.h"

#include "CmdOpDrawFrame.h"
#include "CmdSyncDevice.h"
#include "IRenderPass.h"
#include "LoopSequence.h"
#include "OnceSequence.h"
#include "RenderManager.h"

#include "shader/terrain_cpu_vert.hpp"
#include "shader/terrain_frag.hpp"
#include "shader/terrain_gpu_tesc.hpp"
#include "shader/terrain_gpu_tese.hpp"
#include "shader/terrain_gpu_vert.hpp"

namespace {

const char *kHeightmapPath = "assets/heightmaps/iceland_heightmap.png";

// One world unit per texel: the terrain is 2624 x 1756 units across, exactly
// like the reference.  The near/far planes have to cover that, which is why this
// example builds its own projection instead of using MakeCameraMatrix (whose
// far plane is 100).
constexpr float kUnitsPerTexel = 1.0f;
constexpr float kHeightSpan = 64.0f;  // world-space range of the byte value
constexpr float kHeightShift = 16.0f; // byte 0 sits at y = -16
constexpr float kNearPlane = 1.0f;
constexpr float kFarPlane = 20000.0f;

struct Options {
    std::string mode = "gpu";
    uint32_t patches = 32;
    uint32_t step = 2;
    bool wireframe = false;
    bool grayscale = false;
    bool dump = false;
    // Unlike the smaller examples, the very first frame of this one is not a
    // good capture: a cold process start uploads ~250 MB and can still be
    // carrying a stale swapchain extent, so frame 0 occasionally comes out with
    // a slightly different projection.  Two settling frames make a dump
    // reproducible.
    uint32_t dumpAfter = 2;
};

Options parseOptions(int argc, char **argv)
{
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--mode" && i + 1 < argc) {
            options.mode = argv[++i];
            if (options.mode != "cpu" && options.mode != "gpu") {
                throw std::runtime_error("heightmap_terrain: --mode must be cpu or gpu");
            }
        } else if (arg == "--patches" && i + 1 < argc) {
            options.patches = static_cast<uint32_t>(std::stoul(argv[++i]));
        } else if (arg == "--step" && i + 1 < argc) {
            options.step = std::max(1u, static_cast<uint32_t>(std::stoul(argv[++i])));
        } else if (arg == "--wireframe") {
            options.wireframe = true;
        } else if (arg == "--grayscale") {
            options.grayscale = true;
        } else if (arg == "--dump") {
            options.dump = true;
        } else if (arg == "--dump-after" && i + 1 < argc) {
            options.dumpAfter = static_cast<uint32_t>(std::stoul(argv[++i]));
        } else {
            throw std::runtime_error("heightmap_terrain: unknown argument " + arg);
        }
    }
    if (options.patches < 1) {
        options.patches = 1;
    }
    return options;
}

// ---------------------------------------------------------------------------
// Height field: the single source of truth for both displacement paths.
//
// sample() reproduces exactly what a CLAMP_TO_EDGE / linear / LOD 0 sampler
// returns, so the host-side lattice and the tessellated surface are displaced by
// the same numbers.
// ---------------------------------------------------------------------------
class HeightField {
public:
    HeightField(const LoadedTexture &image) :
        m_pixels(image.pixels), m_width(static_cast<uint32_t>(image.width)),
        m_height(static_cast<uint32_t>(image.height))
    {
        if (m_width < 2 || m_height < 2) {
            throw std::runtime_error("heightmap_terrain: heightmap must be at least 2x2");
        }
    }

    uint32_t width() const { return m_width; }
    uint32_t height() const { return m_height; }
    float worldWidth() const { return static_cast<float>(m_width - 1) * kUnitsPerTexel; }
    float worldDepth() const { return static_cast<float>(m_height - 1) * kUnitsPerTexel; }
    float texelU() const { return 1.0f / static_cast<float>(m_width); }
    float texelV() const { return 1.0f / static_cast<float>(m_height); }

    // u, v in [0, 1] over the image; returns the byte value as [0, 1].
    float sample(float u, float v) const
    {
        const float fx = u * static_cast<float>(m_width) - 0.5f;
        const float fy = v * static_cast<float>(m_height) - 0.5f;
        const float x0f = std::floor(fx);
        const float y0f = std::floor(fy);
        const float tx = fx - x0f;
        const float ty = fy - y0f;
        const int x0 = static_cast<int>(x0f);
        const int y0 = static_cast<int>(y0f);
        const float top = glm::mix(texel(x0, y0), texel(x0 + 1, y0), tx);
        const float bottom = glm::mix(texel(x0, y0 + 1), texel(x0 + 1, y0 + 1), tx);
        return glm::mix(top, bottom, ty);
    }

    float heightAt(float u, float v) const { return sample(u, v) * kHeightSpan - kHeightShift; }

    // Same central differences the tessellation evaluation shader runs.
    glm::vec3 normalAt(float u, float v) const
    {
        const float du = texelU();
        const float dv = texelV();
        const float dHdx = (heightAt(u + du, v) - heightAt(u - du, v)) / (2.0f * du * worldWidth());
        const float dHdz = (heightAt(u, v + dv) - heightAt(u, v - dv)) / (2.0f * dv * worldDepth());
        return glm::normalize(glm::vec3(-dHdx, 1.0f, -dHdz));
    }

    // World position of the (column, row) sample point.
    glm::vec3 worldPosition(float u, float v) const
    {
        return glm::vec3((u - 0.5f) * worldWidth(), heightAt(u, v), (v - 0.5f) * worldDepth());
    }

private:
    uint8_t byteAt(int x, int y) const
    {
        x = std::clamp(x, 0, static_cast<int>(m_width) - 1);
        y = std::clamp(y, 0, static_cast<int>(m_height) - 1);
        return m_pixels[(static_cast<size_t>(y) * m_width + static_cast<size_t>(x)) * 4];
    }

    float texel(int x, int y) const { return static_cast<float>(byteAt(x, y)) / 255.0f; }

private:
    const std::vector<uint8_t> &m_pixels;
    uint32_t m_width = 0;
    uint32_t m_height = 0;
};

struct TerrainUBO {
    alignas(16) glm::mat4 viewProj{};
    // The control shader needs the view matrix itself: its adaptive level comes
    // from the view-space depth of the patch corners.
    alignas(16) glm::mat4 view{};
    alignas(16) glm::vec4 eye{};
    // x: height span, y: height shift, z: grayscale flag
    alignas(16) glm::vec4 height{kHeightSpan, kHeightShift, 0.0f, 0.0f};
    // x: world width, y: world depth, z: one texel in u, w: one texel in v
    alignas(16) glm::vec4 terrain{};
    // x: min level, y: max level, z: min distance, w: max distance
    alignas(16) glm::vec4 tess{};
};

enum class TerrainComponent { eHeightMap, eCamera };
using TerrainConfig = DescriptorComponentSpec<TerrainComponent>;
inline constexpr auto TerrainComponentTable = std::make_tuple(
    TerrainConfig{TerrainComponent::eHeightMap, vk::DescriptorType::eCombinedImageSampler, 1,
                  vk::ShaderStageFlagBits::eTessellationEvaluation},
    TerrainConfig{TerrainComponent::eCamera, vk::DescriptorType::eUniformBuffer, 1,
                  vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eTessellationControl
                      | vk::ShaderStageFlagBits::eTessellationEvaluation | vk::ShaderStageFlagBits::eFragment});

std::shared_ptr<DescriptorLayoutAtlas> makeLayout(const TextureImageSpec &heightmapSpec)
{
    auto layout = std::make_shared<DescriptorLayoutAtlas>(MAX_FRAMES_IN_FLIGHT);

    // Set 0: the heightmap.  Static and shared -- it is uploaded once and never
    // changes.  The CPU path leaves this set bound but unused.
    auto heightSet = DescriptorLayoutAtlas::makeSet<TerrainComponent, TerrainComponentTable>(
        0, {TerrainComponent::eHeightMap}, UpdateFrequency::eStatic, true);
    heightSet.specs[0] = heightmapSpec;

    // Set 1: the per-frame uniform block.  One matrix pair per frame in flight.
    auto cameraSet = DescriptorLayoutAtlas::makeSet<TerrainComponent, TerrainComponentTable>(
        1, {TerrainComponent::eCamera}, UpdateFrequency::eEachFrame, false);
    cameraSet.specs[0] = GBufferSpec{1, static_cast<uint32_t>(sizeof(TerrainUBO)), DataTypes::eCustom};

    layout->setDetails({heightSet, cameraSet});
    return layout;
}

// A heightmap is data, not a photograph: UNORM so the byte is the height, and a
// single mip level so the GPU cannot sample a box-filtered (i.e. smoothed) copy
// of it at distance.  An sRGB format would apply a ~2.2 gamma to every height.
TextureImageSpec makeHeightmapSpec(const LoadedTexture &image)
{
    TextureImageSpec spec;
    spec.width = static_cast<uint32_t>(image.width);
    spec.height = static_cast<uint32_t>(image.height);
    spec.channels = 4;
    spec.miplevels = 1;
    spec.format = vk::Format::eR8G8B8A8Unorm;
    spec.mipsInData = false;
    spec.addressMode = vk::SamplerAddressMode::eClampToEdge;
    spec.anisotropy = false;
    return spec;
}

// CPU displacement: one vertex per sampled texel, plus an index buffer of
// triangle strips (one per row, because a single strip would stitch the rows
// together).  Two indices per quad instead of six is what keeps the buffer
// "only" 4x the vertex count at full resolution.
struct CpuTerrain {
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
    std::vector<DrawRange> ranges;
    uint32_t columns = 0;
    uint32_t rows = 0;
    uint64_t triangles = 0;
};

CpuTerrain buildCpuTerrain(const HeightField &field, uint32_t step)
{
    CpuTerrain terrain;
    const uint32_t width = field.width();
    const uint32_t height = field.height();
    terrain.columns = (width + step - 1) / step;
    terrain.rows = (height + step - 1) / step;
    terrain.vertices.resize(static_cast<size_t>(terrain.columns) * terrain.rows);

    for (uint32_t row = 0; row < terrain.rows; ++row) {
        for (uint32_t column = 0; column < terrain.columns; ++column) {
            const uint32_t x = std::min(column * step, width - 1);
            const uint32_t y = std::min(row * step, height - 1);
            // Texel centres: u = 0.5/w is the middle of the first texel, which is
            // also where the bilinear sample lands exactly on a texel.
            const float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(width);
            const float v = (static_cast<float>(y) + 0.5f) / static_cast<float>(height);

            Vertex &vertex = terrain.vertices[static_cast<size_t>(row) * terrain.columns + column];
            vertex.position = field.worldPosition(u, v);
            vertex.normal = field.normalAt(u, v);
            vertex.texCoord = glm::vec2(u, v);
        }
    }

    const uint32_t indicesPerStrip = terrain.columns * 2;
    terrain.indices.reserve(static_cast<size_t>(terrain.rows - 1) * indicesPerStrip);
    for (uint32_t row = 0; row + 1 < terrain.rows; ++row) {
        DrawRange range;
        range.firstIndex = static_cast<uint32_t>(terrain.indices.size());
        range.indexCount = indicesPerStrip;
        terrain.ranges.push_back(range);
        for (uint32_t column = 0; column < terrain.columns; ++column) {
            terrain.indices.push_back(row * terrain.columns + column);
            terrain.indices.push_back((row + 1) * terrain.columns + column);
        }
    }

    terrain.triangles = static_cast<uint64_t>(terrain.rows - 1) * (terrain.columns - 1) * 2;
    return terrain;
}

// GPU displacement: a flat grid of quads.  All four corners of a patch sit on
// y = 0 with a UV, and the hardware produces everything in between.
struct GpuTerrain {
    std::vector<Vertex> vertices;
    uint32_t patches = 0;
};

GpuTerrain buildGpuTerrain(const HeightField &field, uint32_t patches)
{
    GpuTerrain terrain;
    terrain.patches = patches;
    terrain.vertices.reserve(static_cast<size_t>(patches) * patches * 4);

    const auto corner = [&](float u, float v) {
        Vertex vertex;
        vertex.position = glm::vec3((u - 0.5f) * field.worldWidth(), 0.0f, (v - 0.5f) * field.worldDepth());
        vertex.texCoord = glm::vec2(u, v);
        terrain.vertices.push_back(vertex);
    };

    for (uint32_t row = 0; row < patches; ++row) {
        for (uint32_t column = 0; column < patches; ++column) {
            const float u0 = static_cast<float>(column) / static_cast<float>(patches);
            const float u1 = static_cast<float>(column + 1) / static_cast<float>(patches);
            const float v0 = static_cast<float>(row) / static_cast<float>(patches);
            const float v1 = static_cast<float>(row + 1) / static_cast<float>(patches);
            // Perimeter order: u along 0->1 (+x), v along 0->3 (+z).
            corner(u0, v0);
            corner(u1, v0);
            corner(u1, v1);
            corner(u0, v1);
        }
    }
    return terrain;
}

bool justPressed(bool current, bool &held)
{
    const bool result = current && !held;
    held = current;
    return result;
}

glm::mat4 makeProjection(const Camera &camera, float width, float height)
{
    const float safeHeight = height > 0.0f ? height : 1.0f;
    glm::mat4 projection =
        glm::perspective(glm::radians(camera.Zoom), width / safeHeight, kNearPlane, kFarPlane);
    projection[1][1] *= -1.0f;
    return projection;
}

// Coverage plus unique-colour count from the dump itself: the pass clears every
// texel, so the most frequent colour *is* the background.
void analyseFrame(const std::vector<uint8_t> &rgb, float &coverage, size_t &uniqueColors)
{
    std::unordered_map<uint32_t, uint32_t> histogram;
    histogram.reserve(4096);
    const size_t texels = rgb.size() / 3;
    for (size_t i = 0; i + 2 < rgb.size(); i += 3) {
        const uint32_t key = (static_cast<uint32_t>(rgb[i]) << 16) | (static_cast<uint32_t>(rgb[i + 1]) << 8)
                             | static_cast<uint32_t>(rgb[i + 2]);
        ++histogram[key];
    }

    uint32_t backgroundCount = 0;
    uniqueColors = 0;
    for (const auto &[key, count] : histogram) {
        (void)key;
        backgroundCount = std::max(backgroundCount, count);
        if (count >= 16) {
            ++uniqueColors;
        }
    }
    coverage = texels > 0 ? 1.0f - static_cast<float>(backgroundCount) / static_cast<float>(texels) : 0.0f;
}

} // namespace

void test(int argc, char **argv)
{
    const Options options = parseOptions(argc, argv);
    const bool cpuMode = options.mode == "cpu";

    // ----- Height field -----------------------------------------------------
    const LoadedTexture image = loadTexture(kHeightmapPath);
    const HeightField field(image);
    std::cout << "[heightmap_terrain] heightmap " << field.width() << "x" << field.height() << ", world "
              << field.worldWidth() << " x " << field.worldDepth() << " units, heights " << -kHeightShift << " .. "
              << (kHeightSpan - kHeightShift) << std::endl;

    // ----- Camera -----------------------------------------------------------
    // Standing near the +z border looking down the short axis of the map: the
    // heightmap is a finite plate, and from any viewpoint that also shows the
    // long axis the plate's boundary lands inside the frame (the far corner is
    // only ~1800 units away from an eye 630 units up, and the vertical field is
    // 45 degrees).  Looking along the 1755-unit axis, with the eye 800 units
    // from the far border, keeps the whole frame inside the map: the top of the
    // frame reaches 1515 units out, the border is at 1677.
    auto camera = std::make_shared<Camera>(glm::vec3(0.0f, 627.5f, 800.0f), glm::vec3(0.0f, 1.0f, 0.0f), -90.0f,
                                           -45.0f);
    // The default 2.5 units/s would need 17 minutes to cross the 2623-unit map.
    camera->MovementSpeed = 200.0f;

    GLFWwindow window(1280, 720, camera);

    DeviceRequirement requirement;
    requirement.fillModeNonSolid = true;
    if (!cpuMode) {
        requirement.tessellationShader = true;
    }
    auto mgr = RenderManager::Create(window, MAX_FRAMES_IN_FLIGHT, requirement);

    // ----- Geometry ---------------------------------------------------------
    CpuTerrain cpuTerrain;
    GpuTerrain gpuTerrain;
    uint32_t maxTessLevel = 0;
    if (cpuMode) {
        cpuTerrain = buildCpuTerrain(field, options.step);
        std::cout << "[heightmap_terrain] mode cpu: " << cpuTerrain.columns << " x " << cpuTerrain.rows << " lattice = "
                  << cpuTerrain.vertices.size() << " vertices ("
                  << (cpuTerrain.vertices.size() * sizeof(Vertex)) / (1024.0 * 1024.0) << " MB), "
                  << cpuTerrain.ranges.size() << " strips, " << cpuTerrain.indices.size() << " indices ("
                  << (cpuTerrain.indices.size() * sizeof(uint32_t)) / (1024.0 * 1024.0) << " MB), "
                  << cpuTerrain.triangles << " triangles" << std::endl;
    } else {
        gpuTerrain = buildGpuTerrain(field, options.patches);
        maxTessLevel = std::min(64u, mgr->deviceProperties().limits.maxTessellationGenerationLevel);
        const uint64_t maxTriangles =
            static_cast<uint64_t>(options.patches) * options.patches * maxTessLevel * maxTessLevel * 2;
        std::cout << "[heightmap_terrain] mode gpu: " << options.patches << " x " << options.patches << " = "
                  << (static_cast<uint64_t>(options.patches) * options.patches) << " patches, "
                  << gpuTerrain.vertices.size() << " control points ("
                  << (gpuTerrain.vertices.size() * sizeof(Vertex)) / 1024.0 << " KB), up to " << maxTriangles
                  << " triangles at level " << maxTessLevel << std::endl;
    }

    // ----- Render pass and pipelines ---------------------------------------
    auto renderPass = mgr->CreateRenderPass(std::make_shared<SimpleAttachmentDetail>(
        SimpleAttachmentDetail::Mode::eDirect, *mgr->getSwapChain()->getImageFormat(), mgr->DefaultDepthFormat()));

    const TextureImageSpec heightmapSpec = makeHeightmapSpec(image);
    auto layout = makeLayout(heightmapSpec);
    auto asset = mgr->CreateShaderAssetGroup(layout);

    // Every stage has to be registered before the pipelines are built: the
    // pipeline reads the SPIR-V out of the asset at creation time.
    asset->addShaderSpirvCode("frag",
                              std::vector<uint32_t>(shader::TERRAIN_FRAG_SPV.begin(), shader::TERRAIN_FRAG_SPV.end()));
    if (cpuMode) {
        asset->addShaderSpirvCode("vert", std::vector<uint32_t>(shader::TERRAIN_CPU_VERT_SPV.begin(),
                                                               shader::TERRAIN_CPU_VERT_SPV.end()));
    } else {
        asset->addShaderSpirvCode("vert", std::vector<uint32_t>(shader::TERRAIN_GPU_VERT_SPV.begin(),
                                                               shader::TERRAIN_GPU_VERT_SPV.end()));
        asset->addShaderSpirvCode("tesc", std::vector<uint32_t>(shader::TERRAIN_GPU_TESC_SPV.begin(),
                                                               shader::TERRAIN_GPU_TESC_SPV.end()));
        asset->addShaderSpirvCode("tese", std::vector<uint32_t>(shader::TERRAIN_GPU_TESE_SPV.begin(),
                                                               shader::TERRAIN_GPU_TESE_SPV.end()));
    }

    const auto makeConfig = [&](vk::PolygonMode polygonMode) {
        GraphicsPipelineConfig config;
        config.cullMode = vk::CullModeFlagBits::eNone;
        config.polygonMode = polygonMode;
        if (cpuMode) {
            config.addStage("vert", vk::ShaderStageFlagBits::eVertex)
                .addStage("frag", vk::ShaderStageFlagBits::eFragment);
            config.topology = vk::PrimitiveTopology::eTriangleStrip;
        } else {
            config.addStage("vert", vk::ShaderStageFlagBits::eVertex)
                .addStage("tesc", vk::ShaderStageFlagBits::eTessellationControl)
                .addStage("tese", vk::ShaderStageFlagBits::eTessellationEvaluation)
                .addStage("frag", vk::ShaderStageFlagBits::eFragment);
            config.topology = vk::PrimitiveTopology::ePatchList;
            config.patchControlPoints = 4;
        }
        return config;
    };

    std::shared_ptr<GraphicsPipeline> fillPipeline;
    std::shared_ptr<GraphicsPipeline> linePipeline;
    if (cpuMode) {
        auto vertexInput = std::make_shared<VertexInputAtlas>(
            0, std::vector<VertexComponent>{VertexComponent::ePosition, VertexComponent::eNormal},
            static_cast<uint32_t>(cpuTerrain.vertices.size()), static_cast<uint32_t>(cpuTerrain.indices.size()),
            DataTypes::eUnsignedInt);
        fillPipeline =
            mgr->CreatePipeline(vertexInput, layout, asset, renderPass, 0, makeConfig(vk::PolygonMode::eFill));
        linePipeline =
            mgr->CreatePipeline(vertexInput, layout, asset, renderPass, 0, makeConfig(vk::PolygonMode::eLine));
    } else {
        auto vertexInput = std::make_shared<VertexInputAtlas>(
            0, std::vector<VertexComponent>{VertexComponent::ePosition, VertexComponent::eUV},
            static_cast<uint32_t>(gpuTerrain.vertices.size()));
        fillPipeline =
            mgr->CreatePipeline(vertexInput, layout, asset, renderPass, 0, makeConfig(vk::PolygonMode::eFill));
        linePipeline =
            mgr->CreatePipeline(vertexInput, layout, asset, renderPass, 0, makeConfig(vk::PolygonMode::eLine));
    }

    // ----- Upload -----------------------------------------------------------
    auto upload = mgr->UploadSequence();
    auto sync = std::make_shared<CmdSyncDevice>(asset);
    if (cpuMode) {
        sync->OnUpdateRawData(ResourceTypes::eVertex, cpuTerrain.vertices);
        sync->OnUpdateRawData(ResourceTypes::eIndex, cpuTerrain.indices);
    } else {
        sync->OnUpdateRawData(ResourceTypes::eVertex, gpuTerrain.vertices);
    }
    sync->OnUpdateRawData(ResourceId(0, 0), 0u, image.pixels);
    upload->submit(sync, asset);

    auto draw = std::make_shared<CmdOpDrawFrame>(fillPipeline);
    auto lineDraw = std::make_shared<CmdOpDrawFrame>(linePipeline);
    if (cpuMode) {
        // One draw per row of the lattice: adjacent rows must not be stitched
        // into a single strip.
        draw->setDrawRanges(cpuTerrain.ranges);
        lineDraw->setDrawRanges(cpuTerrain.ranges);
    }

    auto sequence = mgr->DrawFrameSequence();

    bool wireframe = options.wireframe;
    bool grayscale = options.grayscale;
    std::cout << "[heightmap_terrain] press W to toggle wireframe, G to toggle grayscale (start: "
              << (wireframe ? "wireframe" : "solid") << ", " << (grayscale ? "grayscale" : "lit") << ")" << std::endl;

    // Adaptive levels are tuned to this camera, not to a guess: the frame spans
    // 690 units (bottom edge) to 1520 units (top edge), so 600..1700 puts the
    // foreground at ~59 and the far edge at ~14.  The CPU path has no
    // tessellation stage and ignores all four values.
    const float maxLevel = cpuMode ? 8.0f : static_cast<float>(std::max(1u, maxTessLevel));
    const glm::vec4 tessParameters(4.0f, maxLevel, 600.0f, 1700.0f);

    bool dumpPending = false;
    bool dumpDone = false;
    uint32_t renderedFrames = 0;
    bool wHeld = false;
    bool gHeld = false;

    while (!window.shouldClose()) {
        window.poolEvents();
        float currentTime = 0.0f;
        window.processInputEvents(currentTime);
        if (justPressed(window.isKeyDown(GLFW_KEY_W), wHeld)) {
            wireframe = !wireframe;
            std::cout << "[heightmap_terrain] wireframe " << (wireframe ? "ON" : "OFF") << std::endl;
        }
        if (justPressed(window.isKeyDown(GLFW_KEY_G), gHeld)) {
            grayscale = !grayscale;
            std::cout << "[heightmap_terrain] grayscale " << (grayscale ? "ON" : "OFF") << std::endl;
        }

        if (!sequence->prepareFrame()) {
            continue;
        }
        const uint32_t frame = sequence->getFrameInFlight();
        const vk::Extent2D extent = *mgr->getSwapChain()->getExtent2D();

        if (options.dump && !dumpDone && !dumpPending && renderedFrames >= options.dumpAfter) {
            dumpPending = true;
        }

        TerrainUBO ubo;
        ubo.view = camera->GetViewMatrix();
        ubo.viewProj = makeProjection(*camera, static_cast<float>(extent.width), static_cast<float>(extent.height))
                       * ubo.view;
        ubo.eye = glm::vec4(camera->Position, 1.0f);
        ubo.height.z = grayscale ? 1.0f : 0.0f;
        ubo.terrain = glm::vec4(field.worldWidth(), field.worldDepth(), field.texelU(), field.texelV());
        ubo.tess = tessParameters;
        draw->OnUpdateRawData(ResourceId(1, 0), frame, ubo);
        lineDraw->OnUpdateRawData(ResourceId(1, 0), frame, ubo);

        auto active = wireframe ? lineDraw : draw;
        sequence->beginRecording();
        // The uniform block is a per-frame resource: without this the device copy
        // would keep whatever the one-shot upload wrote and the camera would
        // appear frozen.
        sequence->record(sync);
        sequence->recordPass(renderPass, [&]() { sequence->record(active); });
        sequence->endRecording();
        sequence->submitFrame();

        if (dumpPending) {
            ImageReadback readback(mgr->getLogicalDevice(), mgr->memoryProperties(), mgr->getMemoryAllocator(),
                                   mgr->getGraphicsQueue(), *mgr->getQueueFamilyIndices().graphicsFamily);

            const std::vector<vk::Image> &swapImages = mgr->getSwapChain()->getImages();
            const uint32_t imageIndex = sequence->getImageIndex();
            if (imageIndex >= swapImages.size()) {
                throw std::runtime_error("heightmap_terrain: swapchain image index out of range");
            }
            DumpRequest request;
            request.label = options.mode;
            request.image = swapImages[imageIndex];
            request.format = *mgr->getSwapChain()->getImageFormat();
            request.extent = extent;
            request.layout = vk::ImageLayout::ePresentSrcKHR;
            request.encoding = DumpEncoding::eColor;
            const auto results = DumpImages(readback, {request}, "dump", 0);

            float coverage = 0.0f;
            size_t uniqueColors = 0;
            analyseFrame(results[0].rgb8, coverage, uniqueColors);
            std::cout << "[dump] " << results[0].path.string() << " mode=" << options.mode
                      << " wireframe=" << (wireframe ? 1 : 0) << " grayscale=" << (grayscale ? 1 : 0)
                      << " coverage=" << coverage << " uniqueColors=" << uniqueColors << " mean=("
                      << results[0].meanColor.r << "," << results[0].meanColor.g << "," << results[0].meanColor.b
                      << ")" << std::endl;

            dumpPending = false;
            dumpDone = true;
        }

        if (!sequence->presentFrame()) {
            continue;
        }
        ++renderedFrames;
        if (options.dump && dumpDone) {
            break;
        }
    }
}

int main(int argc, char **argv)
{
    try {
        test(argc, argv);
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << "\n";
        return 1;
    }
}
