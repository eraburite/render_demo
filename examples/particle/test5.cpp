// particle: one compute pass updates 8192 particles in an SSBO, the graphics
// pass expands them into camera-facing quads reading the very same asset.
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <memory>
#include <random>
#include <vector>

#include "Camera.h"
#include "GLFWwindow.h"
#include "RenderCommon.h"

#include "AttachmentDetailConfig.h"
#include "ComputeLayoutConfig.h"
#include "VertexInputStateConfig.h"

#include "CmdOpComputeDispatch.h"
#include "CmdOpDrawFrame.h"
#include "CmdSyncDevice.h"
#include "ComputePipeline.h"
#include "GraphicsPipeline.h"
#include "IRenderPass.h"
#include "LoopSequence.h"
#include "OnceSequence.h"
#include "RenderManager.h"

#include "shader/particle_comp.hpp"
#include "shader/particle_frag.hpp"
#include "shader/particle_vert.hpp"

namespace {

constexpr uint32_t PARTICLE_COUNT = 8192;
constexpr uint32_t LOCAL_SIZE_X = 256;

struct Particle {
    glm::vec4 position;
    glm::vec4 velocity;
    glm::vec4 color;
};

std::vector<Particle> makeParticles()
{
    std::vector<Particle> particles;
    particles.reserve(PARTICLE_COUNT);
    std::mt19937 rng(12345u);
    std::uniform_real_distribution<float> dist(-1.0f, 1.0f);
    for (uint32_t i = 0; i < PARTICLE_COUNT; ++i) {
        Particle p{};
        p.position = glm::vec4(dist(rng) * 1.5f, dist(rng) * 1.5f, dist(rng) * 0.5f, 1.0f);
        p.velocity = glm::vec4(dist(rng) * 0.1f, 0.4f + std::abs(dist(rng)) * 0.4f, 0.0f, 0.0f);
        p.color = glm::vec4(0.5f + 0.5f * dist(rng), 0.5f + 0.5f * dist(rng), 0.5f + 0.5f * dist(rng), 1.0f);
        particles.push_back(p);
    }
    return particles;
}

} // namespace

void test()
{
    auto camera = std::make_shared<Camera>(glm::vec3(0.0f, 0.0f, 3.0f));
    GLFWwindow window(800, 600, camera);
    auto mgr = RenderManager::Create(window, MAX_FRAMES_IN_FLIGHT);
    const vk::Extent2D displayExtent = *mgr->getSwapChain()->getExtent2D();

    // ---- one compute layout, shared by compute and graphics --------------
    PushConstantDetail viewProjPush{static_cast<uint32_t>(sizeof(glm::mat4)),
                                    vk::ShaderStageFlagBits::eCompute | vk::ShaderStageFlagBits::eVertex, 0};
    auto layout = std::make_shared<ComputeLayoutAtlas>(
        std::vector<ComputeComponent>{ComputeComponent::eParameter, ComputeComponent::eParticleInput,
                                      ComputeComponent::eParticleOutput},
        MAX_FRAMES_IN_FLIGHT, false, viewProjPush);
    layout->setSpec(0, GBufferSpec{1, 4 * static_cast<uint32_t>(sizeof(float)), DataTypes::eFloat});
    layout->setSpec(1, GBufferSpec{PARTICLE_COUNT, static_cast<uint32_t>(sizeof(Particle)), DataTypes::eCustom});
    layout->setSpec(2, GBufferSpec{PARTICLE_COUNT, static_cast<uint32_t>(sizeof(Particle)), DataTypes::eCustom});

    auto asset = mgr->CreateShaderAssetGroup(layout);
    asset->addShaderSpirvCode("comp", std::vector<uint32_t>(shader::PARTICLE_COMP_SPV.begin(),
                                                            shader::PARTICLE_COMP_SPV.end()));
    asset->addShaderSpirvCode("vert", std::vector<uint32_t>(shader::PARTICLE_VERT_SPV.begin(),
                                                            shader::PARTICLE_VERT_SPV.end()));
    asset->addShaderSpirvCode("frag", std::vector<uint32_t>(shader::PARTICLE_FRAG_SPV.begin(),
                                                            shader::PARTICLE_FRAG_SPV.end()));

    // Ping-pong: input[frame] reads what output[frame-1] wrote last time.
    for (uint32_t frame = 0; frame < MAX_FRAMES_IN_FLIGHT; ++frame) {
        const uint32_t source = (frame + MAX_FRAMES_IN_FLIGHT - 1) % MAX_FRAMES_IN_FLIGHT;
        const ResourceKey inputKey = ResourceKey::make_key(ResourceId(0, 1), frame);
        const ResourceKey outputKey = ResourceKey::make_key(ResourceId(0, 2), source);
        auto output = asset->DescriptorResource(outputKey);
        if (!output) {
            throw std::runtime_error("particle output buffer was not created");
        }
        asset->addDescriptorResource(inputKey, output);
        asset->AliasDescriptorResource(inputKey, outputKey);
    }

    // ---- pipelines -------------------------------------------------------
    auto computePipeline = mgr->CreateComputePipeline(layout, asset, "comp");
    auto swapPass = mgr->CreateRenderPass(std::make_shared<SimpleAttachmentDetail>(
        SimpleAttachmentDetail::Mode::eDirect, *mgr->getSwapChain()->getImageFormat(), mgr->DefaultDepthFormat()));

    auto emptyVertexInput = std::make_shared<VertexInputAtlas>(0, std::vector<VertexComponent>{}, 0u, 0u,
                                                               DataTypes::eUnsignedInt, true, PARTICLE_COUNT * 6);
    auto graphicsPipeline = mgr->CreatePipeline(emptyVertexInput, layout, asset, swapPass, 0);

    // ---- seed every output buffer with the initial particle state --------
    auto particles = makeParticles();
    auto upload = mgr->UploadSequence();
    auto syncOp = std::make_shared<CmdSyncDevice>(asset);
    for (uint32_t frame = 0; frame < MAX_FRAMES_IN_FLIGHT; ++frame) {
        syncOp->OnUpdateRawData(ResourceId(0, 2), frame, particles);
    }
    upload->submit(syncOp, asset);

    // ---- frame loop ------------------------------------------------------
    auto sequence = mgr->DrawFrameSequence();
    auto dispatchOp = std::make_shared<CmdOpComputeDispatch>(
        computePipeline, DispatchSize::FromInvocationCount(PARTICLE_COUNT, LOCAL_SIZE_X));
    auto drawOp = std::make_shared<CmdOpDrawFrame>(graphicsPipeline);
    auto frameSync = std::make_shared<CmdSyncDevice>(asset);

    while (!window.shouldClose()) {
        float currentTime = 0.0f;
        window.processInputEvents(currentTime);
        if (!sequence->prepareFrame()) {
            continue;
        }
        const uint32_t frame = sequence->getFrameInFlight();

        const float deltaTime = 1.0f / 60.0f;
        dispatchOp->OnUpdateRawData(ResourceId(0, 0), frame, deltaTime);
        auto cameraMatrix = MakeCameraMatrix(*camera, static_cast<float>(displayExtent.width),
                                             static_cast<float>(displayExtent.height));
        glm::mat4 viewProj = cameraMatrix.proj * cameraMatrix.view;
        drawOp->OnUpdateRawData(ResourceTypes::ePushConst, frame, viewProj);

        sequence->beginComputeRecording();
        sequence->recordDispatch(dispatchOp);
        sequence->recordComputeBarrier();
        sequence->endComputeRecording(true);

        sequence->beginRecording();
        sequence->record(frameSync);
        sequence->recordPass(swapPass, [&]() { sequence->record(drawOp); });
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
