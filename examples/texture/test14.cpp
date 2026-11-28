// texture: the baseline texture-mapping example (reference: examples/texture).
// A single quad samples a KTX texture that ships with a full mip chain; the
// mip level is selected with an LOD bias so the baked levels can be inspected
// with +/- without regenerating anything.
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include <glm/gtc/matrix_transform.hpp>

#include "Camera.h"
#include "GLFWwindow.h"
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

#include "shader/texture_frag.hpp"
#include "shader/texture_vert.hpp"

namespace {

struct PushBlock {
    glm::mat4 model;
    float lodBias;
};

// 2x2 quad in the XY plane, expanded to six vertices so the draw needs no index
// buffer.  UVs run (0,0)..(1,1) so the texture maps exactly once onto it.
std::vector<Vertex> makeQuad()
{
    struct Corner {
        glm::vec3 position;
        glm::vec2 uv;
    };
    const Corner corners[4] = {
        {{-1.0f, -1.0f, 0.0f}, {0.0f, 1.0f}},
        {{1.0f, -1.0f, 0.0f}, {1.0f, 1.0f}},
        {{1.0f, 1.0f, 0.0f}, {1.0f, 0.0f}},
        {{-1.0f, 1.0f, 0.0f}, {0.0f, 0.0f}},
    };
    const uint32_t order[6] = {0, 1, 2, 0, 2, 3};

    std::vector<Vertex> vertices;
    vertices.reserve(6);
    for (uint32_t index : order) {
        Vertex vertex;
        vertex.position = corners[index].position;
        vertex.texCoord = corners[index].uv;
        vertex.normal = glm::vec3(0.0f, 0.0f, 1.0f);
        vertex.color = glm::vec3(1.0f);
        vertex.tangent = glm::vec3(1.0f, 0.0f, 0.0f);
        vertices.push_back(vertex);
    }
    return vertices;
}

} // namespace

void test(int argc, char **argv)
{
    float lodBias = 0.0f;
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::string(argv[i]) == "--lod-bias") {
            lodBias = std::stof(argv[i + 1]);
        }
    }

    auto camera = std::make_shared<Camera>(glm::vec3(0.0f, 0.0f, 3.0f));
    GLFWwindow window(1024, 768, camera);
    auto mgr = RenderManager::Create(window, MAX_FRAMES_IN_FLIGHT);

    const auto vertices = makeQuad();
    KtxTexture ktx = loadKtx(PROJECT_ROOT_PATH "/assets/textures/metalplate01_rgba.ktx", /*preferSrgb=*/true);

    // The plate is a tiling texture, so the sampler must repeat; the framework
    // default (clamp) is what the per-face meshes need, not this one.
    TextureImageSpec textureSpec = ktx.imageSpec();
    textureSpec.addressMode = vk::SamplerAddressMode::eRepeat;

    std::cout << "[texture] " << ktx.width << "x" << ktx.height << ", " << ktx.mipLevels << " mips, "
              << ktx.byteSize() << " bytes\n";

    auto layout = std::make_shared<DescriptorLayoutAtlas>(MAX_FRAMES_IN_FLIGHT);

    auto globalSet = DescriptorLayoutAtlas::makeSet<GlobalComponent, GlobalComponentTable>(
        0, {GlobalComponent::ebaseColor}, UpdateFrequency::eStatic, true);
    globalSet.specs[0] = textureSpec;

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
        std::vector<VertexComponent>{VertexComponent::ePosition, VertexComponent::eUV, VertexComponent::eNormal},
        static_cast<uint32_t>(vertices.size()));

    auto asset = mgr->CreateShaderAssetGroup(vertexInput, layout);
    asset->addShaderSpirvCode("vert", std::vector<uint32_t>(shader::TEXTURE_VERT_SPV.begin(),
                                                            shader::TEXTURE_VERT_SPV.end()));
    asset->addShaderSpirvCode("frag", std::vector<uint32_t>(shader::TEXTURE_FRAG_SPV.begin(),
                                                            shader::TEXTURE_FRAG_SPV.end()));

    auto detail = std::make_shared<SimpleAttachmentDetail>(
        SimpleAttachmentDetail::Mode::eMSAA, *mgr->getSwapChain()->getImageFormat(), mgr->DefaultDepthFormat());
    auto renderPass = mgr->CreateRenderPass(detail);

    // Culling is off: the quad is a single flat sheet and the example never
    // assumes a winding, matching the reference pipeline state.
    GraphicsPipelineConfig pipelineConfig;
    pipelineConfig.depthCompareOp = vk::CompareOp::eLessOrEqual;
    auto pipeline = mgr->CreatePipeline(vertexInput, layout, asset, renderPass, 0, pipelineConfig);

    auto upload = mgr->UploadSequence();
    auto syncOp = std::make_shared<CmdSyncDevice>(asset);
    syncOp->OnUpdateRawData(ResourceTypes::eVertex, vertices);
    syncOp->OnUpdateRawData(ResourceId(0, 0), 0u, ktx.data);
    upload->submit(syncOp, asset);

    auto sequence = mgr->DrawFrameSequence();
    auto drawOp = std::make_shared<CmdOpDrawFrame>(pipeline);
    auto frameSync = std::make_shared<CmdSyncDevice>(asset);

    std::cout << "[texture] +/- changes the LOD bias (current " << lodBias << ")\n";

    bool plusHeld = false;
    bool minusHeld = false;
    while (!window.shouldClose()) {
        window.poolEvents();
        float currentTime = 0.0f;
        window.processInputEvents(currentTime);

        auto justPressed = [](bool down, bool &previous) {
            const bool edge = down && !previous;
            previous = down;
            return edge;
        };
        if (justPressed(window.isKeyDown(GLFW_KEY_EQUAL) || window.isKeyDown(GLFW_KEY_KP_ADD), plusHeld)) {
            lodBias = std::min(lodBias + 0.5f, 8.0f);
            std::cout << "[texture] lod bias " << lodBias << "\n";
        }
        if (justPressed(window.isKeyDown(GLFW_KEY_MINUS) || window.isKeyDown(GLFW_KEY_KP_SUBTRACT), minusHeld)) {
            lodBias = std::max(lodBias - 0.5f, 0.0f);
            std::cout << "[texture] lod bias " << lodBias << "\n";
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
        push.lodBias = lodBias;

        drawOp->OnUpdateRawData(ResourceId(1, 0), frame, MakeCameraMatrix(*camera, extent));
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
