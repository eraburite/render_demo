#include "CmdOpBase.h"
#include "OnceSequence.h"

#include "RenderManager.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

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

vk::CommandBufferBeginInfo oneTimeBeginInfo()
{
    vk::CommandBufferBeginInfo beginInfo{};
    beginInfo.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit;
    return beginInfo;
}

} // namespace

OnceSequence::OnceSequence(std::shared_ptr<RenderManager> renderMgr) : SequenceBase(std::move(renderMgr))
{
    createCommandPool();
    createFence();
}

OnceSequence::~OnceSequence()
{
    if (m_device) {
        m_device->waitIdle();
    }
    m_fence.reset();
    m_commandPool.reset();
}

void OnceSequence::createCommandPool()
{
    vk::CommandPoolCreateInfo poolInfo{};
    poolInfo.flags = vk::CommandPoolCreateFlagBits::eResetCommandBuffer;
    poolInfo.queueFamilyIndex = m_graphicsFamily;

    vk::CommandPool pool{};
    check(m_device->createCommandPool(&poolInfo, nullptr, &pool), "vkCreateCommandPool");
    m_commandPool = std::shared_ptr<vk::CommandPool>(new vk::CommandPool(pool),
                                                     [device = m_device](vk::CommandPool *handle) {
                                                         if (handle != nullptr && *handle) {
                                                             device->destroyCommandPool(*handle);
                                                         }
                                                         delete handle;
                                                     });
}

void OnceSequence::createFence()
{
    vk::FenceCreateInfo fenceInfo{};
    vk::Fence fence{};
    check(m_device->createFence(&fenceInfo, nullptr, &fence), "vkCreateFence");
    m_fence = std::shared_ptr<vk::Fence>(new vk::Fence(fence), [device = m_device](vk::Fence *handle) {
        if (handle != nullptr && *handle) {
            device->destroyFence(*handle);
        }
        delete handle;
    });
}

void OnceSequence::emitToDevice(const std::shared_ptr<ShaderAssetGroup> &asset)
{
    if (!asset) {
        return;
    }

    std::shared_ptr<vk::CommandBuffer> commandBuffer = allocateCommandBuffer(m_device, m_commandPool);
    commandBuffer->begin(oneTimeBeginInfo());
    asset->SyncDataToDevice(*commandBuffer, 0);
    commandBuffer->end();

    vk::SubmitInfo submitInfo{};
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &*commandBuffer;
    check(m_graphicsQueue->submit(1, &submitInfo, *m_fence), "vkQueueSubmit (upload)");
    check(m_device->waitForFences(1, &*m_fence, VK_TRUE, std::numeric_limits<uint64_t>::max()),
          "vkWaitForFences (upload)");
    check(m_device->resetFences(1, &*m_fence), "vkResetFences (upload)");

    // Command buffer is one-shot; return it to the pool before releasing staging.
    commandBuffer.reset();
    asset->OnUploadCompleted();
}

void OnceSequence::submit(const std::shared_ptr<CmdOpBase> &op, const std::shared_ptr<ShaderAssetGroup> &asset)
{
    if (!op) {
        return;
    }

    std::shared_ptr<vk::CommandBuffer> commandBuffer = allocateCommandBuffer(m_device, m_commandPool);
    commandBuffer->begin(oneTimeBeginInfo());
    DescriptorBindingState bindingState;
    op->Record(*commandBuffer, 0, bindingState);
    commandBuffer->end();

    vk::SubmitInfo submitInfo{};
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &*commandBuffer;
    check(m_graphicsQueue->submit(1, &submitInfo, *m_fence), "vkQueueSubmit (once)");
    check(m_device->waitForFences(1, &*m_fence, VK_TRUE, std::numeric_limits<uint64_t>::max()),
          "vkWaitForFences (once)");
    check(m_device->resetFences(1, &*m_fence), "vkResetFences (once)");

    commandBuffer.reset();
    if (asset) {
        asset->OnUploadCompleted();
    }
}

std::vector<std::byte> OnceSequence::ReadBack(const std::shared_ptr<ShaderAssetGroup> &asset, ResourceKey key,
                                              size_t byteSize)
{
    if (!asset) {
        return {};
    }

    std::shared_ptr<vk::CommandBuffer> commandBuffer = allocateCommandBuffer(m_device, m_commandPool);
    commandBuffer->begin(oneTimeBeginInfo());
    asset->RecordDescriptorResourceReadback(key, *commandBuffer, key.currentFrame());
    commandBuffer->end();

    vk::SubmitInfo submitInfo{};
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &*commandBuffer;
    check(m_graphicsQueue->submit(1, &submitInfo, *m_fence), "vkQueueSubmit (readback)");
    check(m_device->waitForFences(1, &*m_fence, VK_TRUE, std::numeric_limits<uint64_t>::max()),
          "vkWaitForFences (readback)");
    check(m_device->resetFences(1, &*m_fence), "vkResetFences (readback)");

    commandBuffer.reset();
    std::vector<std::byte> result = asset->ReadStagedDescriptorResource(key);
    if (byteSize > 0 && result.size() > byteSize) {
        result.resize(byteSize);
    }
    asset->OnUploadCompleted();
    return result;
}

std::vector<std::byte> OnceSequence::ReadBack(const std::shared_ptr<ShaderAssetGroup> &asset, ResourceId id,
                                              size_t byteSize)
{
    return ReadBack(asset, ResourceKey::make_key(id, 0), byteSize);
}
