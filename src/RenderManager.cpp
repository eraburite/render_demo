#include "RenderManager.h"

// BasicRenderPass.h stores shared_ptr<ImageAttachment> members but relies on
// the including translation unit to provide the declaration.
#include "ImageAttachment.h"
#include "BasicRenderPass.h"
#include "BufferObject.h"
#include "ComputePipeline.h"
#include "DeviceResourceUtils.h"
#include "Event.h"
#include "GraphicsDebugger.h"
#include "GraphicsPipeline.h"
#include "IUnifiedWindow.h"
#include "LoopSequence.h"
#include "OnceSequence.h"
#include "PhysicalDeviceUtils.h"
#include "ShaderAssetGroup.h"
#include "SwapChain.h"
#include "TextureImage2D.h"

#include <algorithm>
#include <set>
#include <stdexcept>
#include <string>
#include <tuple>
#include <type_traits>
#include <variant>

namespace {
#ifdef NDEBUG
constexpr bool enableValidationLayers = false;
#else
constexpr bool enableValidationLayers = true;
#endif

const std::vector<const char *> deviceRequiredExtensions = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};

void check(vk::Result result, const char *what)
{
    if (result != vk::Result::eSuccess) {
        throw std::runtime_error(std::string(what) + " failed: " + vk::to_string(result));
    }
}

vk::SampleCountFlagBits maxUsableSampleCount(const vk::PhysicalDeviceProperties &properties)
{
    const vk::SampleCountFlags counts = properties.limits.framebufferColorSampleCounts
                                        & properties.limits.framebufferDepthSampleCounts;
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

uint32_t indexTypeByteSize(DataTypes type)
{
    switch (type) {
        case DataTypes::eUnsignedChar:
        case DataTypes::eChar:
            return 1;
        case DataTypes::eUnsignedShort:
        case DataTypes::eShort:
            return 2;
        case DataTypes::eUnsignedInt:
        case DataTypes::eInt:
        case DataTypes::eFloat:
            return 4;
        case DataTypes::eDouble:
            return 8;
        default:
            return 4;
    }
}

std::vector<vk::ImageView> swapChainImageViews(const std::shared_ptr<SwapChain> &swapChain)
{
    std::vector<vk::ImageView> views;
    if (!swapChain) {
        return views;
    }
    for (const std::shared_ptr<vk::ImageView> &view : swapChain->getImageViews()) {
        if (view) {
            views.push_back(*view);
        }
    }
    return views;
}
} // namespace

RenderManager::RenderManager(IUnifiedWindow &window, uint32_t maxFramesInFlight, DeviceRequirement requirement) :
    m_window(window), m_maxFramesInFlight(maxFramesInFlight), m_requirement(std::move(requirement))
{
    // Phase one: only wire events.  initVulkan() runs from Create() after the
    // shared control block exists, so shared_from_this() is valid there.
    m_events = std::make_shared<EventManager>();
    m_events->addListener<WindowEvent>([this]() { reCreateSwapChain(); });
    m_events->addListener<SwapChainEvent>([this]() {
        if (!m_swapChain) {
            return;
        }
        const std::vector<vk::ImageView> views = swapChainImageViews(m_swapChain);
        const vk::Extent2D extent = *m_swapChain->getExtent2D();
        for (const std::weak_ptr<IRenderPass> &weakPass : m_renderPasses) {
            std::shared_ptr<IRenderPass> pass = weakPass.lock();
            if (pass && pass->isSwapChainSized()) {
                pass->createFramebuffers(views, extent);
            }
        }
    });
}

RenderManager::~RenderManager()
{
    if (m_device) {
        m_device->waitIdle();
    }
}

std::shared_ptr<RenderManager> RenderManager::Create(IUnifiedWindow &window, uint32_t maxFramesInFlight,
                                                     DeviceRequirement requirement)
{
    auto manager = std::shared_ptr<RenderManager>(new RenderManager(window, maxFramesInFlight, std::move(requirement)));
    manager->initVulkan();
    return manager;
}

void RenderManager::initVulkan()
{
    createInstance();
    m_surface = m_window.createSurface(instanceView());

    // The engine always needs a compute queue and the swapchain extension;
    // layer in caller-requested optional features on top.
    m_requirement.compute = true;
    m_requirement.extensions = deviceRequiredExtensions;
    std::tie(m_physicalDevice, m_queueFamilyIndices) =
        PhysicalDeviceUtils::select(m_instance, m_surface, m_requirement);

    m_physicalDevice->getMemoryProperties(&m_physicalDeviceMemoryProperties);
    m_physicalDevice->getProperties(&m_physicalDeviceProperties);
    m_msaaSamples = maxUsableSampleCount(m_physicalDeviceProperties);
    m_depthFormat = PhysicalDeviceUtils::findDepthFormat(m_physicalDevice);

    createDevice(m_requirement);

    // One cache for every compute pipeline this manager creates.  It starts empty
    // (this run compiles from scratch) and is only reused within the process; the
    // VK_KHR_pipeline_binary / getPipelineCacheData path for on-disk caching is
    // deliberately not wired up (see DESIGN.md 4.11).
    vk::PipelineCacheCreateInfo cacheInfo{};
    vk::PipelineCache cache{};
    check(m_device->createPipelineCache(&cacheInfo, nullptr, &cache), "vkCreatePipelineCache");
    m_pipelineCache = decltype(m_pipelineCache)(cache, [device = m_device](vk::PipelineCache handle) {
        device->destroyPipelineCache(handle);
    });

    m_allocator = std::make_shared<DeviceMemoryAllocator>(deviceView(), m_physicalDeviceMemoryProperties);

    const SwapChainSupportDetails swapChainSupport =
        PhysicalDeviceUtils::querySwapChainSupport(*m_physicalDevice, m_surface);
    m_swapChain = std::make_shared<SwapChain>(deviceView(), m_window, m_queueFamilyIndices, swapChainSupport,
                                              m_surface);

    // Event dispatcher: render passes registered from now on will be notified
    // whenever the swapchain (and therefore its framebuffers) changes.
    m_events->notify<SwapChainEvent>();
}

void RenderManager::createInstance()
{
    if (enableValidationLayers) {
        m_debugger = std::make_shared<GraphicsDebugger>();
        if (!m_debugger->checkValidationLayerSupport()) {
            throw std::runtime_error("validation layers/features requested, but not available!");
        }
    }

    vk::ApplicationInfo appInfo{};
    appInfo.pApplicationName = "HelloVulkan";
    appInfo.applicationVersion = VK_MAKE_VERSION(1, 3, 0);
    appInfo.pEngineName = "HelloVulkan";
    appInfo.engineVersion = VK_MAKE_VERSION(1, 3, 0);
    appInfo.apiVersion = VK_API_VERSION_1_3;

    const std::vector<const char *> extensions = getInstanceRequiredExtensions();
    vk::InstanceCreateInfo createInfo{};
    createInfo.pApplicationInfo = &appInfo;
    createInfo.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    createInfo.ppEnabledExtensionNames = extensions.data();

    // Both arrays/objects must outlive vk::createInstance().
    const std::vector<const char *> layers =
        m_debugger ? m_debugger->getRequiredLayerNames() : std::vector<const char *>{};
    VkValidationFeaturesEXT features{};
    if (!layers.empty()) {
        createInfo.enabledLayerCount = static_cast<uint32_t>(layers.size());
        createInfo.ppEnabledLayerNames = layers.data();
        features = m_debugger->getValidationFeatures();
        createInfo.pNext = &features;
    } else {
        createInfo.enabledLayerCount = 0;
        createInfo.pNext = nullptr;
    }

    vk::Instance instance{};
    check(vk::createInstance(&createInfo, nullptr, &instance), "vkCreateInstance");

    m_instance = std::shared_ptr<vk::Instance>(new vk::Instance(instance), [](vk::Instance *instancePtr) {
        if (instancePtr != nullptr && *instancePtr) {
            instancePtr->destroy();
        }
        delete instancePtr;
    });

    if (m_debugger) {
        m_debugger->setupDebugMessenger(instanceView());
    }
}

void RenderManager::createDevice(const DeviceRequirement &req)
{
    (void)req;
    const uint32_t graphicsFamily = m_queueFamilyIndices.graphicsFamily.value();
    const uint32_t presentFamily = m_queueFamilyIndices.presentFamily.value();
    const uint32_t computeFamily = m_queueFamilyIndices.computeFamily.value_or(graphicsFamily);

    const std::set<uint32_t> uniqueQueueFamilies = {graphicsFamily, presentFamily, computeFamily};
    std::vector<vk::DeviceQueueCreateInfo> queueCreateInfos;
    queueCreateInfos.reserve(uniqueQueueFamilies.size());
    const float queuePriority = 1.0f;
    for (uint32_t family : uniqueQueueFamilies) {
        vk::DeviceQueueCreateInfo queueCreateInfo{};
        queueCreateInfo.queueFamilyIndex = family;
        queueCreateInfo.queueCount = 1;
        queueCreateInfo.pQueuePriorities = &queuePriority;
        queueCreateInfos.push_back(queueCreateInfo);
    }

    vk::PhysicalDeviceFeatures deviceFeatures{};
    deviceFeatures.samplerAnisotropy = req.samplerAnisotropy ? VK_TRUE : VK_FALSE;
    deviceFeatures.geometryShader = req.geometryShader ? VK_TRUE : VK_FALSE;
    deviceFeatures.tessellationShader = req.tessellationShader ? VK_TRUE : VK_FALSE;
    deviceFeatures.fillModeNonSolid = req.fillModeNonSolid ? VK_TRUE : VK_FALSE;

    vk::DeviceCreateInfo createInfo{};
    createInfo.queueCreateInfoCount = static_cast<uint32_t>(queueCreateInfos.size());
    createInfo.pQueueCreateInfos = queueCreateInfos.data();
    createInfo.pEnabledFeatures = &deviceFeatures;
    createInfo.enabledExtensionCount = static_cast<uint32_t>(deviceRequiredExtensions.size());
    createInfo.ppEnabledExtensionNames = deviceRequiredExtensions.data();

    const std::vector<const char *> layers =
        m_debugger ? m_debugger->getRequiredLayerNames() : std::vector<const char *>{};
    if (!layers.empty()) {
        createInfo.enabledLayerCount = static_cast<uint32_t>(layers.size());
        createInfo.ppEnabledLayerNames = layers.data();
    }

    vk::Device device{};
    check(m_physicalDevice->createDevice(&createInfo, nullptr, &device), "vkCreateDevice");
    m_device = std::shared_ptr<vk::Device>(new vk::Device(device), [](vk::Device *devicePtr) {
        if (devicePtr != nullptr && *devicePtr) {
            devicePtr->destroy();
        }
        delete devicePtr;
    });

    vk::Queue graphicsQueue{};
    m_device->getQueue(graphicsFamily, 0, &graphicsQueue);
    m_graphicsQueue = std::make_shared<vk::Queue>(graphicsQueue);

    vk::Queue computeQueue{};
    m_device->getQueue(computeFamily, 0, &computeQueue);
    m_computeQueue = std::make_shared<vk::Queue>(computeQueue);

    vk::Queue presentQueue{};
    m_device->getQueue(presentFamily, 0, &presentQueue);
    m_presentQueue = std::make_shared<vk::Queue>(presentQueue);
}

std::vector<const char *> RenderManager::getInstanceRequiredExtensions()
{
    std::vector<const char *> extensions = m_window.QueryRequiredExtensions();
    if (m_debugger) {
        const std::vector<const char *> debugExtensions = m_debugger->getRequiredExtensions();
        extensions.insert(extensions.end(), debugExtensions.begin(), debugExtensions.end());
    }
    return extensions;
}

std::shared_ptr<vk::Instance> RenderManager::getInstance() const
{
    if (!m_instance) {
        return nullptr;
    }
    return std::shared_ptr<vk::Instance>(shared_from_this(), m_instance.get());
}

std::shared_ptr<vk::Device> RenderManager::getLogicalDevice() const
{
    if (!m_device) {
        return nullptr;
    }
    return std::shared_ptr<vk::Device>(shared_from_this(), m_device.get());
}

std::shared_ptr<vk::Instance> RenderManager::instanceView() const
{
    if (!m_instance) {
        return nullptr;
    }
    return std::shared_ptr<vk::Instance>(m_instance.get(), [](vk::Instance *) {});
}

std::shared_ptr<vk::Device> RenderManager::deviceView() const
{
    if (!m_device) {
        return nullptr;
    }
    return std::shared_ptr<vk::Device>(m_device.get(), [](vk::Device *) {});
}

uint32_t RenderManager::getComputeQueueFamilyIndex() const
{
    return m_queueFamilyIndices.computeFamily.value_or(m_queueFamilyIndices.graphicsFamily.value());
}

std::vector<uint32_t> RenderManager::sharedQueueFamilies() const
{
    std::vector<uint32_t> families;
    if (m_queueFamilyIndices.graphicsFamily.has_value()) {
        families.push_back(*m_queueFamilyIndices.graphicsFamily);
    }
    const uint32_t computeFamily = getComputeQueueFamilyIndex();
    if (std::find(families.begin(), families.end(), computeFamily) == families.end()) {
        families.push_back(computeFamily);
    }
    return families;
}

void RenderManager::registerRenderPass(const std::shared_ptr<IRenderPass> &renderPass)
{
    m_renderPasses.erase(std::remove_if(m_renderPasses.begin(), m_renderPasses.end(),
                                        [](const std::weak_ptr<IRenderPass> &weak) { return weak.expired(); }),
                         m_renderPasses.end());
    m_renderPasses.emplace_back(renderPass);
}

std::shared_ptr<IRenderPass> RenderManager::CreateRenderPass(std::shared_ptr<const IAttachmentDetail> detail)
{
    if (!detail) {
        throw std::runtime_error("CreateRenderPass requires an attachment detail!");
    }
    const vk::Extent2D extent = m_swapChain ? *m_swapChain->getExtent2D() : vk::Extent2D{1, 1};
    RenderPassBlueprint blueprint = detail->build(extent.width, extent.height, m_msaaSamples);

    // Offscreen / MRT passes keep one independent attachment set per in-flight
    // frame so two frames never race on the same target.  The count is an
    // engine-level invariant (driven by Create(window, framesInFlight)), so it
    // must not depend on a hard-coded value in the example-side config.
    if (!blueprint.swapChainSized) {
        blueprint.framebufferCount = m_maxFramesInFlight;
    }

    auto renderPass = std::make_shared<BasicRenderPass>(getLogicalDevice(), m_physicalDeviceMemoryProperties,
                                                        m_allocator, blueprint);
    registerRenderPass(renderPass);
    if (renderPass->isSwapChainSized() && m_swapChain) {
        renderPass->createFramebuffers(swapChainImageViews(m_swapChain), *m_swapChain->getExtent2D());
    }
    return renderPass;
}

std::shared_ptr<ShaderAssetGroup>
RenderManager::CreateShaderAssetGroup(std::shared_ptr<const IDescriptorSetLayoutGroup> descriptorLayout)
{
    return CreateShaderAssetGroup(nullptr, std::move(descriptorLayout));
}

std::shared_ptr<ShaderAssetGroup>
RenderManager::CreateShaderAssetGroup(std::shared_ptr<const IVertexInputStateGroup> vertexInput,
                                      std::shared_ptr<const IDescriptorSetLayoutGroup> descriptorLayout)
{
    // Vertex/index input buffers are created in CreatePipeline(); this factory
    // only materializes descriptor resources.  SPIR-V is added by the caller.
    (void)vertexInput;
    if (!descriptorLayout) {
        throw std::runtime_error("CreateShaderAssetGroup requires a descriptor layout!");
    }

    auto asset = std::make_shared<ShaderAssetGroup>(getLogicalDevice(), descriptorLayout->sharedClone(),
                                                    m_maxFramesInFlight);
    const std::vector<uint32_t> sharedFamilies = sharedQueueFamilies();

    for (const SetLayoutDetail &setDetail : descriptorLayout->setDetails()) {
        const bool shared = setDetail.shared || descriptorLayout->isSetShared(setDetail.setId);
        for (size_t i = 0; i < setDetail.bindings.size(); ++i) {
            const vk::DescriptorSetLayoutBinding &binding = setDetail.bindings[i];
            const ResourceId id = i < setDetail.resourceIds.size()
                                      ? setDetail.resourceIds[i]
                                      : ResourceId(setDetail.setId, binding.binding);
            const std::variant<GBufferSpec, TextureImageSpec> spec =
                i < setDetail.specs.size()
                    ? setDetail.specs[i]
                    : std::variant<GBufferSpec, TextureImageSpec>{GBufferSpec{}};

            auto createResource = [&]() -> std::shared_ptr<IDeviceResource> {
                return std::visit(
                    [&](const auto &typedSpec) -> std::shared_ptr<IDeviceResource> {
                        using Spec = std::decay_t<decltype(typedSpec)>;
                        if constexpr (std::is_same_v<Spec, GBufferSpec>) {
                            if (binding.descriptorType == vk::DescriptorType::eUniformBuffer) {
                                return std::make_shared<BufferObject>(getLogicalDevice(),
                                                                      m_physicalDeviceMemoryProperties, m_allocator,
                                                                      ResourceTypes::eUniform, MemoryTypes::eHost,
                                                                      typedSpec, &sharedFamilies);
                            }
                            if (binding.descriptorType == vk::DescriptorType::eStorageBuffer) {
                                return std::make_shared<BufferObject>(getLogicalDevice(),
                                                                      m_physicalDeviceMemoryProperties, m_allocator,
                                                                      ResourceTypes::eStorage, MemoryTypes::eDevice,
                                                                      typedSpec, &sharedFamilies);
                            }
                            return nullptr;
                        } else {
                            (void)binding;
                            return std::make_shared<TextureImage2D>(getLogicalDevice(),
                                                                    m_physicalDeviceMemoryProperties, m_allocator,
                                                                    typedSpec);
                        }
                    },
                    spec);
            };

            if (shared) {
                std::shared_ptr<IDeviceResource> resource = createResource();
                if (!resource) {
                    continue;
                }
                const ResourceKey first = ResourceKey::make_key(id, 0);
                asset->addDescriptorResource(first, resource);
                for (uint32_t frame = 1; frame < m_maxFramesInFlight; ++frame) {
                    asset->AliasDescriptorResource(ResourceKey::make_key(id, frame), first);
                }
            } else {
                for (uint32_t frame = 0; frame < m_maxFramesInFlight; ++frame) {
                    std::shared_ptr<IDeviceResource> resource = createResource();
                    if (resource) {
                        asset->addDescriptorResource(ResourceKey::make_key(id, frame), resource);
                    }
                }
            }
        }
    }
    return asset;
}

std::shared_ptr<GraphicsPipeline>
RenderManager::CreatePipeline(std::shared_ptr<const IVertexInputStateGroup> vertexInput,
                              std::shared_ptr<const IDescriptorSetLayoutGroup> descriptorLayout,
                              std::shared_ptr<ShaderAssetGroup> asset, const std::shared_ptr<IRenderPass> &renderPass,
                              uint32_t subpass, GraphicsPipelineConfig config)
{
    const std::shared_ptr<const IVertexInputStateGroup> vertexInputClone =
        vertexInput ? vertexInput->sharedClone() : nullptr;
    const std::shared_ptr<const IDescriptorSetLayoutGroup> descriptorLayoutClone =
        descriptorLayout ? descriptorLayout->sharedClone() : nullptr;

    if (asset && vertexInputClone && !vertexInputClone->isEmptyInputState()) {
        const std::vector<uint32_t> sharedFamilies = sharedQueueFamilies();
        const GBufferSpec vertexSpec{vertexInputClone->vertexCount(), vertexInputClone->vertexByteSize(),
                                     DataTypes::eCustom};
        asset->addInputResource(ResourceTypes::eVertex,
                                std::make_shared<BufferObject>(getLogicalDevice(), m_physicalDeviceMemoryProperties,
                                                               m_allocator, ResourceTypes::eVertex, MemoryTypes::eDevice,
                                                               vertexSpec, &sharedFamilies));
        if (vertexInputClone->hasIndexBuffer() && vertexInputClone->indexCount() > 0) {
            // The element type must stay concrete: BufferObject::bindToPipeline
            // derives vk::IndexType from it.  eCustom would make every indexed
            // draw throw "cannot infer index type from buffer spec".
            const GBufferSpec indexSpec{vertexInputClone->indexCount(),
                                        indexTypeByteSize(vertexInputClone->indexType()),
                                        vertexInputClone->indexType()};
            asset->addInputResource(ResourceTypes::eIndex,
                                    std::make_shared<BufferObject>(getLogicalDevice(),
                                                                   m_physicalDeviceMemoryProperties, m_allocator,
                                                                   ResourceTypes::eIndex, MemoryTypes::eDevice,
                                                                   indexSpec, &sharedFamilies));
        }
    }

    DeviceFeatureProperty feature;
    feature.msaaSamples = m_msaaSamples;
    feature.maxPushConstantSize = m_physicalDeviceProperties.limits.maxPushConstantsSize;

    auto pipeline = std::make_shared<GraphicsPipeline>(getLogicalDevice(), feature, vertexInputClone,
                                                       descriptorLayoutClone, asset, renderPass, subpass,
                                                       std::move(config));
    pipeline->builtUp();
    return pipeline;
}

std::shared_ptr<ComputePipeline>
RenderManager::CreateComputePipeline(std::shared_ptr<const IDescriptorSetLayoutGroup> descriptorLayout,
                                     std::shared_ptr<ShaderAssetGroup> asset, const std::string &spirvEntry,
                                     ComputePipelineOptions options)
{
    const std::shared_ptr<const IDescriptorSetLayoutGroup> layoutClone =
        descriptorLayout ? descriptorLayout->sharedClone() : nullptr;
    // The manager cache is the default; an explicit cache in `options` wins so a
    // caller can isolate (or share) compiles on purpose.
    if (!options.pipelineCache) {
        options.pipelineCache = pipelineCache();
    }
    auto pipeline = std::make_shared<ComputePipeline>(getLogicalDevice(), layoutClone, asset, spirvEntry,
                                                      m_physicalDeviceProperties.limits, std::move(options));
    pipeline->builtUp();
    return pipeline;
}

std::shared_ptr<OnceSequence> RenderManager::UploadSequence()
{
    return std::make_shared<OnceSequence>(shared_from_this());
}

std::shared_ptr<LoopSequence> RenderManager::DrawFrameSequence()
{
    return std::make_shared<LoopSequence>(shared_from_this(), m_maxFramesInFlight);
}

bool RenderManager::checkAndResetResize()
{
    if (!m_window.checkAndResetResize()) {
        return false;
    }
    m_events->notify<WindowEvent>();
    return true;
}

void RenderManager::reCreateSwapChain()
{
    int width = 0;
    int height = 0;
    int depth = 0;
    std::tie(width, height, depth) = m_window.getFramebufferSize();
    // A minimised / unfocused window reports 0x0.  Waiting here with
    // waitEvents() deadlocks: the loop blocks until an event arrives, and the
    // event that would fix the size (restore) may never come while we hold the
    // thread.  SwapChain::chooseSwapExtent already clamps to the device's
    // minImageExtent, so a 0x0 request produces a valid (small) swapchain; the
    // next real resize event rebuilds it at the correct size.  Only clamp to 1
    // to guard against drivers with a degenerate minImageExtent.
    if (width <= 0) {
        width = 1;
    }
    if (height <= 0) {
        height = 1;
    }

    m_device->waitIdle();
    m_swapChain.reset();

    const SwapChainSupportDetails swapChainSupport =
        PhysicalDeviceUtils::querySwapChainSupport(*m_physicalDevice, m_surface);
    m_swapChain = std::make_shared<SwapChain>(deviceView(), m_window, m_queueFamilyIndices, swapChainSupport,
                                              m_surface);
    m_events->notify<SwapChainEvent>();
}
