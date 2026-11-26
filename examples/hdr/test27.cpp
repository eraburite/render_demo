// hdr: tone mapping over an RGBA16F render target, migrated from
// LearnOpenGL/src/5.advanced_lighting/6.hdr.
//
// The reference renders a tunnel lit by four point lights (one with radiance
// 200) into GL_RGBA16F, then applies `1 - exp(-hdr * exposure)` and a 1/2.2
// display encode in a fullscreen pass.  The whole point is the intermediate
// format: an RGBA8 target would clip the 200 unit light to white before the
// tone map could compress it, and exposure would stop changing the picture.
//
// Two deliberate differences from the reference:
//   * The tunnel is a procedural checker instead of a textured model, so the
//     dump is reproducible without an external asset.
//   * The light positions are expressed relative to a camera looking down -Z
//     (Vulkan's forward direction here) rather than the reference's +Z tunnel.
#include <algorithm>
#include <array>
#include <cmath>
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

#include "shader/scene_frag.hpp"
#include "shader/scene_vert.hpp"
#include "shader/screen_vert.hpp"
#include "shader/tonemap_frag.hpp"

namespace {

constexpr float kNearPlane = 0.1f;
constexpr float kFarPlane = 100.0f;

struct HdrSceneUBO {
    alignas(16) glm::mat4 viewProj{1.0f};
    alignas(16) glm::vec4 eye{0.0f};
    // x = exposure, y = HDR enable, z = raw Reinhard view, w unused.
    alignas(16) glm::vec4 params{0.0f};
};
static_assert(sizeof(HdrSceneUBO) == 96, "HdrSceneUBO must match the std140 HdrBlock");

struct ToneUBO {
    alignas(16) glm::vec4 params{0.0f};
};
static_assert(sizeof(ToneUBO) == 16, "ToneUBO must match ToneBlock");

struct Instance {
    alignas(16) glm::mat4 model{1.0f};
    // rgb = base colour or emissive radiance, a > 0.5 selects emissive.
    alignas(16) glm::vec4 material{1.0f};
};
static_assert(sizeof(Instance) == 80, "Instance must be model(64) + material(16)");

enum class SceneComponent { eSceneBlock };
using SceneConfig = DescriptorComponentSpec<SceneComponent>;
inline constexpr auto SceneComponentTable = std::make_tuple(SceneConfig{
    SceneComponent::eSceneBlock, vk::DescriptorType::eUniformBuffer, 1,
    vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment});

enum class SceneInstanceComponent { eInstances };
using SceneInstanceConfig = DescriptorComponentSpec<SceneInstanceComponent>;
inline constexpr auto SceneInstanceComponentTable = std::make_tuple(SceneInstanceConfig{
    SceneInstanceComponent::eInstances, vk::DescriptorType::eStorageBuffer, 1,
    vk::ShaderStageFlagBits::eVertex});

enum class ToneComponent { eParams, eHdrColor };
using ToneConfig = DescriptorComponentSpec<ToneComponent>;
inline constexpr auto ToneComponentTable = std::make_tuple(
    ToneConfig{ToneComponent::eParams, vk::DescriptorType::eUniformBuffer, 1,
               vk::ShaderStageFlagBits::eFragment},
    ToneConfig{ToneComponent::eHdrColor, vk::DescriptorType::eCombinedImageSampler, 1,
               vk::ShaderStageFlagBits::eFragment});

struct Options {
    bool dump = false;
    uint32_t dumpAfter = 3;
    float exposure = 1.0f;
    bool noHdr = false;
    bool debugRaw = false;
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
        } else if (arg == "--no-hdr") {
            options.noHdr = true;
        } else if (arg == "--debug-raw") {
            options.debugRaw = true;
        } else {
            throw std::runtime_error("hdr: unknown argument " + arg);
        }
    }
    options.exposure = std::clamp(options.exposure, 0.0f, 100.0f);
    return options;
}

// Same cube winding as examples/csm: 36 vertices, CCW from outside.  The
// tunnel is this cube seen from inside with culling disabled, so normals also
// have to be inverted by the renderer for the shading to face the camera.
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
    // Tunnel.  Material alpha 0 means "lit by the point lights".
    // Camera at z=12 sits inside this cube (z in [-40, 20]); the reference
    // does the same by centring its tunnel at z=25 with a 27.5 half-extent.
    glm::mat4 tunnel = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, -10.0f));
    tunnel = glm::scale(tunnel, glm::vec3(3.0f, 3.0f, 60.0f));
    instances.push_back({tunnel, glm::vec4(0.42f, 0.40f, 0.38f, 0.0f)});

    // The four emitters have to match LIGHT_POS / LIGHT_COLOR in scene.frag.
    struct Light {
        glm::vec3 position;
        glm::vec3 radiance;
    };
    static const Light lights[] = {
        {glm::vec3(0.0f, 0.0f, -35.0f), glm::vec3(200.0f)},
        {glm::vec3(-1.4f, -1.9f, -2.0f), glm::vec3(0.10f, 0.0f, 0.0f)},
        {glm::vec3(0.0f, -1.8f, -8.0f), glm::vec3(0.0f, 0.0f, 0.20f)},
        {glm::vec3(0.8f, -1.7f, -5.0f), glm::vec3(0.0f, 0.10f, 0.0f)},
    };
    for (const Light &light : lights) {
        glm::mat4 model = glm::translate(glm::mat4(1.0f), light.position);
        model = glm::scale(model, glm::vec3(0.35f));
        instances.push_back({model, glm::vec4(light.radiance, 1.0f)});
    }
    return instances;
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
        throw std::runtime_error("hdr: failed to create the HDR sampler");
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

    auto camera = std::make_shared<Camera>(glm::vec3(0.0f, 0.0f, 12.0f));
    camera->MovementSpeed = 6.0f;
    GLFWwindow window(1280, 720, camera);
    auto mgr = RenderManager::Create(window, MAX_FRAMES_IN_FLIGHT);

    const vk::Format depthFormat = mgr->DefaultDepthFormat();
    const std::vector<Vertex> vertices = makeCubeVertices();
    const std::vector<Instance> instances = makeScene();
    const uint32_t instanceCount = static_cast<uint32_t>(instances.size());
    const vk::Format hdrFormat = vk::Format::eR16G16B16A16Sfloat;

    std::cout << "[hdr] " << instanceCount << " instances (1 tunnel + " << (instanceCount - 1)
              << " emissive lights), RGBA16F target, exposure " << options.exposure << "\n";

    // ----- render passes ----------------------------------------------------
    auto hdrDetail = std::make_shared<SimpleAttachmentDetail>(SimpleAttachmentDetail::Mode::eOffscreen, hdrFormat,
                                                              depthFormat, vk::ImageLayout::eShaderReadOnlyOptimal,
                                                              DependencyPreset::eOffscreen);
    auto hdrPass = mgr->CreateRenderPass(hdrDetail);
    hdrPass->createFramebuffers({}, hdrPass->extent());

    auto screenPass = mgr->CreateRenderPass(std::make_shared<SimpleAttachmentDetail>(
        SimpleAttachmentDetail::Mode::eDirect, *mgr->getSwapChain()->getImageFormat(), depthFormat));

    // ----- descriptor layouts ----------------------------------------------
    auto sceneLayout = std::make_shared<DescriptorLayoutAtlas>(MAX_FRAMES_IN_FLIGHT);
    auto sceneBlockSet = DescriptorLayoutAtlas::makeSet<SceneComponent, SceneComponentTable>(
        0, {SceneComponent::eSceneBlock}, UpdateFrequency::eEachFrame, false);
    sceneBlockSet.specs[0] = GBufferSpec{1, static_cast<uint32_t>(sizeof(HdrSceneUBO)), DataTypes::eCustom};
    auto sceneInstanceSet = DescriptorLayoutAtlas::makeSet<SceneInstanceComponent, SceneInstanceComponentTable>(
        1, {SceneInstanceComponent::eInstances}, UpdateFrequency::eStatic, true);
    sceneInstanceSet.specs[0] = GBufferSpec{instanceCount, static_cast<uint32_t>(sizeof(Instance)),
                                            DataTypes::eCustom};
    sceneLayout->setDetails({sceneBlockSet, sceneInstanceSet});

    auto toneLayout = std::make_shared<DescriptorLayoutAtlas>(MAX_FRAMES_IN_FLIGHT);
    auto toneBlockSet = DescriptorLayoutAtlas::makeSet<ToneComponent, ToneComponentTable>(
        0, {ToneComponent::eParams}, UpdateFrequency::eEachFrame, false);
    toneBlockSet.specs[0] = GBufferSpec{1, static_cast<uint32_t>(sizeof(ToneUBO)), DataTypes::eCustom};
    auto toneColorSet = DescriptorLayoutAtlas::makeSet<ToneComponent, ToneComponentTable>(
        1, {ToneComponent::eHdrColor}, UpdateFrequency::eEachFrame, false);
    toneLayout->setDetails({toneBlockSet, toneColorSet});

    // ----- assets -----------------------------------------------------------
    auto sceneAsset = mgr->CreateShaderAssetGroup(sceneLayout);
    sceneAsset->addShaderSpirvCode("vert", std::vector<uint32_t>(shader::SCENE_VERT_SPV.begin(),
                                                                  shader::SCENE_VERT_SPV.end()));
    sceneAsset->addShaderSpirvCode("frag", std::vector<uint32_t>(shader::SCENE_FRAG_SPV.begin(),
                                                                  shader::SCENE_FRAG_SPV.end()));

    auto toneAsset = mgr->CreateShaderAssetGroup(toneLayout);
    toneAsset->addShaderSpirvCode("vert", std::vector<uint32_t>(shader::SCREEN_VERT_SPV.begin(),
                                                                 shader::SCREEN_VERT_SPV.end()));
    toneAsset->addShaderSpirvCode("frag", std::vector<uint32_t>(shader::TONEMAP_FRAG_SPV.begin(),
                                                                 shader::TONEMAP_FRAG_SPV.end()));

    // The HDR target view must be bound before pipeline creation: descriptor
    // sets are written once, when the pipeline is built.
    auto hdrSampler = makeLinearSampler(mgr->getLogicalDevice());
    const std::vector<vk::ImageView> &hdrViews = hdrPass->offscreenViews();
    if (hdrViews.size() != MAX_FRAMES_IN_FLIGHT || hdrPass->offscreenColorCountPerSet() != 1) {
        throw std::runtime_error("hdr: expected one HDR target per in-flight frame");
    }
    std::vector<std::shared_ptr<TextureSamplerCombine>> hdrResources(MAX_FRAMES_IN_FLIGHT);
    for (uint32_t frame = 0; frame < MAX_FRAMES_IN_FLIGHT; ++frame) {
        auto combine = std::shared_ptr<TextureSamplerCombine>(
            new TextureSamplerCombine(hdrViews[frame], *hdrSampler, vk::ImageLayout::eShaderReadOnlyOptimal),
            [hdrSampler](TextureSamplerCombine *pointer) { delete pointer; });
        toneAsset->addDescriptorResource(ResourceKey::make_key(ResourceId(1, 0), frame), combine);
        hdrResources[frame] = std::move(combine);
    }

    // ----- pipelines --------------------------------------------------------
    auto sceneVertexInput = std::make_shared<VertexInputAtlas>(
        0,
        std::vector<VertexComponent>{VertexComponent::ePosition, VertexComponent::eNormal, VertexComponent::eUV},
        static_cast<uint32_t>(vertices.size()), 0u, DataTypes::eUnsignedInt, false, 0u, instanceCount);
    GraphicsPipelineConfig sceneConfig;
    // The tunnel is seen from inside; back-face culling would remove every wall.
    sceneConfig.cullMode = vk::CullModeFlagBits::eNone;
    auto scenePipeline =
        mgr->CreatePipeline(sceneVertexInput, sceneLayout, sceneAsset, hdrPass, 0, sceneConfig);

    auto emptyVertexInput = std::make_shared<VertexInputAtlas>(0, std::vector<VertexComponent>{}, 0u, 0u,
                                                               DataTypes::eUnsignedInt, true, 0u);
    GraphicsPipelineConfig toneConfig;
    toneConfig.depthTestEnable = false;
    toneConfig.depthWriteEnable = false;
    auto tonePipeline =
        mgr->CreatePipeline(emptyVertexInput, toneLayout, toneAsset, screenPass, 0, toneConfig);

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
    auto toneDraw = std::make_shared<CmdOpDrawFrame>(tonePipeline);
    // Uniform data changes when the user presses a key, so the per-frame
    // copies must be recorded every frame (a one-shot upload is not enough).
    auto sceneFrameSync = std::make_shared<CmdSyncDevice>(sceneAsset);
    auto toneFrameSync = std::make_shared<CmdSyncDevice>(toneAsset);

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
        projection[1][1] *= -1.0f; // Vulkan NDC is y-down

        HdrSceneUBO sceneUbo;
        sceneUbo.viewProj = projection * view;
        sceneUbo.eye = glm::vec4(camera->Position, 1.0f);
        // w = inverted normals: the camera is inside the tunnel cube.
        sceneUbo.params = glm::vec4(options.exposure, options.noHdr ? 0.0f : 1.0f, options.debugRaw ? 1.0f : 0.0f,
                                    1.0f);
        ToneUBO toneUbo;
        toneUbo.params = sceneUbo.params;

        sceneDraw->OnUpdateRawData(ResourceId(0, 0), frame, sceneUbo);
        toneDraw->OnUpdateRawData(ResourceId(0, 0), frame, toneUbo);

        sequence->beginRecording();
        sequence->record(sceneFrameSync);
        sequence->record(toneFrameSync);
        sequence->recordPass(hdrPass, [&]() { sequence->record(sceneDraw); });
        sequence->recordPass(screenPass, [&]() { sequence->record(toneDraw); });
        sequence->endRecording();
        sequence->submitFrame();

        ++framesRendered;
        if (options.dump && !dumpDone && framesRendered >= options.dumpAfter) {
            auto readback = ImageReadback(mgr->getLogicalDevice(), mgr->memoryProperties(),
                                          mgr->getMemoryAllocator(), mgr->getGraphicsQueue(),
                                          *mgr->getQueueFamilyIndices().graphicsFamily);
            DumpRequest frameRequest;
            frameRequest.label = options.noHdr ? "frame-no-hdr" : options.debugRaw ? "frame-raw" : "frame";
            frameRequest.image = mgr->getSwapChain()->getImages()[sequence->getImageIndex()];
            frameRequest.format = *mgr->getSwapChain()->getImageFormat();
            frameRequest.extent = extent;
            frameRequest.layout = vk::ImageLayout::ePresentSrcKHR;
            frameRequest.encoding = DumpEncoding::eColor;

            DumpRequest hdrRequest;
            hdrRequest.label = "hdr-color";
            hdrRequest.image = hdrPass->offscreenImages()[frame % static_cast<uint32_t>(hdrViews.size())];
            hdrRequest.format = hdrFormat;
            hdrRequest.extent = hdrPass->extent();
            hdrRequest.layout = vk::ImageLayout::eShaderReadOnlyOptimal;
            // Reinhard is fixed and independent of the exposure switch, so the
            // HDR capture is directly comparable across runs.
            hdrRequest.encoding = DumpEncoding::eToneMap;

            const auto results = DumpImages(readback, {frameRequest, hdrRequest}, "dump", 0);
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
