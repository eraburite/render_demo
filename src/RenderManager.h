#pragma once
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include <vulkan/vulkan.hpp>
#include "ComputePipeline.h"
#include "DeviceMemoryAllocator.h"
#include "IDescriptorSetDetail.h"
#include "IRenderPass.h"
#include "SwapChain.h"
#include "IVertexInputStateGroup.h"
#include "VulkanHandle.h"
#include "typedefs.h"

class IUnifiedWindow;
class EventManager;
class GraphicsDebugger;
class SwapChain;
class OnceSequence;
class LoopSequence;
class ShaderAssetGroup;
class GraphicsPipeline;
class IAttachmentDetail;

// The single public entry point.  Two-phase construction: the constructor only
// wires events; initVulkan() runs inside Create() after the control block exists
// so shared_from_this() is valid.
class RenderManager : public std::enable_shared_from_this<RenderManager> {
public:
    static std::shared_ptr<RenderManager> Create(IUnifiedWindow &window, uint32_t maxFramesInFlight);
    ~RenderManager();

    RenderManager(const RenderManager &) = delete;
    RenderManager &operator=(const RenderManager &) = delete;

    // Factories
    std::shared_ptr<ShaderAssetGroup>
    CreateShaderAssetGroup(std::shared_ptr<const IVertexInputStateGroup> vertexInput,
                           std::shared_ptr<const IDescriptorSetLayoutGroup> descriptorLayout);
    std::shared_ptr<ShaderAssetGroup>
    CreateShaderAssetGroup(std::shared_ptr<const IDescriptorSetLayoutGroup> descriptorLayout); // empty vertex input
    std::shared_ptr<IRenderPass> CreateRenderPass(std::shared_ptr<const IAttachmentDetail> detail);
    std::shared_ptr<GraphicsPipeline> CreatePipeline(std::shared_ptr<const IVertexInputStateGroup> vertexInput,
                                                    std::shared_ptr<const IDescriptorSetLayoutGroup> descriptorLayout,
                                                    std::shared_ptr<ShaderAssetGroup> asset,
                                                    const std::shared_ptr<IRenderPass> &renderPass, uint32_t subpass);
    std::shared_ptr<ComputePipeline> CreateComputePipeline(
        std::shared_ptr<const IDescriptorSetLayoutGroup> descriptorLayout, std::shared_ptr<ShaderAssetGroup> asset,
        const std::string &spirvEntry = "comp", ComputePipelineOptions options = {});

    std::shared_ptr<OnceSequence> UploadSequence();
    std::shared_ptr<LoopSequence> DrawFrameSequence();

    // Accessors (aliasing shared_ptr; keeps manager alive)
    std::shared_ptr<vk::Instance> getInstance() const;
    std::shared_ptr<vk::Device> getLogicalDevice() const;
    std::shared_ptr<vk::PhysicalDevice> getPhysicalDevice() const { return m_physicalDevice; }
    std::shared_ptr<vk::Queue> getGraphicsQueue() const { return m_graphicsQueue; }
    std::shared_ptr<vk::Queue> getComputeQueue() const { return m_computeQueue; }
    std::shared_ptr<vk::Queue> getPresentQueue() const { return m_presentQueue; }
    std::shared_ptr<SwapChain> getSwapChain() const { return m_swapChain; }
    std::shared_ptr<DeviceMemoryAllocator> getMemoryAllocator() const { return m_allocator; }
    QueueFamilyIndices getQueueFamilyIndices() const { return m_queueFamilyIndices; }
    uint32_t getComputeQueueFamilyIndex() const;
    std::vector<uint32_t> sharedQueueFamilies() const;

    vk::Format DefaultDepthFormat() const { return m_depthFormat; }
    vk::SampleCountFlagBits getMaxUsableSampleCount() const { return m_msaaSamples; }
    uint32_t maxFramesInFlight() const { return m_maxFramesInFlight; }
    // Shared PSO cache: compute pipelines created through this manager reuse it,
    // so re-specialising a shader does not pay for a cold compile every time.
    vk::PipelineCache pipelineCache() const { return m_pipelineCache ? *m_pipelineCache : vk::PipelineCache{}; }
    const vk::PhysicalDeviceProperties &deviceProperties() const { return m_physicalDeviceProperties; }
    const vk::PhysicalDeviceMemoryProperties &memoryProperties() const { return m_physicalDeviceMemoryProperties; }

    bool checkAndResetResize();
    void reCreateSwapChain();

private:
    explicit RenderManager(IUnifiedWindow &window, uint32_t maxFramesInFlight);
    void initVulkan();
    void createInstance();
    void createDevice(const DeviceRequirement &req);
    std::vector<const char *> getInstanceRequiredExtensions();
    void registerRenderPass(const std::shared_ptr<IRenderPass> &renderPass);

    // Non-owning views (empty deleter) for manager-owned members, avoiding
    // "member -> handle -> manager" reference cycles.
    std::shared_ptr<vk::Instance> instanceView() const;
    std::shared_ptr<vk::Device> deviceView() const;

private:
    IUnifiedWindow &m_window;
    uint32_t m_maxFramesInFlight = 2;

    std::shared_ptr<vk::Instance> m_instance;
    std::shared_ptr<GraphicsDebugger> m_debugger;
    std::shared_ptr<vk::SurfaceKHR> m_surface;
    std::shared_ptr<vk::PhysicalDevice> m_physicalDevice;
    std::shared_ptr<vk::Device> m_device;
    // Declared after m_device so it is destroyed before the device handle.
    VulkanHandle<vk::PipelineCache, std::function<void(vk::PipelineCache)>> m_pipelineCache;
    std::shared_ptr<DeviceMemoryAllocator> m_allocator;

    vk::Format m_depthFormat = vk::Format::eD24UnormS8Uint;
    vk::SampleCountFlagBits m_msaaSamples = vk::SampleCountFlagBits::e1;
    vk::PhysicalDeviceMemoryProperties m_physicalDeviceMemoryProperties{};
    vk::PhysicalDeviceProperties m_physicalDeviceProperties{};
    QueueFamilyIndices m_queueFamilyIndices;
    std::shared_ptr<SwapChain> m_swapChain;

    std::shared_ptr<vk::Queue> m_graphicsQueue;
    std::shared_ptr<vk::Queue> m_computeQueue;
    std::shared_ptr<vk::Queue> m_presentQueue;

    std::shared_ptr<EventManager> m_events;
    std::vector<std::weak_ptr<IRenderPass>> m_renderPasses;
};
