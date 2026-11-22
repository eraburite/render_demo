#include "LoopSequence.h"

#include "RenderManager.h"
#include "SwapChain.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

void check(vk::Result result, const char *what)
{
    if (result != vk::Result::eSuccess) {
        throw std::runtime_error(std::string(what) + " failed: " + vk::to_string(result));
    }
}

std::shared_ptr<vk::CommandBuffer> allocateCommandBuffer(const std::shared_ptr<vk::Device> &device,
                                                         const std::shared_ptr<vk::CommandPool> &pool)
{
    vk::CommandBufferAllocateInfo allocInfo{};
    allocInfo.commandPool = *pool;
    allocInfo.level = vk::CommandBufferLevel::ePrimary;
    allocInfo.commandBufferCount = 1;

    vk::CommandBuffer commandBuffer{};
    check(device->allocateCommandBuffers(&allocInfo, &commandBuffer), "vkAllocateCommandBuffers");
    return std::shared_ptr<vk::CommandBuffer>(new vk::CommandBuffer(commandBuffer),
                                              [device, pool](vk::CommandBuffer *handle) {
                                                  if (handle != nullptr) {
                                                      device->freeCommandBuffers(*pool, 1, handle);
                                                      delete handle;
                                                  }
                                              });
}

std::shared_ptr<vk::Semaphore> createSemaphore(const std::shared_ptr<vk::Device> &device)
{
    vk::SemaphoreCreateInfo createInfo{};
    vk::Semaphore semaphore{};
    check(device->createSemaphore(&createInfo, nullptr, &semaphore), "vkCreateSemaphore");
    return std::shared_ptr<vk::Semaphore>(new vk::Semaphore(semaphore), [device](vk::Semaphore *handle) {
        if (handle != nullptr && *handle) {
            device->destroySemaphore(*handle);
        }
        delete handle;
    });
}

std::shared_ptr<vk::Fence> createFence(const std::shared_ptr<vk::Device> &device)
{
    vk::FenceCreateInfo createInfo{};
    createInfo.flags = vk::FenceCreateFlagBits::eSignaled;
    vk::Fence fence{};
    check(device->createFence(&createInfo, nullptr, &fence), "vkCreateFence");
    return std::shared_ptr<vk::Fence>(new vk::Fence(fence), [device](vk::Fence *handle) {
        if (handle != nullptr && *handle) {
            device->destroyFence(*handle);
        }
        delete handle;
    });
}

void waitAndResetFence(const std::shared_ptr<vk::Device> &device, const std::shared_ptr<vk::Fence> &fence,
                       const char *what)
{
    check(device->waitForFences(1, &*fence, VK_TRUE, std::numeric_limits<uint64_t>::max()), what);
    check(device->resetFences(1, &*fence), "vkResetFences");
}

// Wait without resetting.  The frame fence is reset in submitFrame() right
// before the submit that signals it, so a frame that ends early (out-of-date
// swapchain -> prepareFrame() returns false, nothing is submitted) keeps its
// fence signaled instead of leaving prepareFrame() blocked forever on the next
// iteration for the same frame slot.
void waitFence(const std::shared_ptr<vk::Device> &device, const std::shared_ptr<vk::Fence> &fence, const char *what)
{
    check(device->waitForFences(1, &*fence, VK_TRUE, std::numeric_limits<uint64_t>::max()), what);
}

} // namespace

LoopSequence::LoopSequence(std::shared_ptr<RenderManager> renderMgr, uint32_t maxFrames) :
    SequenceBase(std::move(renderMgr)), m_maxFrames(maxFrames == 0 ? 1 : maxFrames)
{
    m_computeQueue = m_renderMgr->getComputeQueue();
    if (!m_computeQueue) {
        m_computeQueue = m_graphicsQueue;
    }
    m_computeFamily = m_renderMgr->getComputeQueueFamilyIndex();

    createCommandPools();
    createCommandBuffers();
    createSyncObjects();

    // One query pool per frame slot: reading one frame while another resets
    // its own pool would otherwise race with the GPU.
    for (uint32_t frame = 0; frame < m_maxFrames; ++frame) {
        CreateTimestampQueryPool(2); // may legitimately return nullptr
    }
}

LoopSequence::~LoopSequence()
{
    waitIdle();

    m_renderFinished.clear();
    m_imageAvailable.clear();
    m_computeFinished.clear();
    m_inFlightFences.clear();
    m_computeFences.clear();
    m_commandBuffers.clear();
    m_computeBuffers.clear();
    m_commandPools.clear();
    m_computePools.clear();
    m_timestampPools.clear();
}

void LoopSequence::createCommandPools()
{
    m_commandPools.resize(m_maxFrames);
    m_computePools.resize(m_maxFrames);

    for (uint32_t frame = 0; frame < m_maxFrames; ++frame) {
        vk::CommandPoolCreateInfo graphicsInfo{};
        graphicsInfo.flags = vk::CommandPoolCreateFlagBits::eResetCommandBuffer;
        graphicsInfo.queueFamilyIndex = m_graphicsFamily;
        vk::CommandPool graphicsPool{};
        check(m_device->createCommandPool(&graphicsInfo, nullptr, &graphicsPool), "vkCreateCommandPool (graphics)");
        m_commandPools[frame] = std::shared_ptr<vk::CommandPool>(new vk::CommandPool(graphicsPool),
                                                                 [device = m_device](vk::CommandPool *handle) {
                                                                     if (handle != nullptr && *handle) {
                                                                         device->destroyCommandPool(*handle);
                                                                     }
                                                                     delete handle;
                                                                 });

        vk::CommandPoolCreateInfo computeInfo{};
        computeInfo.flags = vk::CommandPoolCreateFlagBits::eResetCommandBuffer;
        computeInfo.queueFamilyIndex = m_computeFamily;
        vk::CommandPool computePool{};
        check(m_device->createCommandPool(&computeInfo, nullptr, &computePool), "vkCreateCommandPool (compute)");
        m_computePools[frame] = std::shared_ptr<vk::CommandPool>(new vk::CommandPool(computePool),
                                                                 [device = m_device](vk::CommandPool *handle) {
                                                                     if (handle != nullptr && *handle) {
                                                                         device->destroyCommandPool(*handle);
                                                                     }
                                                                     delete handle;
                                                                 });
    }
}

void LoopSequence::createCommandBuffers()
{
    m_commandBuffers.resize(m_maxFrames);
    m_computeBuffers.resize(m_maxFrames);
    for (uint32_t frame = 0; frame < m_maxFrames; ++frame) {
        m_commandBuffers[frame] = allocateCommandBuffer(m_device, m_commandPools[frame]);
        m_computeBuffers[frame] = allocateCommandBuffer(m_device, m_computePools[frame]);
    }
}

void LoopSequence::createSyncObjects()
{
    const std::shared_ptr<SwapChain> swapChain = m_renderMgr->getSwapChain();
    const uint32_t imageCount = swapChain ? static_cast<uint32_t>(swapChain->getImageViews().size()) : 0;
    // renderFinished is waited on by vkQueuePresentKHR, which may still hold it
    // after the graphics fence signals.  Indexing it by swapchain image (rather
    // than by frame slot) guarantees reuse only after that image is re-acquired,
    // at which point its previous presentation is complete (VUID 00067).
    const uint32_t presentSemaphoreCount = std::max(imageCount, static_cast<uint32_t>(1));

    m_imageAvailable.resize(m_maxFrames);
    m_renderFinished.resize(presentSemaphoreCount);
    m_computeFinished.resize(m_maxFrames);
    m_inFlightFences.resize(m_maxFrames);
    m_computeFences.resize(m_maxFrames);

    for (uint32_t frame = 0; frame < m_maxFrames; ++frame) {
        m_imageAvailable[frame] = createSemaphore(m_device);
        m_computeFinished[frame] = createSemaphore(m_device);
        m_inFlightFences[frame] = createFence(m_device);
        m_computeFences[frame] = createFence(m_device);
    }
    for (uint32_t i = 0; i < presentSemaphoreCount; ++i) {
        m_renderFinished[i] = createSemaphore(m_device);
    }
}

void LoopSequence::ensureRenderFinishedSize(uint32_t imageIndex)
{
    // A rebuilt swapchain may expose more images than the one this sequence was
    // constructed with, and m_renderFinished is indexed by image index.  Grow the
    // table on demand; fresh semaphores are unsignaled and unused, so this is
    // safe while older frames are still in flight.
    while (imageIndex >= m_renderFinished.size()) {
        m_renderFinished.push_back(createSemaphore(m_device));
    }
}

void LoopSequence::rebuildSwapChain()
{
    // checkAndResetResize() consumes the window's resize flag and notifies the
    // WindowEvent listener, which performs the rebuild.  Only fall back to an
    // explicit rebuild when the out-of-date condition did not come from that
    // flag, so a plain resize rebuilds the swapchain exactly once.
    if (!m_renderMgr->checkAndResetResize()) {
        m_renderMgr->reCreateSwapChain();
    }
}

void LoopSequence::waitIdle()
{
    if (m_device) {
        m_device->waitIdle();
    }
}

bool LoopSequence::prepareFrame()
{
    const uint32_t frame = m_currentFrame;
    // Wait only; the fence is reset in submitFrame() together with the submit
    // that re-signals it (see waitFence()).
    waitFence(m_device, m_inFlightFences[frame], "vkWaitForFences (frame)");

    // The previous frame's timestamps are safe to read now that its fence
    // has signaled.  On the very first frame there is nothing to sample.
    if (frame != 0) {
        LogExecutionTime((frame + m_maxFrames - 1) % m_maxFrames);
    }

    // Non-const: the reference is dropped before a rebuild, otherwise the old
    // swapchain outlives RenderManager::reCreateSwapChain() and the new
    // vkCreateSwapchainKHR fails with VK_ERROR_NATIVE_WINDOW_IN_USE_KHR.
    std::shared_ptr<SwapChain> swapChain = m_renderMgr->getSwapChain();
    if (!swapChain) {
        return false;
    }

    // Call the device directly so eErrorOutOfDateKHR can be handled here
    // (the SwapChain helper converts that recoverable result into an exception).
    const vk::Result acquireResult = m_device->acquireNextImageKHR(
        swapChain->handle(), std::numeric_limits<uint64_t>::max(), *m_imageAvailable[frame], nullptr, &m_imageIndex);
    if (acquireResult == vk::Result::eErrorOutOfDateKHR) {
        swapChain.reset();
        rebuildSwapChain();
        return false;
    }
    if (acquireResult != vk::Result::eSuccess && acquireResult != vk::Result::eSuboptimalKHR) {
        throw std::runtime_error(std::string("vkAcquireNextImageKHR failed: ") + vk::to_string(acquireResult));
    }
    return true;
}

void LoopSequence::beginComputeRecording()
{
    const uint32_t frame = m_currentFrame;
    waitAndResetFence(m_device, m_computeFences[frame], "vkWaitForFences (compute)");
    // Enhanced-mode overload throws on failure (no C-style Result overload).
    m_device->resetCommandPool(*m_computePools[frame], vk::CommandPoolResetFlags{});
    m_computeBuffers[frame]->begin(vk::CommandBufferBeginInfo{});
}

void LoopSequence::recordComputeBarrier()
{
    vk::MemoryBarrier barrier{};
    barrier.srcAccessMask = vk::AccessFlagBits::eShaderWrite;
    barrier.dstAccessMask = vk::AccessFlagBits::eShaderRead;
    m_computeBuffers[m_currentFrame]->pipelineBarrier(vk::PipelineStageFlagBits::eComputeShader,
                                                      vk::PipelineStageFlagBits::eComputeShader,
                                                      vk::DependencyFlags{}, 1, &barrier, 0, nullptr, 0, nullptr);
}

void LoopSequence::endComputeRecording(bool chainToGraphics)
{
    const uint32_t frame = m_currentFrame;
    const vk::CommandBuffer &cmd = *m_computeBuffers[frame];

    vk::MemoryBarrier barrier{};
    barrier.srcAccessMask = vk::AccessFlagBits::eShaderWrite;
    if (chainToGraphics) {
        // Release compute writes at the end of this queue's execution.  The
        // graphics queue acquires visibility via the computeFinished semaphore
        // plus the graphics-side wait stage; eVertexInput is not a valid
        // pipeline stage for a compute-only queue family.
        barrier.dstAccessMask = vk::AccessFlagBits::eMemoryWrite;
        cmd.pipelineBarrier(vk::PipelineStageFlagBits::eComputeShader, vk::PipelineStageFlagBits::eBottomOfPipe,
                            vk::DependencyFlags{}, 1, &barrier, 0, nullptr, 0, nullptr);
    } else {
        barrier.dstAccessMask = vk::AccessFlagBits::eShaderRead;
        cmd.pipelineBarrier(vk::PipelineStageFlagBits::eComputeShader, vk::PipelineStageFlagBits::eComputeShader,
                            vk::DependencyFlags{}, 1, &barrier, 0, nullptr, 0, nullptr);
    }
    cmd.end();

    vk::SubmitInfo submitInfo{};
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &cmd;
    const vk::Semaphore computeFinished = *m_computeFinished[frame];
    submitInfo.signalSemaphoreCount = 1;
    submitInfo.pSignalSemaphores = &computeFinished;
    check(m_computeQueue->submit(1, &submitInfo, *m_computeFences[frame]), "vkQueueSubmit (compute)");
    m_computeActive = true;
}

void LoopSequence::beginRecording()
{
    const uint32_t frame = m_currentFrame;
    m_device->resetCommandPool(*m_commandPools[frame], vk::CommandPoolResetFlags{});
    const vk::CommandBuffer &cmd = *m_commandBuffers[frame];
    cmd.begin(vk::CommandBufferBeginInfo{});

    if (frame < m_timestampPools.size() && m_timestampPools[frame]) {
        cmd.resetQueryPool(*m_timestampPools[frame], 0, 2);
        cmd.writeTimestamp(vk::PipelineStageFlagBits::eBottomOfPipe, *m_timestampPools[frame], 0);
    }
    m_bindingState.Reset();
}

void LoopSequence::endRecording()
{
    const uint32_t frame = m_currentFrame;
    const vk::CommandBuffer &cmd = *m_commandBuffers[frame];
    if (frame < m_timestampPools.size() && m_timestampPools[frame]) {
        cmd.writeTimestamp(vk::PipelineStageFlagBits::eBottomOfPipe, *m_timestampPools[frame], 1);
    }
    cmd.end();
}

void LoopSequence::recordDispatch(const std::shared_ptr<CmdOpBase> &op)
{
    if (!op) {
        return;
    }
    DescriptorBindingState computeBindingState;
    op->Record(*m_computeBuffers[m_currentFrame], m_currentFrame, computeBindingState);
}

void LoopSequence::record(const std::shared_ptr<CmdOpBase> &op)
{
    if (!op) {
        return;
    }
    op->Record(*m_commandBuffers[m_currentFrame], m_currentFrame, m_bindingState);
}

void LoopSequence::recordPass(const std::shared_ptr<IRenderPass> &renderPass, const std::function<void()> &body)
{
    if (!renderPass) {
        return;
    }
    const vk::CommandBuffer &cmd = *m_commandBuffers[m_currentFrame];
    // Swapchain-sized framebuffers are built one per swapchain image and must
    // be selected by the acquired image index; offscreen framebuffers are
    // built per in-flight frame slot.  Using the frame index for both wrote
    // into the wrong swapchain image and presented an unrendered UNDEFINED one.
    const uint32_t framebufferIndex = renderPass->isSwapChainSized() ? m_imageIndex : m_currentFrame;
    renderPass->begin(cmd, framebufferIndex, renderPass->extent());
    m_activeRenderPass = renderPass;
    try {
        body();
    } catch (...) {
        // Keep the scope guarantee: a throwing body must not leave the render
        // pass open, or the next record on this buffer would hit a validation
        // error ("render pass already active").
        renderPass->end(cmd);
        m_activeRenderPass.reset();
        throw;
    }
    renderPass->end(cmd);
    m_activeRenderPass.reset();
}

void LoopSequence::nextSubpass()
{
    if (!m_activeRenderPass) {
        throw std::runtime_error("nextSubpass() must be called inside recordPass()");
    }
    m_activeRenderPass->nextSubpass(*m_commandBuffers[m_currentFrame]);
}

void LoopSequence::submitFrame()
{
    const uint32_t frame = m_currentFrame;
    // Always acquire the swapchain image first.  When a compute pass feeds the
    // graphics pass, also wait on its completion semaphore; the image-available
    // semaphore still must be consumed or the next acquire finds it signaled.
    vk::Semaphore waitSemaphores[2] = {*m_imageAvailable[frame], {}};
    vk::PipelineStageFlags waitStages[2] = {vk::PipelineStageFlagBits::eColorAttachmentOutput, {}};
    uint32_t waitCount = 1;
    if (m_computeActive) {
        waitSemaphores[waitCount] = *m_computeFinished[frame];
        waitStages[waitCount] = vk::PipelineStageFlagBits::eVertexInput;
        ++waitCount;
    }
    ensureRenderFinishedSize(m_imageIndex);
    const vk::Semaphore renderFinished = *m_renderFinished[m_imageIndex];
    const vk::CommandBuffer &cmd = *m_commandBuffers[frame];

    vk::SubmitInfo submitInfo{};
    submitInfo.waitSemaphoreCount = waitCount;
    submitInfo.pWaitSemaphores = waitSemaphores;
    submitInfo.pWaitDstStageMask = waitStages;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &cmd;
    submitInfo.signalSemaphoreCount = 1;
    submitInfo.pSignalSemaphores = &renderFinished;

    // Reset right before the submit that signals it so an aborted frame cannot
    // leave the fence unsignaled and stall the next wait (see waitFence()).
    check(m_device->resetFences(1, &*m_inFlightFences[frame]), "vkResetFences");
    check(m_graphicsQueue->submit(1, &submitInfo, *m_inFlightFences[frame]), "vkQueueSubmit (graphics)");
    m_computeActive = false;
}

bool LoopSequence::presentFrame()
{
    // Non-const: released before any rebuild so the old swapchain is destroyed
    // by RenderManager::reCreateSwapChain() (VUID / NATIVE_WINDOW_IN_USE).
    std::shared_ptr<SwapChain> swapChain = m_renderMgr->getSwapChain();
    if (!swapChain) {
        return false;
    }

    // Direct presentKHR keeps eErrorOutOfDateKHR recoverable (the SwapChain
    // helper turns it into an exception).
    const vk::Semaphore renderFinished = *m_renderFinished[m_imageIndex];
    vk::PresentInfoKHR presentInfo{};
    presentInfo.waitSemaphoreCount = 1;
    presentInfo.pWaitSemaphores = &renderFinished;
    const vk::SwapchainKHR swapchains[] = {swapChain->handle()};
    presentInfo.swapchainCount = 1;
    presentInfo.pSwapchains = swapchains;
    presentInfo.pImageIndices = &m_imageIndex;

    const vk::Result presentResult = m_presentQueue->presentKHR(&presentInfo);
    // Our reference must be gone before the swapchain is recreated.
    swapChain.reset();
    if (presentResult == vk::Result::eErrorOutOfDateKHR || presentResult == vk::Result::eSuboptimalKHR) {
        rebuildSwapChain();
        return false;
    }
    // The resize flag is consumed by the WindowEvent listener, which already
    // rebuilt the swapchain and framebuffers; rebuilding again here would
    // recreate them twice per resize.
    if (m_renderMgr->checkAndResetResize()) {
        return false;
    }
    if (presentResult != vk::Result::eSuccess) {
        throw std::runtime_error(std::string("vkQueuePresentKHR failed: ") + vk::to_string(presentResult));
    }

    m_currentFrame = (m_currentFrame + 1) % m_maxFrames;
    return true;
}
