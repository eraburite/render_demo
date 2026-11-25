// subpasses: deferred shading plus forward transparency expressed as three
// subpasses of a single render pass (reference: examples/subpasses).
//
//   subpass 0  fills a four-target G-Buffer (position/normal/albedo + swapchain)
//   subpass 1  fullscreen triangle reads the G-Buffer as *input attachments* and
//              shades 64 point lights
//   subpass 2  forward transparency, rejecting covered fragments with the linear
//              depth subpass 0 packed into position.a
//
// The point of the example is that attachments never leave the render pass:
// every G-Buffer read is a subpassLoad() of a tile-local attachment instead of a
// texture fetch of an intermediate image.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <iterator>
#include <limits>
#include <memory>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

#include <glm/gtc/matrix_transform.hpp>

#include "Camera.h"
#include "GLFWwindow.h"
#include "GlTFLoader.h"
#include "KtxLoader.h"
#include "RenderCommon.h"

#include "AttachmentDetailConfig.h"
#include "DescriptorLayoutConfig.h"
#include "VertexInputStateConfig.h"

#include "CmdOpDrawFrame.h"
#include "CmdSyncDevice.h"
#include "IRenderPass.h"
#include "ImageDump.h"
#include "ImageReadback.h"
#include "InputAttachmentView.h"
#include "LoopSequence.h"
#include "OnceSequence.h"
#include "RenderManager.h"
#include "root_directory.h"

#include "shader/composition_frag.hpp"
#include "shader/composition_vert.hpp"
#include "shader/gbuffer_frag.hpp"
#include "shader/gbuffer_vert.hpp"
#include "shader/transparent_frag.hpp"
#include "shader/transparent_vert.hpp"

namespace {

// Height the building is normalised to.  Both models share one transform so the
// glass panels keep lining up with the walls they belong to.
constexpr float kTargetHeight = 3.0f;

struct SceneMesh {
    std::vector<Vertex> vertices;
    glm::vec3 minBounds{0.0f};
    glm::vec3 maxBounds{0.0f};
};

// Applies every node's world transform, multiplies in the material base colour
// (the building carries no COLOR_0 attribute, so without this every surface
// would be white) and expands the index list into a plain triangle stream, which
// is what the framework's well-tested non-indexed draw path expects.
SceneMesh mergeModel(const GlTFModel &model)
{
    SceneMesh mesh;
    const std::vector<glm::mat4> world = computeNodeWorldMatrices(model);

    glm::vec3 minBounds(std::numeric_limits<float>::max());
    glm::vec3 maxBounds(std::numeric_limits<float>::lowest());

    for (size_t nodeIndex = 0; nodeIndex < model.nodes.size(); ++nodeIndex) {
        const GlTFNode &node = model.nodes[nodeIndex];
        if (node.meshIndex < 0 || node.meshIndex >= static_cast<int>(model.meshes.size())
            || nodeIndex >= world.size()) {
            continue;
        }
        const glm::mat4 &transform = world[nodeIndex];
        const glm::mat3 normalMatrix = glm::transpose(glm::inverse(glm::mat3(transform)));

        for (const GlTFPrimitive &primitive : model.meshes[node.meshIndex].primitives) {
            glm::vec3 tint(1.0f);
            if (primitive.materialIndex >= 0
                && primitive.materialIndex < static_cast<int>(model.materials.size())) {
                tint = glm::vec3(model.materials[primitive.materialIndex].baseColorFactor);
            }
            for (uint32_t i = primitive.firstIndex;
                 i < primitive.firstIndex + primitive.indexCount && i < model.indices.size(); ++i) {
                const uint32_t vertexIndex = model.indices[i];
                if (vertexIndex >= model.vertices.size()) {
                    continue;
                }
                const GlTFVertex &src = model.vertices[vertexIndex];
                Vertex vertex;
                vertex.position = glm::vec3(transform * glm::vec4(src.position, 1.0f));
                vertex.normal = glm::normalize(normalMatrix * src.normal);
                vertex.texCoord = src.texCoord;
                vertex.tangent = src.tangent;
                vertex.color = src.color * tint;
                minBounds = glm::min(minBounds, vertex.position);
                maxBounds = glm::max(maxBounds, vertex.position);
                mesh.vertices.push_back(vertex);
            }
        }
    }
    mesh.minBounds = minBounds;
    mesh.maxBounds = maxBounds;
    return mesh;
}

void normalizeMesh(SceneMesh &mesh, const glm::vec3 &center, float scale)
{
    for (Vertex &vertex : mesh.vertices) {
        vertex.position = (vertex.position - center) * scale;
    }
    mesh.minBounds = (mesh.minBounds - center) * scale;
    mesh.maxBounds = (mesh.maxBounds - center) * scale;
}

// 64 coloured point lights scattered around and above the building, exactly the
// scene the reference example randomises with its UI button.
void randomizeLights(SubpassLights &lights, uint32_t seed, const glm::vec3 &eye)
{
    // Saturated colours only: a white light in the mix averages the whole
    // facade back to grey and hides which light produced which pool.
    static const glm::vec3 palette[] = {
        {1.0f, 0.2f, 0.2f}, {0.2f, 1.0f, 0.3f}, {0.25f, 0.45f, 1.0f},
        {1.0f, 0.9f, 0.2f}, {1.0f, 0.3f, 0.9f}, {0.2f, 0.95f, 1.0f},
    };
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> unit(-1.0f, 1.0f);
    std::uniform_int_distribution<int> pick(0, static_cast<int>(std::size(palette)) - 1);

    for (SubpassLight &light : lights.lights) {
        // A ring around the building (radius 1.8..3.2) instead of a random box:
        // lights inside the walls only graze the facades, so the coloured pools
        // that make the technique readable never showed up.
        const float angle = unit(rng) * 3.14159265f;
        const float radius = 1.8f + std::abs(unit(rng)) * 1.4f;
        light.position = glm::vec4(std::cos(angle) * radius, 0.25f + std::abs(unit(rng)) * 2.2f,
                                   std::sin(angle) * radius, 1.0f);
        light.color = palette[pick(rng)];
        // Tuned for 64 lights: the reference radius (1..2) clips every
        // surface to white once the albedo is bright.
        light.radius = 0.3f + 0.3f * std::abs(unit(rng));
    }
    lights.viewPos = glm::vec4(eye, 1.0f);
}

} // namespace

void test(int argc, char **argv)
{
    bool dumpOnce = false;
    uint32_t seed = 20240521u;
    // Frame number at which the window is resized; 0 disables it.  The resize
    // destroys and recreates the G-Buffer attachments, so it is the only way to
    // exercise the input-attachment descriptor refresh from a script.
    uint32_t resizeAfter = 0;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--dump") {
            dumpOnce = true;
        } else if (arg == "--seed" && i + 1 < argc) {
            seed = static_cast<uint32_t>(std::stoul(argv[i + 1]));
        } else if (arg == "--resize-after" && i + 1 < argc) {
            resizeAfter = static_cast<uint32_t>(std::stoul(argv[i + 1]));
        }
    }

    // A slightly downward pitch keeps the building centred in a 16:9 window.
    auto camera = std::make_shared<Camera>(glm::vec3(0.0f, 1.0f, 6.2f), glm::vec3(0.0f, 1.0f, 0.0f),
                                          -90.0f, -6.0f);
    GLFWwindow window(1280, 720, camera);
    auto mgr = RenderManager::Create(window, MAX_FRAMES_IN_FLIGHT);

    // ---- geometry ---------------------------------------------------------
    // samplebuilding has 12 meshes spread over 72 nodes; samplebuilding_glass
    // adds the transparent panes in the same world space.
    SceneMesh building = mergeModel(loadGlTF(PROJECT_ROOT_PATH "/data/models/samplebuilding.gltf"));
    SceneMesh glass = mergeModel(loadGlTF(PROJECT_ROOT_PATH "/data/models/samplebuilding_glass.gltf"));
    if (building.vertices.empty()) {
        throw std::runtime_error("subpasses: samplebuilding.gltf produced no geometry");
    }

    const glm::vec3 extent = building.maxBounds - building.minBounds;
    const float maxExtent = std::max({extent.x, extent.y, extent.z, 0.0001f});
    const float scale = kTargetHeight / maxExtent;
    const glm::vec3 center = (building.minBounds + building.maxBounds) * 0.5f;
    // The model is centred on the origin: the camera looks down -Z and the
    // lights are placed above y = 0, inside the upper half of the building.
    normalizeMesh(building, center, scale);
    normalizeMesh(glass, center, scale);

    KtxTexture glassTexture = loadKtx(PROJECT_ROOT_PATH "/data/textures/colored_glass_rgba.ktx",
                                      /*preferSrgb=*/true);
    TextureImageSpec glassSpec = glassTexture.imageSpec();
    glassSpec.addressMode = vk::SamplerAddressMode::eRepeat;

    std::cout << "[subpasses] 3 subpasses: G-Buffer (4 MRT) -> composition ("
              << kSubpassLightCount << " lights) -> transparency\n";
    std::cout << "[subpasses] building " << building.vertices.size() << " vertices, glass "
              << glass.vertices.size() << " vertices, glass map " << glassTexture.width << "x"
              << glassTexture.height << " / " << glassTexture.mipLevels << " mips" << std::endl;

    // ---- descriptor layouts ----------------------------------------------
    // Each subpass gets its own set: nothing is shared between them, because a
    // pipeline is always created against exactly one subpass.
    auto gbufferLayout = std::make_shared<DescriptorLayoutAtlas>(MAX_FRAMES_IN_FLIGHT);
    auto cameraSet = DescriptorLayoutAtlas::makeSet<EnvironmentComponent, EnvironmentComponentTable>(
        0, {EnvironmentComponent::eCameraMatrix}, UpdateFrequency::eEachFrame, false);
    cameraSet.specs[0] = GBufferSpec{1, static_cast<uint32_t>(sizeof(CameraMatrix)), DataTypes::eCustom};
    gbufferLayout->setDetails({cameraSet});

    auto compositionLayout = std::make_shared<DescriptorLayoutAtlas>(MAX_FRAMES_IN_FLIGHT);
    auto compositionSet = DescriptorLayoutAtlas::makeSet<SubpassCompositionComponent,
                                                           SubpassCompositionComponentTable>(
        0,
        {SubpassCompositionComponent::ePosition, SubpassCompositionComponent::eNormal,
         SubpassCompositionComponent::eAlbedo, SubpassCompositionComponent::eLights},
        UpdateFrequency::eEachFrame, false);
    // The three input attachment slots keep their default spec: the engine only
    // materialises UBO/SSBO/texture resources, and an input attachment view is
    // owned by the render pass, so it is injected below instead.
    compositionSet.specs[3] = GBufferSpec{1, static_cast<uint32_t>(sizeof(SubpassLights)),
                                          DataTypes::eCustom};
    compositionLayout->setDetails({compositionSet});

    auto transparentLayout = std::make_shared<DescriptorLayoutAtlas>(MAX_FRAMES_IN_FLIGHT);
    auto transparentSet = DescriptorLayoutAtlas::makeSet<SubpassTransparentComponent,
                                                           SubpassTransparentComponentTable>(
        0,
        {SubpassTransparentComponent::eCameraMatrix, SubpassTransparentComponent::ePosition,
         SubpassTransparentComponent::eGlass},
        UpdateFrequency::eEachFrame, false);
    transparentSet.specs[0] = GBufferSpec{1, static_cast<uint32_t>(sizeof(CameraMatrix)),
                                          DataTypes::eCustom};
    transparentSet.specs[2] = glassSpec;
    transparentLayout->setDetails({transparentSet});

    // ---- assets -----------------------------------------------------------
    auto gbufferVertexInput = std::make_shared<VertexInputAtlas>(
        0,
        std::vector<VertexComponent>{VertexComponent::ePosition, VertexComponent::eColor,
                                     VertexComponent::eNormal},
        static_cast<uint32_t>(building.vertices.size()));
    auto gbufferAsset = mgr->CreateShaderAssetGroup(gbufferVertexInput, gbufferLayout);
    gbufferAsset->addShaderSpirvCode("vert", std::vector<uint32_t>(shader::GBUFFER_VERT_SPV.begin(),
                                                                    shader::GBUFFER_VERT_SPV.end()));
    gbufferAsset->addShaderSpirvCode("frag", std::vector<uint32_t>(shader::GBUFFER_FRAG_SPV.begin(),
                                                                    shader::GBUFFER_FRAG_SPV.end()));

    auto compositionAsset = mgr->CreateShaderAssetGroup(compositionLayout);
    compositionAsset->addShaderSpirvCode("vert", std::vector<uint32_t>(shader::COMPOSITION_VERT_SPV.begin(),
                                                                       shader::COMPOSITION_VERT_SPV.end()));
    compositionAsset->addShaderSpirvCode("frag", std::vector<uint32_t>(shader::COMPOSITION_FRAG_SPV.begin(),
                                                                       shader::COMPOSITION_FRAG_SPV.end()));

    auto transparentVertexInput = std::make_shared<VertexInputAtlas>(
        0,
        std::vector<VertexComponent>{VertexComponent::ePosition, VertexComponent::eColor,
                                     VertexComponent::eNormal, VertexComponent::eUV},
        static_cast<uint32_t>(glass.vertices.size()));
    auto transparentAsset = mgr->CreateShaderAssetGroup(transparentVertexInput, transparentLayout);
    transparentAsset->addShaderSpirvCode("vert",
                                         std::vector<uint32_t>(shader::TRANSPARENT_VERT_SPV.begin(),
                                                               shader::TRANSPARENT_VERT_SPV.end()));
    transparentAsset->addShaderSpirvCode("frag",
                                         std::vector<uint32_t>(shader::TRANSPARENT_FRAG_SPV.begin(),
                                                               shader::TRANSPARENT_FRAG_SPV.end()));

    // ---- the three-subpass render pass ------------------------------------
    auto renderPass = mgr->CreateRenderPass(std::make_shared<SubpassDeferredAttachmentDetail>(
        *mgr->getSwapChain()->getImageFormat(), mgr->DefaultDepthFormat()));
    const std::vector<vk::ImageView> &gbufferViews = renderPass->offscreenViews();
    if (renderPass->offscreenColorCountPerSet() != 3 || gbufferViews.size() < 3) {
        throw std::runtime_error("subpasses: the pass did not expose three G-Buffer views");
    }

    // Input attachment descriptors point straight at the framebuffer
    // attachments.  One resource per frame slot, because descriptor sets are
    // per frame and every set needs its own descriptor write.
    std::vector<std::shared_ptr<InputAttachmentView>> positionViews;
    std::vector<std::shared_ptr<InputAttachmentView>> normalViews;
    std::vector<std::shared_ptr<InputAttachmentView>> albedoViews;
    std::vector<std::shared_ptr<InputAttachmentView>> depthViews;
    const auto bindInputAttachments = [&](const std::vector<vk::ImageView> &views) {
        positionViews.clear();
        normalViews.clear();
        albedoViews.clear();
        depthViews.clear();
        for (uint32_t frame = 0; frame < MAX_FRAMES_IN_FLIGHT; ++frame) {
            auto position = std::make_shared<InputAttachmentView>(views[0]);
            auto normal = std::make_shared<InputAttachmentView>(views[1]);
            auto albedo = std::make_shared<InputAttachmentView>(views[2]);
            auto depth = std::make_shared<InputAttachmentView>(views[0]);
            compositionAsset->addDescriptorResource(ResourceKey::make_key(ResourceId(0, 0), frame), position);
            compositionAsset->addDescriptorResource(ResourceKey::make_key(ResourceId(0, 1), frame), normal);
            compositionAsset->addDescriptorResource(ResourceKey::make_key(ResourceId(0, 2), frame), albedo);
            transparentAsset->addDescriptorResource(ResourceKey::make_key(ResourceId(0, 1), frame), depth);
            positionViews.push_back(position);
            normalViews.push_back(normal);
            albedoViews.push_back(albedo);
            depthViews.push_back(depth);
        }
    };
    bindInputAttachments(gbufferViews);

    // ---- pipelines --------------------------------------------------------
    // glTF authors counter-clockwise windings.  MakeCameraMatrix flips NDC Y for
    // Vulkan, and that flip also negates the signed area in framebuffer space
    // (whose Y grows downwards), so the two conventions cancel out: the frame
    // buffer winding is counter-clockwise again and the default front face is
    // already right for back-face culling.
    GraphicsPipelineConfig gbufferConfig;
    gbufferConfig.cullMode = vk::CullModeFlagBits::eBack;
    gbufferConfig.frontFace = vk::FrontFace::eCounterClockwise;
    gbufferConfig.depthCompareOp = vk::CompareOp::eLessOrEqual;
    auto gbufferPipeline = mgr->CreatePipeline(gbufferVertexInput, gbufferLayout, gbufferAsset, renderPass, 0,
                                               gbufferConfig);

    auto emptyVertexInput = std::make_shared<VertexInputAtlas>(0, std::vector<VertexComponent>{}, 0u, 0u,
                                                               DataTypes::eUnsignedInt, true, 0u);
    GraphicsPipelineConfig compositionConfig;
    compositionConfig.cullMode = vk::CullModeFlagBits::eNone;
    compositionConfig.depthTestEnable = true;
    compositionConfig.depthWriteEnable = false;
    compositionConfig.depthCompareOp = vk::CompareOp::eLessOrEqual;
    compositionConfig.addSpecialization("frag",
                                        {makeSpecializationConstant<uint32_t>(0, kSubpassLightCount)});
    auto compositionPipeline = mgr->CreatePipeline(emptyVertexInput, compositionLayout, compositionAsset,
                                                   renderPass, 1, compositionConfig);

    GraphicsPipelineConfig transparentConfig;
    transparentConfig.cullMode = vk::CullModeFlagBits::eNone;
    transparentConfig.depthTestEnable = true;
    transparentConfig.depthWriteEnable = false;
    transparentConfig.depthCompareOp = vk::CompareOp::eLessOrEqual;
    transparentConfig.blendEnable = true;
    auto transparentPipeline = mgr->CreatePipeline(transparentVertexInput, transparentLayout, transparentAsset,
                                                   renderPass, 2, transparentConfig);

    // ---- uploads ----------------------------------------------------------
    auto upload = mgr->UploadSequence();
    auto gbufferUpload = std::make_shared<CmdSyncDevice>(gbufferAsset);
    gbufferUpload->OnUpdateRawData(ResourceTypes::eVertex, building.vertices);
    upload->submit(gbufferUpload, gbufferAsset);

    auto transparentUpload = std::make_shared<CmdSyncDevice>(transparentAsset);
    transparentUpload->OnUpdateRawData(ResourceTypes::eVertex, glass.vertices);
    for (uint32_t frame = 0; frame < MAX_FRAMES_IN_FLIGHT; ++frame) {
        transparentUpload->OnUpdateRawData(ResourceId(0, 2), frame, glassTexture.data);
    }
    upload->submit(transparentUpload, transparentAsset);

    // ---- frame loop -------------------------------------------------------
    auto sequence = mgr->DrawFrameSequence();
    auto gbufferDraw = std::make_shared<CmdOpDrawFrame>(gbufferPipeline);
    auto compositionDraw = std::make_shared<CmdOpDrawFrame>(compositionPipeline);
    auto transparentDraw = std::make_shared<CmdOpDrawFrame>(transparentPipeline);
    auto gbufferSync = std::make_shared<CmdSyncDevice>(gbufferAsset);
    auto compositionSync = std::make_shared<CmdSyncDevice>(compositionAsset);
    auto transparentSync = std::make_shared<CmdSyncDevice>(transparentAsset);

    SubpassLights lights{};
    randomizeLights(lights, seed, camera->Position);

    std::cout << "[subpasses] keys: R randomises the lights" << std::endl;

    // Framebuffer attachments are recreated on resize, which invalidates the
    // views held by the input attachment descriptors.  The pass bumps its
    // generation counter when that happens; rebuild the resources and rewrite
    // the affected descriptor sets exactly once per generation.
    uint64_t attachmentGeneration = renderPass->attachmentGeneration();
    bool dumpPending = dumpOnce;
    uint32_t renderedFrames = 0;
    bool resizeRequested = false;

    bool randomizeHeld = false;
    while (!window.shouldClose()) {
        window.poolEvents();
        float currentTime = 0.0f;
        window.processInputEvents(currentTime);

        if (resizeAfter != 0 && !resizeRequested && renderedFrames >= resizeAfter) {
            resizeRequested = true;
            window.resize(1024, 600);
            std::cout << "[subpasses] requested resize to 1024x600" << std::endl;
        }

        const bool randomizeDown = window.isKeyDown(GLFW_KEY_R);
        if (randomizeDown && !randomizeHeld) {
            seed = seed * 1664525u + 1013904223u;
            randomizeLights(lights, seed, camera->Position);
            std::cout << "[subpasses] lights randomised (seed " << seed << ")" << std::endl;
        }
        randomizeHeld = randomizeDown;

        if (!sequence->prepareFrame()) {
            continue;
        }
        const uint32_t frame = sequence->getFrameInFlight();
        const vk::Extent2D extent = *mgr->getSwapChain()->getExtent2D();

        if (renderPass->attachmentGeneration() != attachmentGeneration) {
            // Descriptor sets still referenced by an in-flight submission must
            // not be updated, so drain the device first (resize only).
            mgr->getLogicalDevice()->waitIdle();
            const std::vector<vk::ImageView> &views = renderPass->offscreenViews();
            for (uint32_t slot = 0; slot < MAX_FRAMES_IN_FLIGHT; ++slot) {
                positionViews[slot]->setImageView(views[0]);
                normalViews[slot]->setImageView(views[1]);
                albedoViews[slot]->setImageView(views[2]);
                depthViews[slot]->setImageView(views[0]);
            }
            compositionPipeline->RefreshDescriptors();
            transparentPipeline->RefreshDescriptors();
            attachmentGeneration = renderPass->attachmentGeneration();
            std::cout << "[subpasses] attachments rebuilt, input descriptors refreshed" << std::endl;
        }

        const CameraMatrix cameraMatrix = MakeCameraMatrix(*camera, extent);
        lights.viewPos = glm::vec4(camera->Position, 1.0f);
        gbufferDraw->OnUpdateRawData(ResourceId(0, 0), frame, cameraMatrix);
        transparentDraw->OnUpdateRawData(ResourceId(0, 0), frame, cameraMatrix);
        compositionDraw->OnUpdateRawData(ResourceId(0, 3), frame, lights);

        sequence->beginRecording();
        sequence->record(gbufferSync);
        sequence->record(compositionSync);
        sequence->record(transparentSync);
        sequence->recordPass(renderPass, [&]() {
            sequence->record(gbufferDraw);
            sequence->nextSubpass();
            sequence->record(compositionDraw);
            sequence->nextSubpass();
            sequence->record(transparentDraw);
        });
        sequence->endRecording();
        sequence->submitFrame();

        if (dumpPending) {
            // Capture the G-Buffer of this very frame, before it is overwritten.
            ImageReadback readback(mgr->getLogicalDevice(), mgr->memoryProperties(),
                                   mgr->getMemoryAllocator(), mgr->getGraphicsQueue(),
                                   *mgr->getQueueFamilyIndices().graphicsFamily);
            const auto &images = renderPass->offscreenImages();
            const uint32_t setCount = static_cast<uint32_t>(images.size()) / 3u;
            const uint32_t set = setCount == 0 ? 0u : frame % setCount;
            const vk::Extent2D gbufferExtent = renderPass->extent();
            const auto request = [&](std::string label, uint32_t slot, DumpEncoding encoding) {
                DumpRequest r;
                r.label = std::move(label);
                r.image = images[set * 3u + slot];
                r.format = renderPass->offscreenColorFormat(slot);
                r.extent = gbufferExtent;
                r.layout = vk::ImageLayout::eShaderReadOnlyOptimal;
                r.encoding = encoding;
                r.referenceScale = kTargetHeight;
                r.eyePosition = camera->Position;
                r.nearPlane = 0.1f;
                r.farPlane = 100.0f;
                return r;
            };

            std::vector<DumpRequest> requests;
            requests.push_back(request("subpasses-albedo", 2, DumpEncoding::eColor));
            requests.push_back(request("subpasses-position", 0, DumpEncoding::ePosition));
            // Linear depth of the visible surface: mean brightness tells whether
            // the near or the far facade survived back-face culling, which is
            // the only way to validate the winding choice without a screenshot.
            requests.push_back(request("subpasses-depth", 0, DumpEncoding::eLinearDepth));
            // The pass' far plane is 100; encoding against it would compress the
            // whole building into the top 10% of the range and hide the
            // near-vs-far difference, so the depth capture uses a tighter span.
            requests.back().farPlane = 12.0f;

            // The composed frame itself: attachment 0 is the swapchain image, so
            // this is the only way to see what subpasses 1 and 2 actually
            // produced (the G-Buffer dumps above stop at subpass 0).
            const std::vector<vk::Image> &swapImages = mgr->getSwapChain()->getImages();
            const uint32_t imageIndex = sequence->getImageIndex();
            if (imageIndex < swapImages.size()) {
                DumpRequest frame;
                frame.label = "subpasses-frame";
                frame.image = swapImages[imageIndex];
                frame.format = *mgr->getSwapChain()->getImageFormat();
                frame.extent = extent;
                frame.layout = vk::ImageLayout::ePresentSrcKHR;
                frame.encoding = DumpEncoding::eColor;
                requests.push_back(frame);
            }
            // The normal target is RGBA16F, which the readback path does not
            // support (it copies through a 4x8-bit staging image), so it is the
            // one G-Buffer channel that stays on-screen only.
            const auto results = DumpImages(readback, requests, "dump", 0);
            for (const auto &result : results) {
                std::cout << "[dump] " << result.path.string() << " coverage=" << result.alphaCoverage
                          << " mean=(" << result.meanColor.r << "," << result.meanColor.g << ","
                          << result.meanColor.b << ") |n|=" << result.meanNormalLength << std::endl;
            }
            dumpPending = false;
        }

        if (!sequence->presentFrame()) {
            continue;
        }
        ++renderedFrames;
        if (dumpOnce && !dumpPending) {
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
