#include "TextureSamplerCombine.h"

TextureSamplerCombine::TextureSamplerCombine(vk::ImageView view, vk::Sampler sampler, vk::ImageLayout layout) :
    m_view(view), m_sampler(sampler), m_layout(layout)
{
}

vk::WriteDescriptorSet TextureSamplerCombine::constructWriteDescriptorSet(vk::DescriptorSet set,
                                                                         vk::DescriptorType type, uint32_t binding)
{
    m_imageInfo.imageView = m_view;
    m_imageInfo.sampler = m_sampler;
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

void TextureSamplerCombine::accept(DeviceResourceVisitor &visitor)
{
    visitor.visit(*this);
}
