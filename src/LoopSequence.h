#pragma once
#include <functional>
#include <memory>
#include <vector>
#include <vulkan/vulkan.hpp>
#include "CmdOpBase.h"
#include "DescriptorBindingState.h"
#include "IRenderPass.h"
#include "SequenceBase.h"

// Per-frame command sequence.  Owns per-frame command pool/buffer, semaphores,
// fences, timestamp pool and the descriptor binding cache.  Supports optional
// compute submission chained to the graphics submission via a semaphore.
class LoopSequence : public SequenceBase {
public:
    LoopSequence(std::shared_ptr<RenderManager> renderMgr, uint32_t maxFrames);
    ~LoopSequence();

    [[nodiscard]] bool prepareFrame();

    void beginRecording();
    void endRecording();

    void beginComputeRecording();
    void endComputeRecording(bool chainToGraphics = true);
    void recordComputeBarrier();

    void recordDispatch(const std::shared_ptr<CmdOpBase> &op);
    void record(const std::shared_ptr<CmdOpBase> &op);
    void recordPass(const std::shared_ptr<IRenderPass> &renderPass, const std::function<void()> &body);

    void submitFrame();
    [[nodiscard]] bool presentFrame();

    void InvalidateDescriptorBindings() { m_bindingState.Reset(); }
    uint32_t getFrameInFlight() const { return m_currentFrame; }

private:
    void createCommandPools();
    void createCommandBuffers();
    void createSyncObjects();
    void waitIdle();

private:
    uint32_t m_maxFrames = 2;
    uint32_t m_currentFrame = 0;
    uint32_t m_imageIndex = 0;
    uint32_t m_computeFamily = 0;
    bool m_computeActive = false;

    std::shared_ptr<vk::Queue> m_computeQueue;

    std::vector<std::shared_ptr<vk::CommandPool>> m_commandPools;      // graphics
    std::vector<std::shared_ptr<vk::CommandBuffer>> m_commandBuffers;  // graphics, one per frame
    std::vector<std::shared_ptr<vk::CommandPool>> m_computePools;
    std::vector<std::shared_ptr<vk::CommandBuffer>> m_computeBuffers;

    std::vector<std::shared_ptr<vk::Semaphore>> m_imageAvailable;
    std::vector<std::shared_ptr<vk::Semaphore>> m_renderFinished;
    std::vector<std::shared_ptr<vk::Semaphore>> m_computeFinished;
    std::vector<std::shared_ptr<vk::Fence>> m_inFlightFences;
    std::vector<std::shared_ptr<vk::Fence>> m_computeFences;

    DescriptorBindingState m_bindingState;
};
