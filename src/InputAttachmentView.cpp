#include "InputAttachmentView.h"

vk::WriteDescriptorSet InputAttachmentView::constructWriteDescriptorSet(vk::DescriptorSet set,
                                                                       vk::DescriptorType type, uint32_t binding)
{
    m_imageInfo.imageView = m_view;
    m_imageInfo.sampler = vk::Sampler{}; // input attachments are not sampled
    m_imageInfo.imageLayout = m_layout;

    vk::WriteDescriptorSet write{};
    write.dstSet = set;
    write.dstBinding = binding;
    write.dstArrayElement = 0;
    write.descriptorType = type;
    write.descriptorCount = 1;
    write.pImageInfo = &m_imageInfo;
    return write;
}

void InputAttachmentView::accept(DeviceResourceVisitor &visitor)
{
    visitor.visit(*this);
}
