#include "TextureImage2D.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <string>
#include <utility>

namespace {

void checkResult(vk::Result result, const char *what)
{
    if (result != vk::Result::eSuccess) {
        throw std::runtime_error(std::string(what) + " (Vulkan error: " + vk::to_string(result) + ")");
    }
}

// The staging helpers are file-local because the header exposes no staging
// methods beyond the IDeviceResource lifecycle hooks.
void ensureTextureStaging(const std::shared_ptr<vk::Device> &device,
                          const vk::PhysicalDeviceMemoryProperties &memProps, size_t byteSize,
                          std::shared_ptr<vk::Buffer> &staging, DeviceMemorySlice &stagingMemory, void *&mapped)
{
    if (staging && mapped != nullptr) {
        return;
    }
    if (!staging) {
        DeviceMemoryProperty stagingProp{memProps, vk::MemoryPropertyFlagBits::eHostVisible
                                                    | vk::MemoryPropertyFlagBits::eHostCoherent};
        BufferBundle bundle = DeviceResourceUtils::createBuffer(
            device, byteSize, vk::BufferUsageFlagBits::eTransferSrc | vk::BufferUsageFlagBits::eTransferDst,
            stagingProp, nullptr, nullptr);
        staging = bundle.buffer;
        stagingMemory = bundle.memory;
    }

    void *data = nullptr;
    checkResult(device->mapMemory(*stagingMemory.memory, stagingMemory.offset, byteSize, vk::MemoryMapFlags{}, &data),
                "failed to map texture staging buffer");
    if (data == nullptr) {
        throw std::runtime_error("texture staging buffer mapped to a null pointer");
    }
    mapped = data;
}

void releaseTextureStaging(const std::shared_ptr<vk::Device> &device, std::shared_ptr<vk::Buffer> &staging,
                           DeviceMemorySlice &stagingMemory, void *&mapped)
{
    if (mapped != nullptr && stagingMemory.valid()) {
        device->unmapMemory(*stagingMemory.memory);
        mapped = nullptr;
    }
    staging.reset();
    stagingMemory = DeviceMemorySlice{};
}

} // namespace

TextureImage2D::TextureImage2D(std::shared_ptr<vk::Device> device, vk::PhysicalDeviceMemoryProperties memProps,
                               std::shared_ptr<DeviceMemoryAllocator> allocator, TextureImageSpec spec,
                               vk::Format format) :
    m_device(std::move(device)), m_allocator(std::move(allocator)), m_memProps(memProps), m_spec(spec),
    m_format(format), m_byteSize(spec.getTotalPixels()), m_width(static_cast<int32_t>(spec.width)),
    m_height(static_cast<int32_t>(spec.height))
{
    vk::ImageCreateInfo imageInfo{};
    imageInfo.imageType = vk::ImageType::e2D;
    imageInfo.extent = vk::Extent3D(spec.width, spec.height, 1);
    imageInfo.mipLevels = spec.miplevels;
    imageInfo.arrayLayers = 1;
    imageInfo.format = format;
    imageInfo.tiling = vk::ImageTiling::eOptimal;
    imageInfo.initialLayout = vk::ImageLayout::eUndefined;
    imageInfo.usage = vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eSampled
                      | vk::ImageUsageFlagBits::eTransferSrc;
    imageInfo.samples = vk::SampleCountFlagBits::e1;
    imageInfo.sharingMode = vk::SharingMode::eExclusive;

    DeviceMemoryProperty primaryProp{memProps, vk::MemoryPropertyFlagBits::eDeviceLocal};
    ImageBundle imageBundle = DeviceResourceUtils::createImage(m_device, imageInfo, primaryProp, m_allocator);
    m_image = imageBundle.image;
    m_memory = imageBundle.memory;

    m_imageView = DeviceResourceUtils::createImageView(m_device, *m_image, m_format,
                                                       vk::ImageAspectFlagBits::eColor, spec.miplevels,
                                                       vk::ImageViewType::e2D);
    createSampler();
}

TextureImage2D::~TextureImage2D()
{
    releaseTextureStaging(m_device, m_staging, m_stagingMemory, m_mapped);
    m_imageView.reset();
    m_sampler.reset();
    m_image.reset();
    m_memory = DeviceMemorySlice{};
}

void TextureImage2D::createSampler()
{
    vk::SamplerCreateInfo samplerInfo{};
    samplerInfo.magFilter = vk::Filter::eLinear;
    samplerInfo.minFilter = vk::Filter::eLinear;
    samplerInfo.mipmapMode = vk::SamplerMipmapMode::eLinear;
    // Every mesh in this project maps UVs exactly onto [0, 1] per face, so the
    // wrap modes must not be eRepeat: with linear filtering, u = 0 and u = 1 sit
    // on the texel border and would blend the opposite edge into the face border
    // (a one-texel seam along every cube edge, amplified by mip selection).
    samplerInfo.addressModeU = vk::SamplerAddressMode::eClampToEdge;
    samplerInfo.addressModeV = vk::SamplerAddressMode::eClampToEdge;
    samplerInfo.addressModeW = vk::SamplerAddressMode::eClampToEdge;
    // Without physical-device limits here, use the widely supported maximum.
    samplerInfo.anisotropyEnable = VK_TRUE;
    samplerInfo.maxAnisotropy = 16.0f;
    samplerInfo.borderColor = vk::BorderColor::eFloatOpaqueBlack;
    samplerInfo.unnormalizedCoordinates = VK_FALSE;
    samplerInfo.compareEnable = VK_FALSE;
    samplerInfo.compareOp = vk::CompareOp::eAlways;
    samplerInfo.minLod = 0.0f;
    samplerInfo.maxLod = VK_LOD_CLAMP_NONE;
    samplerInfo.mipLodBias = 0.0f;

    vk::Sampler sampler{};
    checkResult(m_device->createSampler(&samplerInfo, nullptr, &sampler), "failed to create texture sampler");
    m_sampler = std::shared_ptr<vk::Sampler>(new vk::Sampler(sampler), [device = m_device](vk::Sampler *p) {
        if (p && *p) {
            device->destroySampler(*p, nullptr);
        }
        delete p;
    });
}

void TextureImage2D::updateRawData(const void *data, size_t size)
{
    if (data == nullptr || size == 0) {
        return;
    }
    ensureTextureStaging(m_device, m_memProps, m_byteSize, m_staging, m_stagingMemory, m_mapped);
    std::memcpy(m_mapped, data, std::min(size, m_byteSize));
    m_dirty = true;
}

void TextureImage2D::OnUploadCompleted()
{
    releaseTextureStaging(m_device, m_staging, m_stagingMemory, m_mapped);
}

void TextureImage2D::OnReadbackRequested()
{
    ensureTextureStaging(m_device, m_memProps, m_byteSize, m_staging, m_stagingMemory, m_mapped);
}

void TextureImage2D::recordCopyFromStagingToDevice(const vk::CommandBuffer &cmd)
{
    if (!m_image || !m_staging) {
        return;
    }

    DeviceResourceUtils::transitionImageLayout(cmd, *m_image, m_format, vk::ImageLayout::eUndefined,
                                               vk::ImageLayout::eTransferDstOptimal, m_spec.miplevels, 1);
    DeviceResourceUtils::copyBufferToImage(cmd, *m_staging, *m_image, m_spec.width, m_spec.height, 1);

    if (m_spec.miplevels > 1) {
        // generateMipmaps leaves every level in eShaderReadOnlyOptimal.
        DeviceResourceUtils::generateMipmaps(cmd, *m_image, m_format, m_width, m_height, m_spec.miplevels, 1);
    } else {
        DeviceResourceUtils::transitionImageLayout(cmd, *m_image, m_format, vk::ImageLayout::eTransferDstOptimal,
                                                   vk::ImageLayout::eShaderReadOnlyOptimal, m_spec.miplevels, 1);
    }
}

vk::WriteDescriptorSet TextureImage2D::constructWriteDescriptorSet(vk::DescriptorSet set, vk::DescriptorType type,
                                                                   uint32_t binding)
{
    m_imageInfo.imageView = imageView();
    m_imageInfo.sampler = sampler();
    m_imageInfo.imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal;

    vk::WriteDescriptorSet write{};
    write.dstSet = set;
    write.dstBinding = binding;
    write.dstArrayElement = 0;
    write.descriptorType = type;
    write.descriptorCount = 1;
    write.pImageInfo = &m_imageInfo;
    return write;
}

void TextureImage2D::accept(DeviceResourceVisitor &visitor)
{
    visitor.visit(*this);
}
