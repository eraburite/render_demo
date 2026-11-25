// multithreading: record one secondary command buffer per CPU worker thread and
// execute them all from the primary inside a single render pass.  Object model
// matrices are computed and frustum-culled in parallel on the CPU, then grouped
// into a per-thread storage buffer so each secondary issues one instanced draw.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "Camera.h"
#include "GLFWwindow.h"
#include "Geometry.h"
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
#include "SecondaryCommandBuffer.h"

#include "shader/cube_frag.hpp"
#include "shader/cube_vert.hpp"

namespace {

constexpr uint32_t kObjectCount = 1000;
constexpr uint32_t kGridSide = 10;
constexpr float kGridSpacing = 2.2f;
constexpr float kCubeScale = 0.45f;
constexpr float kBoundingRadius = 0.82f; // kCubeScale * sqrt(3), rounded up

struct FrustumPlane {
    glm::vec3 normal{};
    float distance = 0.0f;
};

struct ThreadTint {
    glm::vec4 color{1.0f};
};

struct Object {
    glm::vec3 position{};
    float phase = 0.0f;
};

// Gribb/Hartmann plane extraction from a column-major clip matrix.
std::array<FrustumPlane, 6> extractFrustum(const glm::mat4 &m)
{
    const glm::vec4 row0{m[0][0], m[1][0], m[2][0], m[3][0]};
    const glm::vec4 row1{m[0][1], m[1][1], m[2][1], m[3][1]};
    const glm::vec4 row2{m[0][2], m[1][2], m[2][2], m[3][2]};
    const glm::vec4 row3{m[0][3], m[1][3], m[2][3], m[3][3]};

    const std::array<glm::vec4, 6> raw = {
        row3 + row0, // left
        row3 - row0, // right
        row3 + row1, // bottom
        row3 - row1, // top
        row3 + row2, // near
        row3 - row2  // far
    };

    std::array<FrustumPlane, 6> planes{};
    for (uint32_t i = 0; i < planes.size(); ++i) {
        const glm::vec3 n(raw[i].x, raw[i].y, raw[i].z);
        const float length = glm::length(n);
        if (length > 0.0f) {
            planes[i].normal = n / length;
            planes[i].distance = raw[i].w / length;
        }
    }
    return planes;
}

bool sphereVisible(const std::array<FrustumPlane, 6> &planes, const glm::vec3 &center, float radius)
{
    for (const FrustumPlane &plane : planes) {
        if (glm::dot(plane.normal, center) + plane.distance < -radius) {
            return false;
        }
    }
    return true;
}

uint32_t chooseThreadCount()
{
    uint32_t count = std::thread::hardware_concurrency();
    count = std::clamp(count, 2u, 8u);
    return count;
}

ThreadTint threadTint(uint32_t thread, uint32_t threads)
{
    const float phase = static_cast<float>(thread) / static_cast<float>(threads);
    const float tau = 6.2831853f;
    ThreadTint tint;
    tint.color = glm::vec4(0.55f + 0.45f * std::cos(tau * phase),
                           0.55f + 0.45f * std::cos(tau * phase + 2.094395f),
                           0.55f + 0.45f * std::cos(tau * phase + 4.188790f),
                           1.0f);
    return tint;
}

// The stock IRenderPass begins its subpass with VK_SUBPASS_CONTENTS_INLINE,
// which forbids vkCmdExecuteCommands.  Wrap it so the primary starts the
// subpass with VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS instead; all other
// pass behaviour (handle, framebuffers, clear layout) is delegated unchanged.
class SecondaryRenderPass final : public IRenderPass {
public:
    explicit SecondaryRenderPass(std::shared_ptr<IRenderPass> inner) : m_inner(std::move(inner)) {}

    vk::RenderPass handle() const override { return m_inner->handle(); }
    vk::SampleCountFlagBits sampleCount() const override { return m_inner->sampleCount(); }
    uint32_t colorAttachmentCount() const override { return m_inner->colorAttachmentCount(); }
    bool isSwapChainSized() const override { return m_inner->isSwapChainSized(); }
    void createFramebuffers(const std::vector<vk::ImageView> &views, vk::Extent2D extent) override
    {
        m_inner->createFramebuffers(views, extent);
    }
    uint32_t framebufferCount() const override { return m_inner->framebufferCount(); }
    vk::Framebuffer framebuffer(uint32_t index) const override { return m_inner->framebuffer(index); }
    vk::Extent2D extent() const override { return m_inner->extent(); }

    void begin(const vk::CommandBuffer &cmd, uint32_t frameIndex, vk::Extent2D renderArea) override
    {
        vk::RenderPassBeginInfo info{};
        info.renderPass = m_inner->handle();
        const uint32_t count = m_inner->framebufferCount();
        info.framebuffer = count > 0 ? m_inner->framebuffer(frameIndex % count) : vk::Framebuffer{};
        info.renderArea.offset = vk::Offset2D(0, 0);
        info.renderArea.extent = renderArea;
        // SimpleAttachmentDetail(eDirect) declares one colour + one depth
        // attachment, both with LOAD_OP_CLEAR.
        const std::array<vk::ClearValue, 2> clears{{
            vk::ClearValue(vk::ClearColorValue(std::array<float, 4>{0.05f, 0.06f, 0.09f, 1.0f})),
            vk::ClearValue(vk::ClearDepthStencilValue(1.0f, 0))}};
        info.clearValueCount = static_cast<uint32_t>(clears.size());
        info.pClearValues = clears.data();
        cmd.beginRenderPass(&info, vk::SubpassContents::eSecondaryCommandBuffers);
    }
    void end(const vk::CommandBuffer &cmd) override { m_inner->end(cmd); }
    void nextSubpass(const vk::CommandBuffer &cmd) override { m_inner->nextSubpass(cmd); }

    const std::vector<vk::ImageView> &offscreenViews() const override { return m_inner->offscreenViews(); }
    uint32_t offscreenColorCountPerSet() const override { return m_inner->offscreenColorCountPerSet(); }
    const std::vector<vk::Image> &offscreenImages() const override { return m_inner->offscreenImages(); }
    vk::Format offscreenColorFormat(uint32_t colorSlot) const override
    {
        return m_inner->offscreenColorFormat(colorSlot);
    }

private:
    std::shared_ptr<IRenderPass> m_inner;
};

} // namespace

void test(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    const uint32_t threadCount = chooseThreadCount();
    const uint32_t objectsPerThread = (kObjectCount + threadCount - 1) / threadCount;

    auto camera = std::make_shared<Camera>(glm::vec3(0.0f, 0.0f, 20.0f));
    GLFWwindow window(1280, 720, camera);
    auto mgr = RenderManager::Create(window, MAX_FRAMES_IN_FLIGHT);

    // One cube geometry, shared (uploaded) by every per-thread asset.
    const std::vector<Vertex> vertices = CubeVertices(glm::vec3(1.0f));

    std::vector<Object> objects(kObjectCount);
    for (uint32_t i = 0; i < kObjectCount; ++i) {
        const float gx = static_cast<float>(i % kGridSide) - (kGridSide - 1) * 0.5f;
        const float gy = static_cast<float>((i / kGridSide) % kGridSide) - (kGridSide - 1) * 0.5f;
        const float gz = static_cast<float>(i / (kGridSide * kGridSide)) - (kGridSide - 1) * 0.5f;
        objects[i].position = glm::vec3(gx, gy, gz) * kGridSpacing;
        objects[i].phase = static_cast<float>(i) * 0.37f;
    }

    // set0/set2 are empty placeholders: Vulkan pipeline layouts are contiguous,
    // while the shader only reads set1 (camera) and set3 (instance matrices).
    auto layout = std::make_shared<DescriptorLayoutAtlas>(MAX_FRAMES_IN_FLIGHT);
    auto empty0 = DescriptorLayoutAtlas::makeSet<GlobalComponent, GlobalComponentTable>(
        0, {}, UpdateFrequency::eStatic, true);
    auto cameraSet = DescriptorLayoutAtlas::makeSet<EnvironmentComponent, EnvironmentComponentTable>(
        1, {EnvironmentComponent::eCameraMatrix}, UpdateFrequency::eEachFrame, false);
    cameraSet.specs[0] = GBufferSpec{1, static_cast<uint32_t>(sizeof(CameraMatrix)), DataTypes::eCustom};
    auto empty2 = DescriptorLayoutAtlas::makeSet<FrameComponent, FrameComponentTable>(
        2, {}, UpdateFrequency::eEachFrame, false);
    auto instanceSet = DescriptorLayoutAtlas::makeSet<InstanceComponent, InstanceComponentTable>(
        3, {InstanceComponent::eModelMatrix}, UpdateFrequency::eEachInstance, false);
    instanceSet.specs[0] =
        GBufferSpec{objectsPerThread, static_cast<uint32_t>(sizeof(glm::mat4)), DataTypes::eCustom};
    layout->setDetails({empty0, cameraSet, empty2, instanceSet});
    layout->setPushConstant(static_cast<uint32_t>(sizeof(ThreadTint)), vk::ShaderStageFlagBits::eVertex);

    auto vertexInput = std::make_shared<VertexInputAtlas>(
        0,
        std::vector<VertexComponent>{VertexComponent::ePosition, VertexComponent::eColor, VertexComponent::eUV,
                                     VertexComponent::eNormal},
        static_cast<uint32_t>(vertices.size()), 0u, DataTypes::eUnsignedInt, false, 0u, objectsPerThread);

    auto baseRenderPass = mgr->CreateRenderPass(std::make_shared<SimpleAttachmentDetail>(
        SimpleAttachmentDetail::Mode::eDirect, *mgr->getSwapChain()->getImageFormat(), mgr->DefaultDepthFormat()));
    auto renderPass = std::make_shared<SecondaryRenderPass>(baseRenderPass);

    // One asset / pipeline per worker.  Each secondary binds its own pipeline
    // and descriptor sets, which is what makes parallel recording legal.
    std::vector<std::shared_ptr<ShaderAssetGroup>> assets;
    std::vector<std::shared_ptr<GraphicsPipeline>> pipelines;
    std::vector<std::shared_ptr<CmdOpDrawFrame>> drawOps;
    std::vector<std::shared_ptr<CmdSyncDevice>> frameSyncOps;
    assets.reserve(threadCount);
    pipelines.reserve(threadCount);
    drawOps.reserve(threadCount);
    frameSyncOps.reserve(threadCount);

    auto upload = mgr->UploadSequence();
    for (uint32_t t = 0; t < threadCount; ++t) {
        auto asset = mgr->CreateShaderAssetGroup(vertexInput, layout);
        asset->addShaderSpirvCode("vert", std::vector<uint32_t>(shader::CUBE_VERT_SPV.begin(),
                                                                shader::CUBE_VERT_SPV.end()));
        asset->addShaderSpirvCode("frag", std::vector<uint32_t>(shader::CUBE_FRAG_SPV.begin(),
                                                                shader::CUBE_FRAG_SPV.end()));

        auto pipeline = mgr->CreatePipeline(vertexInput, layout, asset, renderPass, 0);
        auto draw = std::make_shared<CmdOpDrawFrame>(pipeline);
        auto staticSync = std::make_shared<CmdSyncDevice>(asset);
        staticSync->OnUpdateRawData(ResourceTypes::eVertex, vertices);
        upload->submit(staticSync, asset);

        assets.push_back(std::move(asset));
        pipelines.push_back(std::move(pipeline));
        drawOps.push_back(std::move(draw));
        frameSyncOps.push_back(std::make_shared<CmdSyncDevice>(assets.back()));
    }

    auto recorder = std::make_shared<SecondaryRecorder>(
        mgr->getLogicalDevice(), mgr->getQueueFamilyIndices().graphicsFamily.value(), threadCount,
        MAX_FRAMES_IN_FLIGHT);

    auto sequence = mgr->DrawFrameSequence();

    // Compact visible objects to the front of each group's matrix array.
    // Instances beyond the visible count get a near-zero (degenerate) matrix so
    // the fixed instance count stays valid without resizing the SSBO.
    const glm::mat4 culledMatrix = glm::scale(glm::mat4(1.0f), glm::vec3(0.00001f));
    std::vector<std::vector<glm::mat4>> groupModels(threadCount,
                                                    std::vector<glm::mat4>(objectsPerThread, culledMatrix));
    std::vector<uint32_t> visibleCounts(threadCount, 0u);
    std::vector<uint32_t> groupBegin(threadCount, 0u);
    std::vector<uint32_t> groupEnd(threadCount, 0u);
    for (uint32_t t = 0; t < threadCount; ++t) {
        groupBegin[t] = t * objectsPerThread;
        groupEnd[t] = std::min(groupBegin[t] + objectsPerThread, kObjectCount);
    }

    std::cout << "[multithreading] " << threadCount << " worker threads, " << kObjectCount << " objects, "
              << objectsPerThread << " matrix slots per thread\n";

    uint32_t frames = 0;
    while (!window.shouldClose()) {
        window.poolEvents();
        float currentTime = 0.0f;
        window.processInputEvents(currentTime);

        if (!sequence->prepareFrame()) {
            continue;
        }
        const uint32_t frame = sequence->getFrameInFlight();
        const auto extent = mgr->getSwapChain()->getExtent2D();
        const vk::Extent2D renderExtent = *extent;
        const CameraMatrix matrices = MakeCameraMatrix(*camera, renderExtent);
        const std::array<FrustumPlane, 6> frustum = extractFrustum(matrices.proj * matrices.view);

        // Parallel phase: CPU culling / model generation AND secondary recording.
        std::vector<std::thread> workers;
        workers.reserve(threadCount);
        for (uint32_t t = 0; t < threadCount; ++t) {
            workers.emplace_back([&, t] {
                std::vector<glm::mat4> &models = groupModels[t];
                uint32_t visible = 0;
                for (uint32_t i = groupBegin[t]; i < groupEnd[t]; ++i) {
                    const Object &object = objects[i];
                    const glm::vec3 center = object.position
                        + glm::vec3(0.0f, std::sin(currentTime * 1.5f + object.phase) * 0.3f, 0.0f);
                    if (!sphereVisible(frustum, center, kBoundingRadius)) {
                        continue;
                    }
                    glm::mat4 model = glm::translate(glm::mat4(1.0f), center);
                    model = glm::rotate(model, currentTime * 0.8f + object.phase, glm::vec3(0.0f, 1.0f, 0.0f));
                    model = glm::scale(model, glm::vec3(kCubeScale));
                    models[visible++] = model;
                }
                visibleCounts[t] = visible;
                for (uint32_t i = visible; i < objectsPerThread; ++i) {
                    models[i] = culledMatrix;
                }

                // VK_NULL_HANDLE inheritance makes the secondary compatible with
                // whichever swapchain framebuffer recordPass() selects this frame.
                const vk::CommandBuffer secondary =
                    recorder->begin(t, frame, renderPass->handle(), 0, vk::Framebuffer{});
                // Dynamic viewport/scissor are not inherited from the primary;
                // each secondary must set them before issuing its draw.
                const vk::Viewport viewport{0.0f, 0.0f, static_cast<float>(renderExtent.width),
                                            static_cast<float>(renderExtent.height), 0.0f, 1.0f};
                const vk::Rect2D scissor{{0, 0}, renderExtent};
                secondary.setViewport(0, 1, &viewport);
                secondary.setScissor(0, 1, &scissor);
                drawOps[t]->Record(secondary, frame, recorder->bindingState(t));
                recorder->endBuffer(t, frame);
            });
        }
        for (std::thread &worker : workers) {
            worker.join();
        }

        // Per-frame resource updates (camera, instance matrices, per-thread tint).
        for (uint32_t t = 0; t < threadCount; ++t) {
            drawOps[t]->OnUpdateRawData(ResourceId(1, 0), frame, matrices);
            drawOps[t]->OnUpdateRawData(ResourceId(3, 0), frame, groupModels[t]);
            drawOps[t]->OnUpdateRawData(ResourceTypes::ePushConst, frame, threadTint(t, threadCount));
        }

        sequence->beginRecording();
        for (const auto &sync : frameSyncOps) {
            sequence->record(sync);
        }
        // Opening the subpass with eSecondaryCommandBuffers is mandatory:
        // VUID-vkCmdExecuteCommands-contents-09680 rejects executeCommands()
        // when beginRenderPass used eInline.
        sequence->recordPass(renderPass, [&]() { sequence->executeSecondaries(recorder->collect(frame)); },
                             vk::SubpassContents::eSecondaryCommandBuffers);
        sequence->endRecording();
        sequence->submitFrame();

        if (frames % 120 == 0) {
            uint32_t totalVisible = 0;
            for (uint32_t count : visibleCounts) {
                totalVisible += count;
            }
            std::cout << "[multithreading] frame " << frames << ": " << totalVisible << "/" << kObjectCount
                      << " objects survived frustum culling\n";
        }

        if (!sequence->presentFrame()) {
            continue;
        }
        ++frames;
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
