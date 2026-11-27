// physically_based_bloom: the 2022 "physically based bloom" (COD / SIGGRAPH 2014
// progressive downsample + tent upsample) next to the 2017 threshold + 10-pass
// Gaussian, migrated from LearnOpenGL/src/8.guest/2022/6.physically_based_bloom.
//
// The reference has three modes selected with keys 1/2/3:
//   1  no bloom
//   2  threshold + five horizontal + five vertical 9-tap Gaussian, additive
//   3  six-level mip chain: 13-tap downsample (Karis average on the first level)
//      followed by tent upsampling, then mix(hdr, bloom, 0.04)
//
// Everything runs on the same scene so the two approaches can be diffed pixel by
// pixel; --mode picks the branch at run time (the reference does it with keys).
//
// Two deliberate differences from the reference, both documented in DESIGN.md:
//   * The mip chain is driven by the *unthresholded* scene colour (the reference
//     passes its thresholded BrightColor buffer while its own comment says
//     "unthresholded").  With 4% strength, mix() over a thresholded buffer would
//     darken the whole scene instead of adding a halo.  --threshold-input
//     reproduces the reference's exact wiring.
//   * The upsample sum is written explicitly (up[k] = mip[k] + tent(coarse))
//     instead of using additive blending into the downsample target, because a
//     render_demo pass owns its attachments and cannot reuse another pass' image
//     as a colour target.
//
// Command line:
//   --mode <0|1|2>     0 none, 1 old Gaussian, 2 mip chain (default 2)
//   --exposure <f>     tone-map exposure (default 1.0)
//   --strength <f>     mode 2 mix factor (default 0.04)
//   --radius <f>       upsample tent radius in texture units (default 0.005)
//   --mips <n>         mip levels (default 6, clamped to 1..6)
//   --threshold-input  feed the thresholded buffer to the mip chain (reference wiring)
//   --debug-bloom      show the bloom buffer instead of the composite
//   --dump / --dump-after <n>   capture once (frame, scene, bright, bloom, mips)
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

#include "shader/blur_frag.hpp"
#include "shader/composite_frag.hpp"
#include "shader/downsample_frag.hpp"
#include "shader/scene_frag.hpp"
#include "shader/scene_vert.hpp"
#include "shader/screen_vert.hpp"
#include "shader/upsample_frag.hpp"

namespace {

constexpr float kNearPlane = 0.1f;
constexpr float kFarPlane = 100.0f;
// The reference always builds six mips; --mips can lower it for experiments but
// never raises it above this, because the pipeline count is a compile-time array.
constexpr uint32_t kMaxMips = 6;
// Five horizontal + five vertical passes, matching `amount = 10`.
constexpr uint32_t kOldBlurPasses = 10;

struct SceneBlock {
    alignas(16) glm::mat4 viewProj{1.0f};
    alignas(16) glm::vec4 eye{0.0f};
    alignas(16) glm::vec4 params{0.0f};
};
static_assert(sizeof(SceneBlock) == 96, "SceneBlock must match the std140 BloomBlock");

// Shared parameter block of the downsample / upsample / blur passes.
struct MipBlock {
    alignas(16) glm::vec4 params{0.0f}; // xy = src resolution, z = radius, w = mip level
};
static_assert(sizeof(MipBlock) == 16, "MipBlock must match the std140 block");

struct CompositeBlock {
    alignas(16) glm::vec4 params{0.0f}; // x = exposure, y = mode, z = strength, w = debug
};
static_assert(sizeof(CompositeBlock) == 16, "CompositeBlock must match the std140 block");

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
    SceneInstanceComponent::eInstances, vk::DescriptorType::eStorageBuffer, 1, vk::ShaderStageFlagBits::eVertex});

// Post-process passes: set 0 = parameters, set 1 = (base) source, set 2 = coarse
// source for the upsample.  The downsample and blur shaders simply ignore set 2,
// but their pipelines still bind it, so one layout serves all of them.
enum class MipComponent { eParams, eSource, eCoarse };
using MipConfig = DescriptorComponentSpec<MipComponent>;
inline constexpr auto MipComponentTable = std::make_tuple(
    MipConfig{MipComponent::eParams, vk::DescriptorType::eUniformBuffer, 1, vk::ShaderStageFlagBits::eFragment},
    MipConfig{MipComponent::eSource, vk::DescriptorType::eCombinedImageSampler, 1,
              vk::ShaderStageFlagBits::eFragment},
    MipConfig{MipComponent::eCoarse, vk::DescriptorType::eCombinedImageSampler, 1,
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
    uint32_t mode = 2;
    float exposure = 1.0f;
    float strength = 0.04f;
    float radius = 0.005f;
    uint32_t mips = kMaxMips;
    bool thresholdInput = false;
    bool debugBloom = false;
};

Options parseOptions(int argc, char **argv)
{
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const bool hasValue = i + 1 < argc;
        if (arg == "--dump") {
            options.dump = true;
        } else if (arg == "--dump-after" && hasValue) {
            options.dumpAfter = static_cast<uint32_t>(std::stoul(argv[++i]));
        } else if (arg == "--mode" && hasValue) {
            options.mode = static_cast<uint32_t>(std::stoul(argv[++i]));
        } else if (arg == "--exposure" && hasValue) {
            options.exposure = std::stof(argv[++i]);
        } else if (arg == "--strength" && hasValue) {
            options.strength = std::stof(argv[++i]);
        } else if (arg == "--radius" && hasValue) {
            options.radius = std::stof(argv[++i]);
        } else if (arg == "--mips" && hasValue) {
            options.mips = static_cast<uint32_t>(std::stoul(argv[++i]));
        } else if (arg == "--threshold-input") {
            options.thresholdInput = true;
        } else if (arg == "--debug-bloom") {
            options.debugBloom = true;
        } else {
            throw std::runtime_error("physically_based_bloom: unknown argument " + arg);
        }
    }
    if (options.mode > 2) {
        throw std::runtime_error("physically_based_bloom: --mode must be 0, 1 or 2");
    }
    options.mips = std::clamp(options.mips, 1u, kMaxMips);
    options.exposure = std::clamp(options.exposure, 0.0f, 100.0f);
    options.strength = std::clamp(options.strength, 0.0f, 1.0f);
    options.radius = std::clamp(options.radius, 0.0001f, 0.1f);
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

// Two RGBA16F colour targets + depth: scene radiance and the thresholded bright
// buffer that the old Gaussian chain blurs.
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

// Single-colour offscreen target at an explicit size (one mip level).  The size
// has to be baked into the detail: BasicRenderPass derives its extent from the
// first offscreen target spec, and a mip is not swapchain sized.
class SizedOffscreenAttachmentDetail final : public IAttachmentDetail {
public:
    SizedOffscreenAttachmentDetail(vk::Format format, uint32_t width, uint32_t height) :
        m_format(format), m_width(std::max(1u, width)), m_height(std::max(1u, height))
    {
    }

    bool isSwapChainSized() const override { return false; }

    RenderPassBlueprint build(uint32_t, uint32_t, vk::SampleCountFlagBits) const override
    {
        RenderPassBlueprint bp;
        bp.sampleCount = vk::SampleCountFlagBits::e1;
        bp.swapChainSized = false;
        bp.colorAttachmentCount = 1;
        bp.framebufferCount = 2;

        vk::AttachmentDescription color{};
        color.format = m_format;
        color.samples = vk::SampleCountFlagBits::e1;
        color.loadOp = vk::AttachmentLoadOp::eClear;
        color.storeOp = vk::AttachmentStoreOp::eStore;
        color.stencilLoadOp = vk::AttachmentLoadOp::eDontCare;
        color.stencilStoreOp = vk::AttachmentStoreOp::eDontCare;
        color.initialLayout = vk::ImageLayout::eUndefined;
        color.finalLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
        bp.attachments.push_back(color);

        SubpassBlueprint subpass;
        subpass.colorAttachments.push_back(vk::AttachmentReference(0, vk::ImageLayout::eColorAttachmentOptimal));
        bp.subpasses.push_back(subpass);
        bp.dependencies = makeDependencies(DependencyPreset::eOffscreen);

        OffscreenTargetSpec target;
        target.attachmentIndex = 0;
        target.format = m_format;
        target.fixedSize = true;
        target.width = m_width;
        target.height = m_height;
        bp.offscreenTargets.push_back(target);

        bp.clearValues.push_back(vk::ClearValue(vk::ClearColorValue(std::array<float, 4>{0.0f, 0.0f, 0.0f, 0.0f})));
        return bp;
    }

private:
    vk::Format m_format;
    uint32_t m_width;
    uint32_t m_height;
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
        throw std::runtime_error("physically_based_bloom: failed to create the post sampler");
    }
    return std::shared_ptr<vk::Sampler>(new vk::Sampler(sampler), [device](vk::Sampler *pointer) {
        if (pointer) {
            device->destroySampler(*pointer, nullptr);
        }
        delete pointer;
    });
}

} // namespace

void test(int argc, char **argv)
{
    const Options options = parseOptions(argc, argv);
    const uint32_t mipCount = options.mips;

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

    const vk::Extent2D swapExtent = *mgr->getSwapChain()->getExtent2D();
    std::array<vk::Extent2D, kMaxMips> mipExtent{};
    {
        uint32_t w = std::max(1u, swapExtent.width / 2);
        uint32_t h = std::max(1u, swapExtent.height / 2);
        for (uint32_t level = 0; level < kMaxMips; ++level) {
            mipExtent[level] = vk::Extent2D(w, h);
            w = std::max(1u, w / 2);
            h = std::max(1u, h / 2);
        }
    }

    std::cout << "[pbr_bloom] mode " << options.mode << " (" << (options.mode == 0   ? "off"
                                                                : options.mode == 1 ? "old 10-pass Gaussian"
                                                                                    : "mip chain")
              << "), " << mipCount << " mips from " << mipExtent[0].width << "x" << mipExtent[0].height
              << ", exposure " << options.exposure << ", strength " << options.strength
              << (options.thresholdInput ? ", thresholded mip input" : "") << "\n";

    // ----- passes -----------------------------------------------------------
    auto scenePass = mgr->CreateRenderPass(std::make_shared<BloomSceneAttachmentDetail>(hdrFormat, depthFormat));
    scenePass->createFramebuffers({}, scenePass->extent());
    if (scenePass->offscreenColorCountPerSet() != 2) {
        throw std::runtime_error("pbr_bloom: scene pass must expose scene + bright targets");
    }

    // Old-path ping-pong (full resolution).
    std::array<std::shared_ptr<IRenderPass>, 2> blurPasses;
    for (uint32_t i = 0; i < blurPasses.size(); ++i) {
        blurPasses[i] = mgr->CreateRenderPass(std::make_shared<SimpleAttachmentDetail>(
            SimpleAttachmentDetail::Mode::eOffscreen, hdrFormat, vk::Format::eUndefined,
            vk::ImageLayout::eShaderReadOnlyOptimal, DependencyPreset::eOffscreen));
        blurPasses[i]->createFramebuffers({}, blurPasses[i]->extent());
    }

    // Mip chain: one pass per downsample target and one per upsample target.
    std::array<std::shared_ptr<IRenderPass>, kMaxMips> mipPasses{};
    std::array<std::shared_ptr<IRenderPass>, kMaxMips> upPasses{};
    for (uint32_t level = 0; level < mipCount; ++level) {
        mipPasses[level] = mgr->CreateRenderPass(
            std::make_shared<SizedOffscreenAttachmentDetail>(hdrFormat, mipExtent[level].width, mipExtent[level].height));
        mipPasses[level]->createFramebuffers({}, mipPasses[level]->extent());
        if (level + 1 < mipCount) {
            upPasses[level] = mgr->CreateRenderPass(std::make_shared<SizedOffscreenAttachmentDetail>(
                hdrFormat, mipExtent[level].width, mipExtent[level].height));
            upPasses[level]->createFramebuffers({}, upPasses[level]->extent());
        }
    }

    auto screenPass = mgr->CreateRenderPass(std::make_shared<SimpleAttachmentDetail>(
        SimpleAttachmentDetail::Mode::eDirect, *mgr->getSwapChain()->getImageFormat(), depthFormat));

    // ----- layouts ----------------------------------------------------------
    auto sceneLayout = std::make_shared<DescriptorLayoutAtlas>(MAX_FRAMES_IN_FLIGHT);
    auto sceneBlockSet = DescriptorLayoutAtlas::makeSet<SceneComponent, SceneComponentTable>(
        0, {SceneComponent::eBlock}, UpdateFrequency::eEachFrame, false);
    sceneBlockSet.specs[0] = GBufferSpec{1, static_cast<uint32_t>(sizeof(SceneBlock)), DataTypes::eCustom};
    auto sceneInstanceSet = DescriptorLayoutAtlas::makeSet<SceneInstanceComponent, SceneInstanceComponentTable>(
        1, {SceneInstanceComponent::eInstances}, UpdateFrequency::eStatic, true);
    sceneInstanceSet.specs[0] = GBufferSpec{instanceCount, static_cast<uint32_t>(sizeof(Instance)),
                                            DataTypes::eCustom};
    sceneLayout->setDetails({sceneBlockSet, sceneInstanceSet});

    auto mipLayout = std::make_shared<DescriptorLayoutAtlas>(MAX_FRAMES_IN_FLIGHT);
    auto mipParamsSet = DescriptorLayoutAtlas::makeSet<MipComponent, MipComponentTable>(
        0, {MipComponent::eParams}, UpdateFrequency::eEachFrame, false);
    mipParamsSet.specs[0] = GBufferSpec{1, static_cast<uint32_t>(sizeof(MipBlock)), DataTypes::eCustom};
    auto mipSourceSet = DescriptorLayoutAtlas::makeSet<MipComponent, MipComponentTable>(
        1, {MipComponent::eSource}, UpdateFrequency::eEachFrame, false);
    auto mipCoarseSet = DescriptorLayoutAtlas::makeSet<MipComponent, MipComponentTable>(
        2, {MipComponent::eCoarse}, UpdateFrequency::eEachFrame, false);
    mipLayout->setDetails({mipParamsSet, mipSourceSet, mipCoarseSet});

    auto compositeLayout = std::make_shared<DescriptorLayoutAtlas>(MAX_FRAMES_IN_FLIGHT);
    auto compositeParamsSet = DescriptorLayoutAtlas::makeSet<CompositeComponent, CompositeComponentTable>(
        0, {CompositeComponent::eParams}, UpdateFrequency::eEachFrame, false);
    compositeParamsSet.specs[0] = GBufferSpec{1, static_cast<uint32_t>(sizeof(CompositeBlock)),
                                             DataTypes::eCustom};
    auto compositeSceneSet = DescriptorLayoutAtlas::makeSet<CompositeComponent, CompositeComponentTable>(
        1, {CompositeComponent::eScene}, UpdateFrequency::eEachFrame, false);
    auto compositeBloomSet = DescriptorLayoutAtlas::makeSet<CompositeComponent, CompositeComponentTable>(
        2, {CompositeComponent::eBloom}, UpdateFrequency::eEachFrame, false);
    compositeLayout->setDetails({compositeParamsSet, compositeSceneSet, compositeBloomSet});

    // ----- assets -----------------------------------------------------------
    auto sceneAsset = mgr->CreateShaderAssetGroup(sceneLayout);
    sceneAsset->addShaderSpirvCode("vert", std::vector<uint32_t>(shader::SCENE_VERT_SPV.begin(),
                                                                 shader::SCENE_VERT_SPV.end()));
    sceneAsset->addShaderSpirvCode("frag", std::vector<uint32_t>(shader::SCENE_FRAG_SPV.begin(),
                                                                 shader::SCENE_FRAG_SPV.end()));

    std::array<std::shared_ptr<ShaderAssetGroup>, kMaxMips> downAssets{};
    std::array<std::shared_ptr<ShaderAssetGroup>, kMaxMips> upAssets{};
    for (uint32_t level = 0; level < mipCount; ++level) {
        downAssets[level] = mgr->CreateShaderAssetGroup(mipLayout);
        downAssets[level]->addShaderSpirvCode("vert", std::vector<uint32_t>(shader::SCREEN_VERT_SPV.begin(),
                                                                            shader::SCREEN_VERT_SPV.end()));
        downAssets[level]->addShaderSpirvCode("frag",
                                              std::vector<uint32_t>(shader::DOWNSAMPLE_FRAG_SPV.begin(),
                                                                    shader::DOWNSAMPLE_FRAG_SPV.end()));
        if (level + 1 < mipCount) {
            upAssets[level] = mgr->CreateShaderAssetGroup(mipLayout);
            upAssets[level]->addShaderSpirvCode("vert", std::vector<uint32_t>(shader::SCREEN_VERT_SPV.begin(),
                                                                              shader::SCREEN_VERT_SPV.end()));
            upAssets[level]->addShaderSpirvCode("frag", std::vector<uint32_t>(shader::UPSAMPLE_FRAG_SPV.begin(),
                                                                              shader::UPSAMPLE_FRAG_SPV.end()));
        }
    }

    std::array<std::shared_ptr<ShaderAssetGroup>, kOldBlurPasses> blurAssets{};
    for (uint32_t pass = 0; pass < kOldBlurPasses; ++pass) {
        blurAssets[pass] = mgr->CreateShaderAssetGroup(mipLayout);
        blurAssets[pass]->addShaderSpirvCode("vert", std::vector<uint32_t>(shader::SCREEN_VERT_SPV.begin(),
                                                                           shader::SCREEN_VERT_SPV.end()));
        blurAssets[pass]->addShaderSpirvCode("frag", std::vector<uint32_t>(shader::BLUR_FRAG_SPV.begin(),
                                                                          shader::BLUR_FRAG_SPV.end()));
    }

    auto compositeAsset = mgr->CreateShaderAssetGroup(compositeLayout);
    compositeAsset->addShaderSpirvCode("vert", std::vector<uint32_t>(shader::SCREEN_VERT_SPV.begin(),
                                                                     shader::SCREEN_VERT_SPV.end()));
    compositeAsset->addShaderSpirvCode("frag", std::vector<uint32_t>(shader::COMPOSITE_FRAG_SPV.begin(),
                                                                     shader::COMPOSITE_FRAG_SPV.end()));

    // Descriptor sets are written once at pipeline build time, so every view a
    // pass will ever sample has to be bound right here.
    auto sampler = makeLinearSampler(mgr->getLogicalDevice());
    const auto bindView = [&](const std::shared_ptr<ShaderAssetGroup> &asset, uint32_t set, uint32_t frame,
                              vk::ImageView view) {
        auto combine = std::shared_ptr<TextureSamplerCombine>(
            new TextureSamplerCombine(view, *sampler, vk::ImageLayout::eShaderReadOnlyOptimal),
            [](TextureSamplerCombine *pointer) { delete pointer; });
        asset->addDescriptorResource(ResourceKey::make_key(ResourceId(set, 0), frame), combine);
    };

    // The mip chain input: the full scene radiance, or -- to reproduce the
    // reference's wiring -- the thresholded bright buffer.
    const vk::ImageLayout unused = vk::ImageLayout::eUndefined;
    (void)unused;

    for (uint32_t frame = 0; frame < MAX_FRAMES_IN_FLIGHT; ++frame) {
        const std::vector<vk::ImageView> &sceneViews = scenePass->offscreenViews();
        const vk::ImageView sceneColor = sceneViews[frame * 2];
        const vk::ImageView sceneBright = sceneViews[frame * 2 + 1];

        bindView(compositeAsset, 1, frame, sceneColor);

        // Old path: pass 0 samples the bright target, pass i samples the previous
        // ping-pong result (the reference's first_iteration branch).
        for (uint32_t pass = 0; pass < kOldBlurPasses; ++pass) {
            const vk::ImageView source =
                pass == 0 ? sceneBright : blurPasses[(pass - 1) % 2]->offscreenViews()[frame];
            bindView(blurAssets[pass], 1, frame, source);
        }

        // Downsample: level 0 reads the scene, level k reads mip k-1.
        for (uint32_t level = 0; level < mipCount; ++level) {
            const vk::ImageView source =
                level == 0 ? (options.thresholdInput ? sceneBright : sceneColor)
                           : mipPasses[level - 1]->offscreenViews()[frame];
            bindView(downAssets[level], 1, frame, source);
            // Set 2 is unused by the downsample shader; bind a valid view so the
            // descriptor set is complete.
            bindView(downAssets[level], 2, frame, mipPasses[0]->offscreenViews()[frame]);
        }

        // Upsample: base = the matching downsample level, coarse = the next
        // coarser upsample result (or the coarsest mip for the first step).
        for (uint32_t level = 0; level + 1 < mipCount; ++level) {
            bindView(upAssets[level], 1, frame, mipPasses[level]->offscreenViews()[frame]);
            const vk::ImageView coarse = (level + 2 == mipCount)
                                             ? mipPasses[mipCount - 1]->offscreenViews()[frame]
                                             : upPasses[level + 1]->offscreenViews()[frame];
            bindView(upAssets[level], 2, frame, coarse);
        }

        // Composite reads the best available bloom result for the selected mode.
        const vk::ImageView bloomView = options.mode == 1
                                            ? blurPasses[(kOldBlurPasses - 1) % 2]->offscreenViews()[frame]
                                            : upPasses[0]->offscreenViews()[frame];
        bindView(compositeAsset, 2, frame, bloomView);
    }

    // ----- pipelines --------------------------------------------------------
    auto sceneVertexInput = std::make_shared<VertexInputAtlas>(
        0,
        std::vector<VertexComponent>{VertexComponent::ePosition, VertexComponent::eNormal, VertexComponent::eUV},
        static_cast<uint32_t>(vertices.size()), 0u, DataTypes::eUnsignedInt, false, 0u, instanceCount);
    GraphicsPipelineConfig sceneConfig;
    sceneConfig.cullMode = vk::CullModeFlagBits::eNone;
    auto scenePipeline = mgr->CreatePipeline(sceneVertexInput, sceneLayout, sceneAsset, scenePass, 0, sceneConfig);

    auto emptyVertexInput = std::make_shared<VertexInputAtlas>(0, std::vector<VertexComponent>{}, 0u, 0u,
                                                               DataTypes::eUnsignedInt, true, 0u);
    GraphicsPipelineConfig postConfig;
    postConfig.depthTestEnable = false;
    postConfig.depthWriteEnable = false;

    std::array<std::shared_ptr<GraphicsPipeline>, kMaxMips> downPipelines{};
    std::array<std::shared_ptr<GraphicsPipeline>, kMaxMips> upPipelines{};
    for (uint32_t level = 0; level < mipCount; ++level) {
        downPipelines[level] =
            mgr->CreatePipeline(emptyVertexInput, mipLayout, downAssets[level], mipPasses[level], 0, postConfig);
        if (level + 1 < mipCount) {
            upPipelines[level] =
                mgr->CreatePipeline(emptyVertexInput, mipLayout, upAssets[level], upPasses[level], 0, postConfig);
        }
    }

    std::array<std::shared_ptr<GraphicsPipeline>, kOldBlurPasses> blurPipelines{};
    for (uint32_t pass = 0; pass < kOldBlurPasses; ++pass) {
        blurPipelines[pass] = mgr->CreatePipeline(emptyVertexInput, mipLayout, blurAssets[pass],
                                                  blurPasses[pass % 2], 0, postConfig);
    }

    auto compositePipeline =
        mgr->CreatePipeline(emptyVertexInput, compositeLayout, compositeAsset, screenPass, 0, postConfig);

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
    auto compositeDraw = std::make_shared<CmdOpDrawFrame>(compositePipeline);
    std::array<std::shared_ptr<CmdOpDrawFrame>, kMaxMips> downDraws{};
    std::array<std::shared_ptr<CmdOpDrawFrame>, kMaxMips> upDraws{};
    for (uint32_t level = 0; level < mipCount; ++level) {
        downDraws[level] = std::make_shared<CmdOpDrawFrame>(downPipelines[level]);
        if (level + 1 < mipCount) {
            upDraws[level] = std::make_shared<CmdOpDrawFrame>(upPipelines[level]);
        }
    }
    std::array<std::shared_ptr<CmdOpDrawFrame>, kOldBlurPasses> blurDraws{};
    for (uint32_t pass = 0; pass < kOldBlurPasses; ++pass) {
        blurDraws[pass] = std::make_shared<CmdOpDrawFrame>(blurPipelines[pass]);
    }

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

        SceneBlock sceneUbo;
        sceneUbo.viewProj = projection * view;
        sceneUbo.eye = glm::vec4(camera->Position, 1.0f);
        // Only the old path consumes the thresholded buffer; mode 2 wants the
        // full radiance unless --threshold-input asks for the reference wiring.
        const bool wantBright = options.mode == 1 || (options.mode == 2 && options.thresholdInput);
        sceneUbo.params = glm::vec4(wantBright ? 1.0f : 0.0f, options.debugBloom ? 1.0f : 0.0f, 0.0f, 0.0f);

        CompositeBlock compositeUbo;
        compositeUbo.params = glm::vec4(options.exposure, static_cast<float>(options.mode), options.strength,
                                        options.debugBloom ? 1.0f : 0.0f);

        sceneDraw->OnUpdateRawData(ResourceId(0, 0), frame, sceneUbo);
        compositeDraw->OnUpdateRawData(ResourceId(0, 0), frame, compositeUbo);

        // Downsample parameters: level 0 applies the Karis average, the rest use
        // the plain 13-tap weights.
        for (uint32_t level = 0; level < mipCount; ++level) {
            const vk::Extent2D sourceExtent =
                level == 0 ? extent : mipPasses[level - 1]->extent();
            MipBlock mipUbo;
            mipUbo.params = glm::vec4(static_cast<float>(sourceExtent.width),
                                      static_cast<float>(sourceExtent.height), options.radius,
                                      level == 0 ? 0.0f : 1.0f);
            downDraws[level]->OnUpdateRawData(ResourceId(0, 0), frame, mipUbo);
            if (level + 1 < mipCount) {
                // The upsample only reads z (radius); w and xy are informational.
                MipBlock upUbo;
                upUbo.params = glm::vec4(static_cast<float>(mipExtent[level + 1].width),
                                         static_cast<float>(mipExtent[level + 1].height), options.radius,
                                         static_cast<float>(level + 1));
                upDraws[level]->OnUpdateRawData(ResourceId(0, 0), frame, upUbo);
            }
        }

        for (uint32_t pass = 0; pass < kOldBlurPasses; ++pass) {
            MipBlock blurUbo;
            blurUbo.params = glm::vec4(pass % 2 == 0 ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f);
            blurDraws[pass]->OnUpdateRawData(ResourceId(0, 0), frame, blurUbo);
        }

        sequence->beginRecording();
        sequence->record(sceneFrameSync);
        sequence->record(compositeFrameSync);
        sequence->recordPass(scenePass, [&]() { sequence->record(sceneDraw); });

        if (options.mode == 1) {
            for (uint32_t pass = 0; pass < kOldBlurPasses; ++pass) {
                sequence->recordPass(blurPasses[pass % 2], [&, pass]() { sequence->record(blurDraws[pass]); });
            }
        } else if (options.mode == 2) {
            for (uint32_t level = 0; level < mipCount; ++level) {
                sequence->recordPass(mipPasses[level], [&, level]() { sequence->record(downDraws[level]); });
            }
            // Coarsest first: each upsample needs the level above it.
            for (uint32_t level = mipCount - 1; level-- > 0;) {
                sequence->recordPass(upPasses[level], [&, level]() { sequence->record(upDraws[level]); });
            }
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
            frameRequest.label = options.mode == 0 ? "frame-no-bloom" : options.mode == 1 ? "frame-old" : "frame-mip";
            frameRequest.image = mgr->getSwapChain()->getImages()[sequence->getImageIndex()];
            frameRequest.format = *mgr->getSwapChain()->getImageFormat();
            frameRequest.extent = extent;
            frameRequest.layout = vk::ImageLayout::ePresentSrcKHR;
            frameRequest.encoding = DumpEncoding::eColor;

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

            std::vector<DumpRequest> requests = {frameRequest, sceneRequest, brightRequest};

            if (options.mode == 1) {
                DumpRequest bloomRequest = sceneRequest;
                bloomRequest.label = "bloom-old";
                const uint32_t finalPass = (kOldBlurPasses - 1) % 2;
                bloomRequest.image = blurPasses[finalPass]->offscreenImages()[frame];
                bloomRequest.extent = blurPasses[finalPass]->extent();
                requests.push_back(bloomRequest);
            } else if (options.mode == 2) {
                DumpRequest bloomRequest = sceneRequest;
                bloomRequest.label = "bloom-mip";
                bloomRequest.image = upPasses[0]->offscreenImages()[frame];
                bloomRequest.extent = upPasses[0]->extent();
                requests.push_back(bloomRequest);
                for (uint32_t level = 0; level < mipCount; ++level) {
                    DumpRequest down = sceneRequest;
                    down.label = "mip" + std::to_string(level);
                    down.image = mipPasses[level]->offscreenImages()[frame];
                    down.extent = mipPasses[level]->extent();
                    requests.push_back(down);
                }
            }

            const auto results = DumpImages(readback, requests, "dump", 0);
            for (const DumpResult &result : results) {
                std::cout << "[dump] " << result.path.string() << " mean=(" << result.meanColor.r << ","
                          << result.meanColor.g << "," << result.meanColor.b << ")\n";
            }
            dumpDone = true;
            (void)sequence->presentFrame();
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
