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
    // Records raw commands outside any render pass.  The callback receives the
    // command buffer and the swapchain image index acquired for this frame, so a
    // pass can dispatch into a storage image and copy the result to the
    // swapchain instead of going through a render pass.  Ray tracing has no
    // render pass at all (see DESIGN.md 31.6).  Using this widens the
    // image-available wait stage in submitFrame() to eAllCommands, because the
    // write then happens in the ray-tracing / transfer stages rather than
    // COLOR_ATTACHMENT_OUTPUT.
    void recordRaw(const std::function<void(vk::CommandBuffer, uint32_t imageIndex)> &body);
    void recordPass(const std::shared_ptr<IRenderPass> &renderPass, const std::function<void()> &body);
    // Same as above but opens the first subpass with the given contents, e.g.
    // eSecondaryCommandBuffers when the body executes secondary buffers.
    void recordPass(const std::shared_ptr<IRenderPass> &renderPass, const std::function<void()> &body,
                    vk::SubpassContents contents);
    // Executes secondary command buffers inside the currently open renderPass
    // body.  Throws when called outside recordPass().
    void executeSecondaries(const std::vector<vk::CommandBuffer> &secondaries);
    // Advances the render pass opened by the enclosing recordPass() body to the
    // next subpass.  Throws when called outside recordPass().
    void nextSubpass();

    void submitFrame();
    [[nodiscard]] bool presentFrame();

    void InvalidateDescriptorBindings() { m_bindingState.Reset(); }
    uint32_t getFrameInFlight() const { return m_currentFrame; }
    // Swapchain image acquired for the current frame.  Exposed so a capture can
    // pick the image that was just rendered (see IRenderPass / ImageReadback).
    uint32_t getImageIndex() const { return m_imageIndex; }

private:
    void createCommandPools();
    void createCommandBuffers();
    void createSyncObjects();
    void ensureRenderFinishedSize(uint32_t imageIndex);
    void rebuildSwapChain();
    void waitIdle();

private:
    uint32_t m_maxFrames = 2;
    uint32_t m_currentFrame = 0;
    uint32_t m_imageIndex = 0;
    uint32_t m_computeFamily = 0;
    bool m_computeActive = false;
    // Set by recordRaw() and cleared at the start of every frame; widens the
    // image-available wait stage in submitFrame() (see recordRaw).
    bool m_rawRecording = false;

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
    // Render pass currently open inside recordPass(); used by nextSubpass().
    std::shared_ptr<IRenderPass> m_activeRenderPass;
};
