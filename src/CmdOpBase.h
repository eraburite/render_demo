#pragma once
#include <cstddef>
#include <memory>
#include <vulkan/vulkan.hpp>
#include "DescriptorBindingState.h"
#include "typedefs.h"

// A recorded operation.  LoopSequence drives the frame lifecycle; CmdOp only
// records its own commands into the supplied command buffer.
class CmdOpBase {
public:
    CmdOpBase() = default;
    CmdOpBase(const CmdOpBase &) = delete;
    CmdOpBase &operator=(const CmdOpBase &) = delete;
    virtual ~CmdOpBase() noexcept = default;

    template <typename T>
    void OnUpdateRawData(ResourceTypes resourceType, const std::vector<T> &data)
    {
        OnUpdateRawData(resourceType, data.data(), data.size() * sizeof(T));
    }
    template <typename T>
    void OnUpdateRawData(ResourceTypes resourceType, const T &data)
    {
        OnUpdateRawData(resourceType, &data, sizeof(T));
    }
    template <typename T>
    void OnUpdateRawData(ResourceTypes resourceType, uint32_t frame, const std::vector<T> &data)
    {
        OnUpdateRawData(resourceType, frame, data.data(), data.size() * sizeof(T));
    }
    template <typename T>
    void OnUpdateRawData(ResourceTypes resourceType, uint32_t frame, const T &data)
    {
        OnUpdateRawData(resourceType, frame, &data, sizeof(T));
    }
    template <typename T>
    void OnUpdateRawData(ResourceId id, uint32_t frame, const std::vector<T> &data)
    {
        OnUpdateRawData(id, frame, data.data(), data.size() * sizeof(T));
    }
    template <typename T>
    void OnUpdateRawData(ResourceId id, uint32_t frame, const T &data)
    {
        OnUpdateRawData(id, frame, &data, sizeof(T));
    }

    virtual void OnUpdateRawData(ResourceTypes, const void *, size_t) {}
    virtual void OnUpdateRawData(ResourceTypes, uint32_t, const void *, size_t) {}
    virtual void OnUpdateRawData(ResourceId, uint32_t, const void *, size_t) {}
    virtual UpdateFrequency GetFrequency() const = 0;

    virtual void Record(const vk::CommandBuffer &cmd, uint32_t frame, DescriptorBindingState &state) = 0;
};
