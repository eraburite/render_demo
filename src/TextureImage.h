#pragma once
#include <memory>
#include <vulkan/vulkan.hpp>
#include "DeviceMemoryAllocator.h"
#include "DeviceResource.h"
#include "DeviceResourceUtils.h"
#include "typedefs.h"

// Sampled image bound through a combined image sampler.  Covers every view type
// the examples need: 2D, 2D array, cube, cube array and 3D.  The upload block is
// either a single base level (mips then generated on the GPU) or a full mip
// chain packed as (mip -> layer -> face) straight out of a KTX file.
class TextureImage : public IDeviceResource {
public:
    TextureImage(std::shared_ptr<vk::Device> device, vk::PhysicalDeviceMemoryProperties memProps,
                 std::shared_ptr<DeviceMemoryAllocator> allocator, TextureImageSpec spec);
    ~TextureImage() override;

    void updateRawData(const void *data, size_t size) override;
    ResourceTypes resourceType() const override { return ResourceTypes::eTexture; }
    bool NeedsUpload() const override { return m_dirty; }
    void OnUploaded() override { m_dirty = false; }
    void OnUploadCompleted() override;
    void OnReadbackRequested() override;
    void *MappedData() override { return m_mapped; }
    size_t MappedByteSize() const override { return m_stagingBytes; }

    void recordCopyFromStagingToDevice(const vk::CommandBuffer &cmd) override;
    void recordCopyFromDeviceToStaging(const vk::CommandBuffer &cmd) override {}
    void bindToPipeline(const vk::CommandBuffer &cmd) override {}
    vk::WriteDescriptorSet constructWriteDescriptorSet(vk::DescriptorSet set, vk::DescriptorType type,
                                                       uint32_t binding) override;
    void accept(DeviceResourceVisitor &visitor) override;

    vk::ImageView imageView() const { return m_imageView ? *m_imageView : vk::ImageView{}; }
    vk::Sampler sampler() const { return m_sampler ? *m_sampler : vk::Sampler{}; }
    // Raw handle, needed by the debug readback path (ImageReadback::capture).
    vk::Image imageHandle() const { return m_image ? *m_image : vk::Image{}; }

    uint32_t mipLevels() const { return m_spec.miplevels; }
    vk::Format format() const { return m_spec.format; }
    const TextureImageSpec &spec() const { return m_spec; }

private:
    void createSampler();

private:
    std::shared_ptr<vk::Device> m_device;
    std::shared_ptr<DeviceMemoryAllocator> m_allocator;
    vk::PhysicalDeviceMemoryProperties m_memProps;
    TextureImageSpec m_spec;
    size_t m_stagingBytes = 0;
    int32_t m_width = 1;
    int32_t m_height = 1;

    std::shared_ptr<vk::Image> m_image;
    DeviceMemorySlice m_memory;
    std::shared_ptr<vk::ImageView> m_imageView;
    std::shared_ptr<vk::Sampler> m_sampler;

    std::shared_ptr<vk::Buffer> m_staging;
    DeviceMemorySlice m_stagingMemory;
    void *m_mapped = nullptr;
    bool m_dirty = false;
    vk::DescriptorImageInfo m_imageInfo{};
};
