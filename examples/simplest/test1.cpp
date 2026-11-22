// simplest: one render pass (MSAA), one global texture, one camera UBO and a
// per-object model push constant.
#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <vector>

#include "Camera.h"
#include "GLFWwindow.h"
#include "Geometry.h"
#include "RenderCommon.h"
#include "TextureLoader.h"

#include "AttachmentDetailConfig.h"
#include "DescriptorLayoutConfig.h"
#include "VertexInputStateConfig.h"

#include "CmdOpDrawFrame.h"
#include "CmdSyncDevice.h"
#include "IRenderPass.h"
#include "LoopSequence.h"
#include "OnceSequence.h"
#include "RenderManager.h"

#include "shader/depth_buffer_frag.hpp"
#include "shader/depth_buffer_vert.hpp"

namespace {

LoadedTexture loadTextureAuto(const std::string &primary, const std::string &fallback)
{
    try {
        return loadTexture(primary);
    } catch (const std::exception &) {
        return loadTexture(fallback);
    }
}

} // namespace

void test()
{
    auto camera = std::make_shared<Camera>(glm::vec3(0.0f, 0.0f, 3.0f));
    GLFWwindow window(800, 600, camera);
    auto mgr = RenderManager::Create(window, MAX_FRAMES_IN_FLIGHT);
    const vk::Extent2D displayExtent = *mgr->getSwapChain()->getExtent2D();

    auto vertices = CubeVertices(glm::vec3(1.0f));
    auto texture = loadTextureAuto("assets/textures/awesomeface.png", "build/textures/awesomeface.png");

    // ---- descriptor layout -------------------------------------------------
    auto layout = std::make_shared<DescriptorLayoutAtlas>(MAX_FRAMES_IN_FLIGHT);

    auto globalSet = DescriptorLayoutAtlas::makeSet<GlobalComponent, GlobalComponentTable>(
        0, {GlobalComponent::ebaseColor}, UpdateFrequency::eStatic, true);
    globalSet.specs[0] = TextureImageSpec{static_cast<uint32_t>(texture.width),
                                          static_cast<uint32_t>(texture.height),
                                          static_cast<uint32_t>(texture.channels), (texture.mipLevels)};

    auto environmentSet = DescriptorLayoutAtlas::makeSet<EnvironmentComponent, EnvironmentComponentTable>(
        1, {EnvironmentComponent::eCameraMatrix}, UpdateFrequency::eEachFrame, false);
    environmentSet.specs[0] = GBufferSpec{1, static_cast<uint32_t>(sizeof(CameraMatrix)), DataTypes::eCustom};

    layout->setDetails({globalSet, environmentSet});
    layout->setPushConstant(static_cast<uint32_t>(sizeof(ObjectParams)), vk::ShaderStageFlagBits::eVertex);

    // ---- vertex input / asset ---------------------------------------------
    auto vertexInput = std::make_shared<VertexInputAtlas>(
        0,
        std::vector<VertexComponent>{VertexComponent::ePosition, VertexComponent::eColor, VertexComponent::eUV,
                                     VertexComponent::eNormal},
        static_cast<uint32_t>(vertices.size()), 0u, DataTypes::eUnsignedInt, false, 0u);

    auto asset = mgr->CreateShaderAssetGroup(vertexInput, layout);
    asset->addShaderSpirvCode("vert", std::vector<uint32_t>(shader::DEPTH_BUFFER_VERT_SPV.begin(),
                                                            shader::DEPTH_BUFFER_VERT_SPV.end()));
    asset->addShaderSpirvCode("frag", std::vector<uint32_t>(shader::DEPTH_BUFFER_FRAG_SPV.begin(),
                                                            shader::DEPTH_BUFFER_FRAG_SPV.end()));

    // ---- render pass / pipeline -------------------------------------------
    auto detail = std::make_shared<SimpleAttachmentDetail>(
        SimpleAttachmentDetail::Mode::eMSAA, *mgr->getSwapChain()->getImageFormat(), mgr->DefaultDepthFormat());
    auto renderPass = mgr->CreateRenderPass(detail);
    auto pipeline = mgr->CreatePipeline(vertexInput, layout, asset, renderPass, 0);

    // ---- one-shot upload --------------------------------------------------
    auto upload = mgr->UploadSequence();
    auto syncOp = std::make_shared<CmdSyncDevice>(asset);
    syncOp->OnUpdateRawData(ResourceTypes::eVertex, vertices);
    syncOp->OnUpdateRawData(ResourceId(0, 0), 0u, texture.pixels);
    upload->submit(syncOp, asset);

    // ---- frame loop -------------------------------------------------------
    auto sequence = mgr->DrawFrameSequence();
    auto drawOp = std::make_shared<CmdOpDrawFrame>(pipeline);
    auto frameSync = std::make_shared<CmdSyncDevice>(asset);

    while (!window.shouldClose()) {
        float currentTime = 0.0f;
        window.processInputEvents(currentTime);
        if (!sequence->prepareFrame()) {
            continue;
        }

        const uint32_t frame = sequence->getFrameInFlight();
        auto cameraMatrix = MakeCameraMatrix(*camera, static_cast<float>(displayExtent.width),
                                             static_cast<float>(displayExtent.height));
        auto model = MakeSpinningModel(currentTime);
        drawOp->OnUpdateRawData(ResourceId(1, 0), frame, cameraMatrix);
        drawOp->OnUpdateRawData(ResourceTypes::ePushConst, frame, model);

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

int main()
{
    try {
        test();
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << "\n";
        return 1;
    }
}
