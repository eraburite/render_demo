#include "LoopSequence.h"
#include "RenderManager.h"
#include "SwapChain.h"

LoopSequence::LoopSequence(std::shared_ptr<RenderManager> renderMgr,
                           const vk::PhysicalDeviceProperties &physicalDeviceProperties, uint32_t maxFrameInFlight) :
    m_renderMgr(renderMgr), m_physicalDeviceProperties(physicalDeviceProperties), m_maxFrameInFlight(maxFrameInFlight)
{
    m_device = renderMgr->getLogicalDevice();
    m_graphicsQueue = renderMgr->getGraphicsQueue();
    m_presentQueue = renderMgr->getPresentQueue();
    m_queueIndex = renderMgr->getQueueFamilyIndices().graphicsFamily.value();
    m_isRecordDone.resize(m_maxFrameInFlight, 0);
    createCommandPools();
    createCommandBuffers();
    createSyncObjects();
    createTimestampQueryPools(2);
}

LoopSequence::~LoopSequence() {}

uint32_t LoopSequence::getFrameInFlight()
{
    return m_currFrameIndex;
}

void LoopSequence::eval(std::shared_ptr<CmdOpBase> op)
{
    vk::Result result = vk::Result::eErrorUnknown;
    m_currFrameIndex = (m_frameCounter) % m_maxFrameInFlight;
    m_lastFrameIndex = (m_frameCounter + m_maxFrameInFlight - 1) % m_maxFrameInFlight;
    // Block the current thread and wait for the previous commit of the current group to complete.
    // std:: cout << " before waitForFences\n";
    auto start = std::chrono::steady_clock::now();
    result = m_device->waitForFences(1, m_inFlightFences[m_currFrameIndex].get(), VK_TRUE, UINT64_MAX);
    result = m_device->resetFences(1, m_inFlightFences[m_currFrameIndex].get());
    auto end = std::chrono::steady_clock::now();
    std::chrono::duration<double, std::milli> diff = end - start;
    // std:: cout << " after waitForFences cost: " << diff. count() << " ms\n";
    // At this point,  the GPU has already written the previous timestamp to the Query Pool, and the CPU can safely read
    // it.
    // Note that the timestamp data is not yet ready when printing for the first time;  this step needs to be skipped.
    if (isRecordDone(m_lastFrameIndex)) {
        printShaderExecutionTime(m_lastFrameIndex);
    }

    uint32_t imageIndex = -1;
    auto swapChain = m_renderMgr->getSwapChain();
    result = swapChain->acquireNextImage(m_imageAvailableSemaphores[m_currFrameIndex], imageIndex);
    if (result == vk::Result::eErrorOutOfDateKHR) {
        m_renderMgr->recreateSwapChain();
        return;
    } else if (result != vk::Result::eSuccess && result != vk::Result::eSuboptimalKHR) {
        throw std::runtime_error(" failed to acquire swap chain   image!");
    }

    // std:: cout << " before recordCommandBuffers\n":
    start = std::chrono::steady_clock::now();
    m_isRecordDone[m_currFrameIndex] = 0;
    // reset commandPool before each-frame record
    m_device->resetCommandPool(*m_commandPools[m_currFrameIndex], vk::CommandPoolResetFlagBits::eReleaseResources);
    // begin, Recording has started.Currently,  only  the 0th commandBuffer is being used; see createCommandBuffers.
    auto activeCommandBuffer = m_commandBuffers[m_currFrameIndex][0];
    activeCommandBuffer->begin(vk::CommandBufferBeginInfo());
    // Reset the query pool after command-> begin(must be executed during recording).
    if (m_timestampQueryPools[m_currFrameIndex]) {
        // queryCount should match to createTimestampQueryPool(2)
        activeCommandBuffer->resetQueryPool(*m_timestampQueryPools[m_currFrameIndex], 0, 2);
    }
    // Record command start [Ime
    if (m_timestampQueryPools[m_currFrameIndex]) {
        activeCommandBuffer->writeTimestamp(vk::PipelineStageFlagBits::eBottomOfPipe,
                                            *m_timestampQueryPools[m_currFrameIndex], 0);
    }

    // loop call, record concrete Shader command
    op->BindFrameBuffer(swapChain->getFramebuffer(imageIndex), swapChain->getExtent2D());
    op->Record(*activeCommandBuffer, m_currFrameIndex);
    // Record command end time
    if (m_timestampQueryPools[m_currFrameIndex]) {
        activeCommandBuffer->writeTimestamp(vk::PipelineStageFlagBits::eBottomOfPipe,
                                            *m_timestampQueryPools[m_currFrameIndex], 1);
    }

    // end
    activeCommandBuffer->end();
    m_isRecordDone[m_currFrameIndex] = 1;
    end = std::chrono::steady_clock::now();
    diff = end - start;
    // std:: cout << " after recordCommandBuffers cost" " << dɪff. count() <<" ms\n";

    // do eval
    vk::SubmitInfo submitInfo{};
    std::vector<vk::Semaphore> waitSemaphores = {*m_imageAvailableSemaphores[m_currFrameIndex]};
    vk::PipelineStageFlags waitStages[] = {vk::PipelineStageFlagBits::eColorAttachmentOutput};
    submitInfo.waitSemaphoreCount = waitSemaphores.size();
    submitInfo.pWaitSemaphores = waitSemaphores.data();
    submitInfo.pWaitDstStageMask = waitStages;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = activeCommandBuffer.get();

    std::vector<vk::Semaphore> signalSemaphores = {*m_renderFinishedSemaphores[m_currFrameIndex]};
    submitInfo.signalSemaphoreCount = signalSemaphores.size();
    submitInfo.pSignalSemaphores = signalSemaphores.data();
    // std:: cout << " before submit\n";
    start = std::chrono::steady_clock::now();
    if (m_graphicsQueue->submit(1, &submitInfo, *m_inFlightFences[m_currFrameIndex]) != vk::Result::eSuccess) {
        throw std::runtime_error(" failed to submit draw command buffer!");
    }
    end = std::chrono::steady_clock::now();
    diff = end - start;
    // std:: cout << " after submit cost: " << diff. count() << " ms\n";
    // start = std:: chrono:: steady   clock:: now();
    // result = m_ device->waitForFences(1, m_ inFlightFences[m_currFrame]. get(),     VK      TRUE,
    // UINT64 MAX); end = std:: chrono:: steady   clock:: now(); diff = end - start; std:: co ut << "
    // after submit, waitForFences cost:  " << diff. count() << " ms\n"; after eval, then q ueue to
    // present
    result = swapChain->queuePresent(m_presentQueue, imageIndex, signalSemaphores);
    if (result == vk::Result::eErrorOutOfDateKHR || result == vk::Result::eSuboptimalKHR
        || m_renderMgr->checkAndResetResize()) {
        m_renderMgr->recreateSwapChain();
    } else if (result != vk::Result::eSuccess) {
        throw std::runtime_error(" failed to present swap chain image!");
    }
    // finally, finished all, add m_frameCounter
    m_frameCounter += 1;
    return;
}

void LoopSequence::createCommandPools()
{
    m_commandPools.resize(m_maxFrameInFlight);
    for (int i = 0; i < m_maxFrameInFlight; i++) {
        vk::CommandPoolCreateInfo poolInfo{};
        poolInfo.queueFamilyIndex = m_queueIndex;
        vk::CommandPool commandPool;
        if (m_device->createCommandPool(&poolInfo, nullptr, &commandPool) != vk::Result::eSuccess) {
            throw std::runtime_error(" failed to create graphics command pool!");
        }
        m_commandPools[i] =
            std::shared_ptr<vk::CommandPool>(new vk::CommandPool(commandPool), [device = m_device](vk::CommandPool *p) {
                if (p && *p) {
                    device->destroyCommandPool(*p, nullptr);
                }
                delete p;
            });
    }
}

void LoopSequence::createTimestampQueryPools(uint32_t totalTimestamps)
{
    if (m_physicalDeviceProperties.limits.timestampComputeAndGraphics) {
        m_timestampQueryPools.resize(m_maxFrameInFlight);
        for (int i = 0; i < m_maxFrameInFlight; i++) {
            vk::QueryPoolCreateInfo queryPoolInfo;
            queryPoolInfo.setQueryCount(
                totalTimestamps); // For example,    it's      set  to 2 now, one         will   store the start
            queryPoolInfo.setQueryType(vk::QueryType::eTimestamp);
            auto pool = m_device->createQueryPool(queryPoolInfo);
            m_timestampQueryPools[i] =
                std::shared_ptr<vk::QueryPool>(new vk::QueryPool(pool), [device = m_device](vk::QueryPool *p) {
                    if (p && *p) {
                        device->destroyQueryPool(*p, nullptr);
                    }
                    delete p;
                });
        }
    } else {
        throw std::runtime_error(" Device does not support timestamps");
    }
}

void LoopSequence::createCommandBuffers()
{
    m_commandBuffers.reserve(m_maxFrameInFlight);
    for (int i = 0; i < m_maxFrameInFlight; i++) {
        std::vector<vk::CommandBuffer> commandBuffers{};
        commandBuffers.resize(1); // TODO: we only need a command for each-frame now
        vk::CommandBufferAllocateInfo allocInfo{};
        allocInfo.commandPool = *m_commandPools[i];
        allocInfo.level = vk::CommandBufferLevel::ePrimary;
        allocInfo.commandBufferCount = commandBuffers.size();
        if (m_device->allocateCommandBuffers(&allocInfo, commandBuffers.data()) != vk::Result::eSuccess) {
            throw std::runtime_error(" failed to allocate command buffers!");
        }

        for (auto &buffer : commandBuffers) {
            m_commandBuffers[i].emplace_back(
                new vk::CommandBuffer(buffer),
                [device = m_device, commandPool = m_commandPools[i]](vk::CommandBuffer *ptr) {
                    if (ptr) {
                        device->freeCommandBuffers(*commandPool, 1, ptr); // free GPU resources
                        delete ptr; // free the memory of the shared   ptr on the heap.
                    }
                });
        }
    }
}

void LoopSequence::createSyncObjects()
{
    m_imageAvailableSemaphores.resize(m_maxFrameInFlight);
    m_renderFinishedSemaphores.resize(m_maxFrameInFlight);
    m_inFlightFences.resize(m_maxFrameInFlight);
    vk::SemaphoreCreateInfo semaphoreInfo{};
    vk::FenceCreateInfo fenceInfo{};
    fenceInfo.flags = vk::FenceCreateFlagBits::eSignaled;
    for (size_t i = 0; i < m_maxFrameInFlight; i++) {
        vk::Semaphore imageAvailableSemaphores{};
        if (m_device->createSemaphore(&semaphoreInfo, nullptr, &imageAvailableSemaphores) != vk::Result::eSuccess) {
            throw std::runtime_error(" failed to create synchronization objects for a frame!");
        }

        m_imageAvailableSemaphores[i] = std::shared_ptr<vk::Semaphore>(new vk::Semaphore(imageAvailableSemaphores),
                                                                       [device = m_device](vk::Semaphore *p) {
                                                                           if (p && *p) {
                                                                               device->waitIdle();
                                                                               device->destroySemaphore(*p, nullptr);
                                                                               delete p;
                                                                           }
                                                                       });

        vk::Semaphore renderFinishedSemaphores{};
        if (m_device->createSemaphore(&semaphoreInfo, nullptr, &renderFinishedSemaphores) != vk::Result::eSuccess) {
            throw std::runtime_error(" failed to create synchronization objects for a frame!");
        }

        m_renderFinishedSemaphores[i] = std::shared_ptr<vk::Semaphore>(new vk::Semaphore(renderFinishedSemaphores),
                                                                       [device = m_device](vk::Semaphore *p) {
                                                                           if (p && *p) {
                                                                               device->waitIdle();
                                                                               device->destroySemaphore(*p, nullptr);
                                                                               delete p;
                                                                           }
                                                                       });

        vk::Fence inFlightFences{};
        if (m_device->createFence(&fenceInfo, nullptr, &inFlightFences) != vk::Result::eSuccess) {
            throw std::runtime_error(" failed to create synchronization objects for a frame!");
        }

        m_inFlightFences[i] =
            std::shared_ptr<vk::Fence>(new vk::Fence(inFlightFences), [device = m_device](vk::Fence *p) {
                device->waitIdle();
                device->destroyFence(*p, nullptr);
                delete p;
            });
    }
}

void LoopSequence::printShaderExecutionTime(uint32_t lastFrame)
{
    if (m_timestampQueryPools[lastFrame]) {
        uint64_t results[2];
        // Get results from_the GPU (use WAIT BIT to ensure synchronization)
        auto result = m_device->getQueryPoolResults(*m_timestampQueryPools[lastFrame], 0, 2, sizeof(results), results,
                                                    sizeof(uint64_t),
                                                    vk::QueryResultFlagBits::e64 | vk::QueryResultFlagBits::eWait);
        // Calculate the difference and multiply it by the hardware clock cycle (timestampPeriod is
        // in nanoseconds).
        float timestampPeriod = m_physicalDeviceProperties.limits.timestampPeriod;
        double duration_ms = (results[1] - results[0]) * (double)timestampPeriod / 1e6;
        std::cout << " Shader:: draw a frame total cost: " << duration_ms << " ms\n";
    }
}
