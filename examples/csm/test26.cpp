// csm: cascaded shadow maps, migrated from guest/2021/2.csm.
//
// A directional light covers a 120 unit box; a single 2048x2048 depth map spread
// over that box would give roughly 17 cm per texel, which turns every shadow
// edge into a staircase.  Cascaded shadow maps split the camera frustum into N
// depth slices (cascades), fit an orthographic light frustum to each one and
// render the scene once per slice -- so the slice nearest the eye gets the same
// texel budget as the far slice, and the shadow texel density lands where it is
// visible.
//
// The reference implementation renders the scene five times with five different
// matrices.  Here a geometry shader with `invocations = 5` writes all five array
// layers from a *single* instanced draw (gl_InvocationID picks both the matrix
// and gl_Layer), which is what the shadow map's 2D-array layout is for.
//
// Three engine gaps had to be closed for this to be possible at all; all three
// are in the engine now, and this example is their only consumer:
//
//   * ImageAttachment / OffscreenTargetSpec / RenderPassBlueprint carry a layer
//     count, so a depth attachment can be a 2D array and the framebuffer can
//     declare more than one layer (gl_Layer is illegal otherwise).
//   * IRenderPass::depthStencilView() exposes the array view the framebuffer was
//     built from, so the lighting pass can sample the depth array it just wrote.
//   * GraphicsPipelineConfig::addGeometryStage(key) registers the geometry
//     stage.  The invocation count itself is a SPIR-V execution mode baked into
//     depth.geom, so the shader literal and kMaxCascades have to agree.
//
// Differences from the reference, each deliberate:
//   * The scene is 12 boxes and a ground slab instead of a loaded model, and the
//     placement is a fixed table instead of std::random_device -- two runs of
//     the reference produce different layouts, which makes --dump useless as a
//     regression artifact.
//   * The light projection writes Vulkan depth ([0,1]) instead of glm::ortho's
//     [-1,1]; with the OpenGL depth range the near half of every cascade is
//     clipped by Vulkan's 0 <= z <= w test.
//   * The light box is measured with std::numeric_limits<float>::lowest() as the
//     max seed.  The reference uses ::min() (the smallest *positive* float), so
//     every box whose light-space extent is negative gets clamped to zero and
//     the cascade silently covers more area than it needs to.
//   * --far defaults to 120 instead of 500: the splits are far/{50,25,10,2}, and
//     with far = 500 the four near cascades (10/20/50/250) all fall inside the
//     ground slab, so nothing visible ever reaches cascade 2..4.
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include "Camera.h"
#include "GLFWwindow.h"
#include "RenderCommon.h"

#include "AttachmentDetailConfig.h"
#include "DescriptorLayoutConfig.h"
#include "ImageDump.h"
#include "ImageReadback.h"
#include "VertexInputStateConfig.h"

#include "CmdOpDrawFrame.h"
#include "CmdSyncDevice.h"
#include "IRenderPass.h"
#include "LoopSequence.h"
#include "OnceSequence.h"
#include "RenderManager.h"
#include "TextureSamplerCombine.h"

#include "shader/csm_frag.hpp"
#include "shader/csm_vert.hpp"
#include "shader/depth_frag.hpp"
#include "shader/depth_geom.hpp"
#include "shader/depth_vert.hpp"

namespace {

// Layers of the shadow map.  Must match the `invocations = 5` literal in
// depth.geom and the array size in every shader block.
constexpr uint32_t kMaxCascades = 5;
// Camera near plane.  Small on purpose: cascade 0 starts here, and the whole
// point of CSM is that this range gets its own depth map.
constexpr float kNearPlane = 0.1f;
// Top surface of the ground slab.
constexpr float kFloorY = -2.0f;

// ---------------------------------------------------------------------------
// UBO.  Mirrors the CsmBlock std140 layout declared in csm.vert / csm.frag /
// depth.geom; the static_assert is the only thing keeping the three copies in
// sync, so keep them together.
// ---------------------------------------------------------------------------
struct CsmUBO {
    alignas(16) glm::mat4 lightSpaceMatrices[kMaxCascades]{};
    alignas(16) glm::mat4 view{1.0f};
    alignas(16) glm::mat4 viewProj{1.0f};
    // Split distances of cascades 0..3 (cascade 4 is open-ended).  A single
    // vec4 because std140 cannot express a 4-element float array without
    // padding every element to 16 bytes anyway.
    alignas(16) glm::vec4 cascadePlaneDistances{0.0f};
    // xyz = direction towards the light, w = camera far plane.
    alignas(16) glm::vec4 lightDir{0.0f};
    alignas(16) glm::vec4 eye{0.0f};
    // x = debug cascade tint, y = layer count, z = shadow enable, w = 1 / resolution.
    alignas(16) glm::vec4 flags{0.0f};
};
static_assert(sizeof(CsmUBO) == kMaxCascades * 64 + 64 + 64 + 4 * 16, "CsmUBO must match the std140 block");

struct Instance {
    alignas(16) glm::mat4 model{1.0f};
    // rgb = base colour, a > 0.5 selects the procedural checker.
    alignas(16) glm::vec4 material{1.0f};
};
static_assert(sizeof(Instance) == 80, "Instance must be model(64) + material(16)");

// set0: per-frame cascade block, read by the vertex, geometry and fragment
// stages (the vertex shader needs viewProj, the geometry shader the light
// matrices, the fragment shader the splits).
enum class CsmComponent { eCsmBlock };
using CsmConfig = DescriptorComponentSpec<CsmComponent>;
inline constexpr auto CsmComponentTable = std::make_tuple(CsmConfig{
    CsmComponent::eCsmBlock, vk::DescriptorType::eUniformBuffer, 1,
    vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eGeometry | vk::ShaderStageFlagBits::eFragment});

// set1: one entry per instance.
enum class CsmInstanceComponent { eInstances };
using CsmInstanceConfig = DescriptorComponentSpec<CsmInstanceComponent>;
inline constexpr auto CsmInstanceComponentTable = std::make_tuple(CsmInstanceConfig{
    CsmInstanceComponent::eInstances, vk::DescriptorType::eStorageBuffer, 1,
    vk::ShaderStageFlagBits::eVertex});

// set2: the shadow depth array, sampled by the lighting pass only.
enum class ShadowComponent { eShadowMap };
using ShadowConfig = DescriptorComponentSpec<ShadowComponent>;
inline constexpr auto ShadowComponentTable = std::make_tuple(ShadowConfig{
    ShadowComponent::eShadowMap, vk::DescriptorType::eCombinedImageSampler, 1,
    vk::ShaderStageFlagBits::eFragment});

struct Options {
    bool dump = false;
    // First frames carry a stale swapchain extent and an unfilled depth array,
    // so the capture waits two frames (same default as heightmap_terrain).
    uint32_t dumpAfter = 2;
    // 0 = lit, 1 = cascade tint, 2 = shadow term, 3 = fragment depth,
    // 4 = shadow-map depth, 5 = fragment/map split (0.5 grey = equal).
    // Matches the flags.x branch in csm.frag.
    uint32_t debugView = 0;
    bool noShadow = false;
    uint32_t resolution = 2048;
    float farPlane = 120.0f;
    // The reference's sun.  Exposed because the cascade split and the visible
    // shadow length are both much easier to judge at a low sun angle.
    glm::vec3 lightDirection = glm::vec3(20.0f, 50.0f, 20.0f);
};

Options parseOptions(int argc, char **argv)
{
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--dump") {
            options.dump = true;
        } else if (arg == "--dump-after" && i + 1 < argc) {
            options.dumpAfter = static_cast<uint32_t>(std::stoul(argv[++i]));
        } else if (arg == "--debug-cascade") {
            options.debugView = 1;
        } else if (arg == "--debug-shadow") {
            options.debugView = 2;
        } else if (arg == "--debug-depth") {
            options.debugView = 3;
        } else if (arg == "--debug-map") {
            options.debugView = 4;
        } else if (arg == "--debug-diff") {
            options.debugView = 5;
        } else if (arg == "--no-shadow") {
            options.noShadow = true;
        } else if (arg == "--shadow-res" && i + 1 < argc) {
            options.resolution = static_cast<uint32_t>(std::stoul(argv[++i]));
        } else if (arg == "--far" && i + 1 < argc) {
            options.farPlane = std::stof(argv[++i]);
        } else if (arg == "--light" && i + 3 < argc) {
            options.lightDirection = glm::vec3(std::stof(argv[i + 1]), std::stof(argv[i + 2]),
                                               std::stof(argv[i + 3]));
            i += 3;
        } else {
            throw std::runtime_error("csm: unknown argument " + arg);
        }
    }
    if (options.resolution == 0) {
        options.resolution = 1;
    }
    // The split law divides by 50, so a far plane below 50 would put the first
    // three split distances inside the near plane.
    if (!(options.farPlane > 60.0f)) {
        options.farPlane = 60.0f;
    }
    return options;
}

// ---------------------------------------------------------------------------
// Cascade math
// ---------------------------------------------------------------------------

// Eight world-space corners of the camera frustum slice [nearPlane, farPlane].
// The projection is rebuilt from (fov, aspect, near, far) rather than taken from
// the renderer because the slice's near/far are *not* the camera's: each cascade
// reconstructs a sub-frustum.
std::array<glm::vec4, 8> frustumCornersWorldSpace(const glm::mat4 &view, float fovDegrees, float aspect,
                                                  float nearPlane, float farPlane)
{
    // The renderer's own projection carries a Y flip for Vulkan; the corner set
    // is symmetric in y, so an unflipped perspective yields the same eight
    // points and keeps the arithmetic readable.
    const glm::mat4 inverse = glm::inverse(glm::perspective(glm::radians(fovDegrees), aspect, nearPlane, farPlane)
                                           * view);
    std::array<glm::vec4, 8> corners{};
    size_t index = 0;
    for (uint32_t x = 0; x < 2; ++x) {
        for (uint32_t y = 0; y < 2; ++y) {
            for (uint32_t z = 0; z < 2; ++z) {
                const glm::vec4 point =
                    inverse * glm::vec4(2.0f * static_cast<float>(x) - 1.0f, 2.0f * static_cast<float>(y) - 1.0f,
                                        2.0f * static_cast<float>(z) - 1.0f, 1.0f);
                corners[index++] = point / point.w;
            }
        }
    }
    return corners;
}

// Orthographic light frustum that tightly bounds the given slice, expressed in
// Vulkan's clip convention: x/y in [-1,1], z in [0,1].
glm::mat4 lightSpaceMatrix(const std::array<glm::vec4, 8> &corners, const glm::vec3 &lightDirection)
{
    glm::vec3 center(0.0f);
    for (const glm::vec4 &corner : corners) {
        center += glm::vec3(corner);
    }
    center /= static_cast<float>(corners.size());

    // Standing the light "behind" the slice and looking at its centre makes the
    // light-space bounding box independent of the light's distance.
    const glm::mat4 lightView = glm::lookAt(center + lightDirection, center, glm::vec3(0.0f, 1.0f, 0.0f));

    glm::vec3 boxMin(std::numeric_limits<float>::max());
    glm::vec3 boxMax(std::numeric_limits<float>::lowest());
    for (const glm::vec4 &corner : corners) {
        const glm::vec3 projected = glm::vec3(lightView * corner);
        boxMin = glm::min(boxMin, projected);
        boxMax = glm::max(boxMax, projected);
    }

    // Widen the depth range tenfold.  Geometry that is outside the slice but
    // between the light and the slice (a tall box in front of the cascade) still
    // has to be *in* the depth map, otherwise it casts no shadow into this
    // cascade and the shadow pops as the camera moves.
    constexpr float kZMult = 10.0f;
    float minZ = boxMin.z < 0.0f ? boxMin.z * kZMult : boxMin.z / kZMult;
    float maxZ = boxMax.z < 0.0f ? boxMax.z / kZMult : boxMax.z * kZMult;
    if (maxZ - minZ < 1e-4f) {
        maxZ = minZ + 1e-4f;
    }

    glm::mat4 projection(1.0f);
    projection[0][0] = 2.0f / (boxMax.x - boxMin.x);
    projection[1][1] = 2.0f / (boxMax.y - boxMin.y);
    // Vulkan depth range [0,1], with 0 at the plane *closest to the light*.
    // Light-space z grows towards the light (the view looks down -z), so the
    // depth has to fall as z rises: depth = (maxZ - z) / (maxZ - minZ).  Get the
    // sign wrong and the ordinary LESS depth test keeps the farthest surface in
    // every pixel -- the shadow map then holds nothing but the ground and the
    // scene renders unshadowed, with no validation error to point at it.
    projection[2][2] = -1.0f / (maxZ - minZ);
    projection[3][0] = -(boxMax.x + boxMin.x) / (boxMax.x - boxMin.x);
    projection[3][1] = -(boxMax.y + boxMin.y) / (boxMax.y - boxMin.y);
    projection[3][2] = maxZ / (maxZ - minZ);
    // Vulkan NDC is y-down.  Flipping here lets the fragment shader use the
    // plain GL-style v = y * 0.5 + 0.5 without a second mirror.  The translation
    // has to be flipped with the scale: negating only [1][1] mirrors about y=0
    // instead of about the AABB centre, which is exact only when the light-space
    // box happens to be symmetric around the origin.
    projection[1][1] *= -1.0f;
    projection[3][1] *= -1.0f;

    return projection * lightView;
}

struct CascadeSet {
    std::array<glm::mat4, kMaxCascades> lightSpaceMatrices{};
    std::array<float, kMaxCascades - 1> splits{};
};

CascadeSet buildCascades(const glm::mat4 &view, float fovDegrees, float aspect, float farPlane,
                         const glm::vec3 &lightDirection)
{
    CascadeSet result;
    // The reference's split law.  Deliberately not a practical/logarithmic
    // blend: those spread the cascades more evenly and would hide the point of
    // this example, which is to show that the near field gets the density.
    for (uint32_t i = 0; i + 1 < kMaxCascades; ++i) {
        const float divisor = (i == 0) ? 50.0f : (i == 1) ? 25.0f : (i == 2) ? 10.0f : 2.0f;
        result.splits[i] = farPlane / divisor;
    }
    for (uint32_t i = 0; i < kMaxCascades; ++i) {
        const float sliceNear = (i == 0) ? kNearPlane : result.splits[i - 1];
        const float sliceFar = (i + 1 == kMaxCascades) ? farPlane : result.splits[i];
        result.lightSpaceMatrices[i] =
            lightSpaceMatrix(frustumCornersWorldSpace(view, fovDegrees, aspect, sliceNear, sliceFar), lightDirection);
    }
    return result;
}

// ---------------------------------------------------------------------------
// Scene
// ---------------------------------------------------------------------------

// Unit cube centred on the origin, 36 vertices, counter-clockwise seen from
// outside (which is what the Y-flipped projection + CullMode::eBack expect).
std::vector<Vertex> makeCubeVertices()
{
    std::vector<Vertex> vertices;
    vertices.reserve(36);

    const std::array<glm::vec3, 6> normals = {glm::vec3(1.0f, 0.0f, 0.0f),  glm::vec3(-1.0f, 0.0f, 0.0f),
                                              glm::vec3(0.0f, 1.0f, 0.0f),  glm::vec3(0.0f, -1.0f, 0.0f),
                                              glm::vec3(0.0f, 0.0f, 1.0f),  glm::vec3(0.0f, 0.0f, -1.0f)};

    for (const glm::vec3 &normal : normals) {
        // Any up vector works as long as it is not parallel to the normal; the
        // cross products then give an orthonormal (right, up) pair whose
        // right x up == normal, which is what makes the winding come out CCW.
        const glm::vec3 up = std::abs(normal.y) > 0.9f ? glm::vec3(0.0f, 0.0f, 1.0f) : glm::vec3(0.0f, 1.0f, 0.0f);
        const glm::vec3 right = glm::normalize(glm::cross(up, normal));
        const glm::vec3 up2 = glm::cross(normal, right);

        const std::array<glm::vec2, 4> uvs = {glm::vec2(0.0f, 0.0f), glm::vec2(1.0f, 0.0f), glm::vec2(1.0f, 1.0f),
                                              glm::vec2(0.0f, 1.0f)};
        const std::array<glm::vec3, 4> corners = {(-right - up2) * 0.5f, (right - up2) * 0.5f,
                                                  (right + up2) * 0.5f, (-right + up2) * 0.5f};
        const std::array<uint32_t, 6> order = {0, 1, 2, 0, 2, 3};
        for (uint32_t i : order) {
            Vertex vertex;
            vertex.position = corners[i] + normal * 0.5f;
            vertex.normal = normal;
            vertex.color = glm::vec3(1.0f);
            vertex.texCoord = uvs[i];
            vertex.tangent = glm::vec3(0.0f);
            vertices.push_back(vertex);
        }
    }
    return vertices;
}

// Instance 0 is the ground slab (a very flat box: one mesh means one draw);
// the rest are boxes standing on it, spread from just in front of the camera out
// to the far cascade.
std::vector<Instance> makeScene()
{
    std::vector<Instance> instances;
    {
        glm::mat4 ground = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, kFloorY - 0.5f, 0.0f));
        ground = glm::scale(ground, glm::vec3(140.0f, 1.0f, 140.0f));
        // a = 1 marks the procedural checker.
        instances.push_back({ground, glm::vec4(0.62f, 0.60f, 0.57f, 1.0f)});
    }

    struct Box {
        float x;
        float z;
        float size;
        float yawDegrees;
        glm::vec3 color;
    };
    // Hand-placed rather than random: the dump is a regression artifact, and a
    // random layout would make every run differ.  The z coordinates sweep
    // 16 -> -44 so all five cascades contain geometry.
    static const Box boxes[] = {
        {-1.5f, 16.0f, 2.2f, 15.0f, glm::vec3(0.85f, 0.55f, 0.35f)},
        {2.8f, 11.0f, 1.8f, 55.0f, glm::vec3(0.40f, 0.55f, 0.75f)},
        {-4.2f, 6.0f, 2.8f, 30.0f, glm::vec3(0.55f, 0.75f, 0.45f)},
        {5.4f, 2.0f, 2.4f, 70.0f, glm::vec3(0.80f, 0.75f, 0.40f)},
        {-6.8f, -3.0f, 3.2f, 10.0f, glm::vec3(0.70f, 0.45f, 0.60f)},
        {1.2f, -8.0f, 2.0f, 45.0f, glm::vec3(0.45f, 0.70f, 0.70f)},
        {8.4f, -13.0f, 3.6f, 25.0f, glm::vec3(0.75f, 0.60f, 0.40f)},
        {-9.6f, -17.0f, 2.6f, 60.0f, glm::vec3(0.50f, 0.50f, 0.80f)},
        {-2.4f, -23.0f, 4.4f, 5.0f, glm::vec3(0.70f, 0.70f, 0.70f)},
        {12.0f, -30.0f, 3.0f, 40.0f, glm::vec3(0.60f, 0.75f, 0.50f)},
        {-14.0f, -36.0f, 4.0f, 20.0f, glm::vec3(0.85f, 0.50f, 0.50f)},
        {4.0f, -44.0f, 5.2f, 35.0f, glm::vec3(0.55f, 0.60f, 0.72f)},
    };
    for (const Box &box : boxes) {
        glm::mat4 model = glm::translate(glm::mat4(1.0f), glm::vec3(box.x, kFloorY + box.size * 0.5f, box.z));
        model = glm::rotate(model, glm::radians(box.yawDegrees), glm::vec3(0.0f, 1.0f, 0.0f));
        model = glm::scale(model, glm::vec3(box.size));
        instances.push_back({model, glm::vec4(box.color, 0.0f)});
    }
    return instances;
}

// ---------------------------------------------------------------------------
// Render passes
// ---------------------------------------------------------------------------

// Depth-only pass whose attachment is a 2D array of `layers` slices.  It leaves
// the image in DEPTH_STENCIL_READ_ONLY_OPTIMAL, which is both the layout the
// lighting pass samples and the layout the next frame's beginRenderPass expects
// to find (initialLayout is UNDEFINED, so the contents are discarded either way).
// DEPTH_READ_ONLY_OPTIMAL would express the same thing for a depth-only format,
// but it needs the optional separateDepthStencilLayouts feature; the combined
// read-only layout is always legal.
class ShadowMapAttachmentDetail final : public IAttachmentDetail {
public:
    ShadowMapAttachmentDetail(vk::Format depthFormat, uint32_t resolution, uint32_t layers) :
        m_depthFormat(depthFormat), m_resolution(resolution), m_layers(layers)
    {
    }

    bool isSwapChainSized() const override { return false; }

    RenderPassBlueprint build(uint32_t, uint32_t, vk::SampleCountFlagBits) const override
    {
        RenderPassBlueprint blueprint;
        blueprint.swapChainSized = false;
        // No colour attachment at all: this is the case that needed
        // GraphicsPipeline's "0 blend attachments" path.
        blueprint.colorAttachmentCount = 0;
        blueprint.sampleCount = vk::SampleCountFlagBits::e1;
        // Layer count of the framebuffers.  gl_Layer is only legal when this is
        // greater than 1, and it has to equal OffscreenTargetSpec::layers.
        blueprint.layers = m_layers;

        vk::AttachmentDescription depth{};
        depth.format = m_depthFormat;
        depth.samples = vk::SampleCountFlagBits::e1;
        depth.loadOp = vk::AttachmentLoadOp::eClear;
        depth.storeOp = vk::AttachmentStoreOp::eStore; // sampled right after the pass
        depth.stencilLoadOp = vk::AttachmentLoadOp::eDontCare;
        depth.stencilStoreOp = vk::AttachmentStoreOp::eDontCare;
        depth.initialLayout = vk::ImageLayout::eUndefined;
        depth.finalLayout = vk::ImageLayout::eDepthStencilReadOnlyOptimal;
        blueprint.attachments.push_back(depth);

        OffscreenTargetSpec target;
        target.attachmentIndex = 0;
        target.format = m_depthFormat;
        // The lighting pass samples it, so the image needs SAMPLED usage on top
        // of DEPTH_STENCIL_ATTACHMENT.
        target.extraUsage = vk::ImageUsageFlagBits::eSampled;
        target.fixedSize = true;
        target.width = m_resolution;
        target.height = m_resolution;
        target.layers = m_layers;
        blueprint.offscreenTargets.push_back(target);

        SubpassBlueprint subpass;
        subpass.depthStencil = vk::AttachmentReference(0, vk::ImageLayout::eDepthStencilAttachmentOptimal);
        blueprint.subpasses.push_back(subpass);

        const auto depthWrite = vk::AccessFlagBits::eDepthStencilAttachmentWrite;
        // Previous frame's writes to this frame slot's image -> this frame's
        // clear.  Each in-flight frame owns a separate array, but the *same*
        // array is written again two frames later.
        blueprint.dependencies.push_back(vk::SubpassDependency(
            VK_SUBPASS_EXTERNAL, 0, vk::PipelineStageFlagBits::eLateFragmentTests,
            vk::PipelineStageFlagBits::eEarlyFragmentTests | vk::PipelineStageFlagBits::eLateFragmentTests, depthWrite,
            depthWrite | vk::AccessFlagBits::eDepthStencilAttachmentRead, vk::DependencyFlagBits::eByRegion));
        // Release the depth writes to the lighting pass' fragment shader.
        // BOTTOM_OF_PIPE does not support SHADER_READ, so the consumer stage is
        // named explicitly (same reasoning as the offscreen preset).
        blueprint.dependencies.push_back(vk::SubpassDependency(
            0, VK_SUBPASS_EXTERNAL, vk::PipelineStageFlagBits::eLateFragmentTests,
            vk::PipelineStageFlagBits::eFragmentShader, depthWrite, vk::AccessFlagBits::eShaderRead,
            vk::DependencyFlagBits::eByRegion));

        blueprint.clearValues.push_back(vk::ClearValue(vk::ClearDepthStencilValue(1.0f, 0)));
        return blueprint;
    }

private:
    vk::Format m_depthFormat;
    uint32_t m_resolution = 1;
    uint32_t m_layers = 1;
};

// The lighting pass samples a depth array another render pass wrote.  The
// producer releases its writes (see above); this is the acquire half.  The
// engine's swapchain preset does not provide one, which is fine for sampling an
// offscreen *colour* target (the layout transition that accompanies it acts as
// the acquire) but leaves a depth array written by a foreign pass unordered.
class ShadowSamplingMainPassDetail final : public IAttachmentDetail {
public:
    explicit ShadowSamplingMainPassDetail(std::shared_ptr<const IAttachmentDetail> inner) : m_inner(std::move(inner))
    {
    }

    bool isSwapChainSized() const override { return m_inner->isSwapChainSized(); }

    RenderPassBlueprint build(uint32_t width, uint32_t height, vk::SampleCountFlagBits msaa) const override
    {
        RenderPassBlueprint blueprint = m_inner->build(width, height, msaa);
        blueprint.dependencies.push_back(vk::SubpassDependency(
            VK_SUBPASS_EXTERNAL, 0, vk::PipelineStageFlagBits::eLateFragmentTests,
            vk::PipelineStageFlagBits::eFragmentShader, vk::AccessFlagBits::eDepthStencilAttachmentWrite,
            vk::AccessFlagBits::eShaderRead, vk::DependencyFlagBits::eByRegion));
        return blueprint;
    }

private:
    std::shared_ptr<const IAttachmentDetail> m_inner;
};

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// NEAREST + CLAMP_TO_BORDER with a white border, exactly like the reference: the
// filter must not blur depth (the 9-tap PCF provides the smoothing), and the
// border has to read as 1.0 so fragments outside a cascade come out *lit*
// instead of black.
std::shared_ptr<vk::Sampler> makeShadowSampler(const std::shared_ptr<vk::Device> &device)
{
    vk::SamplerCreateInfo info{};
    info.magFilter = vk::Filter::eNearest;
    info.minFilter = vk::Filter::eNearest;
    info.mipmapMode = vk::SamplerMipmapMode::eNearest;
    info.addressModeU = vk::SamplerAddressMode::eClampToBorder;
    info.addressModeV = vk::SamplerAddressMode::eClampToBorder;
    info.addressModeW = vk::SamplerAddressMode::eClampToBorder;
    info.borderColor = vk::BorderColor::eFloatOpaqueWhite;
    info.minLod = 0.0f;
    info.maxLod = 0.0f;

    vk::Sampler sampler{};
    if (device->createSampler(&info, nullptr, &sampler) != vk::Result::eSuccess) {
        throw std::runtime_error("csm: failed to create the shadow sampler");
    }
    // The deleter keeps the device alive; TextureSamplerCombine does not own the
    // sampler, so this shared_ptr has to outlive every resource using it.
    return std::shared_ptr<vk::Sampler>(new vk::Sampler(sampler), [device](vk::Sampler *pointer) {
        if (pointer) {
            device->destroySampler(*pointer);
        }
        delete pointer;
    });
}

struct LayerStats {
    float covered = 0.0f; // fraction of texels with depth < 1 (i.e. geometry)
    float meanDepth = 0.0f;
};

// Read the statistics back out of the PPM bytes the dump already produced: a
// cleared depth texel is exactly 1.0 (255) because the pass clears to 1.0.
LayerStats analyseLayer(const std::vector<uint8_t> &rgb)
{
    LayerStats stats;
    if (rgb.empty()) {
        return stats;
    }
    const size_t texels = rgb.size() / 3;
    double sum = 0.0;
    size_t covered = 0;
    for (size_t i = 0; i + 2 < rgb.size(); i += 3) {
        const uint8_t value = rgb[i];
        if (value < 255) {
            ++covered;
            sum += static_cast<double>(value) / 255.0;
        }
    }
    stats.covered = static_cast<float>(static_cast<double>(covered) / static_cast<double>(texels));
    stats.meanDepth = covered == 0 ? 1.0f : static_cast<float>(sum / static_cast<double>(covered));
    return stats;
}

} // namespace

void test(int argc, char **argv)
{
    const Options options = parseOptions(argc, argv);

    auto camera = std::make_shared<Camera>(glm::vec3(0.0f, 2.4f, 22.0f), glm::vec3(0.0f, 1.0f, 0.0f), -90.0f, -9.0f);
    camera->MovementSpeed = 8.0f;
    GLFWwindow window(1280, 720, camera);

    // The geometry stage is not enabled by default; requesting it makes the
    // manager skip devices that cannot run this example at all.
    DeviceRequirement requirement;
    requirement.geometryShader = true;
    auto mgr = RenderManager::Create(window, MAX_FRAMES_IN_FLIGHT, requirement);

    const vk::Format depthFormat = mgr->DefaultDepthFormat();
    const glm::vec3 lightDirection = glm::normalize(options.lightDirection);
    const std::vector<Vertex> vertices = makeCubeVertices();
    const std::vector<Instance> instances = makeScene();
    const uint32_t vertexCount = static_cast<uint32_t>(vertices.size());
    const uint32_t instanceCount = static_cast<uint32_t>(instances.size());

    std::cout << "[csm] " << kMaxCascades << " cascades of " << options.resolution << "x" << options.resolution
              << " (" << (static_cast<uint64_t>(options.resolution) * options.resolution * 4 * kMaxCascades
                          * MAX_FRAMES_IN_FLIGHT)
              << " bytes of depth across " << MAX_FRAMES_IN_FLIGHT << " in-flight frames)\n";
    std::cout << "[csm] scene: 1 ground slab + " << (instanceCount - 1) << " boxes, one instanced draw per pass\n";
    std::cout << "[csm] light direction " << lightDirection.x << " " << lightDirection.y << " " << lightDirection.z
              << ", far plane " << options.farPlane << "\n";

    // ----- shadow pass -----------------------------------------------------
    auto shadowDetail =
        std::make_shared<ShadowMapAttachmentDetail>(depthFormat, options.resolution, kMaxCascades);
    auto shadowPass = mgr->CreateRenderPass(shadowDetail);
    // A fixed-size offscreen pass is not rebuilt by the manager, so the caller
    // creates its framebuffers (no swapchain views involved).
    shadowPass->createFramebuffers({}, shadowPass->extent());
    std::cout << "[csm] shadow pass: " << shadowPass->framebufferCount() << " framebuffer set(s), "
              << shadowPass->depthStencilLayers() << " layer(s) each, extent " << shadowPass->extent().width << "x"
              << shadowPass->extent().height << "\n";
    // depth.geom hard-codes `invocations = 5` and every shader block declares
    // five matrices.  A layer count that silently drifts from that literal
    // produces a shadow map whose upper layers are never written, with no
    // validation error to point at it.
    if (shadowPass->depthStencilLayers() != kMaxCascades) {
        throw std::runtime_error("csm: shadow pass layer count does not match depth.geom invocations");
    }
    // The dump path decodes depth texels as 32-bit floats.  Other depth formats
    // are fine for rendering, but capturing them needs a different texel layout.
    if (options.dump && depthFormat != vk::Format::eD32Sfloat) {
        throw std::runtime_error("csm: --dump needs a D32Sfloat depth format (got " + vk::to_string(depthFormat) +
                                 ")");
    }

    // ----- main pass -------------------------------------------------------
    auto mainDetail = std::make_shared<ShadowSamplingMainPassDetail>(std::make_shared<SimpleAttachmentDetail>(
        SimpleAttachmentDetail::Mode::eDirect, *mgr->getSwapChain()->getImageFormat(), depthFormat));
    auto mainPass = mgr->CreateRenderPass(mainDetail);

    // ----- descriptor layouts ---------------------------------------------
    // The shadow pass never samples anything, so it gets a 2-set layout: the
    // engine would otherwise allocate a placeholder texture for set2.
    auto shadowLayout = std::make_shared<DescriptorLayoutAtlas>();
    auto shadowCsmSet = DescriptorLayoutAtlas::makeSet<CsmComponent, CsmComponentTable>(
        0, {CsmComponent::eCsmBlock}, UpdateFrequency::eEachFrame, false);
    shadowCsmSet.specs[0] = GBufferSpec{1, static_cast<uint32_t>(sizeof(CsmUBO)), DataTypes::eCustom};
    auto shadowInstanceSet = DescriptorLayoutAtlas::makeSet<CsmInstanceComponent, CsmInstanceComponentTable>(
        1, {CsmInstanceComponent::eInstances}, UpdateFrequency::eEachInstance, false);
    shadowInstanceSet.specs[0] = GBufferSpec{instanceCount, static_cast<uint32_t>(sizeof(Instance)),
                                            DataTypes::eCustom};
    shadowLayout->setDetails({shadowCsmSet, shadowInstanceSet});

    auto mainLayout = std::make_shared<DescriptorLayoutAtlas>();
    auto mainCsmSet = DescriptorLayoutAtlas::makeSet<CsmComponent, CsmComponentTable>(
        0, {CsmComponent::eCsmBlock}, UpdateFrequency::eEachFrame, false);
    mainCsmSet.specs[0] = GBufferSpec{1, static_cast<uint32_t>(sizeof(CsmUBO)), DataTypes::eCustom};
    auto mainInstanceSet = DescriptorLayoutAtlas::makeSet<CsmInstanceComponent, CsmInstanceComponentTable>(
        1, {CsmInstanceComponent::eInstances}, UpdateFrequency::eEachInstance, false);
    mainInstanceSet.specs[0] = GBufferSpec{instanceCount, static_cast<uint32_t>(sizeof(Instance)),
                                           DataTypes::eCustom};
    // No TextureImageSpec for set2: the engine creates nothing for a
    // eCombinedImageSampler binding whose spec stayed a GBufferSpec, and the
    // shadow array is supplied by hand below.
    auto mainShadowSet = DescriptorLayoutAtlas::makeSet<ShadowComponent, ShadowComponentTable>(
        2, {ShadowComponent::eShadowMap}, UpdateFrequency::eEachFrame, false);
    mainLayout->setDetails({mainCsmSet, mainInstanceSet, mainShadowSet});

    // ----- assets ----------------------------------------------------------
    // Two assets, not one: the engine's input (vertex/index) buffers belong to
    // the asset, and one asset shared by two pipelines would have its vertex
    // buffer replaced by the second CreatePipeline call.  The geometry is
    // therefore uploaded twice -- a few kilobytes for this scene.
    auto shadowAsset = mgr->CreateShaderAssetGroup(shadowLayout);
    shadowAsset->addShaderSpirvCode("vert", std::vector<uint32_t>(shader::DEPTH_VERT_SPV.begin(),
                                                                  shader::DEPTH_VERT_SPV.end()));
    shadowAsset->addShaderSpirvCode("geom", std::vector<uint32_t>(shader::DEPTH_GEOM_SPV.begin(),
                                                                  shader::DEPTH_GEOM_SPV.end()));
    shadowAsset->addShaderSpirvCode("frag", std::vector<uint32_t>(shader::DEPTH_FRAG_SPV.begin(),
                                                                  shader::DEPTH_FRAG_SPV.end()));

    auto mainAsset = mgr->CreateShaderAssetGroup(mainLayout);
    mainAsset->addShaderSpirvCode("vert", std::vector<uint32_t>(shader::CSM_VERT_SPV.begin(),
                                                                shader::CSM_VERT_SPV.end()));
    mainAsset->addShaderSpirvCode("frag", std::vector<uint32_t>(shader::CSM_FRAG_SPV.begin(),
                                                                shader::CSM_FRAG_SPV.end()));

    // ----- shadow array -> lighting pass descriptor ------------------------
    // Must happen before CreatePipeline: descriptor sets are written once, when
    // the pipeline is built, so a resource added later is never seen by
    // vkUpdateDescriptorSets.
    auto shadowSampler = makeShadowSampler(mgr->getLogicalDevice());
    std::vector<std::shared_ptr<TextureSamplerCombine>> shadowResources(MAX_FRAMES_IN_FLIGHT);
    for (uint32_t frame = 0; frame < MAX_FRAMES_IN_FLIGHT; ++frame) {
        // One depth array per in-flight frame, so the view has to follow the
        // frame slot or two frames would sample each other's shadow map.
        const uint32_t slot = frame % shadowPass->framebufferCount();
        const vk::ImageView view = shadowPass->depthStencilView(slot);
        if (!view) {
            throw std::runtime_error("csm: shadow pass exposes no depth view");
        }
        auto combine = std::shared_ptr<TextureSamplerCombine>(
            new TextureSamplerCombine(view, *shadowSampler, vk::ImageLayout::eDepthStencilReadOnlyOptimal),
            [shadowSampler](TextureSamplerCombine *pointer) { delete pointer; });
        mainAsset->addDescriptorResource(ResourceKey::make_key(ResourceId(2, 0), frame), combine);
        shadowResources[frame] = std::move(combine);
    }

    // ----- pipelines -------------------------------------------------------
    auto shadowVertexInput = std::make_shared<VertexInputAtlas>(
        0, std::vector<VertexComponent>{VertexComponent::ePosition}, vertexCount, 0u, DataTypes::eUnsignedInt,
        false, 0u, instanceCount);
    GraphicsPipelineConfig shadowConfig;
    shadowConfig.addStage("vert", vk::ShaderStageFlagBits::eVertex);
    // One invocation per cascade, declared in depth.geom as `invocations = 5`:
    // this is the single draw that fills all five array layers.
    shadowConfig.addGeometryStage("geom");
    shadowConfig.addStage("frag", vk::ShaderStageFlagBits::eFragment);
    // The reference culls front faces in the depth pass, which removes acne on
    // thin geometry but pushes the stored depth away from the light: with a
    // closed box the contact shadow detaches from the object and the shadow
    // visibly floats.  Culling nothing stores the nearest surface, so the
    // contact is correct and the 9-tap PCF plus the slope-scaled bias covers the
    // acne instead.
    shadowConfig.cullMode = vk::CullModeFlagBits::eNone;
    shadowConfig.depthCompareOp = vk::CompareOp::eLess;
    shadowConfig.depthWriteEnable = true;
    auto shadowPipeline = mgr->CreatePipeline(shadowVertexInput, shadowLayout, shadowAsset, shadowPass, 0,
                                              shadowConfig);

    auto mainVertexInput = std::make_shared<VertexInputAtlas>(
        0,
        std::vector<VertexComponent>{VertexComponent::ePosition, VertexComponent::eNormal, VertexComponent::eUV},
        vertexCount, 0u, DataTypes::eUnsignedInt, false, 0u, instanceCount);
    GraphicsPipelineConfig mainConfig;
    mainConfig.cullMode = vk::CullModeFlagBits::eBack;
    mainConfig.depthCompareOp = vk::CompareOp::eLess;
    auto mainPipeline = mgr->CreatePipeline(mainVertexInput, mainLayout, mainAsset, mainPass, 0, mainConfig);

    // ----- uploads ---------------------------------------------------------
    auto upload = mgr->UploadSequence();
    {
        auto sync = std::make_shared<CmdSyncDevice>(shadowAsset);
        sync->OnUpdateRawData(ResourceTypes::eVertex, vertices);
        for (uint32_t frame = 0; frame < MAX_FRAMES_IN_FLIGHT; ++frame) {
            sync->OnUpdateRawData(ResourceId(1, 0), frame, instances);
        }
        upload->submit(sync, shadowAsset);
    }
    {
        auto sync = std::make_shared<CmdSyncDevice>(mainAsset);
        sync->OnUpdateRawData(ResourceTypes::eVertex, vertices);
        for (uint32_t frame = 0; frame < MAX_FRAMES_IN_FLIGHT; ++frame) {
            sync->OnUpdateRawData(ResourceId(1, 0), frame, instances);
        }
        upload->submit(sync, mainAsset);
    }

    // ----- frame loop ------------------------------------------------------
    auto sequence = mgr->DrawFrameSequence();
    auto shadowDraw = std::make_shared<CmdOpDrawFrame>(shadowPipeline);
    auto mainDraw = std::make_shared<CmdOpDrawFrame>(mainPipeline);
    // Both assets own a copy of the cascade block, so both have to be updated;
    // and the per-frame UBO only reaches the device if the sync op is *recorded*
    // each frame (a one-shot upload leaves the device copy at its first value).
    // CmdSyncDevice drives exactly one asset, hence two ops.
    auto shadowFrameSync = std::make_shared<CmdSyncDevice>(shadowAsset);
    auto mainFrameSync = std::make_shared<CmdSyncDevice>(mainAsset);

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
        const float aspect = static_cast<float>(extent.width) / static_cast<float>(extent.height > 0 ? extent.height : 1);

        const glm::mat4 view = camera->GetViewMatrix();
        glm::mat4 projection =
            glm::perspective(glm::radians(camera->Zoom), aspect, kNearPlane, options.farPlane);
        projection[1][1] *= -1.0f; // Vulkan NDC is y-down

        const CascadeSet cascades =
            buildCascades(view, camera->Zoom, aspect, options.farPlane, lightDirection);

        CsmUBO ubo;
        for (uint32_t i = 0; i < kMaxCascades; ++i) {
            ubo.lightSpaceMatrices[i] = cascades.lightSpaceMatrices[i];
        }
        ubo.view = view;
        ubo.viewProj = projection * view;
        ubo.cascadePlaneDistances = glm::vec4(cascades.splits[0], cascades.splits[1], cascades.splits[2],
                                              cascades.splits[3]);
        ubo.lightDir = glm::vec4(lightDirection, options.farPlane);
        ubo.eye = glm::vec4(camera->Position, 1.0f);
        ubo.flags = glm::vec4(static_cast<float>(options.debugView), static_cast<float>(kMaxCascades),
                              options.noShadow ? 0.0f : 1.0f, 1.0f / static_cast<float>(options.resolution));

        shadowDraw->OnUpdateRawData(ResourceId(0, 0), frame, ubo);
        mainDraw->OnUpdateRawData(ResourceId(0, 0), frame, ubo);

        sequence->beginRecording();
        // Device-side copies for this frame's UBO (see the comment on the sync ops).
        sequence->record(shadowFrameSync);
        sequence->record(mainFrameSync);
        sequence->recordPass(shadowPass, [&]() { sequence->record(shadowDraw); });
        sequence->recordPass(mainPass, [&]() { sequence->record(mainDraw); });
        sequence->endRecording();
        sequence->submitFrame();

        ++framesRendered;
        if (options.dump && !dumpDone && framesRendered >= options.dumpAfter) {
            ImageReadback readback(mgr->getLogicalDevice(), mgr->memoryProperties(), mgr->getMemoryAllocator(),
                                   mgr->getGraphicsQueue(), *mgr->getQueueFamilyIndices().graphicsFamily);

            std::vector<DumpRequest> requests;
            DumpRequest frameRequest;
            static const char *kDebugNames[] = {"frame",     "frame-cascade", "frame-shadow",
                                                "frame-depth", "frame-map", "frame-diff"};
            frameRequest.label = kDebugNames[std::min<uint32_t>(options.debugView, 5)];
            frameRequest.image = mgr->getSwapChain()->getImages()[sequence->getImageIndex()];
            frameRequest.format = *mgr->getSwapChain()->getImageFormat();
            frameRequest.extent = extent;
            frameRequest.layout = vk::ImageLayout::ePresentSrcKHR;
            frameRequest.encoding = DumpEncoding::eColor;
            requests.push_back(frameRequest);

            // One PPM per cascade: a PPM is a single 2D slice, so the array is
            // walked layer by layer.  Values are raw orthographic depth in
            // [0,1], which is why eColor (not eLinearDepth) is the right
            // encoding here -- 255 means "cleared, nothing there".
            const uint32_t slot = frame % shadowPass->framebufferCount();
            for (uint32_t layer = 0; layer < kMaxCascades; ++layer) {
                DumpRequest request;
                request.label = "shadow-layer" + std::to_string(layer);
                request.image = shadowPass->depthStencilImage(slot);
                request.format = shadowPass->depthStencilFormat();
                request.extent = vk::Extent2D(options.resolution, options.resolution);
                request.layout = vk::ImageLayout::eDepthStencilReadOnlyOptimal;
                request.encoding = DumpEncoding::eColor;
                request.layer = layer;
                requests.push_back(request);
            }

            const auto results = DumpImages(readback, requests, "dump", 0);
            std::cout << "[dump] " << results[0].path.string() << " mean=(" << results[0].meanColor.r << ","
                      << results[0].meanColor.g << "," << results[0].meanColor.b << ")\n";
            for (uint32_t layer = 0; layer < kMaxCascades; ++layer) {
                const LayerStats stats = analyseLayer(results[layer + 1].rgb8);
                const float sliceNear = (layer == 0) ? kNearPlane : cascades.splits[layer - 1];
                const float sliceFar = (layer + 1 == kMaxCascades) ? options.farPlane : cascades.splits[layer];
                std::cout << "[dump] " << results[layer + 1].path.string() << " layer=" << layer
                          << " slice=[" << sliceNear << "," << sliceFar << "] covered=" << stats.covered
                          << " meanDepth=" << stats.meanDepth << "\n";
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
