// bloom: HDR MRT + threshold + 10-step Gaussian blur + additive composite,
// migrated from LearnOpenGL/src/5.advanced_lighting/7.bloom.
//
// The reference uses two RGBA16F scene attachments (scene and BrightColor), a
// two-texture ping-pong chain of five horizontal and five vertical 9-tap
// blurs, then adds the blurred result to the scene before exponential tone
// mapping.  All of that maps directly onto render_demo: MRT attachments are
// already supported by GBuffer/OIT, fullscreen passes by offscreen, and the
// ping-pong is expressed as two alternating offscreen render passes.
//
// Deliberate differences: the scene is procedural (no backpack model) and the
// light box positions are kept in one C++ table that exactly matches scene.frag
// -- in the reference these constants are duplicated between cpp and shader.
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
#include "VertexInputStateConfig.h"

#include "CmdOpDrawFrame.h"
#include "CmdSyncDevice.h"
#include "IRenderPass.h"
#include "LoopSequence.h"
#include "OnceSequence.h"
#include "RenderManager.h"
#include "TextureSamplerCombine.h"

#include "shader/blur_h_frag.hpp"
#include "shader/blur_v_frag.hpp"
#include "shader/composite_frag.hpp"
#include "shader/scene_frag.hpp"
#include "shader/scene_vert.hpp"
#include "shader/screen_vert.hpp"

namespace {

constexpr float kNearPlane = 0.1f;
constexpr float kFarPlane = 100.0f;
// Five horizontal + five vertical passes, matching `amount = 10` in the
// reference.  The final result lives in blurPasses[(kBlurPasses - 1) % 2].
constexpr uint32_t kBlurPasses = 10;

struct BloomSceneUBO {
    alignas(16) glm::mat4 viewProj{1.0f};
    alignas(16) glm::vec4 eye{0.0f};
    alignas(16) glm::vec4 params{0.0f};
};
static_assert(sizeof(BloomSceneUBO) == 96, "BloomSceneUBO must match the std140 BloomBlock");

struct CompositeUBO {
    alignas(16) glm::vec4 params{0.0f};
};
static_assert(sizeof(CompositeUBO) == 16, "CompositeUBO must match CompositeBlock");

struct Instance {
    alignas(16) glm::mat4 model{1.0f};
    alignas(16) glm::vec4 material{1.0f};
};
static_assert(sizeof(Instance) == 80, "Instance must be model(64) + material(16)");

enum class SceneComponent { eBlock };
using SceneConfig = DescriptorComponentSpec<SceneComponent>;
inline constexpr auto SceneComponentTable = std::make_tuple(SceneConfig{
    SceneComponent::eBlock, vk::DescriptorType::eUniformBuffer, 1,
    vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment});

enum class SceneInstanceComponent { eInstances };
using SceneInstanceConfig = DescriptorComponentSpec<SceneInstanceComponent>;
inline constexpr auto SceneInstanceComponentTable = std::make_tuple(SceneInstanceConfig{
    SceneInstanceComponent::eInstances, vk::DescriptorType::eStorageBuffer, 1,
    vk::ShaderStageFlagBits::eVertex});

enum class BlurComponent { eSource };
using BlurConfig = DescriptorComponentSpec<BlurComponent>;
inline constexpr auto BlurComponentTable = std::make_tuple(BlurConfig{
    BlurComponent::eSource, vk::DescriptorType::eCombinedImageSampler, 1,
    vk::ShaderStageFlagBits::eFragment});

enum class CompositeComponent { eParams, eScene, eBloom };
using CompositeConfig = DescriptorComponentSpec<CompositeComponent>;
inline constexpr auto CompositeComponentTable = std::make_tuple(
    CompositeConfig{CompositeComponent::eParams, vk::DescriptorType::eUniformBuffer, 1,
                    vk::ShaderStageFlagBits::eFragment},
    CompositeConfig{CompositeComponent::eScene, vk::DescriptorType::eCombinedImageSampler, 1,
                    vk::ShaderStageFlagBits::eFragment},
    CompositeConfig{CompositeComponent::eBloom, vk::DescriptorType::eCombinedImageSampler, 1,
                    vk::ShaderStageFlagBits::eFragment});

struct Options {
    bool dump = false;
    uint32_t dumpAfter = 3;
    float exposure = 1.0f;
    bool noBloom = false;
    bool debugBloom = false;
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
        } else if (arg == "--exposure" && i + 1 < argc) {
            options.exposure = std::stof(argv[++i]);
        } else if (arg == "--no-bloom") {
            options.noBloom = true;
        } else if (arg == "--debug-bloom") {
            options.debugBloom = true;
        } else {
            throw std::runtime_error("bloom: unknown argument " + arg);
        }
    }
    options.exposure = std::clamp(options.exposure, 0.0f, 100.0f);
    return options;
}

std::vector<Vertex> makeCubeVertices()
{
    std::vector<Vertex> vertices;
    vertices.reserve(36);
    const std::array<glm::vec3, 6> normals = {glm::vec3(1.0f, 0.0f, 0.0f), glm::vec3(-1.0f, 0.0f, 0.0f),
                                              glm::vec3(0.0f, 1.0f, 0.0f), glm::vec3(0.0f, -1.0f, 0.0f),
                                              glm::vec3(0.0f, 0.0f, 1.0f), glm::vec3(0.0f, 0.0f, -1.0f)};
    for (const glm::vec3 &normal : normals) {
        const glm::vec3 up = std::abs(normal.y) > 0.9f ? glm::vec3(0.0f, 0.0f, 1.0f)
                                                        : glm::vec3(0.0f, 1.0f, 0.0f);
        const glm::vec3 right = glm::normalize(glm::cross(up, normal));
        const glm::vec3 up2 = glm::cross(normal, right);
        const std::array<glm::vec2, 4> uvs = {glm::vec2(0.0f, 0.0f), glm::vec2(1.0f, 0.0f),
                                              glm::vec2(1.0f, 1.0f), glm::vec2(0.0f, 1.0f)};
        const std::array<glm::vec3, 4> corners = {(-right - up2) * 0.5f, (right - up2) * 0.5f,
                                                  (right + up2) * 0.5f, (-right + up2) * 0.5f};
        for (uint32_t index : {0u, 1u, 2u, 0u, 2u, 3u}) {
            Vertex vertex;
            vertex.position = corners[index] + normal * 0.5f;
            vertex.normal = normal;
            vertex.color = glm::vec3(1.0f);
            vertex.texCoord = uvs[index];
            vertices.push_back(vertex);
        }
    }
    return vertices;
}

std::vector<Instance> makeScene()
{
    std::vector<Instance> instances;
    glm::mat4 floorModel = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, -1.4f, -3.0f));
    floorModel = glm::scale(floorModel, glm::vec3(12.0f, 0.2f, 14.0f));
    instances.push_back({floorModel, glm::vec4(0.45f, 0.44f, 0.42f, 0.0f)});

    struct Block {
        glm::vec3 position;
        glm::vec3 scale;
        glm::vec3 color;
    };
    static const Block blocks[] = {
        {glm::vec3(-2.3f, -0.3f, -2.0f), glm::vec3(1.2f, 2.0f, 1.2f), glm::vec3(0.55f, 0.50f, 0.45f)},
        {glm::vec3(2.0f, -0.2f, -5.0f), glm::vec3(1.6f, 2.2f, 1.6f), glm::vec3(0.45f, 0.52f, 0.60f)},
        {glm::vec3(-0.6f, 0.1f, -7.5f), glm::vec3(2.0f, 2.8f, 2.0f), glm::vec3(0.50f, 0.48f, 0.55f)},
    };
    for (const Block &block : blocks) {
        glm::mat4 model = glm::translate(glm::mat4(1.0f), block.position);
        model = glm::scale(model, block.scale);
        instances.push_back({model, glm::vec4(block.color, 0.0f)});
    }

    struct Light {
        glm::vec3 position;
        glm::vec3 radiance;
    };
    // Must match LIGHT_POS / LIGHT_COLOR in scene.frag.
    static const Light lights[] = {
        {glm::vec3(0.0f, 0.5f, 1.5f), glm::vec3(5.0f)},
        {glm::vec3(-4.0f, 0.5f, -3.0f), glm::vec3(10.0f, 0.0f, 0.0f)},
        {glm::vec3(3.0f, 0.5f, 1.0f), glm::vec3(0.0f, 0.0f, 15.0f)},
        {glm::vec3(-0.8f, 2.4f, -1.0f), glm::vec3(0.0f, 5.0f, 0.0f)},
    };
    for (const Light &light : lights) {
        glm::mat4 model = glm::translate(glm::mat4(1.0f), light.position);
        model = glm::scale(model, glm::vec3(0.25f));
        instances.push_back({model, glm::vec4(light.radiance, 1.0f)});
    }
    return instances;
}

// Two RGBA16F colour targets + depth, both sampled by later passes.
class BloomSceneAttachmentDetail final : public IAttachmentDetail {
public:
    BloomSceneAttachmentDetail(vk::Format colorFormat, vk::Format depthFormat) :
        m_colorFormat(colorFormat), m_depthFormat(depthFormat)
    {
    }

    bool isSwapChainSized() const override { return false; }

    RenderPassBlueprint build(uint32_t width, uint32_t height, vk::SampleCountFlagBits) const override
    {
        RenderPassBlueprint bp;
        bp.sampleCount = vk::SampleCountFlagBits::e1;
        bp.swapChainSized = false;
        bp.colorAttachmentCount = 2;
        bp.framebufferCount = 2; // RenderManager overwrites with framesInFlight

        SubpassBlueprint subpass;
        for (uint32_t slot = 0; slot < 2; ++slot) {
            vk::AttachmentDescription color{};
            color.format = m_colorFormat;
            color.samples = vk::SampleCountFlagBits::e1;
            color.loadOp = vk::AttachmentLoadOp::eClear;
            color.storeOp = vk::AttachmentStoreOp::eStore;
            color.stencilLoadOp = vk::AttachmentLoadOp::eDontCare;
            color.stencilStoreOp = vk::AttachmentStoreOp::eDontCare;
            color.initialLayout = vk::ImageLayout::eUndefined;
            color.finalLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
            bp.attachments.push_back(color);
            subpass.colorAttachments.push_back(
                vk::AttachmentReference(slot, vk::ImageLayout::eColorAttachmentOptimal));

            OffscreenTargetSpec target;
            target.attachmentIndex = slot;
            target.format = m_colorFormat;
            target.fixedSize = true;
            target.width = width;
            target.height = height;
            bp.offscreenTargets.push_back(target);
        }

        vk::AttachmentDescription depth{};
        depth.format = m_depthFormat;
        depth.samples = vk::SampleCountFlagBits::e1;
        depth.loadOp = vk::AttachmentLoadOp::eClear;
        depth.storeOp = vk::AttachmentStoreOp::eDontCare;
        depth.stencilLoadOp = vk::AttachmentLoadOp::eDontCare;
        depth.stencilStoreOp = vk::AttachmentStoreOp::eDontCare;
        depth.initialLayout = vk::ImageLayout::eUndefined;
        depth.finalLayout = vk::ImageLayout::eDepthStencilAttachmentOptimal;
        bp.attachments.push_back(depth);
        subpass.depthStencil = vk::AttachmentReference(2, vk::ImageLayout::eDepthStencilAttachmentOptimal);
        bp.subpasses.push_back(subpass);
        bp.dependencies = makeDependencies(DependencyPreset::eOffscreen);

        bp.clearValues.push_back(vk::ClearValue(vk::ClearColorValue(std::array<float, 4>{0.0f, 0.0f, 0.0f, 0.0f})));
        bp.clearValues.push_back(vk::ClearValue(vk::ClearColorValue(std::array<float, 4>{0.0f, 0.0f, 0.0f, 0.0f})));
        bp.clearValues.push_back(vk::ClearValue(vk::ClearDepthStencilValue(1.0f, 0)));
        return bp;
    }

private:
    vk::Format m_colorFormat;
    vk::Format m_depthFormat;
};

std::shared_ptr<vk::Sampler> makeLinearSampler(const std::shared_ptr<vk::Device> &device)
{
    vk::SamplerCreateInfo info{};
    info.magFilter = vk::Filter::eLinear;
    info.minFilter = vk::Filter::eLinear;
    info.mipmapMode = vk::SamplerMipmapMode::eLinear;
    info.addressModeU = vk::SamplerAddressMode::eClampToEdge;
    info.addressModeV = vk::SamplerAddressMode::eClampToEdge;
    info.addressModeW = vk::SamplerAddressMode::eClampToEdge;
    info.minLod = 0.0f;
    info.maxLod = 0.0f;
    vk::Sampler sampler{};
    if (device->createSampler(&info, nullptr, &sampler) != vk::Result::eSuccess) {
        throw std::runtime_error("bloom: failed to create the blur sampler");
    }
    return std::shared_ptr<vk::Sampler>(new vk::Sampler(sampler), [device](vk::Sampler *pointer) {
        if (pointer) {
            device->destroySampler(*pointer);
        }
        delete pointer;
    });
}

} // namespace

void test(int argc, char **argv)
{
    const Options options = parseOptions(argc, argv);

    auto camera = std::make_shared<Camera>(glm::vec3(0.0f, 2.2f, 10.0f), glm::vec3(0.0f, 1.0f, 0.0f), -90.0f,
                                           -8.0f);
    camera->MovementSpeed = 6.0f;
    GLFWwindow window(1280, 720, camera);
    auto mgr = RenderManager::Create(window, MAX_FRAMES_IN_FLIGHT);

    const vk::Format depthFormat = mgr->DefaultDepthFormat();
    const vk::Format hdrFormat = vk::Format::eR16G16B16A16Sfloat;
    const std::vector<Vertex> vertices = makeCubeVertices();
    const std::vector<Instance> instances = makeScene();
    const uint32_t instanceCount = static_cast<uint32_t>(instances.size());

    std::cout << "[bloom] " << instanceCount << " instances, 2 RGBA16F scene targets, " << kBlurPasses
              << " blur passes, exposure " << options.exposure << "\n";

    // ----- passes -----------------------------------------------------------
    auto scenePass = mgr->CreateRenderPass(std::make_shared<BloomSceneAttachmentDetail>(hdrFormat, depthFormat));
    scenePass->createFramebuffers({}, scenePass->extent());
    if (scenePass->offscreenColorCountPerSet() != 2) {
        throw std::runtime_error("bloom: scene pass must expose scene + bright targets");
    }

    std::array<std::shared_ptr<IRenderPass>, 2> blurPasses;
    for (uint32_t i = 0; i < blurPasses.size(); ++i) {
        blurPasses[i] = mgr->CreateRenderPass(std::make_shared<SimpleAttachmentDetail>(
            SimpleAttachmentDetail::Mode::eOffscreen, hdrFormat, vk::Format::eUndefined,
            vk::ImageLayout::eShaderReadOnlyOptimal, DependencyPreset::eOffscreen));
        blurPasses[i]->createFramebuffers({}, blurPasses[i]->extent());
    }
    auto screenPass = mgr->CreateRenderPass(std::make_shared<SimpleAttachmentDetail>(
        SimpleAttachmentDetail::Mode::eDirect, *mgr->getSwapChain()->getImageFormat(), depthFormat));

    // ----- layouts ----------------------------------------------------------
    auto sceneLayout = std::make_shared<DescriptorLayoutAtlas>(MAX_FRAMES_IN_FLIGHT);
    auto sceneBlockSet = DescriptorLayoutAtlas::makeSet<SceneComponent, SceneComponentTable>(
        0, {SceneComponent::eBlock}, UpdateFrequency::eEachFrame, false);
    sceneBlockSet.specs[0] = GBufferSpec{1, static_cast<uint32_t>(sizeof(BloomSceneUBO)), DataTypes::eCustom};
    auto sceneInstanceSet = DescriptorLayoutAtlas::makeSet<SceneInstanceComponent, SceneInstanceComponentTable>(
        1, {SceneInstanceComponent::eInstances}, UpdateFrequency::eStatic, true);
    sceneInstanceSet.specs[0] = GBufferSpec{instanceCount, static_cast<uint32_t>(sizeof(Instance)),
                                            DataTypes::eCustom};
    sceneLayout->setDetails({sceneBlockSet, sceneInstanceSet});

    auto blurLayout = std::make_shared<DescriptorLayoutAtlas>(MAX_FRAMES_IN_FLIGHT);
    auto blurSet = DescriptorLayoutAtlas::makeSet<BlurComponent, BlurComponentTable>(
        0, {BlurComponent::eSource}, UpdateFrequency::eEachFrame, false);
    blurLayout->setDetails({blurSet});

    auto compositeLayout = std::make_shared<DescriptorLayoutAtlas>(MAX_FRAMES_IN_FLIGHT);
    auto paramsSet = DescriptorLayoutAtlas::makeSet<CompositeComponent, CompositeComponentTable>(
        0, {CompositeComponent::eParams}, UpdateFrequency::eEachFrame, false);
    paramsSet.specs[0] = GBufferSpec{1, static_cast<uint32_t>(sizeof(CompositeUBO)), DataTypes::eCustom};
    auto compositeSceneSet = DescriptorLayoutAtlas::makeSet<CompositeComponent, CompositeComponentTable>(
        1, {CompositeComponent::eScene}, UpdateFrequency::eEachFrame, false);
    auto compositeBloomSet = DescriptorLayoutAtlas::makeSet<CompositeComponent, CompositeComponentTable>(
        2, {CompositeComponent::eBloom}, UpdateFrequency::eEachFrame, false);
    compositeLayout->setDetails({paramsSet, compositeSceneSet, compositeBloomSet});

    // ----- assets -----------------------------------------------------------
    auto sceneAsset = mgr->CreateShaderAssetGroup(sceneLayout);
    sceneAsset->addShaderSpirvCode("vert", std::vector<uint32_t>(shader::SCENE_VERT_SPV.begin(),
                                                                  shader::SCENE_VERT_SPV.end()));
    sceneAsset->addShaderSpirvCode("frag", std::vector<uint32_t>(shader::SCENE_FRAG_SPV.begin(),
                                                                  shader::SCENE_FRAG_SPV.end()));

    auto compositeAsset = mgr->CreateShaderAssetGroup(compositeLayout);
    compositeAsset->addShaderSpirvCode("vert", std::vector<uint32_t>(shader::SCREEN_VERT_SPV.begin(),
                                                                      shader::SCREEN_VERT_SPV.end()));
    compositeAsset->addShaderSpirvCode("frag", std::vector<uint32_t>(shader::COMPOSITE_FRAG_SPV.begin(),
                                                                      shader::COMPOSITE_FRAG_SPV.end()));

    std::array<std::shared_ptr<ShaderAssetGroup>, kBlurPasses> blurAssets;
    for (uint32_t i = 0; i < kBlurPasses; ++i) {
        blurAssets[i] = mgr->CreateShaderAssetGroup(blurLayout);
        blurAssets[i]->addShaderSpirvCode("vert", std::vector<uint32_t>(shader::SCREEN_VERT_SPV.begin(),
                                                                         shader::SCREEN_VERT_SPV.end()));
        const auto &sprv = (i % 2 == 0) ? shader::BLUR_H_FRAG_SPV : shader::BLUR_V_FRAG_SPV;
        blurAssets[i]->addShaderSpirvCode("frag", std::vector<uint32_t>(sprv.begin(), sprv.end()));
    }

    // Cross-pass sampled views must be bound before CreatePipeline: descriptor
    // sets are written once at pipeline build time.
    auto sampler = makeLinearSampler(mgr->getLogicalDevice());
    const auto bindView = [&](const std::shared_ptr<ShaderAssetGroup> &asset, uint32_t set, uint32_t frame,
                              vk::ImageView view) {
        auto combine = std::shared_ptr<TextureSamplerCombine>(
            new TextureSamplerCombine(view, *sampler, vk::ImageLayout::eShaderReadOnlyOptimal),
            [sampler](TextureSamplerCombine *pointer) { delete pointer; });
        asset->addDescriptorResource(ResourceKey::make_key(ResourceId(set, 0), frame), combine);
    };

    for (uint32_t frame = 0; frame < MAX_FRAMES_IN_FLIGHT; ++frame) {
        const std::vector<vk::ImageView> &sceneViews = scenePass->offscreenViews();
        bindView(compositeAsset, 1, frame, sceneViews[frame * 2]); // scene radiance
        for (uint32_t i = 0; i < kBlurPasses; ++i) {
            // Pass 0 samples the bright MRT target; pass i>0 samples the
            // previous blur target, alternating between the two ping-pong passes.
            const vk::ImageView source =
                (i == 0) ? sceneViews[frame * 2 + 1]
                         : blurPasses[(i - 1) % 2]->offscreenViews()[frame];
            bindView(blurAssets[i], 0, frame, source);
        }
        bindView(compositeAsset, 2, frame,
                 blurPasses[(kBlurPasses - 1) % 2]->offscreenViews()[frame]);
    }

    // ----- pipelines --------------------------------------------------------
    auto sceneVertexInput = std::make_shared<VertexInputAtlas>(
        0,
        std::vector<VertexComponent>{VertexComponent::ePosition, VertexComponent::eNormal, VertexComponent::eUV},
        static_cast<uint32_t>(vertices.size()), 0u, DataTypes::eUnsignedInt, false, 0u, instanceCount);
    GraphicsPipelineConfig sceneConfig;
    sceneConfig.cullMode = vk::CullModeFlagBits::eNone;
    auto scenePipeline =
        mgr->CreatePipeline(sceneVertexInput, sceneLayout, sceneAsset, scenePass, 0, sceneConfig);

    auto emptyVertexInput = std::make_shared<VertexInputAtlas>(0, std::vector<VertexComponent>{}, 0u, 0u,
                                                               DataTypes::eUnsignedInt, true, 0u);
    GraphicsPipelineConfig screenConfig;
    screenConfig.depthTestEnable = false;
    screenConfig.depthWriteEnable = false;
    std::array<std::shared_ptr<GraphicsPipeline>, kBlurPasses> blurPipelines;
    for (uint32_t i = 0; i < kBlurPasses; ++i) {
        blurPipelines[i] =
            mgr->CreatePipeline(emptyVertexInput, blurLayout, blurAssets[i], blurPasses[i % 2], 0, screenConfig);
    }
    auto compositePipeline =
        mgr->CreatePipeline(emptyVertexInput, compositeLayout, compositeAsset, screenPass, 0, screenConfig);

    // ----- uploads ----------------------------------------------------------
    auto upload = mgr->UploadSequence();
    {
        auto sync = std::make_shared<CmdSyncDevice>(sceneAsset);
        sync->OnUpdateRawData(ResourceTypes::eVertex, vertices);
        sync->OnUpdateRawData(ResourceId(1, 0), 0u, instances);
        upload->submit(sync, sceneAsset);
    }

    // ----- frame loop -------------------------------------------------------
    auto sequence = mgr->DrawFrameSequence();
    auto sceneDraw = std::make_shared<CmdOpDrawFrame>(scenePipeline);
    std::array<std::shared_ptr<CmdOpDrawFrame>, kBlurPasses> blurDraws;
    for (uint32_t i = 0; i < kBlurPasses; ++i) {
        blurDraws[i] = std::make_shared<CmdOpDrawFrame>(blurPipelines[i]);
    }
    auto compositeDraw = std::make_shared<CmdOpDrawFrame>(compositePipeline);
    auto sceneFrameSync = std::make_shared<CmdSyncDevice>(sceneAsset);
    auto compositeFrameSync = std::make_shared<CmdSyncDevice>(compositeAsset);

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
        glm::mat4 projection = glm::perspective(glm::radians(camera->Zoom), aspect, kNearPlane, kFarPlane);
        projection[1][1] *= -1.0f;

        BloomSceneUBO sceneUbo;
        sceneUbo.viewProj = projection * view;
        sceneUbo.eye = glm::vec4(camera->Position, 1.0f);
        sceneUbo.params = glm::vec4(options.noBloom ? 0.0f : 1.0f, options.debugBloom ? 1.0f : 0.0f, 0.0f, 0.0f);
        CompositeUBO compositeUbo;
        compositeUbo.params = glm::vec4(options.exposure, options.noBloom ? 0.0f : 1.0f,
                                        options.debugBloom ? 1.0f : 0.0f, 0.0f);

        sceneDraw->OnUpdateRawData(ResourceId(0, 0), frame, sceneUbo);
        compositeDraw->OnUpdateRawData(ResourceId(0, 0), frame, compositeUbo);

        sequence->beginRecording();
        sequence->record(sceneFrameSync);
        sequence->record(compositeFrameSync);
        sequence->recordPass(scenePass, [&]() { sequence->record(sceneDraw); });
        for (uint32_t i = 0; i < kBlurPasses; ++i) {
            sequence->recordPass(blurPasses[i % 2], [&, i]() { sequence->record(blurDraws[i]); });
        }
        sequence->recordPass(screenPass, [&]() { sequence->record(compositeDraw); });
        sequence->endRecording();
        sequence->submitFrame();

        ++framesRendered;
        if (options.dump && !dumpDone && framesRendered >= options.dumpAfter) {
            auto readback = ImageReadback(mgr->getLogicalDevice(), mgr->memoryProperties(),
                                          mgr->getMemoryAllocator(), mgr->getGraphicsQueue(),
                                          *mgr->getQueueFamilyIndices().graphicsFamily);
            DumpRequest frameRequest;
            frameRequest.label = options.noBloom ? "frame-no-bloom" : options.debugBloom ? "frame-bloom" : "frame";
            frameRequest.image = mgr->getSwapChain()->getImages()[sequence->getImageIndex()];
            frameRequest.format = *mgr->getSwapChain()->getImageFormat();
            frameRequest.extent = extent;
            frameRequest.layout = vk::ImageLayout::ePresentSrcKHR;
            frameRequest.encoding = DumpEncoding::eColor;

            const std::vector<vk::ImageView> &sceneViews = scenePass->offscreenViews();
            DumpRequest sceneRequest;
            sceneRequest.label = "scene-color";
            sceneRequest.image = scenePass->offscreenImages()[frame * 2];
            sceneRequest.format = hdrFormat;
            sceneRequest.extent = scenePass->extent();
            sceneRequest.layout = vk::ImageLayout::eShaderReadOnlyOptimal;
            sceneRequest.encoding = DumpEncoding::eToneMap;

            DumpRequest brightRequest = sceneRequest;
            brightRequest.label = "scene-bright";
            brightRequest.image = scenePass->offscreenImages()[frame * 2 + 1];

            DumpRequest bloomRequest = sceneRequest;
            bloomRequest.label = "bloom-final";
            const uint32_t finalPass = (kBlurPasses - 1) % 2;
            bloomRequest.image = blurPasses[finalPass]->offscreenImages()[frame];
            bloomRequest.extent = blurPasses[finalPass]->extent();

            const auto results = DumpImages(readback, {frameRequest, sceneRequest, brightRequest, bloomRequest},
                                            "dump", 0);
            for (const DumpResult &result : results) {
                std::cout << "[dump] " << result.path.string() << " mean=(" << result.meanColor.r << ","
                          << result.meanColor.g << "," << result.meanColor.b << ")\n";
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
