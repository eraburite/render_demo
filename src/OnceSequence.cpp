#include "typedefs.h"
#include "RenderManager.h"
#include <cstdint>
#include "OnceSequence.h"

OnceSequence::OnceSequence(std::shared_ptr<RenderManager> renderMgr) : m_renderMgr(renderMgr)
{
    m_physicalDevice = renderMgr->getPhysicalDevice();
    m_device = renderMgr->getLogicalDevice();
    m_graphicsQueue = renderMgr->getGraphicsQueue();
    m_queueIndex = renderMgr->getQueueFamilyIndices().graphicsFamily.value();
    m_fence = m_device->createFence(vk::FenceCreateInfo());
    createCommandPool();
    createTimestampQueryPool(2);
}

OnceSequence::~OnceSequence()
{
    m_device->destroy(m_fence, (vk::Optional<const vk::AllocationCallbacks>)nullptr);
}

void OnceSequence::createCommandPool()
{
    vk::CommandPoolCreateInfo poolInfo{};
    // poolInfo. flags = vk::CommandPoolCreateFlagBits::eResetCommandBuffer;
    poolInfo.queueFamilyIndex = m_queueIndex;
    vk::CommandPool commandPool;
    if (m_device->createCommandPool(&poolInfo, nullptr, &commandPool) != vk::Result::eSuccess) {
        throw std::runtime_error(" failed to create graphics command pool!");
    }

    m_commandPool =
        std::shared_ptr<vk::CommandPool>(new vk::CommandPool(commandPool), [device = m_device](vk::CommandPool *p) {
            if (p && *p) {
                device->destroyCommandPool(*p, nullptr);
            }
            delete p;
        });
}

void OnceSequence::createTimestampQueryPool(uint32_t totalTimestamps)
{
    if (!m_physicalDevice) {
        throw std::runtime_error(" Sequence physical device is null");
    }
    vk::PhysicalDeviceProperties physicalDeviceProperties = m_physicalDevice->getProperties();
    if (physicalDeviceProperties.limits.timestampComputeAndGraphics) {
        vk::QueryPoolCreateInfo queryPoolInfo;
        queryPoolInfo.setQueryCount(totalTimestamps); // For example, it's set to 2 now,  one will store the start
        queryPoolInfo.setQueryType(vk::QueryType::eTimestamp);
        auto pool = m_device->createQueryPool(queryPoolInfo);
        m_timestampQueryPool =
            std::shared_ptr<vk::QueryPool>(new vk::QueryPool(pool), [device = m_device](vk::QueryPool *p) {
                if (p && *p) {
                    device->destroy(*p, nullptr);
                }
                delete p;
            });
        // std::cout << " Query pool for timestamps created\n";
    } else {
        throw std::runtime_error(" Device does not support timestamps");
    }
}

void OnceSequence::printShaderExecutionTime()
{
    if (m_timestampQueryPool) {
        uint64_t results[2];

        auto result =
            m_device->getQueryPoolResults(*m_timestampQueryPool, 0, 2, sizeof(results), results, sizeof(uint64_t),
                                          vk::QueryResultFlagBits::e64 | vk::QueryResultFlagBits::eWait);

        vk::PhysicalDeviceProperties physicalDeviceProperties = m_physicalDevice->getProperties();
        float timestampPeriod = physicalDeviceProperties.limits.timestampPeriod;
        double duration_ms = (results[1] - results[0]) * (double)timestampPeriod / 1e6;
        std::cout << " Shader:: upload data total cost: " << duration_ms << " ms\n";
    }
}

void OnceSequence::eval(std::shared_ptr<CmdOpBase> op)
{
    // alloc command buffer
    vk::CommandBuffer commandBuffer{};
    vk::CommandBufferAllocateInfo allocInfo{};
    allocInfo.commandPool = *m_commandPool;
    allocInfo.level = vk::CommandBufferLevel::ePrimary;
    allocInfo.commandBufferCount = 1;
    if (m_device->allocateCommandBuffers(&allocInfo, &commandBuffer) != vk::Result::eSuccess) {
        throw std::runtime_error("failed to allocate command buffers!");
    }

    // begin record
    // NOTE: Silently updating textures in the background is a classic Vulka n performance optimization scenario
    // Core strategy: pre-recording + vk::CommandBufferUsageFlagBits::eSimultaneousUse
    // Allocate a dedicated Transfer Queue (if the GPU supports it, specifically for background data transfer, withol
    // interfering with the rendering stream)
    vk::CommandBufferBeginInfo beginInfo{};
    commandBuffer.begin(beginInfo);

    // Reset the query pool after command-> begin(must be executed during recording).
    if (m_timestampQueryPool) {
        commandBuffer.resetQueryPool(*m_timestampQueryPool, 0, 2);
    }
    // Record command start time
    if (m_timestampQueryPool) {
        commandBuffer.writeTimestamp(vk::PipelineStageFlagBits::eVertexInput, *m_timestampQueryPool, 0);
    }

    // record op,  such as  'Cmd0pSyncDevice',  ' Copy
    op->Record(commandBuffer, 0);

    // Record command end time
    if (m_timestampQueryPool) {
        commandBuffer.writeTimestamp(vk::PipelineStageFlagBits::eBottomOfPipe, *m_timestampQueryPool, 1);
    }

    // end record
    commandBuffer.end();

    // submit
    vk::SubmitInfo submitInfo{};
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &commandBuffer;
    auto result = m_graphicsQueue->submit(1, &submitInfo, m_fence);

    //  Waiting for finish
    uint64_t waitFor = UINT64_MAX;
    result = m_device->waitForFences(1, &m_fence, VK_TRUE, waitFor);
    m_device->resetFences({m_fence});

    // At this point, the GPU has already written the timestamp to the Query Pool,// and the CPU can safely read it.
    printShaderExecutionTime();
    m_device->freeCommandBuffers(*m_commandPool, 1, &commandBuffer);
}