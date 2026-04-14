#pragma once
#include <memory>
#include <unordered_map>
#include <vulkan/vulkan.hpp>
#include "typedefs.h"
#include "SwapChain.h"
#include "RenderPass.h"
#include "GraphicsPipeline.h"
#include "MemoryObject.h"
#include "VertexInputStructs.h"
#include "DescriptorStructs.h"

class ImageResources;
class IUnifiedWindow;
class OnceSequence;
class LoopSequence;
class GraphicsDebugger;

class RenderManager : public std::enable_shared_from_this<RenderManager> {
public:
    RenderManager(IUnifiedWindow &window, uint32_t maxFrameInFlight);
    ~RenderManager();

    std::shared_ptr<vk::PhysicalDevice> getPhysicalDevice() const
    {
        return m_physicalDevice;
    }

    std::shared_ptr<vk::Device> getLogicalDevice() const
    {
        return m_device;
    }

    std::shared_ptr<vk::Queue> getGraphicsQueue() const
    {
        return m_graphicsQueue;
    }

    std::shared_ptr<vk::Queue> getPresentQueue() const
    {
        return m_presentQueue;
    }

    QueueFamilyIndices getQueueFamilyIndices() const
    {
        return m_queueFamilyIndices;
    }

    std::shared_ptr<SwapChain> getSwapChain() const
    {
        return m_swapChain;
    }

    bool checkAndResetResize();
    void recreateSwapChain();

    std::shared_ptr<GraphicsPipeline>
    CreatePipeline(const VertexInputParam &inputLayout, const PipelineDescriptorParams &descLayout,
                   const std::unordered_map<std::string, std::vector<uint32_t>> &shadersSpirv);

    std::shared_ptr<OnceSequence> UploadSequence();
    std::shared_ptr<LoopSequence> DrawFrameSequence();

private:
    void initVulkan();
    void createInstance();
    void createDevice(const DeviceRequirement &req); // Logical device
    std::vector<const char *> getInstanceRequiredExtensions();
    vk::SampleCountFlagBits getMaxUsableSampleCount();
    uint32_t getMaxPushConstantSize();
    // the buffer layout/ buffersize is certain
    std::shared_ptr<MemoryObject> VertexBuffer(GBufferSpec buffMeta);
    std::shared_ptr<MemoryObject> IndexBuffer(GBufferSpec buffMeta);
    std::shared_ptr<MemoryObject> UniformBuffers(GBufferSpec buffMeta);
    std::shared_ptr<MemoryObject> StorageBuffers(GBufferSpec buffMeta);
    std::shared_ptr<MemoryObject> TextureObjects(TextureImageSpec imagMeta);

private:
    IUnifiedWindow &m_window;
    std::shared_ptr<vk::Instance> m_instance{nullptr};
    std::shared_ptr<GraphicsDebugger> m_debugger{nullptr};
    std::shared_ptr<vk::SurfaceKHR> m_surface;
    std::shared_ptr<vk::PhysicalDevice> m_physicalDevice{nullptr};
    std::shared_ptr<vk::Device> m_device{nullptr}; // Associated with m_physicalDevice
    std::shared_ptr<MyRenderPass> m_renderPass;
    const uint32_t m_maxFrameInFlight{1};

    uint32_t m_maxPushConstantSize{0};
    vk::SampleCountFlagBits m_msaaSamples{vk::SampleCountFlagBits::e1};
    std::shared_ptr<ImageResources> m_msaaSamplesImage;
    std::shared_ptr<ImageResources> m_depthStencil;
    vk::PhysicalDeviceMemoryProperties m_physicalDeviceMemoryProperties{};
    vk::PhysicalDeviceProperties m_physicalDeviceProperties{};
    QueueFamilyIndices m_queueFamilyIndices; // Associated with m_ physicalDevice
    std::shared_ptr<SwapChain> m_swapChain;
    std::shared_ptr<vk::Queue> m_graphicsQueue; // Associated with m_ device
    std::shared_ptr<vk::Queue> m_presentQueue;  // Associated with m_ device
};