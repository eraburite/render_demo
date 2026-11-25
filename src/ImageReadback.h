#pragma once
#include <cstdint>
#include <memory>
#include <vector>
#include <vulkan/vulkan.hpp>
#include "DeviceMemoryAllocator.h"
#include "DeviceResourceUtils.h"

// One-shot GPU -> host readback of a single mip level / array layer.
//
// This is a *debug* capability, not a frame path: every capture transitions the
// image to TRANSFER_SRC, copies it into host-visible memory, transitions it
// back and blocks on a fence.  Calling it between two frames is safe and leaves
// the image exactly as it was; calling it per frame would stall the pipeline and
// is not what it is for.
class ImageReadback {
public:
    struct Capture {
        vk::Extent2D extent{};
        vk::Format format = vk::Format::eUndefined;
        // Tightly packed texels, rows in image order (row 0 is the first row of
        // the image, i.e. the top of the frame -- no flip is applied here).
        std::vector<uint8_t> pixels;
    };

    ImageReadback(std::shared_ptr<vk::Device> device, const vk::PhysicalDeviceMemoryProperties &memProps,
                  std::shared_ptr<DeviceMemoryAllocator> allocator, std::shared_ptr<vk::Queue> queue,
                  uint32_t queueFamily);
    ~ImageReadback();

    ImageReadback(const ImageReadback &) = delete;
    ImageReadback &operator=(const ImageReadback &) = delete;

    // layout is the layout the image is in right now; it is restored before
    // capture() returns.  `aspect` selects a single image aspect explicitly:
    // the S8 plane of a depth-stencil attachment is copied with
    // format = eS8Uint + aspect = eStencil.  An empty value derives the aspect
    // from the format (colour / depth / depth+stencil).
    //
    // `barrierAspect` is the aspect used by the layout-transition barriers.
    // Without the separateDepthStencilLayouts feature a barrier on a combined
    // depth-stencil image must name DEPTH|STENCIL (VUID 03320), while the copy
    // region must name exactly one aspect (VUID 00188) -- so the two differ.
    // Empty means "same as aspect".
    Capture capture(vk::Image image, vk::Format format, vk::Extent2D extent, vk::ImageLayout layout,
                    vk::ImageAspectFlags aspect = {}, vk::ImageAspectFlags barrierAspect = {});

    // Bytes per texel for the formats this path understands; throws otherwise.
    // Shared with the PPM writer so both agree on the memory layout.
    static uint32_t texelSize(vk::Format format);

private:
    void ensureStaging(vk::DeviceSize size);

private:
    std::shared_ptr<vk::Device> m_device;
    vk::PhysicalDeviceMemoryProperties m_memProps;
    std::shared_ptr<DeviceMemoryAllocator> m_allocator;
    std::shared_ptr<vk::Queue> m_queue;
    uint32_t m_queueFamily = 0;

    std::shared_ptr<vk::CommandPool> m_pool;
    std::shared_ptr<vk::CommandBuffer> m_commandBuffer;
    std::shared_ptr<vk::Fence> m_fence;

    // Grow-only staging buffer: a capture is synchronous, so one buffer can be
    // reused across captures and only has to be reallocated when the requested
    // size grows (e.g. a resize).
    BufferBundle m_staging;
    vk::DeviceSize m_stagingSize = 0;
};
