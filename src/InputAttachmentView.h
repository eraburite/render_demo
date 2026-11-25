#pragma once
#include <vulkan/vulkan.hpp>
#include "DeviceResource.h"

// Descriptor resource for a framebuffer attachment that a later subpass reads
// with subpassLoad().  It owns nothing: the view belongs to the render pass, so
// the class only remembers the handle and the layout the attachment has while
// the reading subpass runs.
//
// Why this is not a TextureSamplerCombine: an input attachment descriptor is
// written with VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, carries no sampler, and
// validation compares its imageLayout against the layout declared by the
// subpass input attachment reference (SHADER_READ_ONLY_OPTIMAL here).
class InputAttachmentView : public IDeviceResource {
public:
    InputAttachmentView(vk::ImageView view,
                        vk::ImageLayout layout = vk::ImageLayout::eShaderReadOnlyOptimal) :
        m_view(view), m_layout(layout)
    {
    }
    ~InputAttachmentView() override = default;

    // Attachment views die with the framebuffer, so a resize replaces them.
    // Re-point and rewrite the descriptors (GraphicsPipeline::RefreshDescriptors).
    void setImageView(vk::ImageView view) { m_view = view; }

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
    vk::ImageLayout imageLayout() const { return m_layout; }

private:
    vk::ImageView m_view;
    vk::ImageLayout m_layout;
    vk::DescriptorImageInfo m_imageInfo{};
};
