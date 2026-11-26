// shadow_mapping: LearnOpenGL 5.advanced_lighting 3.1.x, migrated to Vulkan.
//
// The three reference examples are a progression:
//   3.1.1 - render depth from the light and show it on a full-screen quad;
//   3.1.2 - light the scene and compare depth manually, no bias / PCF;
//   3.1.3 - add a slope-scaled bias and 3x3 software PCF.
// This single example keeps the whole progression behind runtime switches:
// --debug-quad reproduces 3.1.1, --no-bias/--no-pcf reproduce 3.1.2, and the
// defaults are the final 3.1.3 algorithm.  Only one 1024x1024 depth texture is
// used -- no array, no geometry shader and no gl_Layer, unlike examples/csm.
//
// Deliberate differences from the GL reference:
//   * The light projection writes Vulkan depth [0,1] instead of glm::ortho's
//     [-1,1]; using the GL range directly would clip the near half.
//   * The floor is a thin box with a procedural checker instead of a textured
//     plane, so the example needs no image asset.
//   * The camera is pulled back to (0, 1.2, 8) so all three reference cubes and
//     their shadows are inside one reproducible dump frame.
//   * The depth pass culls nothing.  Front-face culling on a closed floor box
//     stores its far face and detaches the contact shadow; the bias covers acne.
#include <array>
#include <cmath>
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

#include "shader/depth_frag.hpp"
#include "shader/depth_vert.hpp"
#include "shader/quad_frag.hpp"
#include "shader/quad_vert.hpp"
#include "shader/scene_frag.hpp"
#include "shader/scene_vert.hpp"

namespace {

constexpr float kFloorY = -0.5f;
constexpr float kCameraNear = 0.1f;
constexpr float kCameraFar = 60.0f;
constexpr float kLightNear = 1.0f;
constexpr float kLightFar = 7.5f;
constexpr float kLightExtent = 10.0f;

// std140 mirror of ShadowBlock in scene.vert / scene.frag / depth.vert.
// eye.w carries 1 / shadow-res so the PCF texel step follows --shadow-res.
struct ShadowUBO {
    alignas(16) glm::mat4 lightSpaceMatrix{1.0f};
    alignas(16) glm::mat4 viewProj{1.0f};
    alignas(16) glm::vec4 lightDir{0.0f}; // xyz = light position (per-fragment direction)
    alignas(16) glm::vec4 eye{0.0f};
    // x = debug view, y = bias on, z = PCF on, w = shadow on.
    alignas(16) glm::vec4 flags{0.0f};
};
static_assert(sizeof(ShadowUBO) == 2 * 64 + 3 * 16, "ShadowUBO must match the std140 block");

struct Instance {
    alignas(16) glm::mat4 model{1.0f};
    alignas(16) glm::vec4 material{1.0f};
};
static_assert(sizeof(Instance) == 80, "Instance must be model(64) + material(16)");

enum class BlockComponent { eShadowBlock };
using BlockConfig = DescriptorComponentSpec<BlockComponent>;
inline constexpr auto BlockComponentTable = std::make_tuple(BlockConfig{
    BlockComponent::eShadowBlock, vk::DescriptorType::eUniformBuffer, 1,
    vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment});

enum class SceneInstanceComponent { eInstances };
using InstanceConfig = DescriptorComponentSpec<SceneInstanceComponent>;
inline constexpr auto SceneInstanceComponentTable = std::make_tuple(InstanceConfig{
    SceneInstanceComponent::eInstances, vk::DescriptorType::eStorageBuffer, 1, vk::ShaderStageFlagBits::eVertex});

enum class ShadowComponent { eShadowMap };
using ShadowConfig = DescriptorComponentSpec<ShadowComponent>;
inline constexpr auto ShadowComponentTable = std::make_tuple(ShadowConfig{
    ShadowComponent::eShadowMap, vk::DescriptorType::eCombinedImageSampler, 1,
    vk::ShaderStageFlagBits::eFragment});

enum class QuadComponent { eDepthMap };
using QuadConfig = DescriptorComponentSpec<QuadComponent>;
inline constexpr auto QuadComponentTable = std::make_tuple(QuadConfig{
    QuadComponent::eDepthMap, vk::DescriptorType::eCombinedImageSampler, 1, vk::ShaderStageFlagBits::eFragment});

struct Options {
    bool dump = false;
    uint32_t dumpAfter = 2;
    uint32_t resolution = 1024;
    // 0 = lit, 1 = shadow term, 2 = stored depth, 3 = fragment/map diff,
    // 4 = fragment depth.  Matches flags.x in scene.frag.
    uint32_t debugView = 0;
    bool debugQuad = false;
    bool noBias = false;
    bool noPcf = false;
    bool noShadow = false;
    glm::vec3 lightPosition = glm::vec3(-2.0f, 4.0f, -1.0f);
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
        } else if (arg == "--shadow-res" && i + 1 < argc) {
            options.resolution = static_cast<uint32_t>(std::stoul(argv[++i]));
        } else if (arg == "--debug-shadow") {
            options.debugView = 1;
        } else if (arg == "--debug-map") {
            options.debugView = 2;
        } else if (arg == "--debug-diff") {
            options.debugView = 3;
        } else if (arg == "--debug-depth") {
            options.debugView = 4;
        } else if (arg == "--debug-quad") {
            options.debugQuad = true;
        } else if (arg == "--no-bias") {
            options.noBias = true;
        } else if (arg == "--no-pcf") {
            options.noPcf = true;
        } else if (arg == "--no-shadow") {
            options.noShadow = true;
        } else if (arg == "--light" && i + 3 < argc) {
            options.lightPosition = glm::vec3(std::stof(argv[i + 1]), std::stof(argv[i + 2]),
                                              std::stof(argv[i + 3]));
            i += 3;
        } else {
            throw std::runtime_error("shadow_mapping: unknown argument " + arg);
        }
    }
    if (options.resolution == 0) {
        options.resolution = 1;
    }
    return options;
}

// Fixed ortho light frustum from 3.1.3: [-10,10] in x/y, near=1, far=7.5.
// The GL reference uses glm::ortho, whose z is [-1,1].  Rescale z into Vulkan's
// [0,1] explicitly: z_view = -near -> 0, z_view = -far -> 1.  Forgetting this
// makes the near half of every receiver fail the 0 <= z <= w clip test.
glm::mat4 makeLightSpaceMatrix(const glm::vec3 &lightPosition)
{
    const glm::mat4 lightView = glm::lookAt(lightPosition, glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f));
    glm::mat4 projection = glm::ortho(-kLightExtent, kLightExtent, -kLightExtent, kLightExtent, kLightNear, kLightFar);
    const float depthSpan = kLightFar - kLightNear;
    projection[2][2] = -1.0f / depthSpan;
    // GLM is column-major: the z translation lives in column 3 ([3][2]), not
    // row 3 ([2][3]).  Putting it in the wrong slot zeroes every projected
    // depth, which looks exactly like a shadow map that silently does nothing.
    projection[3][2] = -kLightNear / depthSpan;
    // Vulkan NDC is y-down.  Flip the scale and its translation together.
    projection[1][1] *= -1.0f;
    projection[3][1] *= -1.0f;
    return projection * lightView;
}

// Unit cube centred on the origin, 36 vertices, CCW seen from outside.
std::vector<Vertex> makeCubeVertices()
{
    std::vector<Vertex> vertices;
    vertices.reserve(36);
    const std::array<glm::vec3, 6> normals = {glm::vec3(1.0f, 0.0f, 0.0f),  glm::vec3(-1.0f, 0.0f, 0.0f),
                                              glm::vec3(0.0f, 1.0f, 0.0f),  glm::vec3(0.0f, -1.0f, 0.0f),
                                              glm::vec3(0.0f, 0.0f, 1.0f),  glm::vec3(0.0f, 0.0f, -1.0f)};
    for (const glm::vec3 &normal : normals) {
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

// Instance 0 is the floor; 1..3 are the reference's three cubes.  Our cube is
// 1 unit wide while LearnOpenGL's raw cube is 2 units wide, so scales are 2x
// the reference numbers (1.0 / 1.0 / 0.5 instead of 0.5 / 0.5 / 0.25).
std::vector<Instance> makeScene()
{
    std::vector<Instance> instances;
    glm::mat4 floor = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, kFloorY - 0.05f, 0.0f));
    floor = glm::scale(floor, glm::vec3(25.0f, 0.1f, 25.0f));
    instances.push_back({floor, glm::vec4(0.62f, 0.60f, 0.57f, 1.0f)}); // a=1 -> checker

    glm::mat4 cube = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 1.5f, 0.0f));
    cube = glm::scale(cube, glm::vec3(1.0f));
    instances.push_back({cube, glm::vec4(0.85f, 0.55f, 0.35f, 0.0f)});

    cube = glm::translate(glm::mat4(1.0f), glm::vec3(2.0f, 0.0f, 1.0f));
    cube = glm::scale(cube, glm::vec3(1.0f));
    instances.push_back({cube, glm::vec4(0.40f, 0.55f, 0.75f, 0.0f)});

    cube = glm::translate(glm::mat4(1.0f), glm::vec3(-1.0f, 0.0f, 2.0f));
    cube = glm::rotate(cube, glm::radians(60.0f), glm::normalize(glm::vec3(1.0f, 0.0f, 1.0f)));
    cube = glm::scale(cube, glm::vec3(0.5f));
    instances.push_back({cube, glm::vec4(0.55f, 0.75f, 0.45f, 0.0f)});
    return instances;
}

// Depth-only pass with a single 2D depth attachment.  `layers` defaults to 1
// in ImageAttachment, so depthStencilView() is an e2D view usable by sampler2D.
class ShadowMapAttachmentDetail final : public IAttachmentDetail {
public:
    ShadowMapAttachmentDetail(vk::Format depthFormat, uint32_t resolution) :
        m_depthFormat(depthFormat), m_resolution(resolution)
    {
    }

    bool isSwapChainSized() const override { return false; }

    RenderPassBlueprint build(uint32_t, uint32_t, vk::SampleCountFlagBits) const override
    {
        RenderPassBlueprint blueprint;
        blueprint.swapChainSized = false;
        blueprint.colorAttachmentCount = 0; // depth-only subpass
        blueprint.sampleCount = vk::SampleCountFlagBits::e1;

        vk::AttachmentDescription depth{};
        depth.format = m_depthFormat;
        depth.samples = vk::SampleCountFlagBits::e1;
        depth.loadOp = vk::AttachmentLoadOp::eClear;
        depth.storeOp = vk::AttachmentStoreOp::eStore;
        depth.stencilLoadOp = vk::AttachmentLoadOp::eDontCare;
        depth.stencilStoreOp = vk::AttachmentStoreOp::eDontCare;
        depth.initialLayout = vk::ImageLayout::eUndefined;
        // DEPTH_READ_ONLY_OPTIMAL would need the optional
        // separateDepthStencilLayouts feature; the combined layout is universal.
        depth.finalLayout = vk::ImageLayout::eDepthStencilReadOnlyOptimal;
        blueprint.attachments.push_back(depth);

        OffscreenTargetSpec target;
        target.attachmentIndex = 0;
        target.format = m_depthFormat;
        target.extraUsage = vk::ImageUsageFlagBits::eSampled;
        target.fixedSize = true;
        target.width = m_resolution;
        target.height = m_resolution;
        target.layers = 1;
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

// Acquire half of the dependency above: the swapchain preset does not order a
// foreign pass' depth writes against this pass' fragment shader reads.
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

// NEAREST + white border, exactly like 3.1.3: the 9-tap PCF provides smoothing,
// and a white border keeps fragments outside the light frustum lit.
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
        throw std::runtime_error("shadow_mapping: failed to create the shadow sampler");
    }
    return std::shared_ptr<vk::Sampler>(new vk::Sampler(sampler), [device](vk::Sampler *pointer) {
        if (pointer) {
            device->destroySampler(*pointer);
        }
        delete pointer;
    });
}

struct MapStats {
    float covered = 0.0f;
    float meanDepth = 1.0f;
};

MapStats analyseMap(const std::vector<uint8_t> &rgb)
{
    MapStats stats;
    if (rgb.empty()) {
        return stats;
    }
    const size_t texels = rgb.size() / 3;
    size_t covered = 0;
    double sum = 0.0;
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

    auto camera = std::make_shared<Camera>(glm::vec3(0.0f, 1.2f, 8.0f), glm::vec3(0.0f, 1.0f, 0.0f), -90.0f, -8.0f);
    GLFWwindow window(1280, 720, camera);
    auto mgr = RenderManager::Create(window, MAX_FRAMES_IN_FLIGHT);

    const vk::Format depthFormat = mgr->DefaultDepthFormat();
    const glm::mat4 lightSpace = makeLightSpaceMatrix(options.lightPosition);
    const std::vector<Vertex> vertices = makeCubeVertices();
    const std::vector<Instance> instances = makeScene();
    const uint32_t vertexCount = static_cast<uint32_t>(vertices.size());
    const uint32_t instanceCount = static_cast<uint32_t>(instances.size());

    std::cout << "[shadow_mapping] " << options.resolution << "x" << options.resolution << " single 2D depth map\n"
              << "[shadow_mapping] scene: 1 ground slab + " << (instanceCount - 1)
              << " cubes, one instanced draw per pass\n"
              << "[shadow_mapping] light at (" << options.lightPosition.x << ", " << options.lightPosition.y << ", "
              << options.lightPosition.z << "), ortho +/-" << kLightExtent << ", near " << kLightNear << ", far "
              << kLightFar << "\n";

    // ----- shadow pass -----------------------------------------------------
    auto shadowDetail = std::make_shared<ShadowMapAttachmentDetail>(depthFormat, options.resolution);
    auto shadowPass = mgr->CreateRenderPass(shadowDetail);
    shadowPass->createFramebuffers({}, shadowPass->extent());
    if (shadowPass->depthStencilLayers() != 1) {
        throw std::runtime_error("shadow_mapping: expected a single 2D depth attachment");
    }
    if (options.dump && depthFormat != vk::Format::eD32Sfloat) {
        throw std::runtime_error("shadow_mapping: --dump needs a D32Sfloat depth format (got " +
                                 vk::to_string(depthFormat) + ")");
    }

    // ----- main pass -------------------------------------------------------
    auto mainDetail = std::make_shared<ShadowSamplingMainPassDetail>(std::make_shared<SimpleAttachmentDetail>(
        SimpleAttachmentDetail::Mode::eDirect, *mgr->getSwapChain()->getImageFormat(), depthFormat));
    auto mainPass = mgr->CreateRenderPass(mainDetail);

    // ----- descriptor layouts ---------------------------------------------
    auto shadowLayout = std::make_shared<DescriptorLayoutAtlas>();
    auto shadowBlockSet = DescriptorLayoutAtlas::makeSet<BlockComponent, BlockComponentTable>(
        0, {BlockComponent::eShadowBlock}, UpdateFrequency::eEachFrame, false);
    shadowBlockSet.specs[0] = GBufferSpec{1, static_cast<uint32_t>(sizeof(ShadowUBO)), DataTypes::eCustom};
    auto shadowInstanceSet = DescriptorLayoutAtlas::makeSet<SceneInstanceComponent, SceneInstanceComponentTable>(
        1, {SceneInstanceComponent::eInstances}, UpdateFrequency::eEachInstance, false);
    shadowInstanceSet.specs[0] =
        GBufferSpec{instanceCount, static_cast<uint32_t>(sizeof(Instance)), DataTypes::eCustom};
    shadowLayout->setDetails({shadowBlockSet, shadowInstanceSet});

    auto mainLayout = std::make_shared<DescriptorLayoutAtlas>();
    auto mainBlockSet = DescriptorLayoutAtlas::makeSet<BlockComponent, BlockComponentTable>(
        0, {BlockComponent::eShadowBlock}, UpdateFrequency::eEachFrame, false);
    mainBlockSet.specs[0] = GBufferSpec{1, static_cast<uint32_t>(sizeof(ShadowUBO)), DataTypes::eCustom};
    auto mainInstanceSet = DescriptorLayoutAtlas::makeSet<SceneInstanceComponent, SceneInstanceComponentTable>(
        1, {SceneInstanceComponent::eInstances}, UpdateFrequency::eEachInstance, false);
    mainInstanceSet.specs[0] =
        GBufferSpec{instanceCount, static_cast<uint32_t>(sizeof(Instance)), DataTypes::eCustom};
    // No TextureImageSpec for set2: the engine allocates nothing for an
    // eCombinedImageSampler whose spec stayed a GBufferSpec.
    auto mainShadowSet = DescriptorLayoutAtlas::makeSet<ShadowComponent, ShadowComponentTable>(
        2, {ShadowComponent::eShadowMap}, UpdateFrequency::eEachFrame, false);
    mainLayout->setDetails({mainBlockSet, mainInstanceSet, mainShadowSet});

    auto quadLayout = std::make_shared<DescriptorLayoutAtlas>();
    auto quadDepthSet = DescriptorLayoutAtlas::makeSet<QuadComponent, QuadComponentTable>(
        0, {QuadComponent::eDepthMap}, UpdateFrequency::eEachFrame, false);
    quadLayout->setDetails({quadDepthSet});

    // ----- assets ----------------------------------------------------------
    auto shadowAsset = mgr->CreateShaderAssetGroup(shadowLayout);
    shadowAsset->addShaderSpirvCode("vert", std::vector<uint32_t>(shader::DEPTH_VERT_SPV.begin(),
                                                                  shader::DEPTH_VERT_SPV.end()));
    shadowAsset->addShaderSpirvCode("frag", std::vector<uint32_t>(shader::DEPTH_FRAG_SPV.begin(),
                                                                  shader::DEPTH_FRAG_SPV.end()));

    auto mainAsset = mgr->CreateShaderAssetGroup(mainLayout);
    mainAsset->addShaderSpirvCode("vert", std::vector<uint32_t>(shader::SCENE_VERT_SPV.begin(),
                                                                shader::SCENE_VERT_SPV.end()));
    mainAsset->addShaderSpirvCode("frag", std::vector<uint32_t>(shader::SCENE_FRAG_SPV.begin(),
                                                                shader::SCENE_FRAG_SPV.end()));

    auto quadAsset = mgr->CreateShaderAssetGroup(quadLayout);
    quadAsset->addShaderSpirvCode("vert", std::vector<uint32_t>(shader::QUAD_VERT_SPV.begin(),
                                                                shader::QUAD_VERT_SPV.end()));
    quadAsset->addShaderSpirvCode("frag", std::vector<uint32_t>(shader::QUAD_FRAG_SPV.begin(),
                                                                shader::QUAD_FRAG_SPV.end()));

    // ----- depth texture -> both consumers --------------------------------
    // Descriptor sets are written once when the pipeline is created, so these
    // resources must be registered before CreatePipeline below.
    auto shadowSampler = makeShadowSampler(mgr->getLogicalDevice());
    std::vector<std::shared_ptr<TextureSamplerCombine>> mainShadowResources(MAX_FRAMES_IN_FLIGHT);
    std::vector<std::shared_ptr<TextureSamplerCombine>> quadShadowResources(MAX_FRAMES_IN_FLIGHT);
    for (uint32_t frame = 0; frame < MAX_FRAMES_IN_FLIGHT; ++frame) {
        const uint32_t slot = frame % shadowPass->framebufferCount();
        const vk::ImageView view = shadowPass->depthStencilView(slot);
        if (!view) {
            throw std::runtime_error("shadow_mapping: shadow pass exposes no depth view");
        }
        const auto deleter = [shadowSampler](TextureSamplerCombine *pointer) { delete pointer; };
        auto mainCombine = std::shared_ptr<TextureSamplerCombine>(
            new TextureSamplerCombine(view, *shadowSampler, vk::ImageLayout::eDepthStencilReadOnlyOptimal), deleter);
        mainAsset->addDescriptorResource(ResourceKey::make_key(ResourceId(2, 0), frame), mainCombine);
        mainShadowResources[frame] = std::move(mainCombine);

        auto quadCombine = std::shared_ptr<TextureSamplerCombine>(
            new TextureSamplerCombine(view, *shadowSampler, vk::ImageLayout::eDepthStencilReadOnlyOptimal), deleter);
        quadAsset->addDescriptorResource(ResourceKey::make_key(ResourceId(0, 0), frame), quadCombine);
        quadShadowResources[frame] = std::move(quadCombine);
    }

    // ----- pipelines -------------------------------------------------------
    auto shadowVertexInput = std::make_shared<VertexInputAtlas>(
        0, std::vector<VertexComponent>{VertexComponent::ePosition}, vertexCount, 0u, DataTypes::eUnsignedInt, false,
        0u, instanceCount);
    GraphicsPipelineConfig shadowConfig;
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

    auto emptyVertexInput = std::make_shared<VertexInputAtlas>(0, std::vector<VertexComponent>{}, 0u, 0u,
                                                               DataTypes::eUnsignedInt, true, 0u);
    GraphicsPipelineConfig quadConfig;
    quadConfig.depthWriteEnable = false;
    auto quadPipeline = mgr->CreatePipeline(emptyVertexInput, quadLayout, quadAsset, mainPass, 0, quadConfig);

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
    auto quadDraw = std::make_shared<CmdOpDrawFrame>(quadPipeline);
    auto shadowFrameSync = std::make_shared<CmdSyncDevice>(shadowAsset);
    auto mainFrameSync = std::make_shared<CmdSyncDevice>(mainAsset);

    static const char *kDebugNames[] = {"frame", "frame-shadow", "frame-map", "frame-diff", "frame-depth"};
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
        glm::mat4 projection = glm::perspective(glm::radians(camera->Zoom), aspect, kCameraNear, kCameraFar);
        projection[1][1] *= -1.0f;

        ShadowUBO ubo;
        ubo.lightSpaceMatrix = lightSpace;
        ubo.viewProj = projection * view;
        ubo.lightDir = glm::vec4(options.lightPosition, 0.0f);
        ubo.eye = glm::vec4(camera->Position, 1.0f / static_cast<float>(options.resolution));
        ubo.flags = glm::vec4(static_cast<float>(options.debugView), options.noBias ? 0.0f : 1.0f,
                              options.noPcf ? 0.0f : 1.0f, options.noShadow ? 0.0f : 1.0f);

        shadowDraw->OnUpdateRawData(ResourceId(0, 0), frame, ubo);
        mainDraw->OnUpdateRawData(ResourceId(0, 0), frame, ubo);

        sequence->beginRecording();
        sequence->record(shadowFrameSync);
        sequence->record(mainFrameSync);
        sequence->recordPass(shadowPass, [&]() { sequence->record(shadowDraw); });
        // The quad draws the exact depth texture the lit path samples, which
        // is 3.1.1's full-screen debug view without a second render pass.
        sequence->recordPass(mainPass, [&]() {
            if (options.debugQuad) {
                sequence->record(quadDraw);
            } else {
                sequence->record(mainDraw);
            }
        });
        sequence->endRecording();
        sequence->submitFrame();

        ++framesRendered;
        if (options.dump && !dumpDone && framesRendered >= options.dumpAfter) {
            ImageReadback readback(mgr->getLogicalDevice(), mgr->memoryProperties(), mgr->getMemoryAllocator(),
                                   mgr->getGraphicsQueue(), *mgr->getQueueFamilyIndices().graphicsFamily);

            std::vector<DumpRequest> requests;
            DumpRequest frameRequest;
            frameRequest.label = options.debugQuad ? "frame-quad" : kDebugNames[options.debugView];
            frameRequest.image = mgr->getSwapChain()->getImages()[sequence->getImageIndex()];
            frameRequest.format = *mgr->getSwapChain()->getImageFormat();
            frameRequest.extent = extent;
            frameRequest.layout = vk::ImageLayout::ePresentSrcKHR;
            frameRequest.encoding = DumpEncoding::eColor;
            requests.push_back(frameRequest);

            // Orthographic depth is already linear, so eColor is the right
            // encoding: 255 means "cleared, nothing there".
            DumpRequest mapRequest;
            mapRequest.label = "shadow-map";
            mapRequest.image = shadowPass->depthStencilImage(frame % shadowPass->framebufferCount());
            mapRequest.format = shadowPass->depthStencilFormat();
            mapRequest.extent = shadowPass->extent();
            mapRequest.layout = vk::ImageLayout::eDepthStencilReadOnlyOptimal;
            mapRequest.encoding = DumpEncoding::eColor;
            requests.push_back(mapRequest);

            const auto results = DumpImages(readback, requests, "dump", 0);
            const MapStats stats = analyseMap(results[1].rgb8);
            std::cout << "[dump] " << results[0].path.string() << " view="
                      << (options.debugQuad ? "quad" : kDebugNames[options.debugView]) << "\n"
                      << "[dump] " << results[1].path.string() << " covered=" << stats.covered
                      << " meanDepth=" << stats.meanDepth << "\n";
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
