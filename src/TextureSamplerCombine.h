#pragma once
#include <memory>
#include <vulkan/vulkan.hpp>
#include "DeviceResource.h"

// Combined image sampler that references an existing ImageView + Sampler
// (e.g. an offscreen resolve target).  Does not own the image memory.
class TextureSamplerCombine : public IDeviceResource {
public:
    TextureSamplerCombine(vk::ImageView view, vk::Sampler sampler, vk::ImageLayout layout =
                                                                        vk::ImageLayout::eShaderReadOnlyOptimal);
    ~TextureSamplerCombine() override = default;

    void updateRawData(const void *, size_t) override {}
    ResourceTypes resourceType() const override { return ResourceTypes::eTexture; }
    bool NeedsUpload() const override { return false; }
    void OnUploaded() override {}
    void OnUploadCompleted() override {}
    void OnReadbackRequested() override {}
    void *MappedData() override { return nullptr; }
    size_t MappedByteSize() const override { return 0; }

    void recordCopyFromStagingToDevice(const vk::CommandBuffer &) override {}
    void recordCopyFromDeviceToStaging(const vk::CommandBuffer &) override {}
    void bindToPipeline(const vk::CommandBuffer &) override {}
    vk::WriteDescriptorSet constructWriteDescriptorSet(vk::DescriptorSet set, vk::DescriptorType type,
                                                       uint32_t binding) override;
    void accept(DeviceResourceVisitor &visitor) override;

    vk::ImageView imageView() const { return m_view; }
    vk::Sampler sampler() const { return m_sampler; }

    // Attachment views die with the framebuffer, so a resize replaces them.
    // Re-point and rewrite the descriptors (GraphicsPipeline::RefreshDescriptors);
    // constructWriteDescriptorSet() rebuilds the image info from m_view, so no
    // cached state has to be invalidated here.
    void setImageView(vk::ImageView view) { m_view = view; }

private:
    vk::ImageView m_view;
    vk::Sampler m_sampler;
    vk::ImageLayout m_layout;
    vk::DescriptorImageInfo m_imageInfo{};
};
