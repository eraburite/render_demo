// ssao: screen-space ambient occlusion, migrated from LearnOpenGL/src/5.advanced_lighting/9.ssao.
//
// The reference is a four-pass chain: a view-space G-Buffer (position / normal /
// albedo), an SSAO pass with a 64-sample hemisphere kernel rotated per-pixel by
// a tiled 4x4 noise texture, a 4x4 box blur, and a deferred lighting pass where
// AO modulates only the ambient term.  All four passes map directly onto render_demo:
// the G-Buffer is GBufferAttachmentDetail, the post passes are offscreen quads,
// the kernel is an SSBO and the noise a small RGBA8 texture.
//
// Deliberate differences from the reference:
//   * The random seed is FIXED.  The reference uses a default-constructed
//     std::default_random_engine, which is implementation-defined and makes the
//     kernel / noise (and therefore any --dump) non-reproducible across runs.
//   * The scene is a hand-placed set of boxes rather than a loaded backpack; AO
//     is strongest in the corners where boxes meet the floor and each other.
//   * AO and blur buffers are RGBA16F with the value replicated into RGB, so the
//     existing ImageDump path can display them without a new R32F readback codec.
//   * The 800/600 hard-coded noiseScale in the reference shader becomes a UBO
//     value derived from the actual framebuffer size.
#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <memory>
#include <random>
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
#include "Geometry.h"
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
#include "shader/gbuffer_frag.hpp"
#include "shader/gbuffer_vert.hpp"
#include "shader/screen_vert.hpp"
#include "shader/ssao_frag.hpp"

namespace {

constexpr uint32_t kKernelSize = 64;
constexpr uint32_t kNoiseSize = 4;
constexpr float kNearPlane = 0.1f;
constexpr float kFarPlane = 50.0f;
constexpr float kRadius = 0.5f;
constexpr float kBias = 0.025f;

// --- UBO / SSBO layouts ----------------------------------------------------

struct CameraBlock {
    alignas(16) glm::mat4 view{1.0f};
    alignas(16) glm::mat4 proj{1.0f};
};

struct SsaoBlock {
    alignas(16) glm::mat4 projection{1.0f};
    alignas(16) glm::vec4 noiseScale{0.0f};
    alignas(16) glm::vec4 params{0.0f}; // x = radius, y = bias, z = kernel size, w = enable
};

struct LightBlock {
    alignas(16) glm::vec4 lightPosView{0.0f};
    alignas(16) glm::vec4 lightColor{0.2f, 0.2f, 0.7f, 1.0f};
    alignas(16) glm::vec4 params{0.0f}; // x = ssao enable, y = AO-only
};

// std430 vec3 stride is 16 bytes, not 12.  Padded explicitly.
struct SsaoSample {
    alignas(16) glm::vec3 value{0.0f};
};
static_assert(sizeof(SsaoSample) == 16, "std430 vec3 must have a 16-byte stride");

// --- Descriptor tables -----------------------------------------------------

enum class GBufferComponent { eCamera };
using GBufferConfig = DescriptorComponentSpec<GBufferComponent>;
inline constexpr auto GBufferComponentTable = std::make_tuple(GBufferConfig{
    GBufferComponent::eCamera, vk::DescriptorType::eUniformBuffer, 1, vk::ShaderStageFlagBits::eVertex});

// set3 uses the framework's shared instance table (eModelMatrix -> SSBO, vertex).

enum class PaletteComponent { eColors };
using PaletteConfig = DescriptorComponentSpec<PaletteComponent>;
inline constexpr auto PaletteComponentTable = std::make_tuple(PaletteConfig{
    PaletteComponent::eColors, vk::DescriptorType::eStorageBuffer, 1, vk::ShaderStageFlagBits::eVertex});

enum class SsaoBlockComponent { eBlock };
using SsaoBlockConfig = DescriptorComponentSpec<SsaoBlockComponent>;
inline constexpr auto SsaoBlockComponentTable = std::make_tuple(SsaoBlockConfig{
    SsaoBlockComponent::eBlock, vk::DescriptorType::eUniformBuffer, 1, vk::ShaderStageFlagBits::eFragment});

enum class SsaoTexComponent { ePosition, eNormal, eNoise };
using SsaoTexConfig = DescriptorComponentSpec<SsaoTexComponent>;
inline constexpr auto SsaoTexComponentTable = std::make_tuple(
    SsaoTexConfig{SsaoTexComponent::ePosition, vk::DescriptorType::eCombinedImageSampler, 1,
                  vk::ShaderStageFlagBits::eFragment},
    SsaoTexConfig{SsaoTexComponent::eNormal, vk::DescriptorType::eCombinedImageSampler, 1,
                  vk::ShaderStageFlagBits::eFragment},
    SsaoTexConfig{SsaoTexComponent::eNoise, vk::DescriptorType::eCombinedImageSampler, 1,
                  vk::ShaderStageFlagBits::eFragment});

enum class SsaoKernelComponent { eKernel };
using SsaoKernelConfig = DescriptorComponentSpec<SsaoKernelComponent>;
inline constexpr auto SsaoKernelComponentTable = std::make_tuple(SsaoKernelConfig{
    SsaoKernelComponent::eKernel, vk::DescriptorType::eStorageBuffer, 1, vk::ShaderStageFlagBits::eFragment});

enum class BlurComponent { eSource };
using BlurConfig = DescriptorComponentSpec<BlurComponent>;
inline constexpr auto BlurComponentTable = std::make_tuple(BlurConfig{
    BlurComponent::eSource, vk::DescriptorType::eCombinedImageSampler, 1, vk::ShaderStageFlagBits::eFragment});

enum class CompositeTexComponent { ePosition, eNormal, eAlbedo, eOcclusion };
using CompositeTexConfig = DescriptorComponentSpec<CompositeTexComponent>;
inline constexpr auto CompositeTexComponentTable = std::make_tuple(
    CompositeTexConfig{CompositeTexComponent::ePosition, vk::DescriptorType::eCombinedImageSampler, 1,
                       vk::ShaderStageFlagBits::eFragment},
    CompositeTexConfig{CompositeTexComponent::eNormal, vk::DescriptorType::eCombinedImageSampler, 1,
                       vk::ShaderStageFlagBits::eFragment},
    CompositeTexConfig{CompositeTexComponent::eAlbedo, vk::DescriptorType::eCombinedImageSampler, 1,
                       vk::ShaderStageFlagBits::eFragment},
    CompositeTexConfig{CompositeTexComponent::eOcclusion, vk::DescriptorType::eCombinedImageSampler, 1,
                       vk::ShaderStageFlagBits::eFragment});

enum class CompositeLightComponent { eLight };
using CompositeLightConfig = DescriptorComponentSpec<CompositeLightComponent>;
inline constexpr auto CompositeLightComponentTable = std::make_tuple(CompositeLightConfig{
    CompositeLightComponent::eLight, vk::DescriptorType::eUniformBuffer, 1, vk::ShaderStageFlagBits::eFragment});

struct Options {
    bool dump = false;
    uint32_t dumpAfter = 3;
    bool noSsao = false;
    bool debugAo = false;
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
        } else if (arg == "--no-ssao") {
            options.noSsao = true;
        } else if (arg == "--debug-ao") {
            options.debugAo = true;
        } else {
            throw std::runtime_error("ssao: unknown argument " + arg);
        }
    }
    return options;
}

// --- Kernel / noise --------------------------------------------------------

std::vector<SsaoSample> makeKernel()
{
    // Fixed seed: the reference leaves default_random_engine unseeded, so its
    // dump is not reproducible.  Distribution and hemisphere scaling are the
    // reference's, including the i/n quadratic lerp towards the origin.
    std::mt19937 generator(12345u);
    std::uniform_real_distribution<float> randomFloats(0.0f, 1.0f);
    std::vector<SsaoSample> kernel;
    kernel.reserve(kKernelSize);
    for (uint32_t i = 0; i < kKernelSize; ++i) {
        glm::vec3 sample(randomFloats(generator) * 2.0f - 1.0f, randomFloats(generator) * 2.0f - 1.0f,
                         randomFloats(generator)); // z in [0,1] => hemisphere
        sample = glm::normalize(sample);
        sample *= randomFloats(generator);
        float scale = static_cast<float>(i) / static_cast<float>(kKernelSize);
        scale = 0.1f + 0.9f * scale * scale; // ourLerp(0.1, 1, scale*scale)
        sample *= scale;
        kernel.push_back({sample});
    }
    return kernel;
}

// 4x4 noise, RGBA8.  z = 0 (rotation about the surface normal).  Encoded to
// [0,1] here and decoded with *2-1 in ssao.frag, because the sampler auto-creates
// an UNORM view.
std::vector<glm::vec4> makeNoise()
{
    std::mt19937 generator(98765u);
    std::uniform_real_distribution<float> randomFloats(0.0f, 1.0f);
    std::vector<glm::vec4> noise;
    noise.reserve(kNoiseSize * kNoiseSize);
    for (uint32_t i = 0; i < kNoiseSize * kNoiseSize; ++i) {
        const glm::vec3 n(randomFloats(generator) * 2.0f - 1.0f, randomFloats(generator) * 2.0f - 1.0f, 0.0f);
        noise.push_back(glm::vec4(n * 0.5f + 0.5f, 1.0f));
    }
    return noise;
}

// --- Scene -----------------------------------------------------------------

std::vector<glm::mat4> makeModels()
{
    std::vector<glm::mat4> models;
    {
        glm::mat4 room = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 7.0f, 0.0f));
        room = glm::scale(room, glm::vec3(15.0f, 15.0f, 15.0f));
        models.push_back(room);
    }
    struct Box {
        glm::vec3 position;
        glm::vec3 scale;
        float yawDeg;
    };
    static const Box boxes[] = {
        {glm::vec3(-2.4f, 0.9f, -1.0f), glm::vec3(1.8f, 1.8f, 1.8f), 15.0f},
        {glm::vec3( 2.2f, 0.7f, -1.6f), glm::vec3(1.4f, 1.4f, 1.4f), 55.0f},
        {glm::vec3( 0.2f, 0.5f,  1.8f), glm::vec3(1.0f, 1.0f, 1.0f), 30.0f},
        {glm::vec3(-3.0f, 0.35f,  2.6f), glm::vec3(0.7f, 0.7f, 0.7f), 70.0f},
        {glm::vec3( 3.1f, 1.3f,  2.2f), glm::vec3(2.6f, 2.6f, 2.6f), 10.0f},
        {glm::vec3(-0.6f, 0.6f, -3.4f), glm::vec3(1.2f, 1.2f, 1.2f), 45.0f},
    };
    for (const Box &box : boxes) {
        glm::mat4 model = glm::translate(glm::mat4(1.0f), box.position);
        model = glm::rotate(model, glm::radians(box.yawDeg), glm::vec3(0.0f, 1.0f, 0.0f));
        models.push_back(glm::scale(model, box.scale));
    }
    return models;
}

// One colour per instance; instance 0 is the room shell (muted grey-blue), the
// rest are distinctly coloured boxes so the AO contact edges stay legible.
std::vector<glm::vec4> makePalette()
{
    return {
        glm::vec4(0.42f, 0.45f, 0.52f, 1.0f), // room
        glm::vec4(0.85f, 0.55f, 0.35f, 1.0f),
        glm::vec4(0.40f, 0.58f, 0.78f, 1.0f),
        glm::vec4(0.60f, 0.78f, 0.48f, 1.0f),
        glm::vec4(0.80f, 0.72f, 0.42f, 1.0f),
        glm::vec4(0.72f, 0.48f, 0.64f, 1.0f),
        glm::vec4(0.45f, 0.72f, 0.72f, 1.0f),
    };
}

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
        throw std::runtime_error("ssao: failed to create a sampler");
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

    auto camera = std::make_shared<Camera>(glm::vec3(0.0f, 0.0f, 7.0f));
    camera->MovementSpeed = 6.0f;
    GLFWwindow window(1280, 720, camera);
    auto mgr = RenderManager::Create(window, MAX_FRAMES_IN_FLIGHT);

    const vk::Format depthFormat = mgr->DefaultDepthFormat();
    const vk::Format aoFormat = vk::Format::eR16G16B16A16Sfloat;
    const auto vertices = CubeVertices(glm::vec3(0.95f));
    const auto models = makeModels();
    const auto palette = makePalette();
    const auto kernel = makeKernel();
    const auto noise = makeNoise();
    const uint32_t vertexCount = static_cast<uint32_t>(vertices.size());
    const uint32_t instanceCount = static_cast<uint32_t>(models.size());

    std::cout << "[ssao] " << instanceCount << " instances, " << kKernelSize << " kernel samples, 4x4 noise\n";

    const vk::Extent2D extent = *mgr->getSwapChain()->getExtent2D();

    // ----- render passes ---------------------------------------------------
    // Fixed-size offscreen passes: GBuffer (3 MRT) -> SSAO -> blur -> screen.
    auto gbufferPass = mgr->CreateRenderPass(std::make_shared<GBufferAttachmentDetail>(
        *mgr->getSwapChain()->getImageFormat(), depthFormat, extent.width, extent.height));
    gbufferPass->createFramebuffers({}, gbufferPass->extent());
    if (gbufferPass->offscreenColorCountPerSet() != 3) {
        throw std::runtime_error("ssao: GBuffer pass did not expose three color targets");
    }
    auto ssaoPass = mgr->CreateRenderPass(std::make_shared<SimpleAttachmentDetail>(
        SimpleAttachmentDetail::Mode::eOffscreen, aoFormat, vk::Format::eUndefined,
        vk::ImageLayout::eShaderReadOnlyOptimal, DependencyPreset::eOffscreen));
    ssaoPass->createFramebuffers({}, ssaoPass->extent());
    auto blurPass = mgr->CreateRenderPass(std::make_shared<SimpleAttachmentDetail>(
        SimpleAttachmentDetail::Mode::eOffscreen, aoFormat, vk::Format::eUndefined,
        vk::ImageLayout::eShaderReadOnlyOptimal, DependencyPreset::eOffscreen));
    blurPass->createFramebuffers({}, blurPass->extent());
    auto screenPass = mgr->CreateRenderPass(std::make_shared<SimpleAttachmentDetail>(
        SimpleAttachmentDetail::Mode::eDirect, *mgr->getSwapChain()->getImageFormat(), depthFormat));

    // ----- descriptor layouts ---------------------------------------------
    // GBuffer: set1 camera UBO, set3 instance SSBO (empty set0 placeholder).
    auto gbufferLayout = std::make_shared<DescriptorLayoutAtlas>(MAX_FRAMES_IN_FLIGHT);
    auto gEmpty0 = DescriptorLayoutAtlas::makeSet<GBufferComponent, GBufferComponentTable>(
        0, {}, UpdateFrequency::eEachFrame, false);
    auto gCameraSet = DescriptorLayoutAtlas::makeSet<GBufferComponent, GBufferComponentTable>(
        1, {GBufferComponent::eCamera}, UpdateFrequency::eEachFrame, false);
    gCameraSet.specs[0] = GBufferSpec{1, static_cast<uint32_t>(sizeof(CameraBlock)), DataTypes::eCustom};
    auto gPaletteSet = DescriptorLayoutAtlas::makeSet<PaletteComponent, PaletteComponentTable>(
        2, {PaletteComponent::eColors}, UpdateFrequency::eStatic, true);
    gPaletteSet.specs[0] = GBufferSpec{instanceCount, static_cast<uint32_t>(sizeof(glm::vec4)), DataTypes::eCustom};
    auto gInstanceSet = DescriptorLayoutAtlas::makeSet<InstanceComponent, InstanceComponentTable>(
        3, {InstanceComponent::eModelMatrix}, UpdateFrequency::eStatic, true);
    gInstanceSet.specs[0] =
        GBufferSpec{instanceCount, static_cast<uint32_t>(sizeof(glm::mat4)), DataTypes::eCustom};
    gbufferLayout->setDetails({gEmpty0, gCameraSet, gPaletteSet, gInstanceSet});

    // SSAO: set0 UBO, set1 three samplers, set2 kernel SSBO.
    auto ssaoLayout = std::make_shared<DescriptorLayoutAtlas>(MAX_FRAMES_IN_FLIGHT);
    auto ssaoBlockSet = DescriptorLayoutAtlas::makeSet<SsaoBlockComponent, SsaoBlockComponentTable>(
        0, {SsaoBlockComponent::eBlock}, UpdateFrequency::eEachFrame, false);
    ssaoBlockSet.specs[0] = GBufferSpec{1, static_cast<uint32_t>(sizeof(SsaoBlock)), DataTypes::eCustom};
    auto ssaoTexSet = DescriptorLayoutAtlas::makeSet<SsaoTexComponent, SsaoTexComponentTable>(
        1, {SsaoTexComponent::ePosition, SsaoTexComponent::eNormal, SsaoTexComponent::eNoise},
        UpdateFrequency::eEachFrame, false);
    // Position / normal stay GBufferSpec (views injected by hand); noise gets a
    // real 4x4 RGBA8 UNORM texture with repeat addressing.
    TextureImageSpec noiseSpec{kNoiseSize, kNoiseSize, 4, 1};
    noiseSpec.format = vk::Format::eR8G8B8A8Unorm;
    noiseSpec.addressMode = vk::SamplerAddressMode::eRepeat;
    ssaoTexSet.specs[2] = noiseSpec;
    auto ssaoKernelSet = DescriptorLayoutAtlas::makeSet<SsaoKernelComponent, SsaoKernelComponentTable>(
        2, {SsaoKernelComponent::eKernel}, UpdateFrequency::eStatic, true);
    ssaoKernelSet.specs[0] =
        GBufferSpec{kKernelSize, static_cast<uint32_t>(sizeof(SsaoSample)), DataTypes::eCustom};
    ssaoLayout->setDetails({ssaoBlockSet, ssaoTexSet, ssaoKernelSet});

    // Blur: set0 source sampler.
    auto blurLayout = std::make_shared<DescriptorLayoutAtlas>(MAX_FRAMES_IN_FLIGHT);
    auto blurSet = DescriptorLayoutAtlas::makeSet<BlurComponent, BlurComponentTable>(
        0, {BlurComponent::eSource}, UpdateFrequency::eEachFrame, false);
    blurLayout->setDetails({blurSet});

    // Composite: set0 four samplers, set1 light UBO.
    auto compositeLayout = std::make_shared<DescriptorLayoutAtlas>(MAX_FRAMES_IN_FLIGHT);
    auto compositeTexSet = DescriptorLayoutAtlas::makeSet<CompositeTexComponent, CompositeTexComponentTable>(
        0, {CompositeTexComponent::ePosition, CompositeTexComponent::eNormal, CompositeTexComponent::eAlbedo,
            CompositeTexComponent::eOcclusion},
        UpdateFrequency::eEachFrame, false);
    auto compositeLightSet = DescriptorLayoutAtlas::makeSet<CompositeLightComponent, CompositeLightComponentTable>(
        1, {CompositeLightComponent::eLight}, UpdateFrequency::eEachFrame, false);
    compositeLightSet.specs[0] = GBufferSpec{1, static_cast<uint32_t>(sizeof(LightBlock)), DataTypes::eCustom};
    compositeLayout->setDetails({compositeTexSet, compositeLightSet});

    // ----- assets ----------------------------------------------------------
    auto gbufferAsset = mgr->CreateShaderAssetGroup(gbufferLayout);
    gbufferAsset->addShaderSpirvCode("vert", std::vector<uint32_t>(shader::GBUFFER_VERT_SPV.begin(),
                                                                   shader::GBUFFER_VERT_SPV.end()));
    gbufferAsset->addShaderSpirvCode("frag", std::vector<uint32_t>(shader::GBUFFER_FRAG_SPV.begin(),
                                                                   shader::GBUFFER_FRAG_SPV.end()));
    auto ssaoAsset = mgr->CreateShaderAssetGroup(ssaoLayout);
    ssaoAsset->addShaderSpirvCode("vert", std::vector<uint32_t>(shader::SCREEN_VERT_SPV.begin(),
                                                                shader::SCREEN_VERT_SPV.end()));
    ssaoAsset->addShaderSpirvCode("frag", std::vector<uint32_t>(shader::SSAO_FRAG_SPV.begin(),
                                                                shader::SSAO_FRAG_SPV.end()));
    auto blurAsset = mgr->CreateShaderAssetGroup(blurLayout);
    blurAsset->addShaderSpirvCode("vert", std::vector<uint32_t>(shader::SCREEN_VERT_SPV.begin(),
                                                                shader::SCREEN_VERT_SPV.end()));
    blurAsset->addShaderSpirvCode("frag", std::vector<uint32_t>(shader::BLUR_FRAG_SPV.begin(),
                                                                shader::BLUR_FRAG_SPV.end()));
    auto compositeAsset = mgr->CreateShaderAssetGroup(compositeLayout);
    compositeAsset->addShaderSpirvCode("vert", std::vector<uint32_t>(shader::SCREEN_VERT_SPV.begin(),
                                                                     shader::SCREEN_VERT_SPV.end()));
    compositeAsset->addShaderSpirvCode("frag", std::vector<uint32_t>(shader::COMPOSITE_FRAG_SPV.begin(),
                                                                     shader::COMPOSITE_FRAG_SPV.end()));

    // Cross-pass sampled views must be bound before CreatePipeline.
    auto sampler = makeLinearSampler(mgr->getLogicalDevice());
    const auto bindView = [&](const std::shared_ptr<ShaderAssetGroup> &asset, uint32_t set, uint32_t binding,
                              uint32_t frame, vk::ImageView view) {
        auto combine = std::shared_ptr<TextureSamplerCombine>(
            new TextureSamplerCombine(view, *sampler, vk::ImageLayout::eShaderReadOnlyOptimal),
            [sampler](TextureSamplerCombine *pointer) { delete pointer; });
        asset->addDescriptorResource(ResourceKey::make_key(ResourceId(set, binding), frame), combine);
    };

    const uint32_t gbufferSets = gbufferPass->offscreenViews().size() / 3;
    for (uint32_t frame = 0; frame < MAX_FRAMES_IN_FLIGHT; ++frame) {
        const uint32_t slot = frame % gbufferSets;
        const auto &gbufferViews = gbufferPass->offscreenViews();
        // SSAO pass samples position (slot 0) and normal (slot 1).
        bindView(ssaoAsset, 1, 0, frame, gbufferViews[slot * 3 + 0]);
        bindView(ssaoAsset, 1, 1, frame, gbufferViews[slot * 3 + 1]);
        // Blur pass samples the raw SSAO output.
        bindView(blurAsset, 0, 0, frame, ssaoPass->offscreenViews()[frame]);
        // Composite samples all three GBuffer targets and the blurred AO.
        bindView(compositeAsset, 0, 0, frame, gbufferViews[slot * 3 + 0]);
        bindView(compositeAsset, 0, 1, frame, gbufferViews[slot * 3 + 1]);
        bindView(compositeAsset, 0, 2, frame, gbufferViews[slot * 3 + 2]);
        bindView(compositeAsset, 0, 3, frame, blurPass->offscreenViews()[frame]);
    }

    // ----- pipelines -------------------------------------------------------
    auto vertexInput = std::make_shared<VertexInputAtlas>(
        0,
        std::vector<VertexComponent>{VertexComponent::ePosition, VertexComponent::eColor, VertexComponent::eUV,
                                     VertexComponent::eNormal},
        vertexCount, 0u, DataTypes::eUnsignedInt, false, 0u, instanceCount);
    // The reference flips the room's normals to render the inside; our cube is
    // seen from inside with no culling, so disable cull for the GBuffer pass.
    GraphicsPipelineConfig gbufferConfig;
    gbufferConfig.cullMode = vk::CullModeFlagBits::eNone;
    auto gbufferPipeline = mgr->CreatePipeline(vertexInput, gbufferLayout, gbufferAsset, gbufferPass, 0, gbufferConfig);

    auto emptyVertexInput = std::make_shared<VertexInputAtlas>(0, std::vector<VertexComponent>{}, 0u, 0u,
                                                               DataTypes::eUnsignedInt, true, 0u);
    GraphicsPipelineConfig screenConfig;
    screenConfig.depthTestEnable = false;
    screenConfig.depthWriteEnable = false;
    screenConfig.cullMode = vk::CullModeFlagBits::eNone;
    auto ssaoPipeline = mgr->CreatePipeline(emptyVertexInput, ssaoLayout, ssaoAsset, ssaoPass, 0, screenConfig);
    auto blurPipeline = mgr->CreatePipeline(emptyVertexInput, blurLayout, blurAsset, blurPass, 0, screenConfig);
    auto compositePipeline =
        mgr->CreatePipeline(emptyVertexInput, compositeLayout, compositeAsset, screenPass, 0, screenConfig);

    // ----- static uploads --------------------------------------------------
    auto upload = mgr->UploadSequence();
    {
        auto sync = std::make_shared<CmdSyncDevice>(gbufferAsset);
        sync->OnUpdateRawData(ResourceTypes::eVertex, vertices);
        sync->OnUpdateRawData(ResourceId(2, 0), 0u, palette);
        sync->OnUpdateRawData(ResourceId(3, 0), 0u, models);
        upload->submit(sync, gbufferAsset);
    }
    {
        auto sync = std::make_shared<CmdSyncDevice>(ssaoAsset);
        // Kernel set is shared (one upload).  The sampler set is eEachFrame /
        // non-shared because position & normal views are injected per frame, so
        // the engine builds a SEPARATE noise TextureImage for every frame slot
        // -- all of them need the pixel data or the untouched ones stay in
        // UNDEFINED layout and trip VUID-vkCmdDraw-None-09600.
        sync->OnUpdateRawData(ResourceId(2, 0), 0u, kernel);
        for (uint32_t frame = 0; frame < MAX_FRAMES_IN_FLIGHT; ++frame) {
            sync->OnUpdateRawData(ResourceId(1, 2), frame, noise);
        }
        upload->submit(sync, ssaoAsset);
    }

    // ----- frame loop ------------------------------------------------------
    auto sequence = mgr->DrawFrameSequence();
    auto gbufferDraw = std::make_shared<CmdOpDrawFrame>(gbufferPipeline);
    auto ssaoDraw = std::make_shared<CmdOpDrawFrame>(ssaoPipeline);
    auto blurDraw = std::make_shared<CmdOpDrawFrame>(blurPipeline);
    auto compositeDraw = std::make_shared<CmdOpDrawFrame>(compositePipeline);
    auto gbufferFrameSync = std::make_shared<CmdSyncDevice>(gbufferAsset);
    auto ssaoFrameSync = std::make_shared<CmdSyncDevice>(ssaoAsset);
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
        const vk::Extent2D swapExtent = *mgr->getSwapChain()->getExtent2D();
        const float aspect =
            static_cast<float>(swapExtent.width) / static_cast<float>(swapExtent.height > 0 ? swapExtent.height : 1);

        const glm::mat4 view = camera->GetViewMatrix();
        glm::mat4 projection = glm::perspective(glm::radians(camera->Zoom), aspect, kNearPlane, kFarPlane);
        projection[1][1] *= -1.0f; // Vulkan NDC y-down

        CameraBlock cameraBlock;
        cameraBlock.view = view;
        cameraBlock.proj = projection;

        SsaoBlock ssaoBlock;
        ssaoBlock.projection = projection;
        ssaoBlock.noiseScale = glm::vec4(static_cast<float>(extent.width) / kNoiseSize,
                                         static_cast<float>(extent.height) / kNoiseSize, 0.0f, 0.0f);
        ssaoBlock.params = glm::vec4(kRadius, kBias, static_cast<float>(kKernelSize), 1.0f);

        // The reference puts the light in view space every frame (view-space
        // deferred shading), so its appearance follows the camera.
        const glm::vec3 lightPosView = glm::vec3(view * glm::vec4(2.0f, 4.0f, -2.0f, 1.0f));
        LightBlock lightBlock;
        lightBlock.lightPosView = glm::vec4(lightPosView, 1.0f);
        lightBlock.params = glm::vec4(options.noSsao ? 0.0f : 1.0f, options.debugAo ? 1.0f : 0.0f, 0.0f, 0.0f);

        gbufferDraw->OnUpdateRawData(ResourceId(1, 0), frame, cameraBlock);
        ssaoDraw->OnUpdateRawData(ResourceId(0, 0), frame, ssaoBlock);
        compositeDraw->OnUpdateRawData(ResourceId(1, 0), frame, lightBlock);

        sequence->beginRecording();
        sequence->record(gbufferFrameSync);
        sequence->record(ssaoFrameSync);
        sequence->record(compositeFrameSync);
        sequence->recordPass(gbufferPass, [&]() { sequence->record(gbufferDraw); });
        sequence->recordPass(ssaoPass, [&]() { sequence->record(ssaoDraw); });
        sequence->recordPass(blurPass, [&]() { sequence->record(blurDraw); });
        sequence->recordPass(screenPass, [&]() { sequence->record(compositeDraw); });
        sequence->endRecording();
        sequence->submitFrame();

        ++framesRendered;
        if (options.dump && !dumpDone && framesRendered >= options.dumpAfter) {
            ImageReadback readback(mgr->getLogicalDevice(), mgr->memoryProperties(), mgr->getMemoryAllocator(),
                                   mgr->getGraphicsQueue(), *mgr->getQueueFamilyIndices().graphicsFamily);

            const uint32_t slot = frame % gbufferSets;
            std::string frameLabel = "frame";
            if (options.noSsao) {
                frameLabel = "frame-no-ssao";
            } else if (options.debugAo) {
                frameLabel = "frame-ao";
            }
            DumpRequest frameRequest;
            frameRequest.label = frameLabel;
            frameRequest.image = mgr->getSwapChain()->getImages()[sequence->getImageIndex()];
            frameRequest.format = *mgr->getSwapChain()->getImageFormat();
            frameRequest.extent = swapExtent;
            frameRequest.layout = vk::ImageLayout::ePresentSrcKHR;
            frameRequest.encoding = DumpEncoding::eColor;

            DumpRequest rawAo;
            rawAo.label = "ssao-raw";
            rawAo.image = ssaoPass->offscreenImages()[frame];
            rawAo.format = aoFormat;
            rawAo.extent = ssaoPass->extent();
            rawAo.layout = vk::ImageLayout::eShaderReadOnlyOptimal;
            rawAo.encoding = DumpEncoding::eColor;

            DumpRequest blurAo = rawAo;
            blurAo.label = "ssao-blur";
            blurAo.image = blurPass->offscreenImages()[frame];

            DumpRequest position = rawAo;
            position.label = "ssao-position";
            position.image = gbufferPass->offscreenImages()[slot * 3 + 0];
            position.format = vk::Format::eR32G32B32A32Sfloat;
            position.encoding = DumpEncoding::ePosition;
            position.referenceScale = 8.0f;

            DumpRequest normal = rawAo;
            normal.label = "ssao-normal";
            normal.image = gbufferPass->offscreenImages()[slot * 3 + 1];
            normal.format = vk::Format::eR32G32B32A32Sfloat;
            normal.encoding = DumpEncoding::eNormal;

            const auto results = DumpImages(readback, {frameRequest, rawAo, blurAo, position, normal}, "dump", 0);
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
