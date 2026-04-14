#pragma once
#include <memory>
#include <vulkan/vulkan.hpp>
#include "typedefs.h"
class SwapChain;

class CmdOpBase {
public:
    CmdOpBase() = default;
    CmdOpBase(const CmdOpBase &) = delete;
    CmdOpBase(const CmdOpBase &&) = delete;
    CmdOpBase &operator=(const CmdOpBase &) = delete;
    CmdOpBase &operator=(const CmdOpBase &&) = delete;
    virtual ~CmdOpBase() noexcept {}

    template <typename T>
    void OnUpdateRawData(ResourceTypes resourceType, const std::vector<T> &data)
    {
        size_t size = data.size() * sizeof(T);
        OnUpdateRawData(resourceType, data.data(), size);
    }

    template <typename T>
    void OnUpdateRawData(ResourceTypes resourceType, const T &data)
    {
        size_t size = sizeof(T);
        OnUpdateRawData(resourceType, &data, size);
    }

    template <typename T>
    void OnUpdateRawData(ResourceTypes resourceType, uint32_t currentFrame, const std::vector<T> &data)
    {
        size_t size = data.size() * sizeof(T);

        OnUpdateRawData(resourceType, currentFrame, data.data(), size);
    }

    template <typename T>
    void OnUpdateRawData(ResourceTypes resourceType, uint32_t currentFrame, const T &data)
    {
        size_t size = sizeof(T);
        OnUpdateRawData(resourceType, currentFrame, &data, size);
    }

    template <typename T>
    void OnUpdateRawData(ResourceId resourceId, uint32_t currentFrame, const std::vector<T> &data)
    {
        size_t size = data.size() * sizeof(T);
        OnUpdateRawData(resourceId, currentFrame, data.data(), size);
    }

    template <typename T>
    void OnUpdateRawData(ResourceId resourceId, uint32_t currentFrame, const T &data)
    {
        size_t size = sizeof(T);
        OnUpdateRawData(resourceId, currentFrame, &data, size);
    }

    virtual void OnUpdateRawData(ResourceTypes resourceType, const void *data, size_t size) = 0;
    virtual void OnUpdateRawData(ResourceTypes resourceType, uint32_t currentFrame, const void *data, size_t size) = 0;
    virtual void OnUpdateRawData(ResourceId resourceId, uint32_t currentFrame, const void *data, size_t size) = 0;
    virtual UpdateFrequency GetFrequency() const = 0;

    virtual void BindFrameBuffer(const std::shared_ptr<vk::Framebuffer> &framebuffer,
                                 const std::shared_ptr<vk::Extent2D> &swapChainExtent)
    {
    }

    virtual void Record(const vk::CommandBuffer &commandBuffer, uint32_t currentFrame) = 0;
};