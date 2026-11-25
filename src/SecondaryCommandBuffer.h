#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <stdexcept>
#include <vector>
#include <vulkan/vulkan.hpp>

#include "CmdOpBase.h"
#include "DescriptorBindingState.h"
#include "VulkanHandle.h"

// Helper for recording secondary command buffers outside the frame's primary
// buffer (typically one per CPU thread), then executing them inside a render
// pass on the primary.  Used by the multithreading example.
//
// Design notes:
//  * One command pool per worker thread: VkCommandPool is not thread-safe, so
//    each thread allocates and records its own buffers.  Pools are created with
//    eResetCommandBuffer so they can be reset every frame.
//  * Each secondary carries its own DescriptorBindingState (the primary's cache
//    is not valid inside a secondary, and Vulkan requires descriptor sets to be
//    bound in each command buffer that uses them).
//  * Secondary buffers recorded inside a render pass must carry a
//    VkCommandBufferInheritanceInfo naming the renderPass/subpass/framebuffer.
class SecondaryRecorder {
public:
    SecondaryRecorder(std::shared_ptr<vk::Device> device, uint32_t queueFamily, uint32_t threadCount,
                      uint32_t framesInFlight) :
        m_device(std::move(device)), m_frames(framesInFlight)
    {
        if (!m_device || threadCount == 0 || framesInFlight == 0) {
            throw std::runtime_error("SecondaryRecorder: invalid arguments");
        }
        m_workers.reserve(threadCount);
        for (uint32_t t = 0; t < threadCount; ++t) {
            Worker worker;
            vk::CommandPoolCreateInfo poolInfo{};
            poolInfo.queueFamilyIndex = queueFamily;
            poolInfo.flags = vk::CommandPoolCreateFlagBits::eResetCommandBuffer;
            check(m_device->createCommandPool(&poolInfo, nullptr, &worker.pool), "vkCreateCommandPool (secondary)");
            worker.poolHandle = VulkanHandle<vk::CommandPool, std::function<void(vk::CommandPool)>>(
                worker.pool, [device = m_device](vk::CommandPool p) { device->destroyCommandPool(p); });

            // One secondary buffer per frame-in-flight per worker.
            worker.buffers.resize(framesInFlight);
            vk::CommandBufferAllocateInfo alloc{};
            alloc.commandPool = worker.pool;
            alloc.level = vk::CommandBufferLevel::eSecondary;
            alloc.commandBufferCount = framesInFlight;
            std::vector<vk::CommandBuffer> raw(framesInFlight);
            check(m_device->allocateCommandBuffers(&alloc, raw.data()), "vkAllocateCommandBuffers (secondary)");
            for (uint32_t f = 0; f < framesInFlight; ++f) {
                worker.bufferHandles.emplace_back(raw[f], [device = m_device, pool = worker.pool](vk::CommandBuffer) {
                    (void)device;
                    (void)pool;
                    // Buffers are freed with the pool; no per-buffer free needed.
                });
                worker.buffers[f] = raw[f];
            }
            m_workers.push_back(std::move(worker));
        }
    }

    SecondaryRecorder(const SecondaryRecorder &) = delete;
    SecondaryRecorder &operator=(const SecondaryRecorder &) = delete;

    ~SecondaryRecorder() = default;

    uint32_t threadCount() const { return static_cast<uint32_t>(m_workers.size()); }

    // Begin recording worker `thread` for frame `frame` inside the given render
    // pass/subpass/framebuffer.  Returns the secondary command buffer; call
    // `end(thread)` after recording ops with `bindingState(thread)`.
    vk::CommandBuffer begin(uint32_t thread, uint32_t frame, vk::RenderPass renderPass, uint32_t subpass,
                            vk::Framebuffer framebuffer)
    {
        Worker &worker = m_workers.at(thread);
        vk::CommandBuffer cmd = worker.buffers.at(frame);
        worker.bindingState.Reset();

        vk::CommandBufferInheritanceInfo inheritance{};
        inheritance.renderPass = renderPass;
        inheritance.subpass = subpass;
        inheritance.framebuffer = framebuffer;

        vk::CommandBufferBeginInfo beginInfo{};
        beginInfo.flags = vk::CommandBufferUsageFlagBits::eRenderPassContinue;
        beginInfo.pInheritanceInfo = &inheritance;
        check(cmd.begin(&beginInfo), "vkBeginCommandBuffer (secondary)");
        return cmd;
    }

    vk::CommandBuffer buffer(uint32_t thread, uint32_t frame) const
    {
        return m_workers.at(thread).buffers.at(frame);
    }

    DescriptorBindingState &bindingState(uint32_t thread) { return m_workers.at(thread).bindingState; }

    // Dynamic viewport/scissor are per-command-buffer state: the primary's
    // renderPass->begin only sets them on the primary, so every secondary must
    // set them again before its draws or validation reports VUID 07831/07832.
    void setViewportScissor(uint32_t thread, uint32_t frame, vk::Extent2D extent)
    {
        vk::CommandBuffer cmd = buffer(thread, frame);
        const vk::Viewport viewport{0.0f, 0.0f, static_cast<float>(extent.width),
                                    static_cast<float>(extent.height), 0.0f, 1.0f};
        const vk::Rect2D scissor{{0, 0}, extent};
        cmd.setViewport(0, 1, &viewport);
        cmd.setScissor(0, 1, &scissor);
    }

    void endBuffer(uint32_t thread, uint32_t frame) { m_workers.at(thread).buffers.at(frame).end(); }

    std::vector<vk::CommandBuffer> collect(uint32_t frame) const
    {
        std::vector<vk::CommandBuffer> result;
        result.reserve(m_workers.size());
        for (const Worker &worker : m_workers) {
            result.push_back(worker.buffers.at(frame));
        }
        return result;
    }

private:
    static void check(vk::Result result, const char *what)
    {
        if (result != vk::Result::eSuccess) {
            throw std::runtime_error(std::string(what) + " failed: " + vk::to_string(result));
        }
    }

    struct Worker {
        vk::CommandPool pool{};
        VulkanHandle<vk::CommandPool, std::function<void(vk::CommandPool)>> poolHandle;
        std::vector<vk::CommandBuffer> buffers;
        std::vector<VulkanHandle<vk::CommandBuffer, std::function<void(vk::CommandBuffer)>>> bufferHandles;
        DescriptorBindingState bindingState;
    };

    std::shared_ptr<vk::Device> m_device;
    uint32_t m_frames = 0;
    std::vector<Worker> m_workers;
};
