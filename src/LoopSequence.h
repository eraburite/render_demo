#pragma once
#include <array>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vulkan/vulkan.hpp>
#include "typedefs.h"
#include "CmdOpBase.h"

class RenderManager;

class LoopSequence : public std::enable_shared_from_this<LoopSequence> {
public:
    LoopSequence(std::shared_ptr<RenderManager> renderMgr, const vk::PhysicalDeviceProperties &physicalDeviceProperties,
                 uint32_t maxFrameInFlight);
    ~LoopSequence();

    void eval(std::shared_ptr<CmdOpBase> op);
    uint32_t getFrameInFlight();

private:
    void createCommandBuffers();
    void createSyncObjects();
    void createCommandPools();
    void createTimestampQueryPools(uint32_t totalTimestamps /* for each */);
    void printShaderExecutionTime(uint32_t lastFrame);
    bool isRecordDone(uint32_t currentFrame) const
    {
        return m_isRecordDone[currentFrame] != 0;
    }

private:
    std::shared_ptr<RenderManager> m_renderMgr;
    vk::PhysicalDeviceProperties m_physicalDeviceProperties;
    std::shared_ptr<vk::Device> m_device = nullptr;
    std::shared_ptr<vk::Queue> m_graphicsQueue = nullptr;
    std::shared_ptr<vk::Queue> m_presentQueue = nullptr;
    uint32_t m_queueIndex = -1;
    // Resource Layout
    // Don't let all frames share a single pool; instead, establish corresponding relationships:
    // Frame 0 : CommandPool 0->CommandBuffers 0->QueryPool 0 // Process frames 0, 2, 4, ...
    // Frame 1 : CommandPool 1->CommandBuffers 1->QueryPool 1 // Process frames 1, 3, 5, ...
    std::vector<std::shared_ptr<vk::CommandPool>> m_commandPools{};
    std::vector<std::shared_ptr<vk::QueryPool>> m_timestampQueryPools{};
    // map: currentFrame->commandBuffers
    std::unordered_map<uint32_t, std::vector<std::shared_ptr<vk::CommandBuffer>>> m_commandBuffers{};
    uint32_t m_frameCounter = 0;
    uint32_t m_currFrameIndex = 0; // Index inflight
    uint32_t m_lastFrameIndex = 0;
    const uint32_t m_maxFrameInFlight;

    std::vector<int> m_isRecordDone = {};
    std::vector<std::shared_ptr<vk::Semaphore>> m_imageAvailableSemaphores;
    std::vector<std::shared_ptr<vk::Semaphore>> m_renderFinishedSemaphores;
    std::vector<std::shared_ptr<vk::Fence>> m_inFlightFences;
};