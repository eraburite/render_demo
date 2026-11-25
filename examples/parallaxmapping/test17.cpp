// parallaxmapping: the four texture-mapping modes of the reference example
// (colour only / parallax / steep parallax / parallax occlusion) on a single
// plane.  The height is packed into the alpha channel of the normal map, so one
// tiling texture pair drives the whole effect.
//
// Reference: examples/parallaxmapping of Sascha Willems' Vulkan Examples.
// Deviations from it are documented where they occur (tangent-space setup is
// split across the two stages because the shared descriptor tables pin the eye
// position to the fragment stage; the mapping scalars travel in the push
// constant instead of a second UBO).
#include <algorithm>
#include <cstdint>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include <glm/gtc/matrix_transform.hpp>

#include "Camera.h"
#include "GLFWwindow.h"
#include "ModelLoader.h"
#include "KtxLoader.h"
#include "RenderCommon.h"

#include "AttachmentDetailConfig.h"
#include "DescriptorLayoutConfig.h"
#include "VertexInputStateConfig.h"

#include "CmdOpDrawFrame.h"
#include "CmdSyncDevice.h"
#include "IRenderPass.h"
#include "LoopSequence.h"
#include "OnceSequence.h"
#include "RenderManager.h"
#include "root_directory.h"

#include "shader/parallax_frag.hpp"
#include "shader/parallax_vert.hpp"

namespace {

// std430-compatible mirror of the shader's push block.  mat4/vec4 sit on
// 16-byte boundaries, the trailing scalars pack into the last 16-byte slot.
struct PushBlock {
    alignas(16) glm::mat4 model;
    alignas(16) glm::vec4 lightPos;
    float heightScale;
    float parallaxBias;
    float numLayers;
    int32_t mappingMode;
};

static_assert(sizeof(PushBlock) == 96, "push block layout must match std430");

const char *const MODE_NAMES[5] = {
    "color only", "normal mapping", "parallax mapping", "steep parallax mapping", "parallax occlusion mapping",
};

// Center and uniformly fit the glTF into a ~2 unit box.  The index list is
// expanded into a plain vertex stream because the engine's draw path does not
// infer an index type from the buffer spec (same approach as gltfloading).
std::vector<Vertex> toEngineModel(const Model &gltf)
{
    const glm::vec3 extent = gltf.maxBounds - gltf.minBounds;
    const float maxExtent = std::max({extent.x, extent.y, extent.z, 0.0001f});
    const float scale = 2.0f / maxExtent;
    const glm::vec3 center = (gltf.minBounds + gltf.maxBounds) * 0.5f;

    std::vector<Vertex> source;
    source.reserve(gltf.vertices.size());
    for (const MeshVertex &src : gltf.vertices) {
        Vertex dst;
        dst.position = (src.position - center) * scale;
        dst.normal = src.normal;
        dst.texCoord = src.texCoord;
        dst.color = src.color;
        // The authored tangent is what the TBN basis needs; a placeholder would
        // make every parallax offset point along +X.
        dst.tangent = src.tangent;
        source.push_back(dst);
    }

    std::vector<Vertex> out;
    out.reserve(gltf.indices.size());
    for (uint32_t index : gltf.indices) {
        if (index < source.size()) {
            out.push_back(source[index]);
        }
    }
    return out;
}

} // namespace

void test(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    // The plane sits on y = 0 and faces +Y, so the camera looks down onto it at
    // ~40 degrees off the normal -- the oblique view the parallax offset needs.
    auto camera = std::make_shared<Camera>(glm::vec3(0.0f, 1.25f, -1.5f), glm::vec3(0.0f, 1.0f, 0.0f), 90.0f,
                                          -40.0f);
    GLFWwindow window(1024, 768, camera);
    auto mgr = RenderManager::Create(window, MAX_FRAMES_IN_FLIGHT);

    // ----- Assets ---------------------------------------------------------
    Model gltf = loadModel(PROJECT_ROOT_PATH "/data/models/plane.gltf");
    if (!gltf.valid()) {
        throw std::runtime_error("parallaxmapping: no triangle geometry in plane.gltf");
    }
    const std::vector<Vertex> vertices = toEngineModel(gltf);

    KtxTexture colorKtx = loadKtx(PROJECT_ROOT_PATH "/data/textures/rocks_color_rgba.ktx", /*preferSrgb=*/true);
    KtxTexture normalHeightKtx = loadKtx(PROJECT_ROOT_PATH "/data/textures/rocks_normal_height_rgba.ktx",
                                         /*preferSrgb=*/false);
    if (!colorKtx.valid() || !normalHeightKtx.valid()) {
        throw std::runtime_error("parallaxmapping: failed to load the rocks texture pair");
    }

    // Both maps tile across the plane, so the samplers must repeat.
    TextureImageSpec colorSpec = colorKtx.imageSpec();
    colorSpec.addressMode = vk::SamplerAddressMode::eRepeat;
    TextureImageSpec normalHeightSpec = normalHeightKtx.imageSpec();
    normalHeightSpec.addressMode = vk::SamplerAddressMode::eRepeat;

    std::cout << "[parallaxmapping] " << gltf.vertices.size() << " source vertices, " << gltf.indices.size()
              << " indices -> " << vertices.size() << " draw vertices\n";
    std::cout << "[parallaxmapping] color map " << colorKtx.width << "x" << colorKtx.height << ", "
              << colorKtx.mipLevels << " mips, " << colorKtx.byteSize() << " bytes, repeat + sRGB\n";
    std::cout << "[parallaxmapping] normal/height map " << normalHeightKtx.width << "x" << normalHeightKtx.height
              << ", " << normalHeightKtx.mipLevels << " mips, " << normalHeightKtx.byteSize()
              << " bytes, repeat, linear\n";

    // ----- Descriptor layout ---------------------------------------------
    auto layout = std::make_shared<DescriptorLayoutAtlas>(MAX_FRAMES_IN_FLIGHT);

    auto globalSet = DescriptorLayoutAtlas::makeSet<GlobalComponent, GlobalComponentTable>(
        0, {GlobalComponent::ebaseColor, GlobalComponent::eNormalHeightMap}, UpdateFrequency::eStatic, true);
    globalSet.specs[0] = colorSpec;
    globalSet.specs[1] = normalHeightSpec;

    auto environmentSet = DescriptorLayoutAtlas::makeSet<EnvironmentComponent, EnvironmentComponentTable>(
        1, {EnvironmentComponent::eCameraMatrix}, UpdateFrequency::eEachFrame, false);
    environmentSet.specs[0] = GBufferSpec{1, static_cast<uint32_t>(sizeof(CameraMatrix)), DataTypes::eCustom};

    auto frameSet = DescriptorLayoutAtlas::makeSet<FrameComponent, FrameComponentTable>(
        2, {FrameComponent::eViewEyePosition}, UpdateFrequency::eEachFrame, false);
    frameSet.specs[0] = GBufferSpec{1, static_cast<uint32_t>(sizeof(ViewEyePoint)), DataTypes::eCustom};

    layout->setDetails({globalSet, environmentSet, frameSet});
    layout->setPushConstant(static_cast<uint32_t>(sizeof(PushBlock)),
                            vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment);

    auto vertexInput = std::make_shared<VertexInputAtlas>(
        0,
        std::vector<VertexComponent>{VertexComponent::ePosition, VertexComponent::eUV, VertexComponent::eNormal,
                                     VertexComponent::eTangent},
        static_cast<uint32_t>(vertices.size()));

    auto asset = mgr->CreateShaderAssetGroup(vertexInput, layout);
    asset->addShaderSpirvCode("vert", std::vector<uint32_t>(shader::PARALLAX_VERT_SPV.begin(),
                                                           shader::PARALLAX_VERT_SPV.end()));
    asset->addShaderSpirvCode("frag", std::vector<uint32_t>(shader::PARALLAX_FRAG_SPV.begin(),
                                                           shader::PARALLAX_FRAG_SPV.end()));

    auto detail = std::make_shared<SimpleAttachmentDetail>(
        SimpleAttachmentDetail::Mode::eMSAA, *mgr->getSwapChain()->getImageFormat(), mgr->DefaultDepthFormat());
    auto renderPass = mgr->CreateRenderPass(detail);

    // The reference disables culling for this single sheet.
    GraphicsPipelineConfig pipelineConfig;
    pipelineConfig.depthCompareOp = vk::CompareOp::eLessOrEqual;
    pipelineConfig.cullMode = vk::CullModeFlagBits::eNone;
    auto pipeline = mgr->CreatePipeline(vertexInput, layout, asset, renderPass, 0, pipelineConfig);

    auto upload = mgr->UploadSequence();
    auto syncOp = std::make_shared<CmdSyncDevice>(asset);
    syncOp->OnUpdateRawData(ResourceTypes::eVertex, vertices);
    syncOp->OnUpdateRawData(ResourceId(0, 0), 0u, colorKtx.data);
    syncOp->OnUpdateRawData(ResourceId(0, 1), 0u, normalHeightKtx.data);
    upload->submit(syncOp, asset);

    // ----- Frame loop -----------------------------------------------------
    auto sequence = mgr->DrawFrameSequence();
    auto drawOp = std::make_shared<CmdOpDrawFrame>(pipeline);
    auto frameSync = std::make_shared<CmdSyncDevice>(asset);

    float heightScale = 0.1f;
    float parallaxBias = -0.02f;
    const float numLayers = 48.0f;
    int32_t mappingMode = 4;

    std::cout << "[parallaxmapping] mapping mode " << mappingMode << " (" << MODE_NAMES[mappingMode] << ")\n";
    std::cout << "[parallaxmapping] keys: 1 colour, 2 parallax, 3 steep parallax, 4 occlusion; +/- height scale ("
              << heightScale << ")\n";
    // stdout is fully buffered when it is a pipe, and the smoke test terminates
    // the process, so the startup banner has to be flushed by hand.
    std::cout.flush();

    bool plusHeld = false;
    bool minusHeld = false;
    bool key1Held = false;
    bool key2Held = false;
    bool key3Held = false;
    bool key4Held = false;

    while (!window.shouldClose()) {
        window.poolEvents();
        float currentTime = 0.0f;
        window.processInputEvents(currentTime);

        auto justPressed = [](bool down, bool &previous) {
            const bool edge = down && !previous;
            previous = down;
            return edge;
        };

        auto selectMode = [&](int32_t mode) {
            if (mappingMode != mode) {
                mappingMode = mode;
                std::cout << "[parallaxmapping] mapping mode " << mappingMode << " (" << MODE_NAMES[mappingMode]
                          << ")\n"
                          << std::flush;
            }
        };
        if (justPressed(window.isKeyDown(GLFW_KEY_1), key1Held)) {
            selectMode(0);
        }
        if (justPressed(window.isKeyDown(GLFW_KEY_2), key2Held)) {
            selectMode(2);
        }
        if (justPressed(window.isKeyDown(GLFW_KEY_3), key3Held)) {
            selectMode(3);
        }
        if (justPressed(window.isKeyDown(GLFW_KEY_4), key4Held)) {
            selectMode(4);
        }
        if (justPressed(window.isKeyDown(GLFW_KEY_EQUAL) || window.isKeyDown(GLFW_KEY_KP_ADD), plusHeld)) {
            heightScale = std::min(heightScale + 0.02f, 0.30f);
            std::cout << "[parallaxmapping] height scale " << heightScale << "\n" << std::flush;
        }
        if (justPressed(window.isKeyDown(GLFW_KEY_MINUS) || window.isKeyDown(GLFW_KEY_KP_SUBTRACT), minusHeld)) {
            heightScale = std::max(heightScale - 0.02f, 0.02f);
            std::cout << "[parallaxmapping] height scale " << heightScale << "\n" << std::flush;
        }

        if (!sequence->prepareFrame()) {
            continue;
        }
        const uint32_t frame = sequence->getFrameInFlight();
        const vk::Extent2D extent = *mgr->getSwapChain()->getExtent2D();

        ViewEyePoint eye{};
        eye.position = MakeEyePoint(*camera);

        PushBlock push{};
        push.model = glm::mat4(1.0f);
        // Orbiting light above the plane, like the reference's animated lightPos
        // (the reference keeps it below y = 0 because its plane faces -Y).
        const float angle = currentTime * 3.14159265f;
        push.lightPos = glm::vec4(std::sin(angle) * 1.5f, 3.0f, std::cos(angle) * 1.5f, 1.0f);
        push.heightScale = heightScale;
        push.parallaxBias = parallaxBias;
        push.numLayers = numLayers;
        push.mappingMode = mappingMode;

        drawOp->OnUpdateRawData(ResourceId(1, 0), frame, MakeCameraMatrix(*camera, extent, 60.0f));
        drawOp->OnUpdateRawData(ResourceId(2, 0), frame, eye);
        drawOp->OnUpdateRawData(ResourceTypes::ePushConst, frame, push);

        sequence->beginRecording();
        sequence->record(frameSync);
        sequence->recordPass(renderPass, [&]() { sequence->record(drawOp); });
        sequence->endRecording();
        sequence->submitFrame();
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
    } catch (const std::exception &e) {
        std::cerr << e.what() << "\n";
        return 1;
    }
}
