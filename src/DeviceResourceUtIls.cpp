#include "DeviceResourceUtils.h"

std::shared_ptr<vk::ImageView> DeviceResourceUtils::createImageView(std::shared_ptr<vk::Device> device,
                                                                    const vk::Image &image, vk::Format format,
                                                                    uint32_t miplevels, vk::ImageAspectFlags aspectMask)
{
    vk::ImageViewCreateInfo createInfo{};
    createInfo.image = image;
    createInfo.viewType = vk::ImageViewType::e2D;
    createInfo.format = format;
    createInfo.components.r = vk::ComponentSwizzle::eIdentity;
    createInfo.components.g = vk::ComponentSwizzle::eIdentity;
    createInfo.components.b = vk::ComponentSwizzle::eIdentity;
    createInfo.components.a = vk::ComponentSwizzle::eIdentity;
    createInfo.subresourceRange.aspectMask = aspectMask;
    createInfo.subresourceRange.baseMipLevel = 0;
    createInfo.subresourceRange.levelCount = miplevels; // 1;
    createInfo.subresourceRange.baseArrayLayer = 0;
    createInfo.subresourceRange.layerCount = 1;

    vk::ImageView swapChainImageView{};
    if (device->createImageView(&createInfo, nullptr, &swapChainImageView) != vk::Result::eSuccess) {
        throw std::runtime_error(" failed to create image views!");
    }

    return std::shared_ptr<vk::ImageView>(new vk::ImageView(swapChainImageView), [device](vk::ImageView *p) {
        if (p && *p) {
            device->destroyImageView(*p, nullptr);
        }
        delete p;
    });
}

std::tuple<std::shared_ptr<vk::Image>, std::shared_ptr<vk::DeviceMemory>>
DeviceResourceUtils::createImage(std::shared_ptr<vk::Device> device, uint32_t width, uint32_t height, vk::Format format,
                                 uint32_t mipLevels, vk::SampleCountFlagBits msaaSamples, vk::ImageTiling tiling,
                                 vk::ImageUsageFlags usage, const DeviceMemoryProperty &memProperties)
{
    vk::ImageCreateInfo imageInfo{};
    imageInfo.imageType = vk::ImageType::e2D;
    imageInfo.extent.width = width;
    imageInfo.extent.height = height;
    imageInfo.extent.depth = 1;
    imageInfo.mipLevels = mipLevels; // 1;
    imageInfo.arrayLayers = 1;
    imageInfo.format = format;
    imageInfo.tiling = tiling;
    imageInfo.initialLayout = vk::ImageLayout::eUndefined;
    imageInfo.usage = usage;
    imageInfo.samples = msaaSamples; // vk::SampleCountFlagBits::e1
    imageInfo.sharingMode = vk::SharingMode::eExclusive;

    vk::Image image{};
    vk::DeviceMemory imageMemory{};
    if (device->createImage(&imageInfo, nullptr, &image) != vk::Result::eSuccess) {
        throw std::runtime_error(" failed to create image!");
    }

    vk::MemoryRequirements memRequirements{};
    device->getImageMemoryRequirements(image, &memRequirements);

    vk::MemoryAllocateInfo allocInfo{};
    allocInfo.allocationSize = memRequirements.size;
    allocInfo.memoryTypeIndex = findMemoryType(memRequirements.memoryTypeBits, memProperties);
    if (device->allocateMemory(&allocInfo, nullptr, &imageMemory) != vk::Result::eSuccess) {
        throw std::runtime_error(" failed to allocate image memory!");
    }

    device->bindImageMemory(image, imageMemory, 0);

    auto refImageMemory =
        std::shared_ptr<vk::DeviceMemory>(new vk::DeviceMemory(imageMemory), [device](vk::DeviceMemory *p) {
            if (p && *p) {
                device->freeMemory(*p, nullptr);
            }
            delete p;
        });

    auto refBuffer =
        std::shared_ptr<vk::Image>(new vk::Image(image), [device, stagingMemory = refImageMemory](vk::Image *p) {
            if (p && *p) {
                device->destroyImage(*p, nullptr);
            }
            delete p;
        });

    return std::tuple(refBuffer, refImageMemory);
}

std::tuple<std::shared_ptr<vk::Buffer>, std::shared_ptr<vk::DeviceMemory>>
DeviceResourceUtils::createBuffer(std::shared_ptr<vk::Device> device, vk::DeviceSize bufferSize,
                                  vk::BufferUsageFlags usage, const DeviceMemoryProperty &memProperties)
{
    vk::Buffer buffer{};
    vk::DeviceMemory bufferMemory{};
    if (bufferSize < 1) {
        throw std::runtime_error(" attempted to create a zero-sized buffer");
    }

    vk::BufferCreateInfo bufferInfo{};
    bufferInfo.size = bufferSize;
    bufferInfo.usage = usage;
    bufferInfo.sharingMode = vk::SharingMode::eExclusive;
    if (device->createBuffer(&bufferInfo, nullptr, &buffer) != vk::Result::eSuccess) {
        throw std::runtime_error(" failed to create buffer!");
    }

    vk::MemoryRequirements memRequirements{};
    device->getBufferMemoryRequirements(buffer, &memRequirements);
    vk::MemoryAllocateInfo allocInfo{};
    allocInfo.allocationSize = memRequirements.size;
    allocInfo.memoryTypeIndex = findMemoryType(memRequirements.memoryTypeBits, memProperties);
    if (device->allocateMemory(&allocInfo, nullptr, &bufferMemory) != vk::Result::eSuccess) {
        throw std::runtime_error(" failed to allocate buffer memory!");
    }

    device->bindBufferMemory(buffer, bufferMemory, 0);

    auto refBufferMemory =
        std::shared_ptr<vk::DeviceMemory>(new vk::DeviceMemory(bufferMemory), [device](vk::DeviceMemory *p) {
            if (p && *p) {
                device->freeMemory(*p, nullptr);
            }
            delete p;
        });

    auto refBuffer =
        std::shared_ptr<vk::Buffer>(new vk::Buffer(buffer), [device, buffMemory = refBufferMemory](vk::Buffer *p) {
            if (p && *p) {
                device->destroyBuffer(*p, nullptr);
            }
            delete p;
        });
    return std::tuple(refBuffer, refBufferMemory);
}

uint32_t DeviceResourceUtils::findMemoryType(uint32_t typeFilter, const DeviceMemoryProperty &memProperties)
{
    for (uint32_t i = 0; i < memProperties.physicalMemProperties.memoryTypeCount; i++) {
        if ((typeFilter & (1 << i))
            && (memProperties.physicalMemProperties.memoryTypes[i].propertyFlags & memProperties.memPropertyFlags)
                   == memProperties.memPropertyFlags) {
            return i;
        }
    }
    throw std::runtime_error(" failed to find suitable memory type!");
}