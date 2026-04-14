#include "TextureImage.h"
#include "DeviceResourceUtils.h"

TextureImage::TextureImage(std::shared_ptr<vk::Device> device, const vk::PhysicalDeviceMemoryProperties &memProperties,
                           const vk::PhysicalDeviceProperties &properties, MemoryTypes memoryType,
                           TextureImageSpec imageMeta) :
    m_imageMeta(imageMeta), MemoryObject(device, ResourceTypes::eTexture, memoryType, imageMeta.getTotalPixels())
{
    reserve(memProperties);
    mapRawData();
    createTextureImageView();
    createTextureSampler(properties);
}

TextureImage::~TextureImage() {}

void TextureImage::createTextureImageView()
{
    m_textureImageView = DeviceResourceUtils::createImageView(m_device, *m_primaryImage, vk::Format::eR8G8B8A8Srgb,
                                                              m_imageMeta.miplevels, vk::ImageAspectFlagBits::eColor);
}

void TextureImage::createTextureSampler(const vk::PhysicalDeviceProperties &properties)
{
    vk::SamplerCreateInfo samplerInfo{};
    samplerInfo.magFilter = vk::Filter::eLinear;
    samplerInfo.minFilter = vk::Filter::eLinear;
    samplerInfo.addressModeU = vk::SamplerAddressMode::eRepeat;
    samplerInfo.addressModeV = vk::SamplerAddressMode::eRepeat;
    samplerInfo.addressModeW = vk::SamplerAddressMode::eRepeat;
    samplerInfo.anisotropyEnable = VK_TRUE;
    samplerInfo.maxAnisotropy = properties.limits.maxSamplerAnisotropy;
    samplerInfo.borderColor = vk::BorderColor::eFloatOpaqueBlack;
    samplerInfo.unnormalizedCoordinates = VK_FALSE;
    samplerInfo.compareEnable = VK_FALSE;
    samplerInfo.compareOp = vk::CompareOp::eAlways;
    samplerInfo.mipmapMode = vk::SamplerMipmapMode::eLinear;
    samplerInfo.minLod = 0.0f;
    samplerInfo.maxLod = VK_LOD_CLAMP_NONE;
    samplerInfo.mipLodBias = 0.0f;

    vk::Sampler textureSampler{};
    if (m_device->createSampler(&samplerInfo, nullptr, &textureSampler) != vk::Result::eSuccess) {
        throw std::runtime_error(" failed to create texture sampler!");
    }

    m_textureSampler = std::shared_ptr<vk::Sampler>(
        new vk::Sampler(textureSampler), [device = m_device, imageView = m_textureImageView](vk::Sampler *p) {
            if (p && *p) {
                device->destroySampler(*p, nullptr);
            }
            delete p;
        });
}

void TextureImage::reserve(vk::PhysicalDeviceMemoryProperties memProperties)
{
    if (this->MemoryType() == MemoryTypes::eDevice) {
        // stagingBuffer, host
        DeviceMemoryProperty stagingProp{memProperties, getStagingMemoryPropertyFlags()};
        std::tie(m_stagingBuffer, m_stagingMemory) = DeviceResourceUtils::createBuffer(
            m_device, MemorySize(), vk::BufferUsageFlagBits::eTransferSrc, stagingProp);
    }

    // primaryImage, deviceLocal
    DeviceMemoryProperty primaryProp{memProperties, getPrimaryMemoryPropertyFlags()};
    std::tie(m_primaryImage, m_primaryMemory) = DeviceResourceUtils::createImage(
        m_device, m_imageMeta.width, m_imageMeta.height, vk::Format::eR8G8B8A8Srgb, m_imageMeta.miplevels,
        vk::SampleCountFlagBits::e1, vk::ImageTiling::eOptimal, TextureImage::getPrimaryImageUsageFlags(), primaryProp);
}

vk::ImageUsageFlags TextureImage::getPrimaryImageUsageFlags()
{
    return vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eTransferDst
           | vk::ImageUsageFlagBits::eSampled;
}

void TextureImage::recordCopyFromStagingToDevice(const vk::CommandBuffer &commandBuffer)
{
    if (this->MemoryType() == MemoryTypes::eDevice) {
        // begin recordCopyFromStagingToDevice, barrier, TODO with a different command (is it needed??)
        recordTransitionImageLayout(commandBuffer, *m_primaryImage, vk::Format::eR8G8B8A8Srgb,
                                    vk::ImageLayout::eUndefined, vk::ImageLayout::eTransferDstOptimal);
        recordCopyBufferToImage(commandBuffer, *m_stagingBuffer, *m_primaryImage, m_imageMeta.width,
                                m_imageMeta.height);
        // after recordCopyFromStagingToDevice, barrier, TODO: with a different command (is it needed??)
        // NOTE: transitioned to VK IMAGE LAYOUT SHADER READ ONLY OPTIMAL while generating mipmaps
        generateMipmaps(commandBuffer, *m_primaryImage, vk::Format::eR8G8B8A8Srgb);
        recordTransitionImageLayout(commandBuffer, *m_primaryImage, vk::Format::eR8G8B8A8Srgb,
                                    vk::ImageLayout::eTransferDstOptimal, vk::ImageLayout::eShaderReadOnlyOptimal);
    }
}

void TextureImage::recordCopyFromDeviceToStaging(const vk::CommandBuffer &commandBuffer)
{
    // TODO: impl if need
}

void TextureImage::bindToPipeline(const vk::CommandBuffer &commandBuffer)
{
    // TODO: impl if need
}

void TextureImage::recordCopyBufferToImage(const vk::CommandBuffer &commandBuffer, vk::Buffer buffer, vk::Image image,
                                           uint32_t width, uint32_t height)
{
    // begin command first
    // VkCommandBuffer commandBuffer = beginSingleTimeCommands();
    vk::BufferImageCopy region{};
    region.bufferOffset = 0;
    region.bufferRowLength = 0;
    region.bufferImageHeight = 0;
    region.imageSubresource.aspectMask = vk::ImageAspectFlagBits::eColor;
    region.imageSubresource.mipLevel = 0;
    region.imageSubresource.baseArrayLayer = 0;
    region.imageSubresource.layerCount = 1;
    region.imageOffset = vk::Offset3D(0, 0, 0);
    region.imageExtent = vk::Extent3D(width, height, 1);
    commandBuffer.copyBufferToImage(buffer, image, vk::ImageLayout::eTransferDstOptimal, 1, &region);
    // end command finally
    // endSingleTimeCommands(commandBuffer);
}

void TextureImage::recordTransitionImageLayout(const vk::CommandBuffer &commandBuffer, vk::Image &image,
                                               vk::Format format, vk::ImageLayout oldLayout, vk::ImageLayout newLayout)
{
    // begin command first
    // VkCommandBuffer commandBuffer = beginSingleTimeCommands();
    vk::ImageMemoryBarrier barrier{};
    barrier.oldLayout = oldLayout;
    barrier.newLayout = newLayout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = vk::ImageAspectFlagBits::eColor;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = m_imageMeta.miplevels; // 1.
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;
    vk::PipelineStageFlags sourceStage{};
    vk::PipelineStageFlags destinationStage{};
    if (oldLayout == vk::ImageLayout::eUndefined && newLayout == vk::ImageLayout::eTransferDstOptimal) {
        barrier.srcAccessMask = vk::AccessFlagBits::eNone;
        barrier.dstAccessMask = vk::AccessFlagBits::eTransferWrite;
        sourceStage = vk::PipelineStageFlagBits::eTopOfPipe;
        destinationStage = vk::PipelineStageFlagBits::eTransfer;
    } else if (oldLayout == vk::ImageLayout::eTransferDstOptimal
               && newLayout == vk::ImageLayout::eShaderReadOnlyOptimal) {
        barrier.srcAccessMask = vk::AccessFlagBits::eTransferWrite;
        barrier.dstAccessMask = vk::AccessFlagBits::eShaderRead;
        sourceStage = vk::PipelineStageFlagBits::eTransfer;
        destinationStage = vk::PipelineStageFlagBits::eFragmentShader;
    } else {
        throw std::invalid_argument("unsupported layout transition!");
    }
    // TODO: how to bind barrier with a new command record (is 1 t needed??)
    commandBuffer.pipelineBarrier(sourceStage, destinationStage, vk::DependencyFlags(), 0, nullptr, 0, nullptr, 1,
                                  &barrier);

    // end command finally
    // endSingleTimeCommands(commandBuffer);
}

void TextureImage::generateMipmaps(const vk::CommandBuffer &commandBuffer, vk::Image &image, vk::Format imageFormat)
{
    vk::ImageMemoryBarrier barrier{};
    barrier.image = image;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.subresourceRange.aspectMask = vk::ImageAspectFlagBits::eColor;
    ;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;
    barrier.subresourceRange.levelCount = 1;

    int32_t mipWidth = m_imageMeta.width;
    int32_t mipHeight = m_imageMeta.height;
    for (uint32_t i = 1; i < m_imageMeta.miplevels; i++) {
        barrier.subresourceRange.baseMipLevel = i - 1;
        barrier.oldLayout = vk::ImageLayout::eTransferDstOptimal;
        barrier.newLayout = vk::ImageLayout::eTransferSrcOptimal;
        barrier.srcAccessMask = vk::AccessFlagBits::eTransferWrite;
        barrier.dstAccessMask = vk::AccessFlagBits::eTransferRead;

        commandBuffer.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eTransfer,
                                      vk::DependencyFlags(), 0, nullptr, 0, nullptr, 1, &barrier);

        vk::ImageBlit blit{};
        blit.srcOffsets[0] = vk::Offset3D{0, 0, 0};
        blit.srcOffsets[1] = vk::Offset3D{mipWidth, mipHeight, 1};
        blit.srcSubresource.aspectMask = vk::ImageAspectFlagBits::eColor;
        blit.srcSubresource.mipLevel = i - 1;
        blit.srcSubresource.baseArrayLayer = 0;
        blit.srcSubresource.layerCount = 1;
        blit.dstOffsets[0] = vk::Offset3D{0, 0, 0};
        blit.dstOffsets[1] = vk::Offset3D{mipWidth > 1 ? mipWidth / 2 : 1, mipHeight > 1 ? mipHeight / 2 : 1, 1};
        blit.dstSubresource.aspectMask = vk::ImageAspectFlagBits::eColor;
        blit.dstSubresource.mipLevel = i;
        blit.dstSubresource.baseArrayLayer = 0;
        blit.dstSubresource.layerCount = 1;
        commandBuffer.blitImage(image, vk::ImageLayout::eTransferSrcOptimal, image,
                                vk::ImageLayout::eTransferDstOptimal, 1, &blit, vk::Filter::eLinear);
        barrier.oldLayout = vk::ImageLayout::eTransferSrcOptimal;
        barrier.newLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
        barrier.srcAccessMask = vk::AccessFlagBits::eTransferRead;
        barrier.dstAccessMask = vk::AccessFlagBits::eShaderRead;
        commandBuffer.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eFragmentShader,
                                      vk::DependencyFlags(), 0, nullptr, 0, nullptr, 1, &barrier);
        mipWidth = (mipWidth > 1) ? (mipWidth >> 1) : 1;
        mipHeight = (mipHeight > 1) ? (mipHeight >> 1) : 1;
    }

    barrier.subresourceRange.baseMipLevel = m_imageMeta.miplevels - 1;
    barrier.oldLayout = vk::ImageLayout::eTransferDstOptimal;
    barrier.newLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
    barrier.srcAccessMask = vk::AccessFlagBits::eTransferWrite;
    barrier.dstAccessMask = vk::AccessFlagBits::eShaderRead;
    commandBuffer.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eFragmentShader,
                                  vk::DependencyFlags(), 0, nullptr, 0, nullptr, 1, &barrier);
}

vk::WriteDescriptorSet TextureImage::constructDescriptorSet(vk::DescriptorSet descriptorSet,
                                                            vk::DescriptorType descriptorType, uint32_t binding)
{
    // NOTE: Pay attention to 'DescriptorImageInfo' lifetime, which refer to descriptorWrite.pBufferInfo
    vk::DescriptorImageInfo &imageInfo = m_descriptorImageInfo;
    imageInfo.imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
    imageInfo.imageView = *m_textureImageView;
    imageInfo.sampler = *m_textureSampler;
    vk::WriteDescriptorSet descriptorWrite{};
    descriptorWrite.dstSet = descriptorSet;
    descriptorWrite.dstBinding = binding;
    descriptorWrite.dstArrayElement = 0;
    descriptorWrite.descriptorType = descriptorType; // vk::DescriptorType::eCombinedImageSampler;
    descriptorWrite.descriptorCount = 1;
    descriptorWrite.pImageInfo = &imageInfo;
    return descriptorWrite;
}