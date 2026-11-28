#pragma once
#include <cmath>
#include <cstdint>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>
#include <vulkan/vulkan.hpp>

// Hardware ray tracing capability layer (see DESIGN.md 31).
//
// Nothing in src/ includes these headers: the core only negotiates *device*
// capability through DeviceRequirement, and this extension layer owns the
// objects that use it.  Every entry point below is resolved at run time with
// vkGetDeviceProcAddr, so the executables do not import the KHR symbols from
// vulkan-1.lib (which does not export them).
namespace rt {

inline uint32_t AlignUp(uint32_t value, uint32_t alignment)
{
    return alignment == 0 ? value : (value + alignment - 1) / alignment * alignment;
}

inline uint64_t AlignUp64(uint64_t value, uint64_t alignment)
{
    return alignment == 0 ? value : (value + alignment - 1) / alignment * alignment;
}

// Shared by the common module and the acceleration-structure builder: both
// allocate dedicated memory for device-address buffers, and neither goes
// through the core allocator (see 31.3).
inline uint32_t FindMemoryType(const vk::PhysicalDeviceMemoryProperties &props, uint32_t typeBits,
                               vk::MemoryPropertyFlags flags)
{
    for (uint32_t i = 0; i < props.memoryTypeCount; ++i) {
        if ((typeBits & (1u << i)) != 0 && (props.memoryTypes[i].propertyFlags & flags) == flags) {
            return i;
        }
    }
    throw std::runtime_error("rt: no memory type matches the requested properties");
}

// What the physical device actually supports.  Query() is safe to call on a
// device without ray tracing: every field then reports false and the caller can
// fall back to a CPU self-test instead of creating a half-working device.
struct Capabilities {
    bool accelerationStructure = false;
    bool rayTracingPipeline = false;
    bool rayQuery = false;
    bool bufferDeviceAddress = false;
    bool deferredHostOperations = false;

    vk::PhysicalDeviceRayTracingPipelinePropertiesKHR pipelineProperties{};

    uint32_t shaderGroupHandleSize() const { return pipelineProperties.shaderGroupHandleSize; }
    uint32_t shaderGroupHandleAlignment() const { return pipelineProperties.shaderGroupHandleAlignment; }
    uint32_t shaderGroupBaseAlignment() const { return pipelineProperties.shaderGroupBaseAlignment; }
    uint32_t maxRayRecursionDepth() const { return pipelineProperties.maxRayRecursionDepth; }

    bool canTraceRays() const { return accelerationStructure && rayTracingPipeline && bufferDeviceAddress; }
    bool canQueryRays() const { return accelerationStructure && rayQuery && bufferDeviceAddress; }

    static Capabilities Query(vk::PhysicalDevice device);
    // One-line summary used by --probe / --self-test output.
    std::string Describe() const;
};

// Dynamically loaded KHR ray tracing entry points.
class Dispatch {
public:
    void Load(vk::Device device);
    // True only when every entry point loaded.  Callers that need just one half of
    // the API should ask for that half instead: a device created with rayQuery
    // alone does not export the pipeline entry points, and demanding them would
    // reject a perfectly usable device (see 31.7).
    bool valid() const;
    // Acceleration structure build + buffer device address: everything ray query
    // needs, and the prerequisite of the ray tracing pipeline.
    bool accelerationStructureReady() const;
    // The vkCmdTraceRaysKHR / shader-group / pipeline trio.
    bool rayTracingPipelineReady() const;
    // Names that are null after Load(), for a readable error message.
    std::string missing() const;

    PFN_vkGetBufferDeviceAddressKHR getBufferDeviceAddress = nullptr;
    PFN_vkCreateAccelerationStructureKHR createAccelerationStructure = nullptr;
    PFN_vkDestroyAccelerationStructureKHR destroyAccelerationStructure = nullptr;
    PFN_vkGetAccelerationStructureBuildSizesKHR getAccelerationStructureBuildSizes = nullptr;
    PFN_vkGetAccelerationStructureDeviceAddressKHR getAccelerationStructureDeviceAddress = nullptr;
    PFN_vkBuildAccelerationStructuresKHR buildAccelerationStructures = nullptr;
    PFN_vkCmdBuildAccelerationStructuresKHR cmdBuildAccelerationStructures = nullptr;
    PFN_vkCmdTraceRaysKHR cmdTraceRays = nullptr;
    PFN_vkGetRayTracingShaderGroupHandlesKHR getRayTracingShaderGroupHandles = nullptr;
    PFN_vkCreateRayTracingPipelinesKHR createRayTracingPipelines = nullptr;

    uint64_t BufferDeviceAddress(vk::Device device, vk::Buffer buffer) const;
};

// Runs `body` in a one-time command buffer and waits for the queue to idle.
// Used for acceleration-structure builds and for the initial image layout
// transition; both are one-off operations where a dedicated command pool is
// cheaper than wiring into the frame graph.
void RunOnce(vk::Device device, vk::Queue queue, uint32_t queueFamilyIndex,
             const std::function<void(vk::CommandBuffer)> &body);

// Records the full sequence that turns a freshly traced storage image into a
// presentable swapchain image: GENERAL -> TRANSFER_SRC, swapchain UNDEFINED ->
// TRANSFER_DST, copy, TRANSFER_DST -> PRESENT_SRC, then the storage image back
// to GENERAL for the next frame.  Doing it here keeps every ray tracing example
// from re-deriving the four barriers (see 31.6).
void RecordCopyToSwapchain(vk::CommandBuffer cmd, vk::Image storageImage, vk::Image swapchainImage,
                           vk::Extent2D extent);

// Byte a channel of the storage image ends up holding for a linear input.
//
// imageStore through an `rgba8`-qualified image variable writes the float value
// verbatim, even when the image format carries the _SRGB suffix: the format
// qualifier in the shader decides the numeric semantics, and rgba8 means UNORM.
// (Measured: 0.05 comes back as 13, not the 62 an sRGB encode would give.)  So
// no transfer function is involved here, which is the opposite of what the
// surface format's name suggests -- keeping it in one function stops the
// --verify comparisons and the PPM dump from disagreeing about it.
inline uint8_t StorageByte(float linear)
{
    const float clamped = linear < 0.0f ? 0.0f : (linear > 1.0f ? 1.0f : linear);
    return static_cast<uint8_t>(std::lround(clamped * 255.0f));
}

// The colour target the ray generation shader writes to with imageStore.  Kept
// in GENERAL for its whole life so one descriptor serves both the write and the
// transfer-source read; only the swapchain image is transitioned per frame.
class StorageImage {public:
    void Create(vk::Device device, const vk::PhysicalDeviceMemoryProperties &memProps, vk::Format format,
                vk::Extent2D extent);
    void Destroy(vk::Device device);
    void TransitionToGeneral(vk::Device device, vk::Queue queue, uint32_t queueFamilyIndex);

    vk::Image image() const { return m_image; }
    vk::ImageView view() const { return m_view; }
    vk::Format format() const { return m_format; }
    vk::Extent2D extent() const { return m_extent; }

private:
    vk::Image m_image{};
    vk::DeviceMemory m_memory{};
    vk::ImageView m_view{};
    vk::Format m_format = vk::Format::eUndefined;
    vk::Extent2D m_extent{};
};

} // namespace rt
