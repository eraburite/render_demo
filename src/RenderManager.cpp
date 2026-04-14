#include "IUnifiedWindow.h"
#include "LoopSequence.h"
#include "OnceSequence.h"
#include "RenderManager.h"
#include "PhysicalDeviceUtils.h"
#include <memory>
#include <set>
#include <iostream>
#include "typedefs.h"
#include "MeshBuffer.h"
#include "TextureImage.h"
#include "ImageResources.h"
#include "GraphicsDebugger.h"
namespace {
#ifdef NDEBUG
const bool enableValidationLayers = false;
#else
bool enableValidationLayers = true;
#endif
std::vector<const char *> deviceRequiredExtensions = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
}; // end namespace

RenderManager::RenderManager(IUnifiedWindow &window, uint32_t maxFrameInFlight) :
    m_window(window), m_maxFrameInFlight(maxFrameInFlight)
{
    initVulkan();
}

RenderManager::~RenderManager()
{
    // before cleanup (with the RAII mechanism)
    m_device->waitIdle();
}

void RenderManager::initVulkan()
{
    createInstance(); // instance
    m_surface = m_window.createSurface(m_instance);

    // Physical device and property
    DeviceRequirement requirement{true, true, deviceRequiredExtensions};
    std::tie(m_physicalDevice, m_queueFamilyIndices) = PhysicalDeviceUtils::select(m_instance, m_surface, requirement);
    m_physicalDevice->getMemoryProperties(&m_physicalDeviceMemoryProperties);
    m_physicalDevice->getProperties(&m_physicalDeviceProperties);
    m_maxPushConstantSize = getMaxPushConstantSize();
    std::cout << " Max Push Constants Size: " << m_maxPushConstantSize << " bytes" << std::endl;
    m_msaaSamples = getMaxUsableSampleCount();
    std::cout << " check msaaSamples: " << static_cast<int>(m_msaaSamples) << "\n";

    // get swapChainSupport and depthFormat
    auto swapChainSupport = PhysicalDeviceUtils::querySwapChainSupport(*m_physicalDevice, m_surface);
    auto depthFormat = PhysicalDeviceUtils::findDepthFormat(m_physicalDevice);

    // create logical device
    createDevice(requirement);
    // createRenderPass, need vk::SurfaceFormatKHR
    m_renderPass = std::make_shared<MyRenderPass>(m_device, m_msaaSamples,
                                                  swapChainSupport.chooseSwapSurfaceFormat().format, depthFormat);

    m_swapChain = std::make_shared<SwapChain>(m_device, m_window, m_queueFamilyIndices, swapChainSupport);

    m_msaaSamplesImage = std::make_shared<ImageResources>(
        m_device, m_physicalDeviceMemoryProperties, m_swapChain->getExtent2D(), *m_swapChain->getImageFormat(),
        m_msaaSamples, vk::ImageUsageFlagBits::eTransientAttachment | vk::ImageUsageFlagBits::eColorAttachment,
        vk::ImageAspectFlagBits::eColor);
    m_depthStencil = std::make_shared<ImageResources>(
        m_device, m_physicalDeviceMemoryProperties, m_swapChain->getExtent2D(), depthFormat, m_msaaSamples,
        vk::ImageUsageFlagBits::eDepthStencilAttachment, vk::ImageAspectFlagBits::eDepth);
    m_swapChain->createFramebuffers(m_renderPass->getRenderPass(), m_msaaSamplesImage->getImageView(),
                                    m_depthStencil->getImageView()); // framebuffers
}

void RenderManager::createInstance()
{
    if (enableValidationLayers) {
        m_debugger = std::make_shared<GraphicsDebugger>();
        if (m_debugger && !m_debugger->checkValidationLayerSupport()) {
            throw std::runtime_error(" validation layers/features requested, but not available!");
        }
    }

    vk::ApplicationInfo appInfo{};
    appInfo.pApplicationName = "HelloVulkan";
    appInfo.applicationVersion = VK_MAKE_VERSION(1, 3, 0);
    appInfo.pEngineName = "HelloVulkan";
    appInfo.engineVersion = VK_MAKE_VERSION(1, 3, 0);
    appInfo.apiVersion = VK_API_VERSION_1_3;

    vk::InstanceCreateInfo createInfo{};
    createInfo.pApplicationInfo = &appInfo;

    auto extensions = getInstanceRequiredExtensions();
    createInfo.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    createInfo.ppEnabledExtensionNames = extensions.data();

    // NOTE: Notice liftime requirements in vk::createInstance
    std::vector<const char *> validationLayers =
        m_debugger ? m_debugger->getRequiredLayerNames() : std::vector<const char *>();

    if (!validationLayers.empty()) {
        createInfo.enabledLayerCount = static_cast<uint32_t>(validationLayers.size());
        createInfo.ppEnabledLayerNames = validationLayers.data();
        auto &features = m_debugger->getValidationFeatures();
        createInfo.pNext = &features;
    } else {
        createInfo.enabledLayerCount = 0;
        createInfo.pNext = nullptr;
    }

    vk::Instance instance;
    auto ret = vk::createInstance(&createInfo, nullptr, &instance);
    if (ret != vk::Result::eSuccess) {
        throw std::runtime_error(" failed to create instance!");
    }
    m_instance = std::shared_ptr<vk::Instance>(new vk::Instance(instance), [](vk::Instance *p) {
        if (p && *p) {
            p->destroy();
        }
        delete p;
    });

    if (m_debugger) {
        m_debugger->setupDebugMessenger(m_instance);
    }
}

void RenderManager::createDevice(const DeviceRequirement &req)
{
    QueueFamilyIndices indices = m_queueFamilyIndices;

    std::vector<vk::DeviceQueueCreateInfo> queueCreateInfos;
    std::set<uint32_t> uniqueQueueFamilies = {indices.graphicsFamily.value(), indices.presentFamily.value()};
    float queuePriority = 1.0f;
    for (uint32_t queueFamily : uniqueQueueFamilies) {
        vk::DeviceQueueCreateInfo queueCreateInfo{};
        queueCreateInfo.queueFamilyIndex = queueFamily;
        queueCreateInfo.queueCount = 1;
        queueCreateInfo.pQueuePriorities = &queuePriority;
        queueCreateInfos.push_back(queueCreateInfo);
    }

    vk::PhysicalDeviceFeatures deviceFeatures{};
    deviceFeatures.samplerAnisotropy = VK_TRUE;

    vk::DeviceCreateInfo createInfo{};
    createInfo.queueCreateInfoCount = static_cast<uint32_t>(queueCreateInfos.size());
    createInfo.pQueueCreateInfos = queueCreateInfos.data();
    createInfo.pEnabledFeatures = &deviceFeatures;
    createInfo.enabledExtensionCount = static_cast<uint32_t>(deviceRequiredExtensions.size());
    createInfo.ppEnabledExtensionNames = deviceRequiredExtensions.data();
    // NOTE: Notice liftime requirements in createDevice
    std::vector<const char *> validationLayers =
        m_debugger ? m_debugger->getRequiredLayerNames() : std::vector<const char *>();
    if (!validationLayers.empty()) {
        createInfo.enabledLayerCount = static_cast<uint32_t>(validationLayers.size());
        createInfo.ppEnabledLayerNames = validationLayers.data();
    } else {
        createInfo.enabledLayerCount = 0;
    }

    vk::Device logicalDevice{};
    if (m_physicalDevice->createDevice(&createInfo, nullptr, &logicalDevice) != vk::Result::eSuccess) {
        throw std::runtime_error(" failed to create logical device!");
    }

    m_device = std::shared_ptr<vk::Device>(new vk::Device(logicalDevice), [instance = m_instance](vk::Device *p) {
        if (p && *p) {
            p->destroy();
        }
        delete p;
    });

    vk::Queue graphicsQueue{};
    m_device->getQueue(indices.graphicsFamily.value(), 0, &graphicsQueue);
    m_graphicsQueue = std::make_shared<vk::Queue>(graphicsQueue);
    vk::Queue presentQueue{};
    m_device->getQueue(indices.presentFamily.value(), 0, &presentQueue);
    m_presentQueue = std::make_shared<vk::Queue>(presentQueue);
}

std::vector<const char *> RenderManager::getInstanceRequiredExtensions()
{
    std::vector<const char *> extensions = m_window.QueryRequiredExtensions();
    if (m_debugger) {
        auto debugExtension = m_debugger->getRequiredExtensions();
        extensions.insert(extensions.end(), debugExtension.begin(), debugExtension.end());
    }
    return extensions;
}

vk::SampleCountFlagBits RenderManager::getMaxUsableSampleCount()
{
    vk::SampleCountFlags counts = m_physicalDeviceProperties.limits.framebufferColorSampleCounts
                                  & m_physicalDeviceProperties.limits.framebufferDepthSampleCounts;

    if (counts & vk::SampleCountFlagBits::e64) {
        return vk::SampleCountFlagBits::e64;
    }
    if (counts & vk::SampleCountFlagBits::e32) {
        return vk::SampleCountFlagBits::e32;
    }
    if (counts & vk::SampleCountFlagBits::e16) {
        return vk::SampleCountFlagBits::e16;
    }
    if (counts & vk::SampleCountFlagBits::e8) {
        return vk::SampleCountFlagBits::e8;
    }
    if (counts & vk::SampleCountFlagBits::e4) {
        return vk::SampleCountFlagBits::e4;
    }
    if (counts & vk::SampleCountFlagBits::e2) {
        return vk::SampleCountFlagBits::e2;
    }
    return vk::SampleCountFlagBits::e1;
}

uint32_t RenderManager::getMaxPushConstantSize()
{
    uint32_t maxSize = m_physicalDeviceProperties.limits.maxPushConstantsSize;
    return maxSize;
}

bool RenderManager::checkAndResetResize()
{
    return m_window.checkAndResetResize();
}

void RenderManager::recreateSwapChain()
{
    int width, height, depth;
    std::tie(width, height, depth) = m_window.getFramebufferSize();
    while (width == 0 || height == 0) {
        std::tie(width, height, depth) = m_window.getFramebufferSize();
        m_window.waitEvents();
    }
    m_device->waitIdle();

    // cleanup swapChain
    m_swapChain.reset();
    m_depthStencil.reset();
    // createSwapChain
    auto swapChainSupport = PhysicalDeviceUtils::querySwapChainSupport(*m_physicalDevice, m_surface);
    m_swapChain = std::make_shared<SwapChain>(m_device, m_window, m_queueFamilyIndices, swapChainSupport);

    m_msaaSamplesImage = std::make_shared<ImageResources>(
        m_device, m_physicalDeviceMemoryProperties, m_swapChain->getExtent2D(), *m_swapChain->getImageFormat(),
        m_msaaSamples, vk::ImageUsageFlagBits::eTransientAttachment | vk::ImageUsageFlagBits::eColorAttachment,
        vk::ImageAspectFlagBits::eColor);
    m_depthStencil = std::make_shared<ImageResources>(
        m_device, m_physicalDeviceMemoryProperties, m_swapChain->getExtent2D(),
        PhysicalDeviceUtils::findDepthFormat(m_physicalDevice), m_msaaSamples,
        vk::ImageUsageFlagBits::eDepthStencilAttachment, vk::ImageAspectFlagBits::eDepth);
    m_swapChain->createFramebuffers(m_renderPass->getRenderPass(), m_msaaSamplesImage->getImageView(),
                                    m_depthStencil->getImageView());
}

std::shared_ptr<GraphicsPipeline>
RenderManager::CreatePipeline(const VertexInputParam &inputLayout, const PipelineDescriptorParams &descLayout,
                              const std::unordered_map<std::string, std::vector<uint32_t>> &shadersSpirv)
{
    // 创建 Buffer/ Texture
    std::unordered_map<ResourceTypes, std::shared_ptr<MemoryObject>> meshBuffers{};
    auto createMeshBuff = [&meshBuffers, this](ResourceTypes type, const GBufferSpec &spec) {
        if (!IsInputResource(type)) {
            return;
        }
        if (type == ResourceTypes::eVertex) {
            auto vertexBuffer = VertexBuffer(spec);
            meshBuffers.emplace(ResourceTypes::eVertex, vertexBuffer);
        } else if (type == ResourceTypes::eIndex && spec.getTotalByte() > 0) {
            auto indexBuffer = IndexBuffer(spec);
            meshBuffers.emplace(ResourceTypes::eIndex, indexBuffer);
        }
    };

    inputLayout.dispatch(createMeshBuff);
    std::unordered_map<ResourceKey, std::shared_ptr<MemoryObject>, ResourceKeyHasher> descResources{};
    auto createResource = [&descResources, this](uint32_t currentFrame, ResourceId rscId,
                                                 vk::DescriptorType descriptorType, const auto &spec) {
        using T = std::decay_t<decltype(spec)>;

        ResourceKey key = ResourceKey::make_key(rscId, currentFrame);
        if constexpr (std::is_same_v<T, GBufferSpec>) {
            if (descriptorType == vk::DescriptorType::eUniformBuffer) {
                auto uniformObject = UniformBuffers(spec);
                descResources.emplace(key, uniformObject);
            } else if (descriptorType == vk::DescriptorType::eStorageBuffer) {
                auto storageObject = StorageBuffers(spec);
                descResources.emplace(key, storageObject);
            }
        } else if constexpr (std::is_same_v<T, TextureImageSpec>) {
            auto textureObject = TextureObjects(spec);
            descResources.emplace(key, textureObject);
        }
    };

    uint32_t maxFrame = descLayout.MaxFrameCountInFlight();
    for (uint32_t i = 0; i < maxFrame; i++) {
        descLayout.for_each_binding(i, createResource);
    }

    auto pipeline = std::make_shared<GraphicsPipeline>(
        m_device, DeviceFeatureProperty{m_msaaSamples, m_maxPushConstantSize}, m_renderPass->getRenderPass(),
        inputLayout, descLayout, meshBuffers, descResources, shadersSpirv);
    return pipeline;
}

std::shared_ptr<MemoryObject> RenderManager::VertexBuffer(GBufferSpec buffMeta)
{
    return std::make_shared<MeshBuffer>(m_device, m_physicalDeviceMemoryProperties, ResourceTypes::eVertex,
                                        MemoryTypes::eDevice, buffMeta);
}

std::shared_ptr<MemoryObject> RenderManager::IndexBuffer(GBufferSpec buffMeta)
{
    return std::make_shared<MeshBuffer>(m_device, m_physicalDeviceMemoryProperties, ResourceTypes::eIndex,
                                        MemoryTypes::eDevice, buffMeta);
}

std::shared_ptr<MemoryObject> RenderManager::UniformBuffers(GBufferSpec buffMeta)
{
    return std::make_shared<MeshBuffer>(m_device, m_physicalDeviceMemoryProperties, ResourceTypes::eUniform,
                                        MemoryTypes::eHost, buffMeta);
}

std::shared_ptr<MemoryObject> RenderManager::StorageBuffers(GBufferSpec buffMeta)
{
    return std::make_shared<MeshBuffer>(m_device, m_physicalDeviceMemoryProperties, ResourceTypes::eStorage,
                                        MemoryTypes::eDevice, buffMeta);
}
std::shared_ptr<MemoryObject> RenderManager::TextureObjects(TextureImageSpec imagMeta)
{
    // Check if image format supports linear blitting
    if (!PhysicalDeviceUtils::isFormatSupportLinearBlitting(m_physicalDevice, vk::Format::eR8G8B8A8Srgb)) {
        // throw std::runtime_error(" texture image format does not support linear blitting!");
        imagMeta.miplevels = 1;
    }
    return std::make_shared<TextureImage>(m_device, m_physicalDeviceMemoryProperties, m_physicalDeviceProperties,
                                          MemoryTypes::eDevice, imagMeta);
}

std::shared_ptr<OnceSequence> RenderManager::UploadSequence()
{
    return std::make_shared<OnceSequence>(shared_from_this());
}

std::shared_ptr<LoopSequence> RenderManager::DrawFrameSequence()
{
    return std::make_shared<LoopSequence>(shared_from_this(), m_physicalDeviceProperties, m_maxFrameInFlight);
}
