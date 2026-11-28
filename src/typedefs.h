#pragma once
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <functional>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>
#include <vulkan/vulkan.hpp>

inline constexpr uint32_t MAX_FRAMES_IN_FLIGHT = 2;

struct QueueFamilyIndices {
    std::optional<uint32_t> graphicsFamily;
    std::optional<uint32_t> presentFamily;
    std::optional<uint32_t> computeFamily;

    bool isComplete() const
    {
        return graphicsFamily.has_value() && presentFamily.has_value();
    }
};

struct SwapChainSupportDetails {
    vk::SurfaceCapabilitiesKHR capabilities;
    std::vector<vk::SurfaceFormatKHR> formats;
    std::vector<vk::PresentModeKHR> presentModes;

    bool IsAdequate() const
    {
        return !formats.empty() && !presentModes.empty();
    }

    vk::SurfaceFormatKHR chooseSwapSurfaceFormat() const
    {
        for (const auto &availableFormat : formats) {
            if (availableFormat.format == vk::Format::eB8G8R8A8Srgb
                && availableFormat.colorSpace == vk::ColorSpaceKHR::eSrgbNonlinear) {
                return availableFormat;
            }
        }
        return formats[0];
    }

    vk::PresentModeKHR chooseSwapPresentMode() const
    {
        for (const auto &mode : presentModes) {
            if (mode == vk::PresentModeKHR::eMailbox) {
                return mode;
            }
        }
        return vk::PresentModeKHR::eImmediate;
    }

    vk::Extent2D chooseSwapExtent(int window_width, int window_height) const
    {
        if (capabilities.currentExtent.width != std::numeric_limits<uint32_t>::max()) {
            return capabilities.currentExtent;
        }
        vk::Extent2D actualExtent = {static_cast<uint32_t>(window_width), static_cast<uint32_t>(window_height)};
        actualExtent.width =
            std::clamp(actualExtent.width, capabilities.minImageExtent.width, capabilities.maxImageExtent.width);
        actualExtent.height =
            std::clamp(actualExtent.height, capabilities.minImageExtent.height, capabilities.maxImageExtent.height);
        return actualExtent;
    }
};

struct DeviceRequirement {
    bool graphics = true;
    bool present = true;
    bool compute = false;
    // Optional features for the extended examples.  All default to false so
    // existing examples keep the legacy device setup.  Requesting one makes
    // PhysicalDeviceUtils skip devices that lack it and createDevice() enable
    // it (failing early with a readable message rather than a validation error).
    bool geometryShader = false;
    bool tessellationShader = false;
    bool fillModeNonSolid = false;
    // VK_QUERY_TYPE_PIPELINE_STATISTICS needs the feature enabled at device
    // creation; occlusion queries need no feature at all (see 28.2).
    bool pipelineStatisticsQuery = false;
    // Hardware ray tracing (see 31.2).  Both default to false so the existing
    // examples keep the exact same device.  Requesting either one makes
    // PhysicalDeviceUtils::checkDeviceSuitable query the RT feature chain
    // (VkPhysicalDeviceFeatures2 + accelerationStructure / rayTracingPipeline /
    // rayQuery / bufferDeviceAddress) and makes createDevice() enable every
    // extension in that dependency chain.  rayTracingPipeline and rayQuery are
    // independent: a device may expose one without the other, and rayQuery needs
    // no ray tracing pipeline at all.
    bool rayTracingPipeline = false;
    bool rayQuery = false;
    bool samplerAnisotropy = true;
    // Per-color-attachment blend factors.  Enabled by default: it is required by
    // weighted-blended OIT (an additive target next to a multiplicative one in
    // the same subpass) and virtually every desktop GPU reports it, so gating it
    // behind a per-example switch would only create a silent failure mode.
    bool independentBlend = true;
    std::vector<const char *> extensions;
};

enum class UpdateFrequency {
    eStatic,
    eEnvironment,
    eEachFrame,
    eEachInstance,
};

enum class MemoryTypes {
    eDevice = 0,
    eHost = 1,
    eDeviceAndHost = 2,
};

enum class DataTypes {
    eBool = 0,
    eInt = 1,
    eUnsignedInt = 2,
    eFloat = 3,
    eDouble = 4,
    eCustom = 5,
    eShort = 6,
    eUnsignedShort = 7,
    eChar = 8,
    eUnsignedChar = 9
};

enum class ResourceTypes {
    eCategoryInput = 0x100,
    eCategoryDescriptor = 0x200,
    eCategoryConstants = 0x400,

    eUnknow = -1,
    eVertex = 0 | eCategoryInput,
    eIndex = 1 | eCategoryInput,
    eUniform = 2 | eCategoryDescriptor,
    eStorage = 3 | eCategoryDescriptor,
    eTexture = 4 | eCategoryDescriptor,
    ePushConst = 5 | eCategoryConstants,
    // Second vertex buffer bound at binding 1 with eInstance input rate.  A
    // distinct ResourceTypes key lets ShaderAssetGroup own it alongside the
    // ordinary eVertex VBO (see 28.3).
    eInstanceVertex = 6 | eCategoryInput,
};

template <class T>
inline void hash_combine(std::size_t &seed, const T &v)
{
    std::hash<T> hasher;
    seed ^= hasher(v) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
}

class ResourceId {
public:
    ResourceId() = default;
    ResourceId(uint32_t setId, uint32_t binding, uint32_t location = 0) :
        m_setId(setId), m_binding(binding), m_location(location)
    {
    }

    void SetResourceId(uint32_t setId, uint32_t binding)
    {
        m_setId = setId;
        m_binding = binding;
    }
    auto getSetId() const { return m_setId; }
    auto getBindingId() const { return m_binding; }
    auto getLocation() const { return m_location; }

    bool operator==(const ResourceId &other) const
    {
        return m_setId == other.m_setId && m_binding == other.m_binding && m_location == other.m_location;
    }

    static std::size_t hash(const ResourceId &id)
    {
        std::size_t seed = 0;
        hash_combine(seed, id.m_setId);
        hash_combine(seed, id.m_binding);
        hash_combine(seed, id.m_location);
        return seed;
    }

private:
    uint32_t m_setId = 0;
    uint32_t m_binding = 0;
    uint32_t m_location = 0;
};

class ResourceKey {
public:
    ResourceKey(ResourceId resourceId, uint32_t currentFrame) :
        m_resourceId(std::move(resourceId)), m_currentFrame(currentFrame)
    {
    }

    bool operator==(const ResourceKey &other) const
    {
        return m_resourceId == other.m_resourceId && m_currentFrame == other.m_currentFrame;
    }
    auto getResourceId() const { return m_resourceId; }
    uint32_t currentFrame() const { return m_currentFrame; }

    static ResourceKey make_key(ResourceId resourceId, uint32_t currentFrame)
    {
        return ResourceKey(resourceId, currentFrame);
    }
    static ResourceKey make_key(ResourceId resourceId)
    {
        return ResourceKey(resourceId, 0);
    }

private:
    ResourceId m_resourceId;
    uint32_t m_currentFrame;
    friend class ResourceKeyHasher;
};

class ResourceIdHasher {
public:
    std::size_t operator()(const ResourceId &rId) const noexcept { return ResourceId::hash(rId); }
};

class ResourceKeyHasher {
public:
    std::size_t operator()(const ResourceKey &k) const noexcept
    {
        std::size_t seed = ResourceId::hash(k.m_resourceId);
        hash_combine(seed, static_cast<size_t>(k.m_currentFrame));
        return seed;
    }
};

struct GBufferSpec {
    uint32_t elementCount = 1;
    uint32_t elementSize = 1;
    DataTypes elementType = DataTypes::eCustom;
    size_t getTotalByte() const { return elementCount * elementSize; }
};

// Texture description shared by the auto-created descriptor resource and the
// upload path.  Defaults describe the historical single-2D-layer RGBA8 case, so
// existing examples keep their meaning; cube / array / 3D / pre-baked mip
// chains opt in explicitly.
struct TextureImageSpec {
    uint32_t width = 1;
    uint32_t height = 1;
    uint32_t channels = 4;
    uint32_t miplevels = 1;
    uint32_t depth = 1;   // > 1 => 3D image
    uint32_t layers = 1;  // array elements (cube => layers of 6 faces each)
    uint32_t faces = 1;   // 6 => cube / cube array

    vk::Format format = vk::Format::eR8G8B8A8Srgb;
    // When true the uploaded block already holds the whole mip chain, packed as
    // (mip -> layer -> face); the GPU then only copies.  When false only the
    // base level is uploaded and the mips are generated with vkCmdBlitImage.
    bool mipsInData = false;

    vk::SamplerAddressMode addressMode = vk::SamplerAddressMode::eClampToEdge;
    bool anisotropy = true;
    // True when the image is written by a shader (imageStore) instead of being
    // sampled only.  Adds STORAGE usage, keeps the whole image in GENERAL so a
    // single descriptor works for both reading and writing, and makes the
    // descriptor write omit the sampler.  See DESIGN.md 25.1.
    bool storageImage = false;

    uint32_t bytesPerTexel() const;
    uint32_t subresourceCount() const { return layers * faces; }
    size_t getTotalPixels() const { return width * height * depth * layers * faces * channels; }
    // Size of one mip level of the whole image (all layers/faces/depth slices).
    size_t mipByteSize(uint32_t mip) const;
    // Size of the host block described by this spec.
    size_t uploadByteSize() const;
    vk::ImageViewType imageViewType() const;
    vk::ImageType imageType() const { return depth > 1 ? vk::ImageType::e3D : vk::ImageType::e2D; }
    uint32_t arrayLayers() const { return layers * faces; }
};

inline uint32_t TextureImageSpec::bytesPerTexel() const
{
    switch (format) {
    case vk::Format::eR8Unorm:
        return 1u;
    case vk::Format::eR16Unorm:
    case vk::Format::eR16Sfloat:
    case vk::Format::eR8G8Unorm:
        return 2u;
    case vk::Format::eR8G8B8A8Unorm:
    case vk::Format::eR8G8B8A8Srgb:
        return 4u;
    case vk::Format::eR16G16B16A16Sfloat:
        return 8u;
    // Full-float formats are the storage-image / HDR-intermediate case.  Without
    // these the default below returns `channels` (4) and the staging buffer is a
    // quarter of what the image needs: copyBufferToImage then reads past it.
    case vk::Format::eR32G32B32A32Sfloat:
        return 16u;
    case vk::Format::eR32G32B32Sfloat:
        return 12u;
    case vk::Format::eR32G32Sfloat:
    case vk::Format::eD32SfloatS8Uint:
        return 8u;
    case vk::Format::eR32Sfloat:
    case vk::Format::eD32Sfloat:
    case vk::Format::eD24UnormS8Uint:
        return 4u;
    default:
        return channels;
    }
}

inline size_t TextureImageSpec::mipByteSize(uint32_t mip) const
{
    const uint32_t w = std::max(1u, width >> mip);
    const uint32_t h = std::max(1u, height >> mip);
    const uint32_t d = std::max(1u, depth >> mip);
    return static_cast<size_t>(w) * h * d * arrayLayers() * bytesPerTexel();
}

inline size_t TextureImageSpec::uploadByteSize() const
{
    if (!mipsInData) {
        return mipByteSize(0);
    }
    size_t total = 0;
    for (uint32_t mip = 0; mip < std::max(1u, miplevels); ++mip) {
        total += mipByteSize(mip);
    }
    return total;
}

inline vk::ImageViewType TextureImageSpec::imageViewType() const
{
    if (faces == 6) {
        return layers > 1 ? vk::ImageViewType::eCubeArray : vk::ImageViewType::eCube;
    }
    if (depth > 1) {
        return vk::ImageViewType::e3D;
    }
    return layers > 1 ? vk::ImageViewType::e2DArray : vk::ImageViewType::e2D;
}


inline constexpr bool IsInputResource(ResourceTypes type)
{
    return (static_cast<uint32_t>(type) & static_cast<uint32_t>(ResourceTypes::eCategoryInput)) != 0;
}
inline constexpr bool IsDescriptorResource(ResourceTypes type)
{
    return (static_cast<uint32_t>(type) & static_cast<uint32_t>(ResourceTypes::eCategoryDescriptor)) != 0;
}
inline constexpr bool IsConstantsResource(ResourceTypes type)
{
    return (static_cast<uint32_t>(type) & static_cast<uint32_t>(ResourceTypes::eCategoryConstants)) != 0;
}

struct DispatchSize {
    uint32_t groupCountX = 1;
    uint32_t groupCountY = 1;
    uint32_t groupCountZ = 1;

    static DispatchSize FromInvocationCount(uint32_t invocationCount, uint32_t localSizeX)
    {
        DispatchSize size;
        size.groupCountX = (invocationCount + localSizeX - 1) / localSizeX;
        return size;
    }
};
