#pragma once
#include <chrono>
#include <cstdint>
#include <optional>
#include <iostream>
#include <vulkan/vulkan.hpp>

struct QueueFamilyIndices {
    std::optional<uint32_t> graphicsFamily;
    std::optional<uint32_t> presentFamily;

    bool isComplete()
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
        const std::vector<vk::SurfaceFormatKHR> &availableFormats = formats;
        for (const auto &availableFormat : availableFormats) {
            if (availableFormat.format == vk::Format::eB8G8R8A8Srgb
                && availableFormat.colorSpace == vk::ColorSpaceKHR::eSrgbNonlinear) {
                return availableFormat;
            }
        }
        return availableFormats[0];
    }

    vk::PresentModeKHR chooseSwapPresentMode() const
    {
        const std::vector<vk::PresentModeKHR> &availablePresentModes = presentModes;
        for (const auto &availablePresentMode : availablePresentModes) {
            if (availablePresentMode == vk::PresentModeKHR::eMailbox) {
                return availablePresentMode;
            }
        }
        return vk::PresentModeKHR::eImmediate;
    }

    vk::Extent2D chooseSwapExtent(int window_width, int window_height) const
    {
        if (capabilities.currentExtent.width != std::numeric_limits<uint32_t>::max()) {
            return capabilities.currentExtent;
        } else {
            vk::Extent2D actualExtent = {static_cast<uint32_t>(window_width), static_cast<uint32_t>(window_height)};

            actualExtent.width =
                std::clamp(actualExtent.width, capabilities.minImageExtent.width, capabilities.maxImageExtent.width);
            actualExtent.height =
                std::clamp(actualExtent.height, capabilities.minImageExtent.height, capabilities.maxImageExtent.height);
            return actualExtent;
        }
    }
};

struct DeviceRequirement {
    bool graphics = true;
    bool present = true;
    std::vector<const char *> extensions;
};

// NOTE: for Layered Descriptor Set Design
enum class UpdateFrequency {
    eStatic,       // Only once (e. g., vertex mesh, global texture)
    eEnvironment,  // Occasional updates (such as those related to SwapChain after resizing events)
    eEachFrame,    // Updated every frame (e. g., UniformBuffer)
    eEachInstance, // Model Matrices or Instance Index
};

enum class MemoryTypes {
    eDevice = 0,        ///< Type is device memory, source and destination
    eHost = 1,          ///< Type is host memory, source and destination
    eDeviceAndHost = 2, ///< Type is host-visible and host-coherent device memory
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
    // Memory0bJecıs type
    // Category Bits
    eCategoryInput = 0x100,      // 0001 0000 0000
    eCategoryDescriptor = 0x200, // 0010 0000 0000
    eCategoryConstants = 0x400,  // 0100 0000 0000

    eUnknow = -1,
    eVertex = 0 | eCategoryInput,
    eIndex = 1 | eCategoryInput,
    eUniform = 2 | eCategoryDescriptor,
    eStorage = 3 | eCategoryDescriptor, // storage buffer, may define storage image if need
    eTexture = 4 | eCategoryDescriptor,
    ePushConst = 5 | eCategoryConstants, // pushConstants, may define specJalizationConstants 11 need
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
    ResourceId(uint32_t setId, uint32_t binding, uint32_t location = 0) : m_setId(setId), m_binding(binding) {}

    void SetResourceId(uint32_t setId, uint32_t binding)
    {
        m_setId = setId;
        m_binding = binding;
    }

    auto getSetId() const
    {
        return m_setId;
    }

    auto getBindingId() const
    {
        return m_binding;
    }

    bool operator==(const ResourceId &other) const
    {
        return m_setId == other.m_setId && m_binding == other.m_binding;
    }

    static std::size_t hash(const ResourceId &id)
    {
        std::size_t seed = 0;
        hash_combine(seed, id.m_setId);
        hash_combine(seed, id.m_binding);
        return seed;
    }

private:
    uint32_t m_setId = 0;
    uint32_t m_binding = 0;
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

    auto getResourceId() const
    {
        return m_resourceId;
    }

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
    std::size_t operator()(const ResourceId &rId) const noexcept
    {
        std::size_t seed = ResourceId::hash(rId);
        return seed;
    }
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
    uint32_t elementCount;
    uint32_t elementSize; // sizeof
    DataTypes elementType;
    size_t getTotalByte() const
    {
        return elementCount * elementSize;
    }
};

struct TextureImageSpec {
    uint32_t width;         // image widrh
    uint32_t height;        // image height
    uint32_t channels;      // rgba
    uint32_t miplevels = 1; // as default
    size_t getTotalPixels() const
    {
        return width * height * channels;
    }
};

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