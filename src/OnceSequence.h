#pragma once
#include <array>
#include <cstdint>
#include <memory>
#include <vulkan/vulkan.hpp>
#include "typedefs.h"
#include "CmdOpBase.h"

class RenderManager;

class OnceSequence : public std::enable_shared_from_this<OnceSequence> {
public:
    OnceSequence(std::shared_ptr<RenderManager> renderMgr);
    ~OnceSequence();

    void eval(std::shared_ptr<CmdOpBase> op);

private:
    void createCommandPool();
    void createTimestampQueryPool(uint32_t totalTimestamps);
    void printShaderExecutionTime();

private:
    std::shared_ptr<RenderManager> m_renderMgr;
    std::shared_ptr<vk::PhysicalDevice> m_physicalDevice = nullptr;
    std::shared_ptr<vk::Device> m_device = nullptr;
    std::shared_ptr<vk::Queue> m_graphicsQueue = nullptr;
    uint32_t m_queueIndex = -1;

    vk::Fence m_fence;
    std::shared_ptr<vk::CommandPool> m_commandPool = nullptr;
    std::shared_ptr<vk::QueryPool> m_timestampQueryPool = nullptr;
};