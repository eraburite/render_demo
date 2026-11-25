#include "TextureImage.h"

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

TextureImage::TextureImage(std::shared_ptr<vk::Device> device, vk::PhysicalDeviceMemoryProperties memProps,
                           std::shared_ptr<DeviceMemoryAllocator> allocator, TextureImageSpec spec) :
    m_device(std::move(device)), m_allocator(std::move(allocator)), m_memProps(memProps), m_spec(spec),
    m_stagingBytes(spec.uploadByteSize()), m_width(static_cast<int32_t>(spec.width)),
    m_height(static_cast<int32_t>(spec.height))
{
    if (m_spec.miplevels == 0) {
        m_spec.miplevels = 1;
    }
    if (m_spec.layers == 0) {
        m_spec.layers = 1;
    }
    if (m_spec.faces == 0) {
        m_spec.faces = 1;
    }
    // Recompute now that the dimensions are known to be sane.
    m_stagingBytes = m_spec.uploadByteSize();

    vk::ImageCreateInfo imageInfo{};
    imageInfo.imageType = m_spec.imageType();
    imageInfo.extent = vk::Extent3D(m_spec.width, m_spec.height, std::max(1u, m_spec.depth));
    // Cubemap faces are array layers of the same image, so a cube is just an
    // array of 6 layers with a cube-compatible view.
    imageInfo.mipLevels = std::max(1u, m_spec.miplevels);
    imageInfo.arrayLayers = m_spec.arrayLayers();
    imageInfo.format = m_spec.format;
    imageInfo.tiling = vk::ImageTiling::eOptimal;
    imageInfo.initialLayout = vk::ImageLayout::eUndefined;
    imageInfo.usage = vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eSampled
                      | vk::ImageUsageFlagBits::eTransferSrc;
    imageInfo.samples = vk::SampleCountFlagBits::e1;
    imageInfo.sharingMode = vk::SharingMode::eExclusive;
    imageInfo.flags = m_spec.faces == 6 ? vk::ImageCreateFlagBits::eCubeCompatible : vk::ImageCreateFlags{};

    DeviceMemoryProperty primaryProp{memProps, vk::MemoryPropertyFlagBits::eDeviceLocal};
    ImageBundle imageBundle = DeviceResourceUtils::createImage(m_device, imageInfo, primaryProp, m_allocator);
    m_image = imageBundle.image;
    m_memory = imageBundle.memory;

    m_imageView = DeviceResourceUtils::createImageView(m_device, *m_image, m_spec.format,
                                                      vk::ImageAspectFlagBits::eColor, std::max(1u, m_spec.miplevels),
                                                      m_spec.imageViewType(), 0, m_spec.arrayLayers());
    createSampler();
}

TextureImage::~TextureImage()
{
    releaseTextureStaging(m_device, m_staging, m_stagingMemory, m_mapped);
    m_imageView.reset();
    m_sampler.reset();
    m_image.reset();
    m_memory = DeviceMemorySlice{};
}

void TextureImage::createSampler()
{
    vk::SamplerCreateInfo samplerInfo{};
    samplerInfo.magFilter = vk::Filter::eLinear;
    samplerInfo.minFilter = vk::Filter::eLinear;
    samplerInfo.mipmapMode = vk::SamplerMipmapMode::eLinear;
    // Mesh UVs in this project map exactly onto [0, 1] per face, so the default
    // wrap mode must not be eRepeat: with linear filtering u = 0 and u = 1 sit
    // on the texel border and would blend the opposite edge into the face
    // border (a one-texel seam, amplified by mip selection).  Tiling textures
    // (terrain, floors) opt into eRepeat through the spec.
    samplerInfo.addressModeU = m_spec.addressMode;
    samplerInfo.addressModeV = m_spec.addressMode;
    samplerInfo.addressModeW = m_spec.addressMode;
    // Without physical-device limits here, use the widely supported maximum.
    samplerInfo.anisotropyEnable = m_spec.anisotropy ? VK_TRUE : VK_FALSE;
    samplerInfo.maxAnisotropy = m_spec.anisotropy ? 16.0f : 1.0f;
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

void TextureImage::updateRawData(const void *data, size_t size)
{
    if (data == nullptr || size == 0) {
        return;
    }
    ensureTextureStaging(m_device, m_memProps, m_stagingBytes, m_staging, m_stagingMemory, m_mapped);
    std::memcpy(m_mapped, data, std::min(size, m_stagingBytes));
    m_dirty = true;
}

void TextureImage::OnUploadCompleted()
{
    releaseTextureStaging(m_device, m_staging, m_stagingMemory, m_mapped);
}

void TextureImage::OnReadbackRequested()
{
    ensureTextureStaging(m_device, m_memProps, m_stagingBytes, m_staging, m_stagingMemory, m_mapped);
}

void TextureImage::recordCopyFromStagingToDevice(const vk::CommandBuffer &cmd)
{
    if (!m_image || !m_staging) {
        return;
    }

    const uint32_t mipCount = std::max(1u, m_spec.miplevels);
    DeviceResourceUtils::transitionImageLayout(cmd, *m_image, m_spec.format, vk::ImageLayout::eUndefined,
                                               vk::ImageLayout::eTransferDstOptimal, mipCount, m_spec.arrayLayers());

    if (m_spec.mipsInData) {
        // The host block already holds every level, packed (mip -> layer -> face).
        vk::DeviceSize offset = 0;
        for (uint32_t mip = 0; mip < mipCount; ++mip) {
            const uint32_t mipWidth = std::max(1u, m_spec.width >> mip);
            const uint32_t mipHeight = std::max(1u, m_spec.height >> mip);
            const uint32_t mipDepth = std::max(1u, m_spec.depth >> mip);
            DeviceResourceUtils::copyBufferToImageRegion(cmd, *m_staging, *m_image, offset, mip, 0,
                                                         m_spec.arrayLayers(), mipWidth, mipHeight, mipDepth);
            offset += m_spec.mipByteSize(mip);
        }
        DeviceResourceUtils::transitionImageLayout(cmd, *m_image, m_spec.format, vk::ImageLayout::eTransferDstOptimal,
                                                   vk::ImageLayout::eShaderReadOnlyOptimal, mipCount,
                                                   m_spec.arrayLayers());
        return;
    }

    DeviceResourceUtils::copyBufferToImageRegion(cmd, *m_staging, *m_image, 0, 0, 0, m_spec.arrayLayers(),
                                                 m_spec.width, m_spec.height, std::max(1u, m_spec.depth));

    if (mipCount > 1) {
        // generateMipmaps leaves every level in eShaderReadOnlyOptimal.
        DeviceResourceUtils::generateMipmaps(cmd, *m_image, m_spec.format, m_width, m_height, mipCount,
                                             m_spec.arrayLayers());
    } else {
        DeviceResourceUtils::transitionImageLayout(cmd, *m_image, m_spec.format, vk::ImageLayout::eTransferDstOptimal,
                                                   vk::ImageLayout::eShaderReadOnlyOptimal, mipCount,
                                                   m_spec.arrayLayers());
    }
}

vk::WriteDescriptorSet TextureImage::constructWriteDescriptorSet(vk::DescriptorSet set, vk::DescriptorType type,
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

void TextureImage::accept(DeviceResourceVisitor &visitor)
{
    visitor.visit(*this);
}
