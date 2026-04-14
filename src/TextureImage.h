#pragma once
#include <cstdint>
#include <memory>
#include <vulkan/vulkan.hpp>
#include "typedefs.h"
#include "MemoryObject.h"

class TextureImage : public MemoryObject {
public:
    TextureImage(std::shared_ptr<vk::Device> device, const vk::PhysicalDeviceMemoryProperties &memProperties,
                 const vk::PhysicalDeviceProperties &properties, MemoryTypes memoryType, TextureImageSpec imageMeta);
    virtual ~TextureImage();
    void recordCopyFromStagingToDevice(const vk::CommandBuffer &commandBuffer) override;
    void recordCopyFromDeviceToStaging(const vk::CommandBuffer &commandBuffer) override;
    void bindToPipeline(const vk::CommandBuffer &commandBuffer) override;
    vk::WriteDescriptorSet constructDescriptorSet(vk::DescriptorSet descriptorSet, vk::DescriptorType descriptorType,
                                                  uint32_t binding) override;

    vk::ImageUsageFlags getPrimaryImageUsageFlags();
    void reserve(vk::PhysicalDeviceMemoryProperties memProperties);
    void createTextureImageView();
    void createTextureSampler(const vk::PhysicalDeviceProperties &properties);
    void recordCopyBufferToImage(const vk::CommandBuffer &commandBuffer, vk::Buffer buffer, vk::Image image,
                                 uint32_t width, uint32_t height);
    void recordTransitionImageLayout(const vk::CommandBuffer &commandBuffer, vk::Image &image, vk::Format format,
                                     vk::ImageLayout oldLayout, vk::ImageLayout newLayout);
    void generateMipmaps(const vk::CommandBuffer &commandBuffer, vk::Image &image, vk::Format imageFormat);

private:
    TextureImageSpec m_imageMeta;
    vk::DescriptorImageInfo m_descriptorImageInfo{};
    std::shared_ptr<vk::Buffer> m_stagingBuffer;
    std::shared_ptr<vk::Image> m_primaryImage;
    std::shared_ptr<vk::ImageView> m_textureImageView;
    std::shared_ptr<vk::Sampler> m_textureSampler;
};