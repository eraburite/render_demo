// point_shadow: omnidirectional shadows from a point light, migrated from
// LearnOpenGL/src/5.advanced_lighting/3.2.1.point_shadows and 3.2.2 (soft).
//
// The reference renders the scene six times into a GL cubemap depth attachment,
// once per face, using a geometry shader to broadcast every triangle to all six
// gl_Layer values in a single draw.  Vulkan here has no hardware cubemap depth
// attachment wired into the engine, but a 2D depth ARRAY with six layers is the
// exact equivalents: ImageAttachment makes an e2DArray view when layers > 1,
// and the sampler2DArray is indexed by a face number computed on the CPU/GPU.
//
// What is kept from the reference:
//   * 1024^2 per face, 90-degree perspective, far_plane = 25, near = 1.
//   * depth stores radial distance / far (gl_FragDepth), not NDC depth.
//   * hard mode: single tap, bias 0.05; soft mode: 20-tap fixed grid disk,
//     bias 0.15, diskRadius = (1 + viewDistance / far) / 25.
//   * the six lookAt directions and up vectors, verbatim.
//
// Deliberate differences:
//   * The light is fixed and the scene is a floor + six boxes instead of a
//     time-animated light inside a room.  The reference moves lightPos.z with
//     sin(time), which makes --dump non-reproducible; a fixed light still shows
//     all six faces and the radial shadow falloff.
//   * No debug cubemap-face quad: the six layers are dumped directly to PPM,
//     which is a stronger check than a single projected face.
#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
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

#include "shader/depth_frag.hpp"
#include "shader/depth_geom.hpp"
#include "shader/depth_vert.hpp"
#include "shader/point_shadow_frag.hpp"
#include "shader/point_shadow_vert.hpp"

namespace {

constexpr uint32_t kFaceCount = 6;
constexpr float kNearPlane = 1.0f;
constexpr float kFarPlane = 25.0f;
constexpr float kFloorY = -2.0f;

// std140 mirror of the PointBlock declared in depth.geom / depth.frag /
// point_shadow.vert / point_shadow.frag.  The static_assert is the only thing
// keeping the four copies in sync.
struct PointUBO {
    alignas(16) glm::mat4 faceMatrices[kFaceCount]{};
    alignas(16) glm::vec4 lightPosFar{0.0f}; // xyz = light, w = far plane
    alignas(16) glm::mat4 viewProj{1.0f};
    alignas(16) glm::vec4 eye{0.0f};
    // x = debug view, y = shadow enable, z = soft, w = 1 / resolution.
    alignas(16) glm::vec4 flags{0.0f};
};
static_assert(sizeof(PointUBO) == kFaceCount * 64 + 16 + 64 + 16 + 16, "PointUBO must match the std140 PointBlock");

struct Instance {
    alignas(16) glm::mat4 model{1.0f};
    // rgb = base colour, a > 0.5 selects the procedural checker.
    alignas(16) glm::vec4 material{1.0f};
};
static_assert(sizeof(Instance) == 80, "Instance must be model(64) + material(16)");

enum class PointComponent { eBlock };
using PointConfig = DescriptorComponentSpec<PointComponent>;
inline constexpr auto PointComponentTable = std::make_tuple(PointConfig{
    PointComponent::eBlock, vk::DescriptorType::eUniformBuffer, 1,
    vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eGeometry | vk::ShaderStageFlagBits::eFragment});

enum class PointInstanceComponent { eInstances };
using PointInstanceConfig = DescriptorComponentSpec<PointInstanceComponent>;
inline constexpr auto PointInstanceComponentTable = std::make_tuple(PointInstanceConfig{
    PointInstanceComponent::eInstances, vk::DescriptorType::eStorageBuffer, 1, vk::ShaderStageFlagBits::eVertex});

enum class ShadowComponent { eShadowMap };
using ShadowConfig = DescriptorComponentSpec<ShadowComponent>;
inline constexpr auto ShadowComponentTable = std::make_tuple(ShadowConfig{
    ShadowComponent::eShadowMap, vk::DescriptorType::eCombinedImageSampler, 1, vk::ShaderStageFlagBits::eFragment});

struct Options {
    bool dump = false;
    uint32_t dumpAfter = 2;
    bool soft = false;
    bool noShadow = false;
    uint32_t resolution = 1024;
    // 0 = lit, 1 = shadow term, 2 = radial depth, 3 = map depth, 4 = diff.
    uint32_t debugView = 0;
    glm::vec3 lightPosition{0.0f, 4.0f, 0.0f};
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
        } else if (arg == "--soft") {
            options.soft = true;
        } else if (arg == "--no-shadow") {
            options.noShadow = true;
        } else if (arg == "--shadow-res" && i + 1 < argc) {
            options.resolution = static_cast<uint32_t>(std::stoul(argv[++i]));
        } else if (arg == "--debug-shadow") {
            options.debugView = 1;
        } else if (arg == "--debug-depth") {
            options.debugView = 2;
        } else if (arg == "--debug-map") {
            options.debugView = 3;
        } else if (arg == "--debug-diff") {
            options.debugView = 4;
        } else if (arg == "--light" && i + 3 < argc) {
            options.lightPosition = glm::vec3(std::stof(argv[i + 1]), std::stof(argv[i + 2]), std::stof(argv[i + 3]));
            i += 3;
        } else {
            throw std::runtime_error("point_shadow: unknown argument " + arg);
        }
    }
    if (options.resolution == 0) {
        options.resolution = 1;
    }
    return options;
}

// --- Face matrices ---------------------------------------------------------

// GLM's perspective is right-handed with GL depth [-1,1]; Vulkan needs [0,1].
// Remap z_clip = 0.5*z_clip_gl + 0.5*w_clip, i.e. m[2][2] = 0.5*m[2][2]-0.5
// and m[3][2] *= 0.5.  No y flip: the same matrix rasterizes and reprojects,
// so Vulkan's y-down framebuffer and the sampled image agree (see uvOnFace()).
glm::mat4 vulkanPerspective(float nearPlane, float farPlane)
{
    glm::mat4 p = glm::perspective(glm::radians(90.0f), 1.0f, nearPlane, farPlane);
    p[2][2] = 0.5f * p[2][2] - 0.5f;
    p[3][2] *= 0.5f;
    return p;
}

std::array<glm::mat4, kFaceCount> buildFaceMatrices(const glm::vec3 &lightPos, float farPlane)
{
    // Face order and up vectors from 3.2.1.point_shadows.cpp:154-159.  +Y/-Y
    // use a z up vector to avoid lookAt degeneracy; the others point down.
    static const glm::vec3 dirs[kFaceCount] = {
        glm::vec3( 1.0f, 0.0f, 0.0f), glm::vec3(-1.0f, 0.0f, 0.0f),
        glm::vec3( 0.0f, 1.0f, 0.0f), glm::vec3( 0.0f,-1.0f, 0.0f),
        glm::vec3( 0.0f, 0.0f, 1.0f), glm::vec3( 0.0f, 0.0f,-1.0f)};
    static const glm::vec3 ups[kFaceCount] = {
        glm::vec3(0.0f,-1.0f, 0.0f), glm::vec3(0.0f,-1.0f, 0.0f),
        glm::vec3(0.0f, 0.0f, 1.0f), glm::vec3(0.0f, 0.0f,-1.0f),
        glm::vec3(0.0f,-1.0f, 0.0f), glm::vec3(0.0f,-1.0f, 0.0f)};

    const glm::mat4 projection = vulkanPerspective(kNearPlane, farPlane);
    std::array<glm::mat4, kFaceCount> matrices{};
    for (uint32_t i = 0; i < kFaceCount; ++i) {
        matrices[i] = projection * glm::lookAt(lightPos, lightPos + dirs[i], ups[i]);
    }
    return matrices;
}

// --- Scene -----------------------------------------------------------------

std::vector<Vertex> makeCubeVertices()
{
    std::vector<Vertex> vertices;
    vertices.reserve(36);
    const std::array<glm::vec3, 6> normals = {glm::vec3(1, 0, 0), glm::vec3(-1, 0, 0), glm::vec3(0, 1, 0),
                                              glm::vec3(0, -1, 0), glm::vec3(0, 0, 1), glm::vec3(0, 0, -1)};
    for (const glm::vec3 &normal : normals) {
        const glm::vec3 up = std::abs(normal.y) > 0.9f ? glm::vec3(0, 0, 1) : glm::vec3(0, 1, 0);
        const glm::vec3 right = glm::normalize(glm::cross(up, normal));
        const glm::vec3 up2 = glm::cross(normal, right);
        const std::array<glm::vec2, 4> uvs = {glm::vec2(0, 0), glm::vec2(1, 0), glm::vec2(1, 1), glm::vec2(0, 1)};
        const std::array<glm::vec3, 4> corners = {(-right - up2) * 0.5f, (right - up2) * 0.5f,
                                                  (right + up2) * 0.5f, (-right + up2) * 0.5f};
        for (uint32_t i : {0u, 1u, 2u, 0u, 2u, 3u}) {
            Vertex vertex;
            vertex.position = corners[i] + normal * 0.5f;
            vertex.normal = normal;
            vertex.color = glm::vec3(1.0f);
            vertex.texCoord = uvs[i];
            vertices.push_back(vertex);
        }
    }
    return vertices;
}

std::vector<Instance> makeScene()
{
    std::vector<Instance> instances;
    {
        glm::mat4 ground = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, kFloorY - 0.5f, 0.0f));
        ground = glm::scale(ground, glm::vec3(24.0f, 1.0f, 24.0f));
        instances.push_back({ground, glm::vec4(0.62f, 0.60f, 0.57f, 1.0f)});
    }
    struct Box {
        float x, z, size, yaw;
        glm::vec3 color;
    };
    // Hand-placed around the light so shadows radiate onto the floor.
    static const Box boxes[] = {
        {-3.0f,  0.0f, 1.8f, 15.0f, glm::vec3(0.85f, 0.55f, 0.35f)},
        { 3.0f,  0.0f, 1.6f, 55.0f, glm::vec3(0.40f, 0.55f, 0.75f)},
        { 0.0f, -3.0f, 2.0f, 30.0f, glm::vec3(0.55f, 0.75f, 0.45f)},
        { 0.0f,  3.0f, 1.4f, 70.0f, glm::vec3(0.80f, 0.75f, 0.40f)},
        {-2.2f, -2.2f, 1.2f, 10.0f, glm::vec3(0.70f, 0.45f, 0.60f)},
        { 2.2f,  2.2f, 1.5f, 45.0f, glm::vec3(0.45f, 0.70f, 0.70f)},
    };
    for (const Box &box : boxes) {
        glm::mat4 model = glm::translate(glm::mat4(1.0f), glm::vec3(box.x, kFloorY + box.size * 0.5f, box.z));
        model = glm::rotate(model, glm::radians(box.yaw), glm::vec3(0.0f, 1.0f, 0.0f));
        model = glm::scale(model, glm::vec3(box.size));
        instances.push_back({model, glm::vec4(box.color, 0.0f)});
    }
    return instances;
}

// --- Render passes ---------------------------------------------------------

class PointShadowAttachmentDetail final : public IAttachmentDetail {
public:
    PointShadowAttachmentDetail(vk::Format depthFormat, uint32_t resolution) :
        m_depthFormat(depthFormat), m_resolution(resolution)
    {
    }

    bool isSwapChainSized() const override { return false; }

    RenderPassBlueprint build(uint32_t, uint32_t, vk::SampleCountFlagBits) const override
    {
        RenderPassBlueprint blueprint;
        blueprint.swapChainSized = false;
        blueprint.colorAttachmentCount = 0;
        blueprint.sampleCount = vk::SampleCountFlagBits::e1;
        // Six layers, one per cube face -- gl_Layer needs framebuffer.layers > 1.
        blueprint.layers = kFaceCount;

        vk::AttachmentDescription depth{};
        depth.format = m_depthFormat;
        depth.samples = vk::SampleCountFlagBits::e1;
        depth.loadOp = vk::AttachmentLoadOp::eClear;
        depth.storeOp = vk::AttachmentStoreOp::eStore;
        depth.stencilLoadOp = vk::AttachmentLoadOp::eDontCare;
        depth.stencilStoreOp = vk::AttachmentStoreOp::eDontCare;
        depth.initialLayout = vk::ImageLayout::eUndefined;
        depth.finalLayout = vk::ImageLayout::eDepthStencilReadOnlyOptimal;
        blueprint.attachments.push_back(depth);

        OffscreenTargetSpec target;
        target.attachmentIndex = 0;
        target.format = m_depthFormat;
        target.extraUsage = vk::ImageUsageFlagBits::eSampled;
        target.fixedSize = true;
        target.width = m_resolution;
        target.height = m_resolution;
        target.layers = kFaceCount;
        blueprint.offscreenTargets.push_back(target);

        SubpassBlueprint subpass;
        subpass.depthStencil = vk::AttachmentReference(0, vk::ImageLayout::eDepthStencilAttachmentOptimal);
        blueprint.subpasses.push_back(subpass);

        const auto depthWrite = vk::AccessFlagBits::eDepthStencilAttachmentWrite;
        blueprint.dependencies.push_back(vk::SubpassDependency(
            VK_SUBPASS_EXTERNAL, 0, vk::PipelineStageFlagBits::eLateFragmentTests,
            vk::PipelineStageFlagBits::eEarlyFragmentTests | vk::PipelineStageFlagBits::eLateFragmentTests, depthWrite,
            depthWrite | vk::AccessFlagBits::eDepthStencilAttachmentRead, vk::DependencyFlagBits::eByRegion));
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
};

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
        throw std::runtime_error("point_shadow: failed to create the shadow sampler");
    }
    return std::shared_ptr<vk::Sampler>(new vk::Sampler(sampler), [device](vk::Sampler *pointer) {
        if (pointer) {
            device->destroySampler(*pointer);
        }
        delete pointer;
    });
}

struct LayerStats {
    float covered = 0.0f;
    float meanDepth = 0.0f;
};

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
        if (rgb[i] < 255) {
            ++covered;
            sum += static_cast<double>(rgb[i]) / 255.0;
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

    auto camera = std::make_shared<Camera>(glm::vec3(0.0f, 3.0f, 12.0f), glm::vec3(0.0f, 1.0f, 0.0f), -90.0f,
                                           -10.0f);
    camera->MovementSpeed = 8.0f;
    GLFWwindow window(1280, 720, camera);

    DeviceRequirement requirement;
    requirement.geometryShader = true;
    auto mgr = RenderManager::Create(window, MAX_FRAMES_IN_FLIGHT, requirement);

    const vk::Format depthFormat = mgr->DefaultDepthFormat();
    const std::vector<Vertex> vertices = makeCubeVertices();
    const std::vector<Instance> instances = makeScene();
    const uint32_t vertexCount = static_cast<uint32_t>(vertices.size());
    const uint32_t instanceCount = static_cast<uint32_t>(instances.size());
    const auto faceMatrices = buildFaceMatrices(options.lightPosition, kFarPlane);

    std::cout << "[point_shadow] " << kFaceCount << " faces of " << options.resolution << "x" << options.resolution
              << ", light at (" << options.lightPosition.x << "," << options.lightPosition.y << ","
              << options.lightPosition.z << ")\n";
    std::cout << "[point_shadow] scene: 1 floor + " << (instanceCount - 1) << " boxes, "
              << (options.soft ? "soft (20-tap)" : "hard (single tap)") << "\n";

    // ----- shadow pass -----------------------------------------------------
    auto shadowPass = mgr->CreateRenderPass(std::make_shared<PointShadowAttachmentDetail>(depthFormat, options.resolution));
    shadowPass->createFramebuffers({}, shadowPass->extent());
    if (shadowPass->depthStencilLayers() != kFaceCount) {
        throw std::runtime_error("point_shadow: layer count does not match depth.geom invocations (6)");
    }
    if (options.dump && depthFormat != vk::Format::eD32Sfloat) {
        throw std::runtime_error("point_shadow: --dump needs a D32Sfloat depth format (got " +
                                 vk::to_string(depthFormat) + ")");
    }

    // ----- main pass -------------------------------------------------------
    auto mainDetail = std::make_shared<ShadowSamplingMainPassDetail>(std::make_shared<SimpleAttachmentDetail>(
        SimpleAttachmentDetail::Mode::eDirect, *mgr->getSwapChain()->getImageFormat(), depthFormat));
    auto mainPass = mgr->CreateRenderPass(mainDetail);

    // ----- descriptor layouts ---------------------------------------------
    auto shadowLayout = std::make_shared<DescriptorLayoutAtlas>();
    auto shadowBlockSet = DescriptorLayoutAtlas::makeSet<PointComponent, PointComponentTable>(
        0, {PointComponent::eBlock}, UpdateFrequency::eEachFrame, false);
    shadowBlockSet.specs[0] = GBufferSpec{1, static_cast<uint32_t>(sizeof(PointUBO)), DataTypes::eCustom};
    auto shadowInstanceSet = DescriptorLayoutAtlas::makeSet<PointInstanceComponent, PointInstanceComponentTable>(
        1, {PointInstanceComponent::eInstances}, UpdateFrequency::eEachInstance, false);
    shadowInstanceSet.specs[0] =
        GBufferSpec{instanceCount, static_cast<uint32_t>(sizeof(Instance)), DataTypes::eCustom};
    shadowLayout->setDetails({shadowBlockSet, shadowInstanceSet});

    auto mainLayout = std::make_shared<DescriptorLayoutAtlas>();
    auto mainBlockSet = DescriptorLayoutAtlas::makeSet<PointComponent, PointComponentTable>(
        0, {PointComponent::eBlock}, UpdateFrequency::eEachFrame, false);
    mainBlockSet.specs[0] = GBufferSpec{1, static_cast<uint32_t>(sizeof(PointUBO)), DataTypes::eCustom};
    auto mainInstanceSet = DescriptorLayoutAtlas::makeSet<PointInstanceComponent, PointInstanceComponentTable>(
        1, {PointInstanceComponent::eInstances}, UpdateFrequency::eEachInstance, false);
    mainInstanceSet.specs[0] =
        GBufferSpec{instanceCount, static_cast<uint32_t>(sizeof(Instance)), DataTypes::eCustom};
    // set2 stays a GBufferSpec: no engine texture, the array view is injected.
    auto mainShadowSet = DescriptorLayoutAtlas::makeSet<ShadowComponent, ShadowComponentTable>(
        2, {ShadowComponent::eShadowMap}, UpdateFrequency::eEachFrame, false);
    mainLayout->setDetails({mainBlockSet, mainInstanceSet, mainShadowSet});

    // ----- assets ----------------------------------------------------------
    auto shadowAsset = mgr->CreateShaderAssetGroup(shadowLayout);
    shadowAsset->addShaderSpirvCode("vert", std::vector<uint32_t>(shader::DEPTH_VERT_SPV.begin(),
                                                                  shader::DEPTH_VERT_SPV.end()));
    shadowAsset->addShaderSpirvCode("geom", std::vector<uint32_t>(shader::DEPTH_GEOM_SPV.begin(),
                                                                  shader::DEPTH_GEOM_SPV.end()));
    shadowAsset->addShaderSpirvCode("frag", std::vector<uint32_t>(shader::DEPTH_FRAG_SPV.begin(),
                                                                  shader::DEPTH_FRAG_SPV.end()));
    auto mainAsset = mgr->CreateShaderAssetGroup(mainLayout);
    mainAsset->addShaderSpirvCode("vert", std::vector<uint32_t>(shader::POINT_SHADOW_VERT_SPV.begin(),
                                                                shader::POINT_SHADOW_VERT_SPV.end()));
    mainAsset->addShaderSpirvCode("frag", std::vector<uint32_t>(shader::POINT_SHADOW_FRAG_SPV.begin(),
                                                                shader::POINT_SHADOW_FRAG_SPV.end()));

    // Must precede CreatePipeline: descriptor sets are written once at build.
    auto shadowSampler = makeShadowSampler(mgr->getLogicalDevice());
    std::vector<std::shared_ptr<TextureSamplerCombine>> shadowResources(MAX_FRAMES_IN_FLIGHT);
    for (uint32_t frame = 0; frame < MAX_FRAMES_IN_FLIGHT; ++frame) {
        const uint32_t slot = frame % shadowPass->framebufferCount();
        const vk::ImageView view = shadowPass->depthStencilView(slot);
        if (!view) {
            throw std::runtime_error("point_shadow: shadow pass exposes no depth view");
        }
        auto combine = std::shared_ptr<TextureSamplerCombine>(
            new TextureSamplerCombine(view, *shadowSampler, vk::ImageLayout::eDepthStencilReadOnlyOptimal),
            [shadowSampler](TextureSamplerCombine *pointer) { delete pointer; });
        mainAsset->addDescriptorResource(ResourceKey::make_key(ResourceId(2, 0), frame), combine);
        shadowResources[frame] = std::move(combine);
    }

    // ----- pipelines -------------------------------------------------------
    auto shadowVertexInput = std::make_shared<VertexInputAtlas>(
        0, std::vector<VertexComponent>{VertexComponent::ePosition}, vertexCount, 0u, DataTypes::eUnsignedInt, false,
        0u, instanceCount);
    GraphicsPipelineConfig shadowConfig;
    shadowConfig.addStage("vert", vk::ShaderStageFlagBits::eVertex);
    shadowConfig.addGeometryStage("geom");
    shadowConfig.addStage("frag", vk::ShaderStageFlagBits::eFragment);
    shadowConfig.cullMode = vk::CullModeFlagBits::eNone;
    shadowConfig.depthCompareOp = vk::CompareOp::eLess;
    shadowConfig.depthWriteEnable = true;
    auto shadowPipeline =
        mgr->CreatePipeline(shadowVertexInput, shadowLayout, shadowAsset, shadowPass, 0, shadowConfig);

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
        const float aspect =
            static_cast<float>(extent.width) / static_cast<float>(extent.height > 0 ? extent.height : 1);

        const glm::mat4 view = camera->GetViewMatrix();
        glm::mat4 projection = glm::perspective(glm::radians(camera->Zoom), aspect, 0.1f, 100.0f);
        projection[1][1] *= -1.0f;

        PointUBO ubo;
        for (uint32_t i = 0; i < kFaceCount; ++i) {
            ubo.faceMatrices[i] = faceMatrices[i];
        }
        ubo.lightPosFar = glm::vec4(options.lightPosition, kFarPlane);
        ubo.viewProj = projection * view;
        ubo.eye = glm::vec4(camera->Position, 1.0f);
        ubo.flags = glm::vec4(static_cast<float>(options.debugView), options.noShadow ? 0.0f : 1.0f,
                              options.soft ? 1.0f : 0.0f, 1.0f / static_cast<float>(options.resolution));

        shadowDraw->OnUpdateRawData(ResourceId(0, 0), frame, ubo);
        mainDraw->OnUpdateRawData(ResourceId(0, 0), frame, ubo);

        sequence->beginRecording();
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

            static const char *kDebugNames[] = {"frame", "frame-shadow", "frame-depth", "frame-map", "frame-diff"};
            DumpRequest frameRequest;
            frameRequest.label = kDebugNames[std::min<uint32_t>(options.debugView, 4)];
            frameRequest.image = mgr->getSwapChain()->getImages()[sequence->getImageIndex()];
            frameRequest.format = *mgr->getSwapChain()->getImageFormat();
            frameRequest.extent = extent;
            frameRequest.layout = vk::ImageLayout::ePresentSrcKHR;
            frameRequest.encoding = DumpEncoding::eColor;

            std::vector<DumpRequest> requests;
            requests.push_back(frameRequest);
            const uint32_t slot = frame % shadowPass->framebufferCount();
            for (uint32_t face = 0; face < kFaceCount; ++face) {
                DumpRequest request;
                request.label = "shadow-face" + std::to_string(face);
                request.image = shadowPass->depthStencilImage(slot);
                request.format = shadowPass->depthStencilFormat();
                request.extent = vk::Extent2D(options.resolution, options.resolution);
                request.layout = vk::ImageLayout::eDepthStencilReadOnlyOptimal;
                request.encoding = DumpEncoding::eColor;
                request.layer = face;
                requests.push_back(request);
            }

            const auto results = DumpImages(readback, requests, "dump", 0);
            std::cout << "[dump] " << results[0].path.string() << " mean=(" << results[0].meanColor.r << ","
                      << results[0].meanColor.g << "," << results[0].meanColor.b << ")\n";
            static const char *kFaceNames[] = {"+X", "-X", "+Y", "-Y", "+Z", "-Z"};
            for (uint32_t face = 0; face < kFaceCount; ++face) {
                const LayerStats stats = analyseLayer(results[face + 1].rgb8);
                std::cout << "[dump] " << kFaceNames[face] << " layer=" << face << " covered=" << stats.covered
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
